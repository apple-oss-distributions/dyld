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

#include "BuilderConfig.h"
#include "BuilderOptions.h"
#include "CodeSigningTypes.h"
#include "Platform.h"

#include "dyld_cache_config.h"

#include <assert.h>

using namespace cache_builder;
using dyld3::GradedArchs;

using mach_o::Platform;

//
// MARK: --- cache_builder::Logger methods ---
//

cache_builder::Logger::Logger(const BuilderOptions& options)
    : logPrefix(options.logPrefix)
{
    this->printTimers = options.timePasses;
    this->printStats  = options.stats;
    this->printDebug  = options.debug;
}

void cache_builder::Logger::log(const char* format, ...) const
{
    char*   output_string;
    va_list list;
    va_start(list, format);
    vasprintf(&output_string, format, list);
    va_end(list);

    fprintf(stderr, "[%s]: %s", this->logPrefix.c_str(), output_string);

    free(output_string);
}

//
// MARK: --- cache_builder::Layout methods ---
//

static uint32_t defaultPageSize(std::string_view archName)
{
    if ( (archName == "x86_64") || (archName == "x86_64h") )
        return 4096;
    else
        return 16384;
}

static bool hasAuthRegion(std::string_view archName)
{
    return archName == "arm64e";
}

static uint32_t supportsTPROMapping(std::string_view archName)
{
    return (archName != "x86_64") && (archName != "x86_64h");
}

cache_builder::Layout::Layout(const BuilderOptions& options)
    : is64(options.archs.supports64())
    , hasAuthRegion(::hasAuthRegion(options.archs.name()))
    , tproIsInData(!::supportsTPROMapping(options.archs.name()))
    , pageSize(defaultPageSize(options.archs.name()))
{
    std::string_view archName = options.archs.name();

    if ( (archName == "x86_64") || (archName == "x86_64h") ) {
        // x86_64 uses discontiguous mappings
        this->discontiguous.emplace();

        this->discontiguous->regionAlignment = 1_GB;
        this->discontiguous->subCacheTextLimit = CacheVMSize(512_MB);
    } else {
        // Everyone else uses contiguous mappings
        this->contiguous.emplace();
        this->contiguous->regionPadding = CacheVMSize(32_MB);

        // Note minus a little to account for rdar://146432183
        this->contiguous->subCacheTextDataLimit = CacheVMSize(2_GB - 16_MB);
        this->contiguous->subCacheStubsLimit = CacheVMSize(110_MB);

        // Note we have 2 padding regions in total in a given TEXT/DATA/AUTH/... region
        // 1 between TEXT/DATA_CONST and DATA, then another from DATA to LINKEDIT.
        this->contiguous->subCachePadding = this->contiguous->regionPadding + this->contiguous->regionPadding;
    }

    struct CacheLayout
    {
        uint64_t baseAddress;
        uint64_t cacheSize;
    };
    CacheLayout layout;

    if ( (archName == "x86_64") || (archName == "x86_64h") ) {
        layout.baseAddress = X86_64_SHARED_REGION_START;
        layout.cacheSize = X86_64_SHARED_REGION_SIZE;
    } else if ( (archName == "arm64") || (archName == "arm64e") ) {
        layout.baseAddress = ARM64_SHARED_REGION_START;

        if ( options.isSimulator() ) {
            // Limit to 4GB to support back deployment to older hosts with 4GB shared regions
            layout.cacheSize = 4_GB;
        } else {
            layout.cacheSize = ARM64_SHARED_REGION_SIZE;
        }

        // Limit the max slide for arm64 based caches to 512MB.  Combined with large
        // caches putting 1.5GB of TEXT in the first cache region, this will ensure that
        // this 1.5GB of TEXT will stay in the same 2GB region.  <rdar://problem/49852839>
        cacheMaxSlide = 512_MB;
    } else if ( archName == "arm64_32" ) {
        layout.baseAddress = ARM64_32_SHARED_REGION_START;
        layout.cacheSize = 2_GB;

        // The cache contents can't exceed 2GB, but use the space above it for the slide
        if ( ARM64_32_SHARED_REGION_SIZE >= layout.cacheSize ) {
            this->cacheFixedSlide = ARM64_32_SHARED_REGION_SIZE - layout.cacheSize;
        }
    } else {
        assert("Unknown arch");
    }

    this->cacheBaseAddress          = CacheVMAddress(layout.baseAddress);
    this->cacheSize                 = CacheVMSize(layout.cacheSize);
}

