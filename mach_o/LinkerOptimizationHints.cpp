/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#include "LinkerOptimizationHints.h"
#include "Fixups.h"
#include "Misc.h"

namespace mach_o {

//
// MARK: --- LinkerOptimizationHints inspection methods ---
//

LinkerOptimizationHints::LinkerOptimizationHints(std::span<const uint8_t> buffer)
    : _buffer(buffer)
{
}

Error LinkerOptimizationHints::forEachLOH(void (^callback)(Kind kind, std::span<uint64_t> addrs, bool& stop)) const
{
    std::span<const uint8_t> currentBuffer = _buffer;
    while ( !currentBuffer.empty() ) {
        bool malformed = false;
        uint64_t kind = read_uleb128(currentBuffer, malformed);

        if ( kind == 0 ) // padding at end of loh buffer
            break;

        if ( kind == -1 ) {
            // FIXME: How do we want to get warnings from mach_o?
            //warning("malformed uleb128 kind in LC_LINKER_OPTIMIZATION_HINTS");
            break;
        }
        uint64_t count = read_uleb128(currentBuffer, malformed);
        if ( count == -1 ) {
            //warning("malformed uleb128 count in LC_LINKER_OPTIMIZATION_HINTS");
            break;
        }

        if ( count == 0 ) {
            //warning("malformed uleb128 count in LC_LINKER_OPTIMIZATION_HINTS");
            break;
        }

        uint64_t addrs[count];
        for (int32_t i=0; i < count; ++i) {
            addrs[i] = read_uleb128(currentBuffer, malformed);
        }

        if ( malformed )
            return Error("malformed uleb128");

        bool stop = false;
        switch ( (Kind)kind ) {
            case Kind::unknown:
                // these are known kinds, so do the callback
                callback((Kind)kind, { &addrs[0], (uint32_t)count }, stop);
                break;
            default:
                // unknown kind, so skip this one
                break;
        }

        if ( stop )
            break;
    }

    return Error::none();
}

static Error validFPAC(const char* name, std::span<const uint64_t> addrs,
                       std::span<const uint32_t> segmentContent, std::span<const uint32_t> expectedContent)
{
    if ( addrs.size() != 5 ) {
        return Error("Expected %s LOH to be 5 instructions.  Got %d", name, (uint32_t)addrs.size());
    }

    // The addresses should all point to subsequent instructions for now.  If this chages
    // we'll need to update the below checks too
    uint64_t baseAddr = addrs[0];
    for ( uint32_t i = 0; i != 5; ++i ) {
        if ( addrs[i] != (baseAddr + (i * 4)) ) {
            return Error("Expected %s addresses to be contiguous.  Got element[%d] at address %lld", name, i, addrs[i]);
        }
    }

    // Make sure the LOH fits in the buffer
    if ( segmentContent.size() < 5 ) {
        return Error("not enough space in segment for %s LOH. Got %d bytes", name, (uint32_t)segmentContent.size() * 4);
    }

    // Check the instructions are the right encodings for the above sequence
    for ( uint32_t i = 0; i != 5; ++i ) {
        if ( expectedContent[i] != segmentContent[i] ) {
            return Error("Mismatched %s content. Expected elt[%d] to be 0x%x, got 0x%x", name, i, expectedContent[i], segmentContent[i]);
        }
    }

    return Error::none();
}

Error LinkerOptimizationHints::valid(std::span<const MappedSegment> segments, uint64_t loadAddress) const
{

    return std::move(lohErr);
}

} // namespace mach_o
