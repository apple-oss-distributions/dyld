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

#include "SplitSeg.h"
#include "Misc.h"

// FIXME: We should get this from cctools
#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

namespace mach_o {

SplitSegInfo::SplitSegInfo(const uint8_t* start, size_t size)
: _infoStart(start), _infoEnd(start + size)
{
}

Error SplitSegInfo::valid() const
{
    return Error::none();
}

bool SplitSegInfo::hasMarker() const
{
    return (_infoStart == _infoEnd);
}

bool SplitSegInfo::isV1() const
{
    return !this->isV2();
}

bool SplitSegInfo::isV2() const
{
    return (*_infoStart == DYLD_CACHE_ADJ_V2_FORMAT);
}

Error SplitSegInfo::forEachReferenceV2(void (^callback)(const Entry& entry, bool& stop)) const
{
    const uint8_t* infoStart = this->_infoStart;
    const uint8_t* infoEnd = this->_infoEnd;

    if ( *infoStart++ != DYLD_CACHE_ADJ_V2_FORMAT ) {
        return Error("Not split seg v2");
    }

    // Whole         :== <count> FromToSection+
    // FromToSection :== <from-sect-index> <to-sect-index> <count> ToOffset+
    // ToOffset         :== <to-sect-offset-delta> <count> FromOffset+
    // FromOffset     :== <kind> <count> <from-sect-offset-delta>
    const uint8_t* p = infoStart;
    bool malformed = false;
    uint64_t sectionCount = read_uleb128(p, infoEnd, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    for (uint64_t i=0; i < sectionCount; ++i) {
        uint64_t fromSectionIndex = read_uleb128(p, infoEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        uint64_t toSectionIndex = read_uleb128(p, infoEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        uint64_t toOffsetCount = read_uleb128(p, infoEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        uint64_t toSectionOffset = 0;
        for (uint64_t j=0; j < toOffsetCount; ++j) {
            uint64_t toSectionDelta = read_uleb128(p, infoEnd, malformed);
            if ( malformed )
                return Error("malformed uleb128");
            uint64_t fromOffsetCount = read_uleb128(p, infoEnd, malformed);
            if ( malformed )
                return Error("malformed uleb128");
            toSectionOffset += toSectionDelta;
            for (uint64_t k=0; k < fromOffsetCount; ++k) {
                uint64_t kind = read_uleb128(p, infoEnd, malformed);
                if ( malformed )
                    return Error("malformed uleb128");
                if ( kind > 13 ) {
                    return Error("bad kind (%llu) value\n", kind);
                }
                uint64_t fromSectDeltaCount = read_uleb128(p, infoEnd, malformed);
                if ( malformed )
                    return Error("malformed uleb128");
                uint64_t fromSectionOffset = 0;
                for (uint64_t l=0; l < fromSectDeltaCount; ++l) {
                    uint64_t delta = read_uleb128(p, infoEnd, malformed);
                    if ( malformed )
                        return Error("malformed uleb128");
                    fromSectionOffset += delta;
                    bool stop = false;
                    callback(Entry { (uint8_t)kind, (uint8_t)fromSectionIndex, (uint8_t)toSectionIndex, fromSectionOffset, toSectionOffset }, stop);
                    if ( stop )
                        return Error::none();
                }
            }
        }
    }

    return Error::none();
}

} // namepace mach_o
