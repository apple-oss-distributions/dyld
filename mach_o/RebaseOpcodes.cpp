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

#include "RebaseOpcodes.h"
#include "Misc.h"
#include "Image.h"

using dyld3::Array;

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
        uint64_t targetVmOffset = (is64 ? *((uint64_t*)loc) : *((uint32_t*)loc)) - prefLoadAdder;
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


#if BUILDING_MACHO_WRITER

void RebaseOpcodes::append_uleb128(uint64_t value)
{
    uint8_t byte;
    do {
        byte = value & 0x7F;
        value &= ~0x7F;
        if ( value != 0 )
            byte |= 0x80;
        _opcodes.push_back(byte);
        value = value >> 7;
    } while( byte >= 0x80 );
}

void RebaseOpcodes::append_byte(uint8_t value)
{
    _opcodes.push_back(value);
}

RebaseOpcodes::RebaseOpcodes(std::span<const Location> sortedLocs, bool is64)
  : _opcodesStart(nullptr), _opcodesEnd(nullptr), _pointerSize(is64 ? 8 : 4)
{
    if ( sortedLocs.empty() )
        return;

    struct RebaseTmp
    {
        RebaseTmp(uint8_t op, uint64_t p1, uint64_t p2=0) : opcode(op), operand1(p1), operand2(p2) {}
        uint8_t		opcode;
        uint64_t	operand1;
        uint64_t	operand2;
    };

	// convert to temp encoding that can be more easily optimized
	std::vector<RebaseTmp> mid;
	uint32_t curSegIndex  = 0;
    uint64_t curSegOffset = -1;
    mid.emplace_back(REBASE_OPCODE_SET_TYPE_IMM, REBASE_TYPE_POINTER);
	for (const Location& loc : sortedLocs) {
        if ( curSegIndex != loc.segIndex ) {
            mid.emplace_back(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB, loc.segIndex, loc.segOffset);
            curSegIndex  = loc.segIndex;
            curSegOffset = loc.segOffset;
        }
        else if ( curSegOffset != loc.segOffset ) {
            mid.emplace_back(REBASE_OPCODE_ADD_ADDR_ULEB, loc.segOffset - curSegOffset);
            curSegOffset = loc.segOffset;
        }
 		mid.emplace_back(REBASE_OPCODE_DO_REBASE_ULEB_TIMES, 1);
		curSegOffset += _pointerSize;
	}
	mid.emplace_back(REBASE_OPCODE_DONE, 0);

	// optimize phase 1, compress packed runs of pointers
	RebaseTmp* dst = &mid[0];
	for (const RebaseTmp* src = &mid[0]; src->opcode != REBASE_OPCODE_DONE; ++src) {
		if ( (src->opcode == REBASE_OPCODE_DO_REBASE_ULEB_TIMES) && (src->operand1 == 1) ) {
			*dst = *src++;
			while (src->opcode == REBASE_OPCODE_DO_REBASE_ULEB_TIMES ) {
				dst->operand1 += src->operand1;
				++src;
			}
			--src;
			++dst;
		}
		else {
			*dst++ = *src;
		}
	}
	dst->opcode = REBASE_OPCODE_DONE;

	// optimize phase 2, combine rebase/add pairs
	dst = &mid[0];
	for (const RebaseTmp* src = &mid[0]; src->opcode != REBASE_OPCODE_DONE; ++src) {
		if ( (src->opcode == REBASE_OPCODE_DO_REBASE_ULEB_TIMES)
				&& (src->operand1 == 1)
				&& (src[1].opcode == REBASE_OPCODE_ADD_ADDR_ULEB)) {
			dst->opcode = REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB;
			dst->operand1 = src[1].operand1;
			++src;
			++dst;
		}
		else {
			*dst++ = *src;
		}
	}
	dst->opcode = REBASE_OPCODE_DONE;

	// optimize phase 3, compress packed runs of REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB with
	// same addr delta into one REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
	dst = &mid[0];
	for (const RebaseTmp* src = &mid[0]; src->opcode != REBASE_OPCODE_DONE; ++src) {
		uint64_t delta = src->operand1;
		if ( (src->opcode == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB)
				&& (src[1].opcode == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB)
				&& (src[2].opcode == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB)
				&& (src[1].operand1 == delta)
				&& (src[2].operand1 == delta) ) {
			// found at least three in a row, this is worth compressing
			dst->opcode = REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB;
			dst->operand1 = 1;
			dst->operand2 = delta;
			++src;
			while ( (src->opcode == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB)
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
	dst->opcode = REBASE_OPCODE_DONE;

	// optimize phase 4, use immediate encodings
	for (RebaseTmp* p = &mid[0]; p->opcode != REBASE_OPCODE_DONE; ++p) {
		if ( (p->opcode == REBASE_OPCODE_ADD_ADDR_ULEB)
			&& (p->operand1 < (15*_pointerSize))
			&& ((p->operand1 % _pointerSize) == 0) ) {
			p->opcode = REBASE_OPCODE_ADD_ADDR_IMM_SCALED;
			p->operand1 = p->operand1/_pointerSize;
		}
		else if ( (p->opcode == REBASE_OPCODE_DO_REBASE_ULEB_TIMES) && (p->operand1 < 15) ) {
			p->opcode = REBASE_OPCODE_DO_REBASE_IMM_TIMES;
		}
	}

	// convert to compressed encoding
	_opcodes.reserve(256);
	bool done = false;
	for (const RebaseTmp& r : mid) {
		switch ( r.opcode ) {
			case REBASE_OPCODE_DONE:
				done = true;
				break;
			case REBASE_OPCODE_SET_TYPE_IMM:
				append_byte(REBASE_OPCODE_SET_TYPE_IMM | r.operand1);
				break;
			case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
				append_byte(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB | r.operand1);
				append_uleb128(r.operand2);
				break;
			case REBASE_OPCODE_ADD_ADDR_ULEB:
				append_byte(REBASE_OPCODE_ADD_ADDR_ULEB);
				append_uleb128(r.operand1);
				break;
			case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
				append_byte(REBASE_OPCODE_ADD_ADDR_IMM_SCALED | r.operand1 );
				break;
			case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
				append_byte(REBASE_OPCODE_DO_REBASE_IMM_TIMES | r.operand1);
				break;
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
				append_byte(REBASE_OPCODE_DO_REBASE_ULEB_TIMES);
				append_uleb128(r.operand1);
				break;
			case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
				append_byte(REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB);
				append_uleb128(r.operand1);
				break;
			case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
				append_byte(REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB);
				append_uleb128(r.operand1);
				append_uleb128(r.operand2);
				break;
		}
        if ( done )
            break;
	}

	// align to pointer size
    while ( (_opcodes.size() % _pointerSize) != 0 )
        append_byte(0);

    _opcodesStart = &_opcodes[0];
    _opcodesEnd   = &_opcodes[_opcodes.size()];
}

#endif // BUILDING_MACHO_WRITER

} // namespace mach_o