//
// MARK: --- cache_builder::SlideInfo methods ---
//

SlideInfo::SlideInfo(const BuilderOptions& options, const Layout& layout)
{
    // Compute slide info.  Note the simulator doesn't slide
    if ( options.isSimulator() )
        return;

    std::string_view archName = options.archs.name();
    if ( (archName == "x86_64") || (archName == "x86_64h") || (archName == "arm64") ) {
        this->slideInfoFormat = SlideInfoFormat::v2;

        // 1 uint16_t per page
        this->slideInfoBytesPerDataPage = 2;

        // x86_64 and arm64 share the same mask, as Swift needs the high byte as if x86_64 had TBI
        this->slideInfoDeltaMask = 0x00FFFF0000000000ULL;

        // Only x86_64 needs a value add field on slide info V2
        if ( (archName == "x86_64") || (archName == "x86_64h") ) {
            this->slideInfoValueAdd = layout.cacheBaseAddress;
        }
        else {
            this->slideInfoValueAdd = CacheVMAddress(0ULL);
        }
    }
    else if ( archName == "arm64e" ) {
        // 1 uint16_t per page
        this->slideInfoBytesPerDataPage = 2;

        if ( layout.cacheSize > CacheVMSize(4_GB) ) {
            this->slideInfoFormat = SlideInfoFormat::v5;

            // 16k pages so that we can also use page-in linking for this format
            this->slideInfoPageSize = 0x4000;
        } else {
            this->slideInfoFormat = SlideInfoFormat::v3;
        }
    }
    else if ( archName == "arm64_32" ) {
        this->slideInfoFormat = SlideInfoFormat::v1;

        // 128 bytes per page.  Enough for a bitmap with 1-bit entry per 32-bit location
        // Plus 2-bytes per page for the TOC offset
        this->slideInfoBytesPerDataPage = 130;
    }
    else {
        assert("Unknown arch");
    }
}

//
// MARK: --- cache_builder::CodeSign methods ---
//

static cache_builder::CodeSign::Mode platformCodeSigningDigestMode(Platform platform)
{
    if ( platform == Platform::watchOS )
        return cache_builder::CodeSign::Mode::agile;
    return cache_builder::CodeSign::Mode::onlySHA256;
}

static uint32_t codeSigningPageSize(Platform platform, const GradedArchs& arch)
{
    std::string_view archName = arch.name();
    if ( (archName == "arm64e") || (archName == "arm64_32") )
        return CS_PAGE_SIZE_16K;

    // arm64 on iOS is new enough for 16k pages, as is arm64 on macOS (ie the simulator)
    if ( archName == "arm64") {
        if ( platform.isSimulator() || (platform == Platform::iOS) )
            return CS_PAGE_SIZE_16K;
        return CS_PAGE_SIZE_4K;
    }

    if ( (archName == "x86_64") || (archName == "x86_64h") )
        return CS_PAGE_SIZE_4K;

    // Unknown arch
    assert(0);
}

cache_builder::CodeSign::CodeSign(const BuilderOptions& options)
    : mode(platformCodeSigningDigestMode(options.platform))
    , pageSize(codeSigningPageSize(options.platform, options.archs))
{
}

//
// MARK: --- cache_builder::BuilderConfig methods ---
//

cache_builder::BuilderConfig::BuilderConfig(const BuilderOptions& options)
    : log(options)
    , timer()
    , layout(options)
    , slideInfo(options, layout)
    , codeSign(options)
{
}
