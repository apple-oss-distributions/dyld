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
#include <mach-o/function-variant-macros.h>

#include "CRC32c.h"

// For Dyld.framework (which uses LTO), we need default visibility so LTO preserves symbols,
// and FUNCTION_VARIANT_TABLE_EXPORTED to export the variant table.
// For libdyld and other targets, use hidden visibility and FUNCTION_VARIANT_TABLE to avoid TAPI export issues.
#if BUILDING_DYLD_FRAMEWORK
#define CRC32C_VISIBILITY __attribute__((visibility("default")))
#define CRC32C_VARIANT_TABLE FUNCTION_VARIANT_TABLE_EXPORTED
#else
#define CRC32C_VISIBILITY VIS_HIDDEN
#define CRC32C_VARIANT_TABLE FUNCTION_VARIANT_TABLE
#endif

// Forward declarations for variant implementations
CRC32C_VISIBILITY uint32_t crc32cSW(const std::span<std::byte> bytes);
#if __x86_64__ || __arm64__
CRC32C_VISIBILITY uint32_t crc32cHW(const std::span<std::byte> data);
#endif

// Fancy magic to generate the table at build time from the polynomial
class VIS_HIDDEN CRC32cSWLookupTable {
    static constexpr uint32_t valueForIndex(uint32_t n) {
        uint32_t result = n;
        for (uint8_t count = 0; count < 8; ++count) {
            result = (result & 1) ? 0x82F63B78u ^ (result >> 1) : result >> 1;
        }
        return result;
    }
    std::array<uint32_t, 256> value;
    template <size_t... indices> constexpr CRC32cSWLookupTable(std::index_sequence<indices...>): value{valueForIndex(indices)...} { }
public:
    constexpr CRC32cSWLookupTable() : CRC32cSWLookupTable(std::make_index_sequence<256>{}) {}
    constexpr uint32_t operator[](uint8_t index) const {
        return value[index];
    }
};

static constexpr auto sCRC32cSWTable = CRC32cSWLookupTable();

CRC32C_VISIBILITY
uint32_t crc32cSW(const std::span<std::byte> bytes) {
    size_t size = bytes.size();
    uint32_t result = 0xffff'ffff;
    // CRC the data
    for (uint32_t i = 0; i < size; ++i) {
        result = (result >> 8) ^ sCRC32cSWTable[(result & 0xff) ^ (uint32_t)bytes[i]];
    }
    // Invert the results
    return ~result;
}

#if 0
// We directly use __builtins because some of the intrinsics depend on preprocessor defines which are not set
#if __x86_64__
#define CRC32C_64(crc, x)   __builtin_ia32_crc32di(crc, *(uint64_t*)x)
#define CRC32C_32(crc, x)   __builtin_ia32_crc32si(crc, *(uint32_t*)x)
#define CRC32C_16(crc, x)   __builtin_ia32_crc32hi(crc, *(uint16_t*)x)
#define CRC32C_8(crc, x)    __builtin_ia32_crc32qi(crc, *(uint8_t*)x)
#define ENABLE_CRC_INTRINSICS __attribute__((__target__(("sse4.2"))))
#elif __arm64__
#define CRC32C_64(crc, x)   __builtin_arm_crc32cd(crc, *(uint64_t*)x)
#define CRC32C_32(crc, x)   __builtin_arm_crc32cw(crc, *(uint32_t*)x)
#define CRC32C_16(crc, x)   __builtin_arm_crc32ch(crc, *(uint16_t*)x)
#define CRC32C_8(crc, x)    __builtin_arm_crc32cb(crc, *(uint8_t*)x)
#define ENABLE_CRC_INTRINSICS __attribute__((__target__(("crc"))))
#endif

CRC32C_VISIBILITY ENABLE_CRC_INTRINSICS
uint32_t crc32cHW(const std::span<std::byte> data) {
    uint32_t result = 0xffff'ffff;
    uint64_t i = 0;
    std::byte* bytes = data.data();
    // First use smaller CRC32c rounds until we read 8 byte alignment
    for (; i < std::min(data.size(), ((uintptr_t)(&bytes[i]) & 0x07)); ++i) {
        result = CRC32C_8(result, &bytes[i]);
    }
    // Now do big rounds for the aligned data
    for (; i+7 < data.size(); i += 8) {
        result = (uint32_t)CRC32C_64(result, &bytes[i]);
    }
    // Finally use smaller CRC32c rounds to finish off the trailing edge
    for (; i < data.size(); ++i) {
        result = CRC32C_8(result, &bytes[i]);
    }
    return ~result;
}

// FUNCTION_VARIANT_TABLE uses mangled names which differ between 32-bit and 64-bit
// (std::dynamic_extent is 2^32-1 vs 2^64-1)
#if __arm64__ && __LP64__
CRC32C_VARIANT_TABLE(_Z6crc32cNSt3__14spanISt4byteLm18446744073709551615EEE,
    { (const void *)&crc32cHW, "crc32" },
    { (const void *)&crc32cSW,  "default"  });
#elif __arm64__ && !__LP64__
CRC32C_VARIANT_TABLE(_Z6crc32cNSt3__14spanISt4byteLm4294967295EEE,
    { (const void *)&crc32cHW, "crc32" },
    { (const void *)&crc32cSW,  "default"  });
#elif __x86_64__
// x86_64 primarily runs under Rosetta which supports CRC32 instructions
CRC32C_VARIANT_TABLE(_Z6crc32cNSt3__14spanISt4byteLm18446744073709551615EEE,
    { (const void *)&crc32cHW, "rosetta" },
    { (const void *)&crc32cSW,  "default"  });
#else
uint32_t crc32c(const std::span<std::byte> data)  {
    return crc32cSW(data);
}
#endif
#else
uint32_t crc32c(const std::span<std::byte> bytes) {
    return crc32cSW(bytes);
}
#endif

extern "C" uint32_t crc32c(const void* start, size_t size) {
    auto span = std::span<std::byte>((std::byte*)start, size);
    return crc32c(span);
}
