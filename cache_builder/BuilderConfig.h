/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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

#ifndef BuilderConfig_hpp
#define BuilderConfig_hpp

#include "Timer.h"
#include "Types.h"

#include <string>

namespace cache_builder
{

constexpr uint64_t operator"" _KB(uint64_t v)
{
    return (1ULL << 10) * v;
}

constexpr uint64_t operator"" _MB(uint64_t v)
{
    return (1ULL << 20) * v;
}

constexpr uint64_t operator"" _GB(uint64_t v)
{
    return (1ULL << 30) * v;
}

constexpr uint64_t operator"" _GB(long double v)
{
    return (1ULL << 30) * v;
}

struct BuilderOptions;

// Layout handles all the different kinds of cache we can build.  They are:
//  - regular contiguous:    The cache is one big file, eg, arm64 simulators
//  - regular discontiguous: The cache is one big file, eg, x86_64 simulators
//  - large contiguous:      The cache is one or more files, which each contain TEXT/DATA/LINKEDIT.  Eg, macOS/iOS/tvOS arm64
//  - large discontiguous:   The cache is one or more files, which each contain TEXT/DATA/LINKEDIT.  Eg, macOS x86_64
struct Layout
{
    Layout(const BuilderOptions& options);

    // Used only for x86_64*
    struct Discontiguous
    {
        // For the host OS, regions should be 1GB aligned
        // If this has a value, then we use it.  Otherwise we fall back to the sim fixed addresses
        std::optional<uint64_t> regionAlignment;

        // For the sim, each region has fixed addresses
        CacheVMAddress simTextBaseAddress;
        CacheVMAddress simDataBaseAddress;
        CacheVMAddress simLinkeditBaseAddress;

        CacheVMSize simTextSize;
        CacheVMSize simDataSize;
        CacheVMSize simLinkeditSize;
    };

    // Used only for arm64
    struct Contiguous
    {
        // How many bytes of padding do we add between each Region
        CacheVMSize regionPadding;

        // How much __TEXT before we make a new stubs subCache
        CacheVMSize subCacheStubsLimit;
    };

    struct Large
    {
        // How much __TEXT in each subCache before we split to a new file
        CacheVMSize subCacheTextLimit;
    };

    // Fields for all layouts
    CacheVMAddress          cacheBaseAddress;
    CacheVMSize             cacheSize;
    const bool              is64;
    const bool              hasAuthRegion;
    const uint32_t          pageSize;
    const uint32_t          machHeaderAlignment = 4096;

    // Whether to put the LINKEDIT in the last subCache
    // Only possible if the total cache limit is <= 4GB
    bool                    allLinkeditInLastSubCache = false;

    // Fields only used for discontiguous layouts, ie, x86_64
    std::optional<Discontiguous>    discontiguous;

    // Fields only used for contiguous layouts, ie, arm64*
    std::optional<Contiguous>       contiguous;

    // Fields only used for large layouts, ie, on device, not simulators
    std::optional<Large>            large;
};

struct SlideInfo
{
    SlideInfo(const BuilderOptions& options, const Layout& layout);

    enum class SlideInfoFormat
    {
        v1,
        v2,
        v3,
        // v4 (deprecated.  arm64_32 uses v1 instead)
    };

    std::optional<SlideInfoFormat>  slideInfoFormat;
    uint32_t                        slideInfoBytesPerDataPage;
    const uint32_t                  slideInfoPageSize           = 4096;
    CacheVMAddress                  slideInfoValueAdd;
    uint64_t                        slideInfoDeltaMask          = 0;
};

struct CodeSign
{
    CodeSign(const BuilderOptions& options);

    enum class Mode
    {
        onlySHA256,
        onlySHA1,
        agile
    };

    const Mode      mode;
    const uint32_t  pageSize;
};

struct BuilderConfig
{
    BuilderConfig(const BuilderOptions& options);

    Logger      log;
    Timer       timer;
    Layout      layout;
    SlideInfo   slideInfo;
    CodeSign    codeSign;
};

} // namespace cache_builder

#endif /* BuilderConfig_hpp */
