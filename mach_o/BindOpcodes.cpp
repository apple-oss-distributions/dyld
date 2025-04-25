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

#include "BindOpcodes.h"
#include "Misc.h"
#include "Image.h"

using dyld3::Array;

namespace mach_o {

//
// MARK: --- BindOpcodes inspection methods ---
//

bool BindOpcodes::LocAndTarget::operator==(const BindOpcodes::LocAndTarget& other) const
{
    if ( this->segIndex != other.segIndex )
        return false;
    if ( this->segOffset != other.segOffset )
        return false;
    if ( this->target->libOrdinal != other.target->libOrdinal )
        return false;
    if ( this->target->weakImport != other.target->weakImport )
        return false;
    if ( this->target->strongOverrideOfWeakDef != other.target->strongOverrideOfWeakDef )
        return false;
    if ( this->target->addend != other.target->addend )
        return false;
    return (strcmp(this->target->symbolName, other.target->symbolName) == 0);
}

BindOpcodes::BindOpcodes(const uint8_t* start, size_t size, bool is64)
  : _opcodesStart(start), _opcodesEnd(start+size), _pointerSize(is64 ? 8 : 4)
{
}

bool BindOpcodes::hasDoneBetweenBinds() const
{
    return false;
}

std::optional<int> BindOpcodes::implicitLibraryOrdinal() const
{
    return std::nullopt;
}

const uint8_t* BindOpcodes::bytes(size_t& size) const
{
    size = (_opcodesEnd - _opcodesStart);
    return _opcodesStart;
}

Error BindOpcodes::valid(std::span<const MappedSegment> segments, uint32_t dylibCount, bool allowTextFixups, bool onlyFixupsInWritableSegments) const
{
    __block Error locError;
    Error opcodeErr = this->forEachBind(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset, bool libraryOrdinalSet,
                                          int libOrdinal, const char* symbolName, bool weakImport, int64_t addend, bool targetOrAddendChanged, bool& stop) {
        if ( !segIndexSet ) {
            locError = Error("%s missing preceding BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", opcodeName);
            stop = true;
        }
        else if ( segmentIndex >= segments.size() )  {
            locError = Error("%s segment index %d too large", opcodeName, segmentIndex);
            stop = true;
        }
        else if ( segmentOffset > (segments[segmentIndex].runtimeSize-_pointerSize) ) {
            locError = Error("%s segment offset 0x%08llX beyond segment '%.*s' size (0x%08llX)", opcodeName, segmentOffset,
                             (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data(),
                             segments[segmentIndex].runtimeSize);
            stop = true;
        }
        else if ( symbolName == nullptr ) {
            locError = Error("%s missing preceding BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", opcodeName);
            stop = true;
        }
        else if ( !libraryOrdinalSet ) {
            locError = Error("%s missing preceding BIND_OPCODE_SET_DYLIB_ORDINAL", opcodeName);
            stop = true;
        }
        else if ( (libOrdinal > 0) && (libOrdinal > dylibCount) ) {
            locError = Error("%s has library ordinal too large (%d) max (%d)", opcodeName, libOrdinal, dylibCount);
            stop = true;
        }
        else if ( libOrdinal < BIND_SPECIAL_DYLIB_WEAK_LOOKUP ) {
            locError = Error("%s has unknown library special ordinal (%d)", opcodeName, libOrdinal);
            stop = true;
        }
        switch ( type )  {
            case BIND_TYPE_POINTER:
                if ( !segments[segmentIndex].writable && onlyFixupsInWritableSegments ) {
                    locError = Error("%s pointer bind is in non-writable segment '%.*s'", opcodeName,
                                     (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                    stop     = true;
                }
                else if ( segments[segmentIndex].executable && onlyFixupsInWritableSegments ) {
                    locError = Error("%s pointer bind is in executable segment '%.*s'", opcodeName,
                                     (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                    stop     = true;
                }
                break;
            case BIND_TYPE_TEXT_ABSOLUTE32:
            case BIND_TYPE_TEXT_PCREL32:
                if ( !allowTextFixups ) {
                    locError = Error("%s text binds not supported for architecture",  opcodeName);
                    stop     = true;
                }
                else if ( segments[segmentIndex].writable ) {
                    locError = Error("%s text bind is in writable segment",  opcodeName);
                    stop     = true;
                }
                else if ( !segments[segmentIndex].executable ) {
                    locError = Error("%s text bind is in non-executable segment",  opcodeName);
                    stop     = true;
                }
                break;
            default:
                locError = Error("%s unknown bind type %d",  opcodeName, type);
                stop     = true;
        }
    }, ^(const char* symbolName){ });
    if ( opcodeErr )
        return opcodeErr;
    return std::move(locError);
}

Error BindOpcodes::forEachBind(void (^handler)(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset,
                                               bool libraryOrdinalSet, int libOrdinal, const char* symbolName, bool weakImport, int64_t addend,
                                               bool targetOrAddendChanged, bool& stop),
                               void (^strongHandler)(const char* symbolName)) const
{
    const uint8_t*  p                 = _opcodesStart;
    int             type              = (hasDoneBetweenBinds() ? BIND_TYPE_POINTER : 0);  // lazy binds are implicitly pointers
    int             segIndex          = 0;
    uint64_t        segOffset         = 0;
    bool            segIndexSet       = false;
    const char*     symbolName        = nullptr;
    int             libraryOrdinal    = 0;
    bool            libraryOrdinalSet = false;
    int64_t         addend            = 0;
    bool            weakImport        = false;
    uint64_t        count;
    uint64_t        skip;
    bool            targetChanged     = true;
    bool            stop              = false;
    bool            malformed         = false;

    // Weak binds implicitly use -3 for everything.  Check if we are in that mode
    if ( std::optional<int> implicitOrdinal = this->implicitLibraryOrdinal() ) {
        libraryOrdinal = implicitOrdinal.value();
        libraryOrdinalSet = true;
    }

    while ( !stop && !malformed && (p < _opcodesEnd) ) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                if ( !hasDoneBetweenBinds() )
                    stop = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                // FIXME: Should we enforce that we never see this opcode when processing weak binds
                libraryOrdinal = immediate;
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                libraryOrdinal = (int)read_uleb128(p, _opcodesEnd, malformed);
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // the special ordinals are negative numbers
                if ( immediate == 0 )
                    libraryOrdinal = 0;
                else {
                    int8_t signExtended = BIND_OPCODE_MASK | immediate;
                    libraryOrdinal = signExtended;
                }
                libraryOrdinalSet = true;
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                if ( immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION ) {
                    strongHandler(symbolName);
                }
                targetChanged = true;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(p, _opcodesEnd, malformed);
                targetChanged = true;
               break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segIndex = immediate;
                segOffset = read_uleb128(p, _opcodesEnd, malformed);
                segIndexSet = true;
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                segOffset += read_uleb128(p, _opcodesEnd, malformed);
                break;
            case BIND_OPCODE_DO_BIND:
                handler("BIND_OPCODE_DO_BIND", type, segIndexSet, segIndex, segOffset, libraryOrdinalSet, libraryOrdinal, symbolName, weakImport, addend, targetChanged, stop);
                segOffset += _pointerSize;
                targetChanged = false;
               break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", type, segIndexSet, segIndex, segOffset, libraryOrdinalSet, libraryOrdinal, symbolName, weakImport, addend, targetChanged, stop);
                segOffset += read_uleb128(p, _opcodesEnd, malformed) + _pointerSize;
                targetChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", type, segIndexSet, segIndex, segOffset, libraryOrdinalSet, libraryOrdinal, symbolName, weakImport, addend, targetChanged, stop);
                segOffset += immediate*_pointerSize + _pointerSize;
                targetChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(p, _opcodesEnd, malformed);
                skip  = read_uleb128(p, _opcodesEnd, malformed);
                for (uint32_t i=0; i < count; ++i) {
                    handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", type, segIndexSet, segIndex, segOffset, libraryOrdinalSet, libraryOrdinal, symbolName, weakImport, addend, targetChanged, stop);
                    segOffset += skip + _pointerSize;
                    targetChanged = false;
                    if ( stop )
                        break;
                }
                break;
            case BIND_OPCODE_THREADED:
                return Error("old arm64e bind opcodes not supported");
                break;
            default:
                return Error("unknown bind opcode 0x%02X", opcode);
        }
    }
    if ( malformed )
        return Error("malformed uleb128");
    else
        return Error::none();
}

void BindOpcodes::forEachBindLocation(void (^callback)(const LocAndTarget&, bool& stop)) const
{
    (void)forEachBind(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segIndex, uint64_t segOffset, bool libraryOrdinalSet,
                  int libOrdinal, const char* symbolName, bool weakImport, int64_t addend, bool targetOrAddendChanged, bool& stop) {
        BindTarget target = {symbolName, libOrdinal, weakImport, false, addend};
        LocAndTarget info = { segIndex, segOffset, &target };
        callback(info, stop);
    }, ^(const char* symbolName){ });
}

void BindOpcodes::forEachBindTarget(void (^callback)(const Fixup::BindTarget& target, bool& stop),
                                    void (^strongHandler)(const char* symbolName)) const
{
    __block const char* lastSymbolName = nullptr;
    (void)forEachBind(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segIndex, uint64_t segOffset, bool libraryOrdinalSet,
                  int libOrdinal, const char* symbolName, bool weakImport, int64_t addend, bool targetOrAddendChanged, bool& stop) {
        // in Bind Opocdes the symbol name string remains the same, while the update location (segOffset) changes, so we can to pointer comparison
        if ( symbolName != lastSymbolName ) {
            Fixup::BindTarget target = {symbolName, libOrdinal, weakImport, addend};
            callback(target, stop);
            lastSymbolName = symbolName;
        }
    }, strongHandler);
}

uint32_t BindOpcodes::forEachBindLocation(std::span<const MappedSegment> segments, uint32_t startBindOrdinal, void (^callback)(const Fixup& fixup, bool& stop)) const
{
    __block uint32_t bindOrdinal = startBindOrdinal;
    (void)forEachBind(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segIndex, uint64_t segOffset, bool libraryOrdinalSet,
                  int libOrdinal, const char* symbolName, bool weakImport, int64_t addend, bool targetOrAddendChanged, bool& stop) {
        uint8_t* loc = (uint8_t*)(segments[segIndex].content) + segOffset;
        Fixup fixup(loc, &segments[segIndex], bindOrdinal, 0, hasDoneBetweenBinds());
        callback(fixup, stop);
        if ( targetOrAddendChanged )
            ++bindOrdinal;
    }, ^(const char* symbolName){ });
    return bindOrdinal;
}

