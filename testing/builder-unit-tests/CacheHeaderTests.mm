
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

#import <XCTest/XCTest.h>

#include "NewSharedCacheBuilder.h"
#include "BuilderFileSystem.h"

using namespace cache_builder;

@interface CacheHeaderTests : XCTestCase

@end

@implementation CacheHeaderTests
{
    // Everyone shares a default file system
    const FileSystemMRM fileSystem;

    // Parameters to Options
    //std::string_view archName;
    //dyld3::Platform platform;
    bool        dylibsRemovedFromDisk;
    bool        isLocallyBuiltCache;
    CacheKind   cacheKind;
    bool        forceDevelopmentSubCacheSuffix;
}

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.

    // Reset default options
    dylibsRemovedFromDisk           = true;
    isLocallyBuiltCache             = true;
    cacheKind                       = CacheKind::development;
    forceDevelopmentSubCacheSuffix  = false;
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testSlide_Sim_iOS_X86_64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_Sim_iOS_Arm64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_Sim_tvOS_X86_64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::tvOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_Sim_tvOS_Arm64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::tvOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_Sim_watchOS_X86_64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::watchOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_Sim_watchOS_Arm64
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::watchOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_macOS_X86_64_1
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (500MB), data (400MB), linkedit (300MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(500ULL * (1 << 20));
    dataRegion.subCacheVMAddress = CacheVMAddress(1024ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(400ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 1GB - textRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == (524 * (1 << 20)));
}

- (void)testSlide_macOS_X86_64_2
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (500MB), data (600MB), linkedit (300MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(500ULL * (1 << 20));
    dataRegion.subCacheVMAddress = CacheVMAddress(1024ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(600ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 1GB - dataRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == (424 * (1 << 20)));
}

- (void)testSlide_macOS_X86_64_3
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (500MB), data (600MB), linkedit (800MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(500ULL * (1 << 20));
    dataRegion.subCacheVMAddress = CacheVMAddress(1024ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(600ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(800ULL * (1 << 20));

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 1GB - linkeditRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == (224 * (1 << 20)));
}

- (void)testSlide_macOS_X86_64_4
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (1024MB), data (600MB), linkedit (800MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(1024ULL * (1 << 20));
    dataRegion.subCacheVMAddress = CacheVMAddress(1024ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(600ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(800ULL * (1 << 20));

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 1GB - textRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == 0);
}

- (void)testSlide_macOS_X86_64_5
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (500MB), data (600MB), dataConst(300MB, linkedit (800MB) regions
    subCache.regions.reserve(4);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& dataConstRegion = subCache.regions.emplace_back(Region::Kind::dataConst);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(500ULL * (1 << 20));
    dataRegion.subCacheVMAddress = CacheVMAddress(1024ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(600ULL * (1 << 20));
    dataConstRegion.subCacheVMAddress = CacheVMAddress((1024ULL + 600) * (1 << 20));
    dataConstRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(800ULL * (1 << 20));

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 1GB - (data + dataConst size)
    XCTAssertTrue(maxSlide == (124 * (1 << 20)));
}

- (void)testSlide_macOS_Arm64e_1
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (500MB), data (400MB), linkedit (300MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(500ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(400ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));
    textRegion.subCacheVMAddress = builder.config.layout.cacheBaseAddress;
    dataRegion.subCacheVMAddress = textRegion.subCacheVMAddress + textRegion.subCacheVMSize;
    linkeditRegion.subCacheVMAddress = dataRegion.subCacheVMAddress + dataRegion.subCacheVMSize;

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 2GB - textRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == ((2048 - 500) * (1 << 20)));
}

- (void)testSlide_macOS_Arm64e_2
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (2048MB), data (400MB), linkedit (300MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(2048ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(400ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));
    textRegion.subCacheVMAddress = builder.config.layout.cacheBaseAddress;
    dataRegion.subCacheVMAddress = textRegion.subCacheVMAddress + textRegion.subCacheVMSize;
    linkeditRegion.subCacheVMAddress = dataRegion.subCacheVMAddress + dataRegion.subCacheVMSize;

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 2GB - textRegion.subCacheVMSize
    XCTAssertTrue(maxSlide == ((2048 - 2048) * (1 << 20)));
}

- (void)testSlide_macOS_Arm64e_3
{
    // Arrange: mock up builder and options
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a single subCache
    SubCache& subCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    subCache.removeEmptyRegions();

    // Add text (3000MB), data (400MB), linkedit (300MB) regions
    subCache.regions.reserve(3);
    Region& textRegion = subCache.regions.emplace_back(Region::Kind::text);
    Region& dataRegion = subCache.regions.emplace_back(Region::Kind::data);
    Region& linkeditRegion = subCache.regions.emplace_back(Region::Kind::linkedit);
    textRegion.subCacheVMSize = CacheVMSize(3000ULL * (1 << 20));
    dataRegion.subCacheVMSize = CacheVMSize(400ULL * (1 << 20));
    linkeditRegion.subCacheVMSize = CacheVMSize(300ULL * (1 << 20));
    textRegion.subCacheVMAddress = builder.config.layout.cacheBaseAddress;
    dataRegion.subCacheVMAddress = textRegion.subCacheVMAddress + textRegion.subCacheVMSize;
    linkeditRegion.subCacheVMAddress = dataRegion.subCacheVMAddress + dataRegion.subCacheVMSize;

    // Act:
    uint64_t maxSlide = builder.getMaxSlide();

    // Assert:
    // Should be 4GB - (text + data + linkedit)
    XCTAssertTrue(maxSlide == ((4096 - (3000 + 400 + 300)) * (1 << 20)));
}

- (void)testCacheType_development
{
    // Arrange: mock up builder and options
    cacheKind = CacheKind::development;
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t cacheType = SubCache::getCacheType(options);

    // Assert:
    XCTAssertTrue(cacheType == kDyldSharedCacheTypeDevelopment);
}

- (void)testCacheType_universal
{
    // Arrange: mock up builder and options
    cacheKind = CacheKind::universal;
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Act:
    uint64_t cacheType = SubCache::getCacheType(options);

    // Assert:
    XCTAssertTrue(cacheType == kDyldSharedCacheTypeUniversal);
}

- (void)testCacheSubTypes
{
    // Arrange: mock up builder and options
    cacheKind = cache_builder::CacheKind::universal;
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add customer and dev main caches
    SubCache customerCache = SubCache::makeMainCache(options, false);
    SubCache developmentCache = SubCache::makeMainCache(options, true);

    // Act:
    uint64_t customerCacheSubType = customerCache.getCacheSubType();
    uint64_t developerCacheSubType = developmentCache.getCacheSubType();

    // Assert:
    XCTAssertTrue(customerCacheSubType == kDyldSharedCacheTypeProduction);
    XCTAssertTrue(developerCacheSubType == kDyldSharedCacheTypeDevelopment);
}

// Test that the total VM size for the cache is just the regions in the mapped subcaches, not
// the .symbols cache
- (void)testCacheSize
{
    // Arrange: mock up builder and options
    cacheKind = cache_builder::CacheKind::universal;
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add customer and dev main caches
    builder.subCaches.reserve(2);
    SubCache& developmentCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    SubCache& symbolsCache = builder.subCaches.emplace_back(SubCache::makeSymbolsCache());

    CacheVMSize cacheSize16KB(16ULL * (1 << 10));
    CacheVMSize cacheSize32KB(32ULL * (1 << 10));
    {
        developmentCache.removeEmptyRegions();

        // Add text (32KB)
        developmentCache.regions.reserve(1);
        Region& textRegion = developmentCache.regions.emplace_back(Region::Kind::text);
        textRegion.subCacheVMSize = cacheSize32KB;
        textRegion.subCacheVMAddress = builder.config.layout.cacheBaseAddress;
    }

    {
        symbolsCache.removeEmptyRegions();

        // Add cache header (16KB)
        symbolsCache.regions.reserve(1);
        Region& textRegion = symbolsCache.regions.emplace_back(Region::Kind::text);
        textRegion.subCacheVMSize = cacheSize16KB;
        textRegion.subCacheVMAddress = builder.config.layout.cacheBaseAddress + cacheSize32KB;
    }

    // Act:
    error::Error err = builder.computeSubCacheContiguousVMLayout();
    CacheVMSize totalVMSize = builder.totalVMSize;

    // Assert:
    XCTAssertFalse(err);
    XCTAssertTrue(totalVMSize == cacheSize32KB);
}

@end
