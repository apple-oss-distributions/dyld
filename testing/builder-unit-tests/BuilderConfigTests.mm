
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

// FIXME: Ideally we'd include our header to get these, but it conflicts with the real
// header, included via XCTest.h
#define CS_PAGE_SIZE_4K     4096
#define CS_PAGE_SIZE_16K    16384

using namespace cache_builder;

@interface BuilderConfigTests : XCTestCase

@end

@implementation BuilderConfigTests
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

// x86_64 devices should use large discontiguous layout
- (void)testLayoutKind_x86_64 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertTrue(isDiscontiguous);
    XCTAssertFalse(isContiguous);
    XCTAssertTrue(isLarge);
}

// x86_64h devices should use large discontiguous layout
- (void)testLayoutKind_x86_64h {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("x86_64h", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertTrue(isDiscontiguous);
    XCTAssertFalse(isContiguous);
    XCTAssertTrue(isLarge);
}

// arm64 devices should use large contiguous layout
- (void)testLayoutKind_arm64 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertFalse(isDiscontiguous);
    XCTAssertTrue(isContiguous);
    XCTAssertTrue(isLarge);
}

// arm64e devices should use large contiguous layout
- (void)testLayoutKind_arm64e {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::tvOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertFalse(isDiscontiguous);
    XCTAssertTrue(isContiguous);
    XCTAssertTrue(isLarge);
}

// arm64_32 devices should use large contiguous layout
- (void)testLayoutKind_arm64_32 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64_32", dyld3::Platform::watchOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertFalse(isDiscontiguous);
    XCTAssertTrue(isContiguous);
    XCTAssertTrue(isLarge);
}

// Simulators should always use "regular" layout
- (void)testLayoutKind_iOS_Simulator_x86_64 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertTrue(isDiscontiguous);
    XCTAssertFalse(isContiguous);
    XCTAssertFalse(isLarge);
}

// Simulators should always use "regular" layout
- (void)testLayoutKind_iOS_Simulator_arm64 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    bool isDiscontiguous = config.layout.discontiguous.has_value();
    bool isContiguous = config.layout.contiguous.has_value();
    bool isLarge = config.layout.large.has_value();

    // Assert:
    XCTAssertFalse(isDiscontiguous);
    XCTAssertTrue(isContiguous);
    XCTAssertFalse(isLarge);
}

- (void)testCodeSignaturePageSize_x86_64 {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("x86_64", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_4K);
}

- (void)testCodeSignaturePageSize_x86_64h {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("x86_64h", dyld3::Platform::macOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_4K);
}

- (void)testCodeSignaturePageSize_arm64_iOS {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64e_iOS {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::iOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64_tvOS {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::tvOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_4K);
}

- (void)testCodeSignaturePageSize_arm64e_tvOS {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64e", dyld3::Platform::tvOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64_32_watchOS {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64_32", dyld3::Platform::watchOS,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64_iOSSim {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::iOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64_tvOSSim {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::tvOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

- (void)testCodeSignaturePageSize_arm64_watchOSSim {
    // Arrange: mock up options and config
    cache_builder::BuilderOptions options("arm64", dyld3::Platform::watchOS_simulator,
                                          dylibsRemovedFromDisk, isLocallyBuiltCache, cacheKind,
                                          forceDevelopmentSubCacheSuffix);
    BuilderConfig config(options);

    // Act:
    uint32_t pageSize = config.codeSign.pageSize;

    // Assert:
    XCTAssertTrue(pageSize == CS_PAGE_SIZE_16K);
}

@end
