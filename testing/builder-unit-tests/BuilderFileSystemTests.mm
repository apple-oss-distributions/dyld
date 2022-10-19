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

#import <XCTest/XCTest.h>

#include "BuilderFileSystem.h"
#include "BuilderOptions.h"

using namespace cache_builder;

@interface BuilderFileSystemTests : XCTestCase

@end

@implementation BuilderFileSystemTests
{
    Diagnostics diag;
}

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
    diag.clearError();
    self.continueAfterFailure = NO;
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

// Test we can add a valid symlink
- (void)testSymlinkResolverAddSymlink1
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo", "bar");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
}

// Test symlinks must start with /
- (void)testSymlinkResolverAddSymlink2
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "foo", "bar");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertTrue(hasError);
}

// Test symlinks must not be an existing file
- (void)testSymlinkResolverAddSymlink3
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo");
    resolver.addSymlink(diag, "/foo", "bar");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertTrue(hasError);
}

// Test duplicates must be identical
- (void)testSymlinkResolverAddSymlink4
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo", "/bar");
    resolver.addSymlink(diag, "/foo", "/bar");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
}

// Test duplicates must be identical
- (void)testSymlinkResolverAddSymlink5
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo", "/bar");
    resolver.addSymlink(diag, "/foo", "/baz");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertTrue(hasError);
}

// Test we can add a valid file
- (void)testSymlinkResolverAddFile1
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
}

// Test files must start with /
- (void)testSymlinkResolverAddFile2
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "foo");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertTrue(hasError);
}

// Test files must not be an existing symlink
- (void)testSymlinkResolverAddFile3
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo", "bar");
    resolver.addFile(diag, "/foo");

    // Act:
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertTrue(hasError);
}

// Test that we can resolve various symlinks
- (void)testSymlinkResolverGetResolvedSymlinks1
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/bar", "/foo");
    resolver.addFile(diag, "/foo");

    // Act:
    __block bool hasError = false;
    auto errorHandler = ^(const std::string& error) {
        hasError = true;
    };
    std::vector<FileAlias> resolvedSymlinks = resolver.getResolvedSymlinks(errorHandler);

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(resolvedSymlinks.size() == 1);
    XCTAssertTrue(resolvedSymlinks[0].aliasPath == "/bar");
    XCTAssertTrue(resolvedSymlinks[0].realPath == "/foo");
}

// Test that we error on loops in symlinks
- (void)testSymlinkResolverGetResolvedSymlinks2
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo/bar/baz", "./baz");

    // Act:
    __block bool hasError = false;
    auto errorHandler = ^(const std::string& error) {
        hasError = true;
    };
    std::vector<FileAlias> resolvedSymlinks = resolver.getResolvedSymlinks(errorHandler);

    // Assert:
    XCTAssertTrue(hasError);
    XCTAssertTrue(resolvedSymlinks.empty());
}

// Test that we return any valid symlinks other than those with loops
- (void)testSymlinkResolverGetResolvedSymlinks3
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/foo/bar/baz", "./baz");
    resolver.addSymlink(diag, "/foo2", "/bar");
    resolver.addSymlink(diag, "/bar", "/baz");
    resolver.addFile(diag, "/bar");
    resolver.addFile(diag, "/baz");

    // Act:
    __block bool hasError = false;
    auto errorHandler = ^(const std::string& error) {
        hasError = true;
    };
    std::vector<FileAlias> resolvedSymlinks = resolver.getResolvedSymlinks(errorHandler);
    std::map<std::string, std::string> aliasToRealPathMap;
    for ( const FileAlias& fileAlias : resolvedSymlinks )
        aliasToRealPathMap[fileAlias.aliasPath] = fileAlias.realPath;

    // Assert:
    XCTAssertTrue(hasError);
    XCTAssertTrue(resolvedSymlinks.size() == 2);
    XCTAssertTrue(aliasToRealPathMap["/foo2"] == "/baz");
    XCTAssertTrue(aliasToRealPathMap["/bar"] == "/baz");
}

// Test that we don't return a resolved symlink unless it points to a file
- (void)testSymlinkResolverGetResolvedSymlinks4
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addSymlink(diag, "/bar", "/foo2");
    resolver.addFile(diag, "/foo");

    // Act:
    __block bool hasError = false;
    auto errorHandler = ^(const std::string& error) {
        hasError = true;
    };
    std::vector<FileAlias> resolvedSymlinks = resolver.getResolvedSymlinks(errorHandler);

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(resolvedSymlinks.empty());
}

// Test calling realpath with //
- (void)testSymlinkResolverRealPath
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo");

    // Act:
    std::string realPath = resolver.realPath(diag, "//foo");
    std::string realPath2 = resolver.realPath(diag, "/foo//");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo");
    XCTAssertTrue(realPath2 == "/foo");
}

