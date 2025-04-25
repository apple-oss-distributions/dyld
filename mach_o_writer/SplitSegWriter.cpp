/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// mach_o
#include "Misc.h"

// mach_o_writer
#include "SplitSegWriter.h"

#include <map>

// FIXME: We should get this from cctools
#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

using mach_o::SplitSegInfo;

namespace mach_o {

static void append_uleb128(uint64_t value, std::vector<uint8_t>& out)
{
    uint8_t byte;
    do {
        byte = value & 0x7F;
        value &= ~0x7F;
        if ( value != 0 )
            byte |= 0x80;
        out.push_back(byte);
        value = value >> 7;
    } while ( byte >= 0x80 );
}

SplitSegInfoWriter::SplitSegInfoWriter(std::span<const SplitSegInfo::Entry> entries) : SplitSegInfo(nullptr, 0)
{
    // Whole         :== <count> FromToSection+
    // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
    // ToOffset      :== <to-sect-offset-delta> <count> FromOffset+
    // FromOffset    :== <kind> <count> <from-sect-offset-delta>

    typedef uint32_t                                  SectionIndexes;
    typedef std::map<uint8_t, std::vector<uint64_t> > FromOffsetMap;
    typedef std::map<uint64_t, FromOffsetMap>         ToOffsetMap;
    typedef std::map<SectionIndexes, ToOffsetMap>     WholeMap;

    // sort into group by adjustment kind
    //fprintf(stderr, "_splitSegV2Infos.size=%lu\n", entries.size());
    WholeMap whole;
    for ( const Entry& entry : entries ) {
        SectionIndexes comboIndex  = (uint32_t)entry.fromSectionIndex << 16 | (uint32_t)entry.toSectionIndex;
        ToOffsetMap&   toOffsets   = whole[comboIndex];
        FromOffsetMap& fromOffsets = toOffsets[entry.toSectionOffset];
        fromOffsets[entry.kind].push_back(entry.fromSectionOffset);
    }

    // Add marker that this is V2 data
    this->_bytes.reserve(8192);
    this->_bytes.push_back(DYLD_CACHE_ADJ_V2_FORMAT);

    // stream out
    // Whole :== <count> FromToSection+
    append_uleb128(whole.size(), this->_bytes);
    for (auto& fromToSection : whole) {
        uint8_t fromSectionIndex = fromToSection.first >> 16;
        uint8_t toSectionIndex   = fromToSection.first & 0xFFFF;
        ToOffsetMap& toOffsets   = fromToSection.second;
        // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
        append_uleb128(fromSectionIndex, this->_bytes);
        append_uleb128(toSectionIndex, this->_bytes);
        append_uleb128(toOffsets.size(), this->_bytes);
        //fprintf(stderr, "from sect=%d, to sect=%d, count=%lu\n", fromSectionIndex, toSectionIndex, toOffsets.size());
        uint64_t lastToOffset = 0;
        for (auto& fromToOffsets : toOffsets) {
            uint64_t       toSectionOffset   = fromToOffsets.first;
            FromOffsetMap& fromOffsets       = fromToOffsets.second;
            // ToOffset    :== <to-sect-offset-delta> <count> FromOffset+
            uint64_t toSectionDelta = toSectionOffset - lastToOffset;
            append_uleb128(toSectionDelta, this->_bytes);
            append_uleb128(fromOffsets.size(), this->_bytes);
            for (auto& kindAndOffsets : fromOffsets) {
                uint8_t kind = kindAndOffsets.first;
                std::vector<uint64_t>& fromSectOffsets = kindAndOffsets.second;
                // FromOffset :== <kind> <count> <from-sect-offset-delta>
                append_uleb128(kind, this->_bytes);
                append_uleb128(fromSectOffsets.size(), this->_bytes);
                std::sort(fromSectOffsets.begin(), fromSectOffsets.end());
                uint64_t lastFromOffset = 0;
                for (uint64_t offset : fromSectOffsets) {
                    append_uleb128(offset - lastFromOffset, this->_bytes);
                    lastFromOffset = offset;
                }
            }
            lastToOffset = toSectionOffset;
        }
    }


    // always add zero byte to mark end
    this->_bytes.push_back(0);

    // pad to be 8-btye aligned
    while ( (this->_bytes.size() % 8) != 0 )
        this->_bytes.push_back(0);

    // set up buffer
    this->_infoStart = &this->_bytes.front();
    this->_infoEnd   = &this->_bytes.back();
}

} // namepace mach_o
