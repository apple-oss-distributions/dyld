/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include "Array.h"

#include "BindOpcodesWriter.h"
#include "Misc.h"
#include "Image.h"

using dyld3::Array;
typedef mach_o::BindOpcodes::LocAndTarget LocAndTarget;

namespace mach_o {

//
// MARK: --- Common methods ---
//

static void sortBindOpcodes(std::span<BindOpcodes::LocAndTarget> binds)
{
    std::sort(binds.begin(), binds.end(), [](const BindOpcodes::LocAndTarget& a, const BindOpcodes::LocAndTarget& b) {
        // sort by library, symbol, type, flags, then address
        if ( a.target != b.target ) {
            if ( a.target->libOrdinal != b.target->libOrdinal )
                return (a.target->libOrdinal < b.target->libOrdinal );
            if ( a.target->symbolName != b.target->symbolName ) {
                int cmpRes = strcmp(a.target->symbolName, b.target->symbolName);
                if ( cmpRes != 0 )
                    return (cmpRes < 0);
            }
            // TODO: type, if we ever need it, which we probably don't
            //if ( this->_type != rhs._type )
            //    return  (this->_type < rhs._type );
            // weak import set the flags field
            if ( a.target->weakImport != b.target->weakImport )
                return a.target->weakImport;
            if ( a.target->addend != b.target->addend )
                return (a.target->addend < b.target->addend );
        }
        // Sort by seg index and seg offset, which gives us address
        if ( a.segIndex != b.segIndex )
            return (a.segIndex < b.segIndex );
        return (a.segOffset < b.segOffset );
    });
}


// to work with dyld2's algorithm, all weak-bind opcodes need to be sorted by symbol name
static void sortWeakBindOpcodes(std::span<BindOpcodes::LocAndTarget> binds)
{
    std::sort(binds.begin(), binds.end(), [](const BindOpcodes::LocAndTarget& a, const BindOpcodes::LocAndTarget& b) {
        // sort by symbol name then address
        if ( a.target != b.target ) {
            if ( a.target->symbolName != b.target->symbolName )
                return ( strcmp(a.target->symbolName, b.target->symbolName) < 0 );
        }
        // Sort by seg index and seg offset, which gives us address
        if ( a.segIndex != b.segIndex )
            return (a.segIndex < b.segIndex );
        return (a.segOffset < b.segOffset );
    });
}

enum class BuilderKind { regular, lazy, weak };

static void append_byte(std::vector<uint8_t>& buffer, uint8_t value)
{
    buffer.push_back(value);
}

static void append_uleb128(std::vector<uint8_t>& buffer, uint64_t value)
{
    uint8_t byte;
    do {
        byte = value & 0x7F;
        value &= ~0x7F;
        if ( value != 0 )
            byte |= 0x80;
        buffer.push_back(byte);
        value = value >> 7;
    } while( byte >= 0x80 );
}

static void append_sleb128(std::vector<uint8_t>& buffer, int64_t value)
{
    bool isNeg = ( value < 0 );
    uint8_t byte;
    bool more;
    do {
        byte = value & 0x7F;
        value = value >> 7;
        if ( isNeg )
            more = ( (value != -1) || ((byte & 0x40) == 0) );
        else
            more = ( (value != 0) || ((byte & 0x40) != 0) );
        if ( more )
            byte |= 0x80;
        buffer.push_back(byte);
    }
    while( more );
}

static void append_string(std::vector<uint8_t>& buffer, const char* str)
{
    for (const char* s = str; *s != '\0'; ++s)
        buffer.push_back(*s);
    buffer.push_back('\0');
}

static std::vector<uint8_t> buildBindOpcodes(std::span<LocAndTarget> binds, bool is64, BuilderKind kind, LazyBindOpcodesWriter::LazyStartRecorder lazyStartsRecorder)
{
    if ( binds.empty() )
        return { };

    uint32_t pointerSize = is64 ? 8 : 4;
    std::vector<uint8_t> opcodes;

    struct BindTmp
    {
        BindTmp(uint8_t op, uint64_t p1, uint64_t p2=0, const char* s=NULL)
            : opcode(op), operand1(p1), operand2(p2), name(s) {}
        uint8_t        opcode;
        uint64_t    operand1;
        uint64_t    operand2;
        const char*    name;
    };

    // convert to temp encoding that can be more easily optimized
    std::vector<BindTmp> mid;
    uint32_t    curSegIndex  = 0;
    uint64_t    curSegOffset = -1;
    int         ordinal      = (kind == BuilderKind::weak) ? -3 : 0x80000000;
    const char* symbolName   = nullptr;
    int64_t     addend       = 0;
    uint8_t     type         = 0;

    if ( kind == BuilderKind::lazy ) {
        for (const LocAndTarget& bind : binds) {
            mid.emplace_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB, bind.segIndex, bind.segOffset);
            if ( bind.target->libOrdinal <= 0 ) {
                // special lookups are encoded as negative numbers in BindingInfo
                mid.emplace_back(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM, bind.target->libOrdinal);
            }
            else if ( bind.target->libOrdinal<= 15 ) {
                // small libOrdinals can fit in opcode
                mid.emplace_back(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM, bind.target->libOrdinal);
            }
            else {
                mid.emplace_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB, bind.target->libOrdinal);
            }
            uint32_t flags = (bind.target->weakImport ? BIND_SYMBOL_FLAGS_WEAK_IMPORT : 0);
            mid.emplace_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM, flags, 0, bind.target->symbolName);
            mid.emplace_back(BIND_OPCODE_DO_BIND, 0);
            mid.emplace_back(BIND_OPCODE_DONE, 0);
        }
    }
    else {
        // sort by library, symbol, type, then address
        if ( kind == BuilderKind::weak )
            sortWeakBindOpcodes(binds);
        else
            sortBindOpcodes(binds);
        for (const LocAndTarget& bind : binds) {
            // weak binds don't have ordinals
            if ( kind != BuilderKind::weak ) {
                if ( ordinal != bind.target->libOrdinal ) {
                    if ( bind.target->libOrdinal <= 0 ) {
                        // special lookups are encoded as negative numbers in BindingInfo
                        mid.emplace_back(BIND_OPCODE_SET_DYLIB_SPECIAL_IMM, bind.target->libOrdinal);
                    }
                    else {
                        mid.emplace_back(BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB, bind.target->libOrdinal);
                    }
                    ordinal = bind.target->libOrdinal;
                }
            }
            if ( (symbolName == nullptr) || (strcmp(symbolName, bind.target->symbolName) != 0) ) {
                uint32_t flags = 0;
                if ( bind.target->weakImport )
                    flags |= BIND_SYMBOL_FLAGS_WEAK_IMPORT;
                if ( bind.target->strongOverrideOfWeakDef )
                    flags |= BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION;
                mid.emplace_back(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM, flags, 0, bind.target->symbolName);
                symbolName = bind.target->symbolName;

                // We don't emit any other opcodes for strong overrides, so skip to the next bind
                if ( bind.target->strongOverrideOfWeakDef )
                    continue;
            }
            if ( type != BIND_TYPE_POINTER ) {
                // Note: this code base does not support creating old i386 text fixups
                mid.emplace_back(BIND_OPCODE_SET_TYPE_IMM, BIND_TYPE_POINTER);
                type = BIND_TYPE_POINTER;
            }
            if ( curSegIndex != bind.segIndex ) {
                mid.emplace_back(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB, bind.segIndex, bind.segOffset);
                curSegIndex  = bind.segIndex;
                curSegOffset = bind.segOffset;
            }
            else if ( curSegOffset != bind.segOffset ) {
                mid.emplace_back(BIND_OPCODE_ADD_ADDR_ULEB, bind.segOffset - curSegOffset);
                curSegOffset = bind.segOffset;
            }
            if ( addend != bind.target->addend ) {
                mid.emplace_back(BIND_OPCODE_SET_ADDEND_SLEB, bind.target->addend);
                addend = bind.target->addend;
            }
            mid.emplace_back(BIND_OPCODE_DO_BIND, 0);
            curSegOffset += pointerSize;
        }
        mid.emplace_back(BIND_OPCODE_DONE, 0);

        // optimize phase 1, combine bind/add pairs
        BindTmp* dst = &mid[0];
        for (const BindTmp* src = &mid[0]; src->opcode != BIND_OPCODE_DONE; ++src) {
            if ( (src->opcode == BIND_OPCODE_DO_BIND)
                && (src[1].opcode == BIND_OPCODE_ADD_ADDR_ULEB) ) {
                dst->opcode = BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB;
                dst->operand1 = src[1].operand1;
                ++src;
                ++dst;
            }
            else {
                *dst++ = *src;
            }
        }
        dst->opcode = BIND_OPCODE_DONE;

        // optimize phase 2, compress packed runs of BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB with
        // same addr delta into one BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB
        dst = &mid[0];
        for (const BindTmp* src = &mid[0]; src->opcode != BIND_OPCODE_DONE; ++src) {
            uint64_t delta = src->operand1;
            if ( (src->opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB)
                    && (src[1].opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB)
                    && (src[1].operand1 == delta) ) {
                // found at least two in a row, this is worth compressing
                dst->opcode = BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB;
                dst->operand1 = 1;
                dst->operand2 = delta;
                ++src;
                while ( (src->opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB)
                        && (src->operand1 == delta) ) {
                    dst->operand1++;
                    ++src;
                }
                --src;
                ++dst;
            }
            else {
                *dst++ = *src;
            }
        }
        dst->opcode = BIND_OPCODE_DONE;

        // optimize phase 3, use immediate encodings
        for (BindTmp* p = &mid[0]; p->opcode != REBASE_OPCODE_DONE; ++p) {
            if ( (p->opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB)
                && (p->operand1 < (15*pointerSize))
                && ((p->operand1 % pointerSize) == 0) ) {
                p->opcode = BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED;
                p->operand1 = p->operand1/pointerSize;
            }
            else if ( (p->opcode == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB) && (p->operand1 <= 15) ) {
                p->opcode = BIND_OPCODE_SET_DYLIB_ORDINAL_IMM;
            }
        }
        dst->opcode = BIND_OPCODE_DONE;
    }

    // convert to compressed encoding
    opcodes.reserve(256);
    bool done = false;
    size_t entryStartOffset = 0;
    for (const BindTmp& tmp : mid) {
        switch ( tmp.opcode ) {
            case BIND_OPCODE_DONE:
                append_byte(opcodes, BIND_OPCODE_DONE);
                if ( kind != BuilderKind::lazy )
                    done = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                append_byte(opcodes, BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | tmp.operand1);
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                append_byte(opcodes, BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB);
                append_uleb128(opcodes, tmp.operand1);
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                append_byte(opcodes, BIND_OPCODE_SET_DYLIB_SPECIAL_IMM | (tmp.operand1 & BIND_IMMEDIATE_MASK));
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                if ( lazyStartsRecorder )
                    lazyStartsRecorder(entryStartOffset, tmp.name);
                append_byte(opcodes, BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM | tmp.operand1);
                append_string(opcodes, tmp.name);
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                append_byte(opcodes, BIND_OPCODE_SET_TYPE_IMM | tmp.operand1);
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                append_byte(opcodes, BIND_OPCODE_SET_ADDEND_SLEB);
                append_sleb128(opcodes, tmp.operand1);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                if ( lazyStartsRecorder )
                    entryStartOffset = opcodes.size();
                append_byte(opcodes, BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | tmp.operand1);
                append_uleb128(opcodes, tmp.operand2);
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                append_byte(opcodes, BIND_OPCODE_ADD_ADDR_ULEB);
                append_uleb128(opcodes, tmp.operand1);
                break;
            case BIND_OPCODE_DO_BIND:
                append_byte(opcodes, BIND_OPCODE_DO_BIND);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                append_byte(opcodes, BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB);
                append_uleb128(opcodes, tmp.operand1);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                append_byte(opcodes, BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED | tmp.operand1 );
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                append_byte(opcodes, BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB);
                append_uleb128(opcodes, tmp.operand1);
                append_uleb128(opcodes, tmp.operand2);
                break;
        }
        if ( done )
            break;
    }

    // align to pointer size
    while ( (opcodes.size() % pointerSize) != 0 )
        append_byte(opcodes, 0);

    return opcodes;
}

