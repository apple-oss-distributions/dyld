
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

#include "ImpCachesBuilder.h"
#include "JSONReader.h"

@interface IMPCachesTests : XCTestCase

@end

@implementation IMPCachesTests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testIMPCaches
{
    self.continueAfterFailure = NO;

    // Arrange: mock up dylibs, classes, methods
    std::string json = R"(
    {
      "version": "2",
      "neededClasses": [
        {
          "name": "NSObject",
          "metaclass": "0"
        },
        {
          "name": "NSObject",
          "metaclass": "1"
        },
      ]
    }
    )";

    Diagnostics diag;
    dyld3::json::Node objcOptimizations = dyld3::json::readJSON(diag, json.data(), json.size());
    XCTAssertFalse(diag.hasError());

    std::vector<imp_caches::Dylib> dylibs;
    uint32_t indexDylibA = (uint32_t)dylibs.size();
    imp_caches::Dylib& dylibA = dylibs.emplace_back("a.dylib");
    imp_caches::Class& classNSObject = dylibA.classes.emplace_back("NSObject", false, true);
    classNSObject.methods.emplace_back("a");
    classNSObject.methods.emplace_back("b");
    imp_caches::Class& metaclassNSObject = dylibA.classes.emplace_back("NSObject", true, true);
    metaclassNSObject.methods.emplace_back("A");
    metaclassNSObject.methods.emplace_back("B");

    imp_caches::Builder builder(dylibs, objcOptimizations);

    // Act:
    builder.buildImpCaches();

    std::optional<imp_caches::IMPCache> cacheClassNSObject = builder.getIMPCache(indexDylibA, "NSObject", false);
    std::optional<imp_caches::IMPCache> cacheMetaclassNSObject = builder.getIMPCache(indexDylibA, "NSObject", true);

    // Assert:
    XCTAssertTrue(cacheClassNSObject.has_value());
    XCTAssertTrue(cacheClassNSObject->buckets.size() == 2);
    XCTAssertTrue(cacheClassNSObject->buckets[0].className == "NSObject");
    XCTAssertTrue(cacheClassNSObject->buckets[0].installName == "a.dylib");
    XCTAssertTrue(cacheClassNSObject->buckets[0].methodName == "a");
    XCTAssertTrue(cacheClassNSObject->buckets[0].isInstanceMethod);
    XCTAssertTrue(cacheClassNSObject->buckets[1].className == "NSObject");
    XCTAssertTrue(cacheClassNSObject->buckets[1].installName == "a.dylib");
    XCTAssertTrue(cacheClassNSObject->buckets[1].methodName == "b");
    XCTAssertTrue(cacheClassNSObject->buckets[1].isInstanceMethod);

    XCTAssertTrue(cacheMetaclassNSObject.has_value());
    XCTAssertTrue(cacheMetaclassNSObject->buckets.size() == 2);
    XCTAssertTrue(cacheMetaclassNSObject->buckets[0].className == "NSObject");
    XCTAssertTrue(cacheMetaclassNSObject->buckets[0].installName == "a.dylib");
    XCTAssertTrue(cacheMetaclassNSObject->buckets[0].methodName == "B");
    XCTAssertFalse(cacheMetaclassNSObject->buckets[0].isInstanceMethod);
    XCTAssertTrue(cacheMetaclassNSObject->buckets[1].className == "NSObject");
    XCTAssertTrue(cacheMetaclassNSObject->buckets[1].installName == "a.dylib");
    XCTAssertTrue(cacheMetaclassNSObject->buckets[1].methodName == "A");
    XCTAssertFalse(cacheMetaclassNSObject->buckets[1].isInstanceMethod);
}

@end
