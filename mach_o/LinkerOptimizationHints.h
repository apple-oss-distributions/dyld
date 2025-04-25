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

#ifndef macho_LinkerOptimizationHints_hpp
#define macho_LinkerOptimizationHints_hpp

// common
#include "MachODefines.h"

// mach_o
#include "Error.h"

#include <stdint.h>
#include <stdio.h>

#include <span>

namespace mach_o {

struct MappedSegment;

/*!
 * @class LinkerOptimizationHints
 *
 * @abstract
 *      Class to encapsulate accessing and building linker optimization hints
 */
class VIS_HIDDEN LinkerOptimizationHints
{
public:
    enum class Kind
    {
        unknown = 0,
        // 1 - 8 are actual LOHs we don't use any more

    };

    // construct from LC_LINKER_OPTIMIZATION_HINT range in .o file
    LinkerOptimizationHints(std::span<const uint8_t> buffer);

    Error           valid(std::span<const MappedSegment> segments, uint64_t loadAddress) const;
    Error           forEachLOH(void (^callback)(Kind kind, std::span<uint64_t> addrs, bool& stop)) const;
    void            printLOHs(FILE* output, int indent=0) const;
    const uint8_t*  bytes(size_t& size) const;

protected:
    // for use by LinkerOptimizationHintsWriter
    LinkerOptimizationHints() = default;

    std::span<const uint8_t>    _buffer;
};

} // namespace mach_o

#endif // macho_LinkerOptimizationHints_hpp
