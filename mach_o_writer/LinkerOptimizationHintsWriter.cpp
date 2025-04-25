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

// mach_o
#include "Fixups.h"
#include "Misc.h"

// mach_o_writer
#include "LinkerOptimizationHintsWriter.h"

namespace mach_o {

//
// MARK: --- LinkerOptimizationHints::Location methods ---
//

LinkerOptimizationHintsWriter::Location::Location(Kind kind, std::vector<uint64_t> addrs)
: kind(kind), addrs(addrs)
{
}

LinkerOptimizationHintsWriter::Location::Location(Kind kind, std::span<uint64_t> addrs)
: kind(kind), addrs(addrs.begin(), addrs.end())
{
}

void LinkerOptimizationHintsWriter::append_uleb128(uint64_t value)
{
    uint8_t byte;
    do {
        byte = value & 0x7F;
        value &= ~0x7F;
        if ( value != 0 )
            byte |= 0x80;
        _bytes.push_back(byte);
        value = value >> 7;
    } while( byte >= 0x80 );
}

LinkerOptimizationHintsWriter::LinkerOptimizationHintsWriter(std::span<const Location> sortedLocs, bool is64)
    : LinkerOptimizationHints()
{
    if ( sortedLocs.empty() )
        return;

    _bytes.reserve(256);
    for ( const Location& loc : sortedLocs ) {
        append_uleb128((uint64_t)loc.kind);
        append_uleb128((uint64_t)loc.addrs.size());
        for ( uint64_t addr : loc.addrs )
            append_uleb128(addr);
    }

    // align to pointer size
    uint32_t pointerSize = is64 ? 8 : 4;
    while ( (_bytes.size() % pointerSize) != 0 )
        _bytes.push_back(0);

    _buffer = _bytes;
}

} // namespace mach_o
