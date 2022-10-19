/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
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

#include <bit>

#include "Defines.h"
#include "BitUtils.h"
#include "PVLEInt64.h"

namespace lsl {
void emitPVLEUInt64(uint64_t value, Vector<std::byte>& data) {
    auto valueBytes = (std::byte*)&value;
    const uint8_t activeBits = std::max<uint8_t>(lsl::bit_width(value),1);
    if (activeBits > 56) {
        data.push_back((std::byte)0);
        std::copy(&valueBytes[0], &valueBytes[8], std::back_inserter(data));
        return;
    }
    const uint8_t bytes = (activeBits+6)/7;
    value <<= bytes;
    value |= 1<<(bytes-1);
    std::copy(&valueBytes[0], &valueBytes[bytes], std::back_inserter(data));
}

uint64_t readPVLEUInt64(std::span<std::byte>& data) {
    uint64_t result = 0;
    contract(data.size() != 0);
    const uint8_t additionalByteCount   = std::countr_zero((uint8_t)data[0]);
    if (additionalByteCount == 8) {
        std::copy(&data[1], &data[9], (std::byte*)&result);
        data = data.last(data.size()-9);
        return result;
    }
    contract(data.size() >= 1+additionalByteCount);
    const uint8_t extraBitCount     = 8 - (additionalByteCount+1);
    const uint8_t extraBits         = (((uint8_t)(data[0]))>>(additionalByteCount+1)) & ((1<<extraBitCount)-1);
    std::copy(&data[1], &data[additionalByteCount+1], (std::byte*)&result);
    result <<= extraBitCount;
    result |= extraBits;
    data = data.last(data.size()-(additionalByteCount+1));
    return result;
}

void emitPVLEInt64(int64_t value, Vector<std::byte>& data) {
    emitPVLEUInt64((value >> 63) ^ (value << 1), data);
}

int64_t readPVLEInt64(std::span<std::byte>& data) {
    uint64_t value = readPVLEUInt64(data);
    return ((value & 1) ? (value >> 1) ^ -1 : (value >> 1));
}

};