//
// MARK: --- BindOpcodesWriter methods ---
//

BindOpcodesWriter::BindOpcodesWriter(std::span<LocAndTarget> binds, bool is64)
    : BindOpcodes(nullptr, 0, is64)
{
    _opcodes = buildBindOpcodes(binds, is64, BuilderKind::regular, nullptr);

    _opcodesStart = _opcodes.data();
    _opcodesEnd   = _opcodes.data()+_opcodes.size();
}

//
// MARK: --- LazyBindOpcodesWriter methods ---
//
LazyBindOpcodesWriter::LazyBindOpcodesWriter(std::span<LocAndTarget> binds, bool is64, LazyStartRecorder lazyStartsRecorder)
    : LazyBindOpcodes(nullptr, 0, is64)
{
    _opcodes = buildBindOpcodes(binds, is64, BuilderKind::lazy, lazyStartsRecorder);

    _opcodesStart = _opcodes.data();
    _opcodesEnd   = _opcodes.data()+_opcodes.size();
}

//
// MARK: --- WeakBindOpcodesWriter methods ---
//
WeakBindOpcodesWriter::WeakBindOpcodesWriter(std::span<LocAndTarget> binds, bool is64)
    : WeakBindOpcodes(nullptr, 0, is64)
{
    _opcodes = buildBindOpcodes(binds, is64, BuilderKind::weak, nullptr);

    _opcodesStart = _opcodes.data();
    _opcodesEnd   = _opcodes.data()+_opcodes.size();
}


} // namespace mach_o
