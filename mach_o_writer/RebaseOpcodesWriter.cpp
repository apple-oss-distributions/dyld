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

// mach_o
#include "Misc.h"
#include "Image.h"

// mach_o_writer
#include "RebaseOpcodesWriter.h"

using dyld3::Array;

namespace mach_o {

void RebaseOpcodesWriter::append_uleb128(uint64_t value)
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

void RebaseOpcodesWriter::append_byte(uint8_t value)
{
    _opcodes.push_back(value);
}

RebaseOpcodesWriter::RebaseOpcodesWriter(std::span<const Location> sortedLocs, bool is64)
    : RebaseOpcodes(nullptr, 0, is64)
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

    _opcodesStart = _opcodes.data();
    _opcodesEnd   = _opcodes.data()+_opcodes.size();
}

} // namespace mach_o