// Test calling realpath with a relative path
- (void)testSymlinkResolverRealPath2
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;

    // Act:
    std::string realPath = resolver.realPath(diag, "foo");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "foo");
}

// Test calling ../ on the root path
- (void)testSymlinkResolverRealPath4
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo");
    resolver.addSymlink(diag, "/bar", "../../../foo");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo");
}

// Test calling ../ on a non-root path
- (void)testSymlinkResolverRealPath5
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar/baz");
    resolver.addSymlink(diag, "/bar", "/foo/bar2/../bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling /. on a non-root path
- (void)testSymlinkResolverRealPath6
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar/baz");
    resolver.addSymlink(diag, "/bar", "/foo/./bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling /. on the end of a path
- (void)testSymlinkResolverRealPath7
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar/baz");
    resolver.addSymlink(diag, "/bar", "/foo/bar/baz/.");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling /. on the start of a path
- (void)testSymlinkResolverRealPath8
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar/baz");
    resolver.addSymlink(diag, "/bar", "/./foo/bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling // on the start of a path
- (void)testSymlinkResolverRealPath9
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "//foo/bar/baz");
    resolver.addSymlink(diag, "/bar", "/foo/bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling // on the end of a path
- (void)testSymlinkResolverRealPath10
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar/baz//");
    resolver.addSymlink(diag, "/bar", "/foo/bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test calling // in the middle of a path
- (void)testSymlinkResolverRealPath11
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/foo/bar//baz");
    resolver.addSymlink(diag, "/bar", "/foo/bar/baz");

    // Act:
    std::string realPath = resolver.realPath(diag, "/bar");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/foo/bar/baz");
}

// Test resolving a typical macOS path
- (void)testSymlinkResolverRealPath12
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation");
    resolver.addSymlink(diag, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", "Versions/Current/CoreFoundation");
    resolver.addSymlink(diag, "/System/Library/Frameworks/CoreFoundation.framework/Versions/Current", "A");

    // Act:
    std::string realPath = resolver.realPath(diag, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation");
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation");
}

// Test resolving a typical macOS path, and make sure we can resolve the "Current" symlink
// in the middle of the chain
- (void)testSymlinkResolverRealPath13
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation");
    resolver.addSymlink(diag, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", "Versions/Current/CoreFoundation");
    resolver.addSymlink(diag, "/System/Library/Frameworks/CoreFoundation.framework/Versions/Current", "A");

    // Act:
    __block std::vector<std::string> seenSymlinks;
    auto callback = ^(const std::string& intermediateSymlink) {
        seenSymlinks.push_back(intermediateSymlink);
    };
    std::string realPath = resolver.realPath(diag, "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
                                             callback);
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation");
    XCTAssertTrue(seenSymlinks.size() == 1);
    XCTAssertTrue(seenSymlinks[0] == "/System/Library/Frameworks/CoreFoundation.framework/Versions/Current/CoreFoundation");
}

// Cryptexes will insolve symlinks to dylibs, where the dylib path doesn't match its install name.  Ensure we can find
// all symlinks we want.
- (void)testSymlinkResolverRealPath14
{
    // Arrange: mock up SymlinkResolver
    SymlinkResolver resolver;
    resolver.addFile(diag, "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/A/Foo");
    resolver.addSymlink(diag, "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Foo", "Versions/Current/Foo");
    resolver.addSymlink(diag, "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/Current", "A");
    resolver.addSymlink(diag, "/System/Library/Frameworks/Foo.framework", "../../../System/Cryptexes/OS/System/Library/Frameworks/Foo.framework");

    // Act:
    __block std::vector<std::string> seenSymlinks;
    auto callback = ^(const std::string& intermediateSymlink) {
        seenSymlinks.push_back(intermediateSymlink);
    };
    std::string realPath = resolver.realPath(diag, "/System/Library/Frameworks/Foo.framework/Foo",
                                             callback);
    std::vector<cache_builder::FileAlias> aliases = resolver.getIntermediateSymlinks();
    bool hasError = diag.hasError();

    // Assert:
    XCTAssertFalse(hasError);
    XCTAssertTrue(realPath == "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/A/Foo");
    XCTAssertTrue(seenSymlinks.size() == 3);
    XCTAssertTrue(seenSymlinks[0] == "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Foo");
    XCTAssertTrue(seenSymlinks[1] == "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/Current/Foo");
    XCTAssertTrue(seenSymlinks[2] == "/System/Library/Frameworks/Foo.framework/Versions/A/Foo");
    XCTAssertTrue(aliases.size() == 1);
    XCTAssertTrue(aliases[0].realPath == "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/A/Foo");
    XCTAssertTrue(aliases[0].aliasPath == "/System/Cryptexes/OS/System/Library/Frameworks/Foo.framework/Versions/Current/Foo");
}

@end