void BindOpcodes::printOpcodes(FILE* output, int indentCount) const
{
    uint8_t         type = 0;
    uint64_t        count;
    uint64_t        skip;
    uint32_t        segIndex;
    uint64_t        segOffset;
    uint32_t        flags             = 0;
    const char*     symbolName        = nullptr;
    int             libOrdinal        = 0;
    int64_t         addend            = 0;
    bool            done              = false;
    bool            malformed         = false;
    const uint8_t*  p = _opcodesStart;
    char            indent[indentCount+1];
    for (int i=0; i < indentCount; ++i)
        indent[i] = ' ';
    indent[indentCount] = '\0';
    while ( !done && !malformed && (p < _opcodesEnd) ) {
        uint8_t  immediate    = *p & BIND_IMMEDIATE_MASK;
        uint8_t  opcode       = *p & BIND_OPCODE_MASK;
        long     opcodeOffset = p - _opcodesStart;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                if ( !hasDoneBetweenBinds() )
                    done = true;
                fprintf(output, "%s0x%04lX BIND_OPCODE_DONE()\n", indent, opcodeOffset);
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                libOrdinal = immediate;
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_DYLIB_ORDINAL_IMM(%d)\n", indent, opcodeOffset, libOrdinal);
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                libOrdinal = (int)read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB(%d)\n", indent, opcodeOffset, libOrdinal);
                break;
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                // the special ordinals are negative numbers
                if ( immediate == 0 )
                    libOrdinal = 0;
                else {
                    int8_t signExtended = BIND_OPCODE_MASK | immediate;
                    libOrdinal = signExtended;
                }
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_DYLIB_SPECIAL_IMM(%d)\n", indent, opcodeOffset, libOrdinal);
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                flags = immediate;
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM(0x%02X, %s)\n", indent, opcodeOffset, flags, symbolName);
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_TYPE_IMM(%d)\n", indent, opcodeOffset, type);
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_ADDEND_SLEB(%lld)\n", indent, opcodeOffset, addend);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segIndex = immediate;
                segOffset = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB(0x%02X, 0x%08llX)\n", indent, opcodeOffset, segIndex, segOffset);
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                skip = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_ADD_ADDR_ULEB(0x%08llX)\n", indent, opcodeOffset, skip);
                break;
            case BIND_OPCODE_DO_BIND:
                fprintf(output, "%s0x%04lX BIND_OPCODE_DO_BIND()\n", indent, opcodeOffset);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                skip = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB(0x%08llX)\n",indent, opcodeOffset, skip);
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                skip = immediate*_pointerSize + _pointerSize;
                fprintf(output, "%s0x%04lX BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED(0x%08llX)\n", indent, opcodeOffset, skip);
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(p, _opcodesEnd, malformed);
                skip  = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB(%llu, 0x%08llX)\n", indent, opcodeOffset, count, skip);
                break;
            case BIND_OPCODE_THREADED:
                // Note the immediate is a sub opcode
                switch (immediate) {
                    case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
                        count = read_uleb128(p, _opcodesEnd, malformed);
                        fprintf(output, "%s0x%04lX BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB(%llu)\n", indent, opcodeOffset, count);
                        break;
                    case BIND_SUBOPCODE_THREADED_APPLY:
                        fprintf(output, "%s0x%04lX BIND_SUBOPCODE_THREADED_APPLY\n", indent, opcodeOffset);
                        break;
                    default:
                        fprintf(output, "%sunknown threaded bind subopcode 0x%02X\n", indent, immediate);
                }
                break;
            default:
                fprintf(output, "%sunknown rebase opcode 0x%02X\n", indent, *p);
        }
    }
}

//
// MARK: --- LazyBindOpcodes inspection methods ---
//

bool LazyBindOpcodes::hasDoneBetweenBinds() const
{
    return true;
}


//
// MARK: --- WeakBindOpcodes inspection methods ---
//

std::optional<int> WeakBindOpcodes::implicitLibraryOrdinal() const
{
    return -3;
}


} // namespace mach_o
