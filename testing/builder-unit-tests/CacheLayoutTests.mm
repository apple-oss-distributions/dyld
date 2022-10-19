
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

@interface CacheLayoutTests : XCTestCase

@end

@implementation CacheLayoutTests
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

// macOS caches should have no .development or other suffixes, just the number
- (void)testSubCacheNames_macOS {
    // Arrange: mock up builder and subcaches
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a main subCache
    builder.subCaches.reserve(3);
    SubCache& mainSubCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    SubCache& subCache1 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& subCache2 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    mainSubCache.subCaches.push_back(&subCache1);
    mainSubCache.subCaches.push_back(&subCache2);

    // Act:
    builder.setSubCacheNames();

    // Assert:
    XCTAssertTrue(mainSubCache.fileSuffix == "");
    XCTAssertTrue(subCache1.fileSuffix == ".01");
    XCTAssertTrue(subCache2.fileSuffix == ".02");
}

// Simulator caches should have no .development or other suffixes, just the number
- (void)testSubCacheNames_iOS_Simulator {
    // Arrange: mock up builder and subcaches
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a main subCache
    builder.subCaches.reserve(3);
    SubCache& mainSubCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    SubCache& subCache1 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& subCache2 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    mainSubCache.subCaches.push_back(&subCache1);
    mainSubCache.subCaches.push_back(&subCache2);

    // Act:
    builder.setSubCacheNames();

    // Assert:
    XCTAssertTrue(mainSubCache.fileSuffix == "");
    XCTAssertTrue(subCache1.fileSuffix == ".01");
    XCTAssertTrue(subCache2.fileSuffix == ".02");
}

// iOS development main caches should end in .developent
// iOS sub caches shouldn't have an extension
- (void)testSubCacheNames_iOS_development {
    // Arrange: mock up builder and subcaches
    cacheKind = CacheKind::universal;
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a main subCache
    builder.subCaches.reserve(3);
    SubCache& mainSubCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, true));
    SubCache& subCache1 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& subCache2 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    mainSubCache.subCaches.push_back(&subCache1);
    mainSubCache.subCaches.push_back(&subCache2);

    // Act:
    builder.setSubCacheNames();

    // Assert:
    XCTAssertTrue(mainSubCache.fileSuffix == ".development");
    XCTAssertTrue(subCache1.fileSuffix == ".01");
    XCTAssertTrue(subCache2.fileSuffix == ".02");
}

// iOS customer main caches should have no extension
// iOS sub caches shouldn't have an extension
- (void)testSubCacheNames_iOS_customer {
    // Arrange: mock up builder and subcaches
    cacheKind = CacheKind::universal;
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);

    // Add a main subCache
    builder.subCaches.reserve(3);
    SubCache& mainSubCache = builder.subCaches.emplace_back(SubCache::makeMainCache(options, false));
    SubCache& subCache1 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& subCache2 = builder.subCaches.emplace_back(SubCache::makeSubCache(options));
    mainSubCache.subCaches.push_back(&subCache1);
    mainSubCache.subCaches.push_back(&subCache2);

    // Act:
    builder.setSubCacheNames();

    // Assert:
    XCTAssertTrue(mainSubCache.fileSuffix == "");
    XCTAssertTrue(subCache1.fileSuffix == ".01");
    XCTAssertTrue(subCache2.fileSuffix == ".02");
}

