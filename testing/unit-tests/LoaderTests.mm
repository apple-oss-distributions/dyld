/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#include <stdio.h>

#include <vector>

#include "DyldTestCase.h"

#include "Loader.h"

using dyld4::Loader;

@interface LoaderTests : DyldTestCase
@end

@implementation LoaderTests

// test having no trailing '/' after @executable_path
- (void)test_expandAndNormalizeAtExecutablePath1
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C", "@executable_path", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A/B/") == 0);
}

// test @executable_path with trailing path
- (void)test_expandAndNormalizeAtExecutablePath2
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C", "@executable_path/", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A/B/") == 0);
}

- (void)test_expandAndNormalizeAtExecutablePath3
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C", "@executable_path/foo", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A/B/foo") == 0);
}


// test @executable_path with trailing path
- (void)test_expandAndNormalizeAtExecutablePath4
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C", "@executable_path/../foo", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A/foo") == 0);
}

// test @executable_path with trailing path
- (void)test_expandAndNormalizeAtExecutablePath5
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C/D", "@executable_path/../../foo", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A/foo") == 0);
}

// test @executable_path with trailing path
- (void)test_expandAndNormalizeAtExecutablePath6
{
    // Arrange: set up RuntimeState
    char fixedPath[PATH_MAX];

    // Act:
    bool result = Loader::expandAndNormalizeAtExecutablePath("/A/B/C/D/E", "@executable_path/../../..", fixedPath);

    // Assert:
    XCTAssertTrue(result);
    XCTAssert(strcmp(fixedPath, "/A") == 0);
}

@end
