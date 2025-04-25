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

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <TargetConditionals.h>

#include <AvailabilityMacros.h>
#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_priv.h>

#include "Misc.h"
#include "SupportedArchs.h"
#include "Universal.h"
#include "Archive.h"
#include "Header.h"

namespace mach_o {

////////////////////////////  ULEB128 helpers  ////////////////////////////////////////

uint64_t read_uleb128(std::span<const uint8_t>& buffer, bool& malformed)
{
    uint64_t result = 0;
    int         bit = 0;
    malformed = false;
    while ( true ) {
        if ( buffer.empty() ) {
            malformed = true;
            break;
        }
        uint8_t elt = buffer.front();
        uint64_t slice = elt & 0x7f;

        if ( bit > 63 ) {
            malformed = true;
            break;
        }
        else {
            result |= (slice << bit);
            bit += 7;
        }

        // Keep iterating if the high bit is set, and advance to next element
        buffer = buffer.subspan(1);
        if ( (elt & 0x80) == 0 )
            break;
    }
    return result;
}

uint64_t read_uleb128(const uint8_t*& p, const uint8_t* end, bool& malformed)
{
    // Use the std::span one
    std::span<const uint8_t> buffer(p, end);
    uint64_t result = read_uleb128(buffer, malformed);

    // Adjust 'p' to the new start of the buffer as read_uleb128() would have advanced it
    if ( buffer.empty() )
        p = end;
    else
        p = &buffer.front();

    return result;
}

int64_t  read_sleb128(const uint8_t*& p, const uint8_t* end, bool& malformed)
{
    int64_t  result = 0;
    int      bit = 0;
    uint8_t  byte = 0;
    malformed = false;
    do {
        if ( p == end ) {
            malformed = true;
            break;
        }
        byte = *p++;
        result |= (((int64_t)(byte & 0x7f)) << bit);
        bit += 7;
    } while (byte & 0x80);
    // sign extend negative numbers
    if ( ((byte & 0x40) != 0) && (bit < 64) )
        result |= (~0ULL) << bit;
    return result;
}

uint32_t uleb128_size(uint64_t value)
{
    uint32_t result = 0;
    do {
        value = value >> 7;
        ++result;
    } while ( value != 0 );
    return result;
}

Error forEachHeader(std::span<const uint8_t> buffer, std::string_view path,
                    void (^callback)(const Header* sliceHeader, size_t sliceLength, bool& stop)) {
    if ( const mach_o::Universal* universal = mach_o::Universal::isUniversal(buffer) ) {
        if ( mach_o::Error err = universal->valid(buffer.size()) )
            return Error("error in file '%s': %s", path.data(), err.message());
        universal->forEachSlice(^(mach_o::Universal::Slice slice, bool &stop) {
            if ( const mach_o::Header* mh = mach_o::Header::isMachO(slice.buffer) ) {
                callback(mh, slice.buffer.size(), stop);
            }
        });
    } else if ( const mach_o::Header* mh = mach_o::Header::isMachO(buffer) ) {
        bool stop = false;
        callback(mh, buffer.size(), stop);
    }

    return Error::none();
}


} // namespace mach_o





