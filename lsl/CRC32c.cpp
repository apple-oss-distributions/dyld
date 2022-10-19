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

#include <array>
#include <cstdint>
#include <System/machine/cpu_capabilities.h>

#include "CRC32c.h"

// We directly use __builtins because some of the intrinsics depeend on preprocessor defines which are not set
#if __x86_64__
#define CRC32C_64   __builtin_ia32_crc32di
#define CRC32C_32   __builtin_ia32_crc32si
#define CRC32C_16   __builtin_ia32_crc32hi
#define CRC32C_8    __builtin_ia32_crc32qi
#define ENABLE_CRC_INTRINSICS __attribute__((__target__(("sse4.2"))))
#define CRC_HW_CHECK (*((uint64_t*)_COMM_PAGE_CPU_CAPABILITIES64) & kHasSSE4_2)
#elif __arm64__
#define CRC32C_64   __builtin_arm_crc32cd
#define CRC32C_32   __builtin_arm_crc32cw
#define CRC32C_16   __builtin_arm_crc32ch
#define CRC32C_8    __builtin_arm_crc32cb
#define ENABLE_CRC_INTRINSICS __attribute__((__target__(("crc"))))
#define CRC_HW_CHECK (*((uint32_t*)_COMM_PAGE_CPU_CAPABILITIES) & kHasARMv8Crc32)
#endif

namespace lsl {

struct VIS_HIDDEN CRC32cImpl {
    CRC32cImpl() = default;
    virtual uint32_t checksum(uint32_t crc, uint8_t data) = 0;
    virtual uint32_t checksum(uint32_t crc, uint16_t data) = 0;
    virtual uint32_t checksum(uint32_t crc, uint32_t data) = 0;
    virtual uint32_t checksum(uint32_t crc, uint64_t data) = 0;
    virtual uint32_t checksum(uint32_t crc, const std::span<std::byte> data) = 0;
};

#ifdef ENABLE_CRC_INTRINSICS
struct VIS_HIDDEN CRC32cHW : CRC32cImpl{
    ENABLE_CRC_INTRINSICS
    uint32_t checksum(uint32_t crc, uint8_t data) override {
        return CRC32C_8(crc, data);
    }
    ENABLE_CRC_INTRINSICS
    uint32_t checksum(uint32_t crc, uint16_t data) override {
        return CRC32C_16(crc, data);
    }
    ENABLE_CRC_INTRINSICS
    uint32_t checksum(uint32_t crc, uint32_t data) override {
        return CRC32C_32(crc, data);
    }
    ENABLE_CRC_INTRINSICS
    uint32_t checksum(uint32_t crc, uint64_t data) override {
        return (uint32_t)CRC32C_64(crc, data);
    }
    uint32_t checksum(uint32_t crc, const std::span<std::byte> data) override {
        auto i = data.begin();
        // First use smaller CRC32c rounds until we read 8 byte alignment
        if ((uintptr_t)(*i) & 0x01) { crc = checksum(crc, (uint8_t)(*i));     i+=1; }
        if ((uintptr_t)(*i) & 0x02) { crc = checksum(crc, (uint16_t)(*i));    i+=2; }
        if ((uintptr_t)(*i) & 0x04) { crc = checksum(crc, (uint16_t)(*i));    i+=2; }
        auto alignedRounds = (data.end()-i)/8;
        for (auto j=0; j < alignedRounds; ++j) { crc = checksum(crc, (uint64_t)(*i));  i+=8; }
        // Finally use smaller CRC32c rounds to finish off the trailing edge
        if ((uintptr_t)(data.end()-i) & 0x04) { crc = checksum(crc, (uint16_t)(*i));    i+=2; }
        if ((uintptr_t)(data.end()-i) & 0x02) { crc = checksum(crc, (uint16_t)(*i));    i+=2; }
        if ((uintptr_t)(data.end()-i) & 0x01) { crc = checksum(crc, (uint8_t)(*i));     i+=1; }
        return crc;
    }
};

static constinit CRC32cHW sCRC32cHW;
#endif

namespace {
class VIS_HIDDEN CRC32cLookupTable {
    static constexpr uint32_t valueForIndex(uint32_t n) {
        uint32_t result = n;
        for (uint8_t count = 0; count < 8; ++count) {
            result = (result & 1) ? 0x82F63B78u ^ (result >> 1) : result >> 1;
        }
        return result;
    }
    std::array<uint32_t, 256> value;
    template <size_t... indices> constexpr CRC32cLookupTable(std::index_sequence<indices...>): value{valueForIndex(indices)...} { }
public:
    constexpr CRC32cLookupTable() : CRC32cLookupTable(std::make_index_sequence<256>{}) {}
    constexpr uint32_t operator[](uint8_t index) const {
        return value[index];
    }
};
}

struct VIS_HIDDEN CRC32cSW : CRC32cImpl {
    static constexpr inline auto sCRC32cTable = CRC32cLookupTable();
    uint32_t checksum(uint32_t crc, uint8_t data) override {
        return (crc >> 8) ^ sCRC32cTable[(crc & 0xff) ^ data];
    }
    uint32_t checksum(uint32_t crc, uint16_t data) override {
        auto data8 = (std::byte*)&data;
        return checksum(crc, std::span(data8, 2));
    }
    uint32_t checksum(uint32_t crc, uint32_t data) override {
        auto data8 = (std::byte*)&data;
        return checksum(crc, std::span(data8, 4));
    }
    uint32_t checksum(uint32_t crc, uint64_t data) override {
        auto data8 = (std::byte*)&data;
        return checksum(crc, std::span(data8, 8));
    }
    uint32_t checksum(uint32_t crc, const std::span<std::byte> data) override {
        for (auto datum : data) {
            crc = (crc >> 8) ^ sCRC32cTable[(crc & 0xff) ^ (uint8_t)datum];
        }
        return crc;
    }
};

static constinit CRC32cSW sCRC32cSW;

CRC32c::CRC32c() : _impl(sCRC32cSW) {
#ifdef ENABLE_CRC_INTRINSICS
    if (CRC_HW_CHECK) {
        _impl = sCRC32cHW;
    }
#endif
}

CRC32c::CRC32c(CRC32cImpl& impl) : _impl(impl) {}

CRC32c::operator uint32_t() {
    return ~_crc;
}
void CRC32c::operator()(uint8_t x) {
    _crc = _impl.checksum(_crc, x);
}
void CRC32c::operator()(uint16_t x) {
    _crc = _impl.checksum(_crc, x);
}
void CRC32c::operator()(uint32_t x) {
    _crc = _impl.checksum(_crc, x);
}
void CRC32c::operator()(uint64_t x) {
    _crc = _impl.checksum(_crc, x);
}
void CRC32c::operator()(const std::span<std::byte> x) {
    _crc = _impl.checksum(_crc, x);
}

void CRC32c::reset() {
    _crc = 0xffffffff;
}

CRC32c CRC32c::softwareChecksumer() {
    return CRC32c(sCRC32cSW);
}

#ifdef ENABLE_CRC_INTRINSICS
CRC32c CRC32c::hardwareChecksumer() {
    return CRC32c(sCRC32cHW);
}
#endif
};
