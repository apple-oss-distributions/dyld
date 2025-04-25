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

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

// mach_o
#include "Misc.h"

// mach_o_writer
#include "FunctionStartsWriter.h"


namespace mach_o {

FunctionStartsWriter::FunctionStartsWriter(uint64_t prefLoadAddr, std::span<const uint64_t> functionAddresses) : FunctionStarts(nullptr, 0)
{
    uint64_t lastAddr = prefLoadAddr;
    for (uint64_t addr : functionAddresses) {
        assert(addr >= lastAddr && "function addresses not sorted");
        // <rdar://problem/10422823> filter out zero-length atoms, so LC_FUNCTION_STARTS address can't spill into next section
        if ( addr == lastAddr)
            continue;
        // FIXME: for 32-bit arm need to check thumbness
        uint64_t delta = addr - lastAddr;
        append_uleb128(delta);
        lastAddr = addr;
    }
    // terminate delta encoded list
    _bytes.push_back(0);
    // 8-byte align
    while ( (_bytes.size() % 8) != 0 )
        _bytes.push_back(0);

    // set up pointers to data can be parsed
    _funcStartsBegin = _bytes.data();
    _funcStartsEnd   = _bytes.data()+_bytes.size();
}

void FunctionStartsWriter::append_uleb128(uint64_t value)
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




} // namespace mach_o
