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

#ifndef mach_o_writer_LinkerOptimizationHints_hpp
#define mach_o_writer_LinkerOptimizationHints_hpp

// common

// mach_o
#include "Error.h"
#include "LinkerOptimizationHints.h"

#include <stdint.h>
#include <stdio.h>

#include <span>
#include <vector>

namespace mach_o {
struct MappedSegment;
}

namespace mach_o {

using namespace mach_o;

/*!
 * @class LinkerOptimizationHintsWriter
 *
 * @abstract
 *      Class to encapsulate building linker optimization hints
 */
class VIS_HIDDEN LinkerOptimizationHintsWriter : public LinkerOptimizationHints
{
public:

    // used by unit tests to build LOHs
    struct Location
    {
        Location(Kind kind, std::vector<uint64_t> addrs);
        Location(Kind kind, std::span<uint64_t> addrs);

        Kind kind;
        std::vector<uint64_t> addrs;

        bool operator==(const Location& other) const
        {
            return kind == other.kind && addrs == other.addrs;
        }
    };

    LinkerOptimizationHintsWriter(std::span<const Location> sortedLocs, bool is64);

private:
    std::vector<uint8_t>        _bytes;

    void append_uleb128(uint64_t value);
};

} // namespace mach_o

#endif // mach_o_writer_LinkerOptimizationHints_hpp
