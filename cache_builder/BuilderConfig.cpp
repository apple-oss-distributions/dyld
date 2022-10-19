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

#include "dyld_cache_config.h"

#include <assert.h>

using namespace cache_builder;
using dyld3::GradedArchs;

//
// MARK: --- cache_builder::Logger methods ---
//

cache_builder::Logger::Logger(const BuilderOptions& options)
    : logPrefix(options.logPrefix)
{
    this->printTimers = options.timePasses;
    this->printStats  = options.stats;
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

cache_builder::Layout::Layout(const BuilderOptions& options)
    : is64(options.archs.supports64())
    , hasAuthRegion(::hasAuthRegion(options.archs.name()))
    , pageSize(defaultPageSize(options.archs.name()))
{
    std::string_view archName = options.archs.name();

    if ( (archName == "x86_64") || (archName == "x86_64h") ) {
        // x86_64 uses discontiguous mappings
        this->discontiguous.emplace();

        if ( options.isSimultor() ) {
            // The simulator has fixed addresses
            this->discontiguous->simTextSize = CacheVMSize(1.5_GB);
            this->discontiguous->simDataSize = CacheVMSize(1_GB);
            this->discontiguous->simLinkeditSize = CacheVMSize(1_GB);
            this->discontiguous->simTextBaseAddress = CacheVMAddress(X86_64_SHARED_REGION_START);
            this->discontiguous->simDataBaseAddress = this->discontiguous->simTextBaseAddress + this->discontiguous->simTextSize;
            this->discontiguous->simLinkeditBaseAddress = this->discontiguous->simDataBaseAddress + this->discontiguous->simDataSize;
        } else {
            this->discontiguous->regionAlignment = 1_GB;
        }
    } else {
        // Everyone else uses contiguous mappings
        this->contiguous.emplace();
        this->contiguous->regionPadding = CacheVMSize(32_MB);
        this->contiguous->subCacheStubsLimit = CacheVMSize(110_MB);
    }

    if ( !options.isSimultor() ) {
        // Devices always get large layout.  Simulators get the regular layout
        this->large.emplace();

        if ( (archName == "x86_64") || (archName == "x86_64h") ) {
            this->large->subCacheTextLimit = CacheVMSize(512_MB);
        } else {
            this->large->subCacheTextLimit = CacheVMSize(1.25_GB);
        }
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
        layout.cacheSize = ARM64_SHARED_REGION_SIZE;
    } else if ( archName == "arm64_32" ) {
        layout.baseAddress = ARM64_32_SHARED_REGION_START;
        layout.cacheSize = ARM64_32_SHARED_REGION_SIZE;
    } else {
        assert("Unknown arch");
    }

    this->cacheBaseAddress          = CacheVMAddress(layout.baseAddress);
    this->cacheSize                 = CacheVMSize(layout.cacheSize);
    this->allLinkeditInLastSubCache = this->cacheSize <= CacheVMSize(4_GB);
}

//
// MARK: --- cache_builder::SlideInfo methods ---
//

SlideInfo::SlideInfo(const BuilderOptions& options, const Layout& layout)
{
    // Compute slide info.  Note the simulator doesn't slide
    if ( options.isSimultor() )
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
        this->slideInfoFormat = SlideInfoFormat::v3;

        // 1 uint16_t per page
        this->slideInfoBytesPerDataPage = 2;
    }
    else if ( archName == "arm64_32" ) {
        this->slideInfoFormat = SlideInfoFormat::v1;

        // 128 bytes per page.  Enough for a bitmap with 1-bit entry per 32-bit location
        this->slideInfoBytesPerDataPage = 128;
    }
    else {
        assert("Unknown arch");
    }
}

//
// MARK: --- cache_builder::CodeSign methods ---
//

static cache_builder::CodeSign::Mode platformCodeSigningDigestMode(dyld3::Platform platform)
{
    if ( platform == dyld3::Platform::watchOS )
        return cache_builder::CodeSign::Mode::agile;
    return cache_builder::CodeSign::Mode::onlySHA256;
}

static uint32_t codeSigningPageSize(dyld3::Platform platform, const GradedArchs& arch)
{
    std::string_view archName = arch.name();
    if ( (archName == "arm64e") || (archName == "arm64_32") )
        return CS_PAGE_SIZE_16K;

    // arm64 on iOS is new enough for 16k pages, as is arm64 on macOS (ie the simulator)
    if ( archName == "arm64") {
        if ( dyld3::MachOFile::isSimulatorPlatform(platform) || (platform == dyld3::Platform::iOS) )
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