// This combines all of the above names
// main caches should have either .development or no extension
// sub caches should either no extension, .dylddata, or .dyldlinkedit
// stub caches should have no extension or .development
// symbols files should end in .symbols
- (void)testSubCacheNames_iOS_universal {
    // Arrange: mock up builder and subcaches
    cacheKind = CacheKind::universal;
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    SharedCacheBuilder builder(options, fileSystem);
    auto& subCaches = builder.subCaches;

    // Add the following:
    // main dev cache
    // main customer cache
    // text subcache
    // stubs dev cache
    // stubs customer cache
    // text subcache
    // stubs dev cache
    // stubs customer cache
    // data sub cache
    // linkedit sub cache
    // text subcache
    // stubs dev cache
    // stubs customer cache
    // text subcache
    // stubs dev cache
    // stubs customer cache
    // data sub cache
    // linkedit sub cache
    subCaches.reserve(20);
    SubCache& mainDevCache = subCaches.emplace_back(SubCache::makeMainCache(options, true));
    SubCache& mainCustCache = subCaches.emplace_back(SubCache::makeMainCache(options, false));
    SubCache& textCache1 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& stubsDevCache2 = subCaches.emplace_back(SubCache::makeStubsCache(options, true));
    SubCache& stubsCustCache2 = subCaches.emplace_back(SubCache::makeStubsCache(options, false));
    SubCache& textCache3 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& stubsDevCache4 = subCaches.emplace_back(SubCache::makeStubsCache(options, true));
    SubCache& stubsCustCache4 = subCaches.emplace_back(SubCache::makeStubsCache(options, false));
    SubCache& dataCache5 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& linkeditCache6 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& textCache7 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& stubsDevCache8 = subCaches.emplace_back(SubCache::makeStubsCache(options, true));
    SubCache& stubsCustCache8 = subCaches.emplace_back(SubCache::makeStubsCache(options, false));
    SubCache& textCache9 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& stubsDevCache10 = subCaches.emplace_back(SubCache::makeStubsCache(options, true));
    SubCache& stubsCustCache10 = subCaches.emplace_back(SubCache::makeStubsCache(options, false));
    SubCache& dataCache11 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& linkeditCache12 = subCaches.emplace_back(SubCache::makeSubCache(options));
    SubCache& symbolsCache = subCaches.emplace_back(SubCache::makeSymbolsCache());

    // Add some data/linkedit to the required subcaches
    ObjCStringsChunk dataChunk1;
    ObjCStringsChunk linkeditChunk1;
    ObjCStringsChunk dataChunk2;
    ObjCStringsChunk dataConstChunk2;
    ObjCStringsChunk authChunk2;
    ObjCStringsChunk authConstChunk2;
    ObjCStringsChunk linkeditChunk2;
    dataCache5.addDataChunk(&dataChunk1);
    linkeditCache6.addLinkeditChunk(&linkeditChunk1);
    dataCache11.addDataChunk(&dataChunk2);
    dataCache11.addDataConstChunk(&dataConstChunk2);
    dataCache11.addAuthChunk(&authChunk2);
    dataCache11.addAuthConstChunk(&authConstChunk2);
    linkeditCache12.addLinkeditChunk(&linkeditChunk2);

    // Track all subCaches on the main caches
    // Development
    mainDevCache.subCaches.push_back(&textCache1);
    mainDevCache.subCaches.push_back(&stubsDevCache2);
    mainDevCache.subCaches.push_back(&textCache3);
    mainDevCache.subCaches.push_back(&stubsDevCache4);
    mainDevCache.subCaches.push_back(&dataCache5);
    mainDevCache.subCaches.push_back(&linkeditCache6);
    mainDevCache.subCaches.push_back(&textCache7);
    mainDevCache.subCaches.push_back(&stubsDevCache8);
    mainDevCache.subCaches.push_back(&textCache9);
    mainDevCache.subCaches.push_back(&stubsDevCache10);
    mainDevCache.subCaches.push_back(&dataCache11);
    mainDevCache.subCaches.push_back(&linkeditCache12);

    // Customer
    mainCustCache.subCaches.push_back(&textCache1);
    mainCustCache.subCaches.push_back(&stubsCustCache2);
    mainCustCache.subCaches.push_back(&textCache3);
    mainCustCache.subCaches.push_back(&stubsCustCache4);
    mainCustCache.subCaches.push_back(&dataCache5);
    mainCustCache.subCaches.push_back(&linkeditCache6);
    mainCustCache.subCaches.push_back(&textCache7);
    mainCustCache.subCaches.push_back(&stubsCustCache8);
    mainCustCache.subCaches.push_back(&textCache9);
    mainCustCache.subCaches.push_back(&stubsCustCache10);
    mainCustCache.subCaches.push_back(&dataCache11);
    mainCustCache.subCaches.push_back(&linkeditCache12);

    // Act:
    builder.setSubCacheNames();

    // Assert:
    XCTAssertTrue(mainDevCache.fileSuffix        == ".development");
    XCTAssertTrue(mainCustCache.fileSuffix       == "");
    XCTAssertTrue(textCache1.fileSuffix          == ".01");
    XCTAssertTrue(stubsDevCache2.fileSuffix      == ".02.development");
    XCTAssertTrue(stubsCustCache2.fileSuffix     == ".02");
    XCTAssertTrue(textCache3.fileSuffix          == ".03");
    XCTAssertTrue(stubsDevCache4.fileSuffix      == ".04.development");
    XCTAssertTrue(stubsCustCache4.fileSuffix     == ".04");
    XCTAssertTrue(dataCache5.fileSuffix          == ".05.dylddata");
    XCTAssertTrue(linkeditCache6.fileSuffix      == ".06.dyldlinkedit");
    XCTAssertTrue(textCache7.fileSuffix          == ".07");
    XCTAssertTrue(stubsDevCache8.fileSuffix      == ".08.development");
    XCTAssertTrue(stubsCustCache8.fileSuffix     == ".08");
    XCTAssertTrue(textCache9.fileSuffix          == ".09");
    XCTAssertTrue(stubsDevCache10.fileSuffix     == ".10.development");
    XCTAssertTrue(stubsCustCache10.fileSuffix    == ".10");
    XCTAssertTrue(dataCache11.fileSuffix         == ".11.dylddata");
    XCTAssertTrue(linkeditCache12.fileSuffix     == ".12.dyldlinkedit");
    XCTAssertTrue(symbolsCache.fileSuffix        == ".symbols");
}

@end
