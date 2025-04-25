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

#include "RebaseOpcodes.h"
#include "Misc.h"
#include "Image.h"

namespace mach_o {

//
// MARK: --- RebaseOpcodes inspection methods ---
//

RebaseOpcodes::RebaseOpcodes(const uint8_t* start, size_t size, bool is64)
  : _opcodesStart(start), _opcodesEnd(start+size), _pointerSize(is64 ? 8 : 4)
{
}


Error RebaseOpcodes::valid(std::span<const MappedSegment> segments, bool allowTextFixups, bool onlyFixupsInWritableSegments) const
{
    __block Error locError;
    Error opcodeErr = this->forEachRebase(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset, bool& stop) {
        if ( !segIndexSet ) {
            locError = Error("%s missing preceding REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", opcodeName);
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
        else {
            switch ( type ) {
                case REBASE_TYPE_POINTER:
                    if ( !segments[segmentIndex].writable && onlyFixupsInWritableSegments ) {
                        locError = Error("%s pointer rebase is in non-writable segment '%.*s'", opcodeName,
                                         (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                        stop = true;
                    }
                    else if ( segments[segmentIndex].executable && onlyFixupsInWritableSegments ) {
                        locError = Error("%s pointer rebase is in executable segment '%.*s'", opcodeName,
                                         (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                        stop = true;
                    }
                    break;
                case REBASE_TYPE_TEXT_ABSOLUTE32:
                case REBASE_TYPE_TEXT_PCREL32:
                    if ( !allowTextFixups ) {
                        locError = Error("%s text rebase not supported for architecture",  opcodeName);
                        stop     = true;
                    }
                    else if ( segments[segmentIndex].writable ) {
                        locError = Error("%s text rebase is in writable segment '%.*s'", opcodeName,
                                         (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                        stop = true;
                    }
                    else if ( !segments[segmentIndex].executable ) {
                        locError = Error("%s text rebase is in non-executable segment '%.*s'", opcodeName,
                                         (int)segments[segmentIndex].segName.size(), segments[segmentIndex].segName.data());
                        stop = true;
                    }
                    break;
                default:
                    locError = Error("%s unknown rebase type", opcodeName);
                    stop = true;
            }
        }
    });
    if ( opcodeErr )
        return opcodeErr;
    return std::move(locError);
}

Error RebaseOpcodes::forEachRebase(void (^handler)(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset, bool& stop)) const
{
    const uint8_t*  p           = _opcodesStart;
    int             type        = 0;
    int             segIndex    = 0;
    uint64_t        segOffset   = 0;
    bool            segIndexSet = false;
    bool            stop        = false;
    bool            malformed   = false;
    uint64_t        count;
    uint64_t        skip;
    while ( !stop && !malformed && (p < _opcodesEnd) ) {
        uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
        uint8_t opcode    = *p & REBASE_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case REBASE_OPCODE_DONE:
                // Allow some padding, in case rebases were somehow aligned to 16-bytes in size
                if ( (_opcodesEnd - p) > 15 )
                    return Error("rebase opcodes terminated early at offset %d of %d", (int)(p-_opcodesStart), (int)(_opcodesEnd-_opcodesStart));
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                switch ( immediate ) {
                    case REBASE_TYPE_POINTER:
                    case REBASE_TYPE_TEXT_ABSOLUTE32:
                    case REBASE_TYPE_TEXT_PCREL32:
                        type = immediate;
                        break;
                    default:
                        type = 0;
                        break;
                }
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segIndex = immediate;
                segOffset = read_uleb128(p, _opcodesEnd, malformed);
                segIndexSet = true;
                break;
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                segOffset += read_uleb128(p, _opcodesEnd, malformed);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                segOffset += immediate*_pointerSize;
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for (int i=0; i < immediate; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_IMM_TIMES", type, segIndexSet, segIndex, segOffset, stop);
                    segOffset += _pointerSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                count = read_uleb128(p, _opcodesEnd, malformed);
                if ( malformed )
                    break;
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", type, segIndexSet, segIndex, segOffset, stop);
                    segOffset += _pointerSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", type, segIndexSet, segIndex, segOffset, stop);
                segOffset += read_uleb128(p, _opcodesEnd, malformed) + _pointerSize;
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(p, _opcodesEnd, malformed);
                if ( malformed )
                    break;
                skip = read_uleb128(p, _opcodesEnd, malformed);
                if ( malformed )
                    break;
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", type, segIndexSet, segIndex, segOffset, stop);
                    segOffset += skip + _pointerSize;
                    if ( stop )
                        break;
                }
                break;
            default:
                return Error("unknown rebase opcode 0x%02X", opcode);
        }
    }
    if ( malformed )
        return Error("malformed uleb128");
    else
        return Error::none();
}

void RebaseOpcodes::forEachRebaseLocation(void (^callback)(uint32_t segIndex, uint64_t segOffset, bool& stop)) const
{
    (void)forEachRebase(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segmentIndex, uint64_t segmentOffset, bool& stop) {
        callback(segmentIndex, segmentOffset, stop);
    });
}

void RebaseOpcodes::forEachRebaseLocation(std::span<const MappedSegment> segments, uint64_t prefLoadAdder, void (^callback)(const Fixup& fixup, bool& stop)) const
{
    const bool is64 = (_pointerSize == 8);
    (void)forEachRebase(^(const char* opcodeName, int type, bool segIndexSet, uint8_t segIndex, uint64_t segOffset, bool& stop) {
        uint8_t* loc = (uint8_t*)(segments[segIndex].content) + segOffset;
        uint64_t targetVmOffset;
        if ( is64 ) {
            targetVmOffset = *((uint64_t*)loc) - prefLoadAdder;
        }
        else {
            // for i386, there may be "text relocations" which are not 4-byte aligned
            uint32_t value;
            memcpy(&value, loc, 4);
            targetVmOffset = (uint64_t)value - prefLoadAdder;
        }
        Fixup fixup(loc, &segments[segIndex], targetVmOffset);
        callback(fixup, stop);
    });
}

const uint8_t* RebaseOpcodes::bytes(size_t& size) const
{
    size = (_opcodesEnd - _opcodesStart);
    return _opcodesStart;
}

void RebaseOpcodes::printOpcodes(FILE* output, int indentCount) const
{
    uint8_t         type = 0;
    uint64_t        count;
    uint64_t        skip;
    uint32_t        segmentIndex;
    uint64_t        segOffset;
    bool            done = false;
    bool            malformed = false;
    char            indent[indentCount+1];
    const uint8_t*  p = _opcodesStart;
    for (int i=0; i < indentCount; ++i)
        indent[i] = ' ';
    indent[indentCount] = '\0';
    while ( !done && !malformed && (p < _opcodesEnd) ) {
        uint8_t  immediate    = *p & REBASE_IMMEDIATE_MASK;
        uint8_t  opcode       = *p & REBASE_OPCODE_MASK;
        long     opcodeOffset = p - _opcodesStart;
        ++p;
        switch (opcode) {
            case REBASE_OPCODE_DONE:
                done = true;
                fprintf(output, "%s0x%04lX REBASE_OPCODE_DONE()\n", indent, opcodeOffset);
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                type = immediate;
                fprintf(output, "%s0x%04lX REBASE_OPCODE_SET_TYPE_IMM(%d)\n", indent, opcodeOffset, type);
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segmentIndex = immediate;
                segOffset = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB(%d, 0x%08llX)\n", indent, opcodeOffset, segmentIndex, segOffset);
                break;
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                segOffset = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX REBASE_OPCODE_ADD_ADDR_ULEB(0x%0llX)\n", indent, opcodeOffset, segOffset);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                segOffset = immediate * _pointerSize;
                fprintf(output, "%s0x%04lX REBASE_OPCODE_ADD_ADDR_IMM_SCALED(0x%0llX)\n", indent, opcodeOffset, segOffset);
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                fprintf(output, "%s0x%04lX REBASE_OPCODE_DO_REBASE_IMM_TIMES(%d)\n", indent, opcodeOffset, immediate);
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                count = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX REBASE_OPCODE_DO_REBASE_ULEB_TIMES(%lld)\n", indent, opcodeOffset, count);
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                skip = read_uleb128(p, _opcodesEnd, malformed) + _pointerSize;
                fprintf(output, "%s0x%04lX REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB(%lld)\n", indent, opcodeOffset, skip);
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(p, _opcodesEnd, malformed);
                skip  = read_uleb128(p, _opcodesEnd, malformed);
                fprintf(output, "%s0x%04lX REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB(%lld, %lld)\n", indent, opcodeOffset, count, skip);
                break;
            default:
                fprintf(output, "%sunknown rebase opcode 0x%02X\n", indent, *p);
        }
    }
}

} // namespace mach_o
