
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

#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/utils.h>
#include <mach-o/utils_priv.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <vector>
#include <string>

#include <XCTest/XCTest.h>

#include "MockO.h"

using dyld3::GradedArchs;
using dyld3::Platform;

int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchs& launchArchs, const GradedArchs& dylibArchs, bool isOSBinary,
                                    void (^ _Nullable bestSlice)(const struct mach_header* slice, uint64_t sliceOffset, size_t sliceSize));


@interface APIUtilsTests : XCTestCase
@end

@implementation APIUtilsTests

// To support running unit tests on only OS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

- (void)testmacho_cpu_type_for_arch_name
{
    // Arrange: nothing to set up

    // Act: call macho_cpu_type_for_arch_name() with many variants
    struct Results { bool result; cpu_type_t cputype; cpu_type_t cpusubtype; };
    Results result1;
    result1.result = macho_cpu_type_for_arch_name("arm64", &result1.cputype, &result1.cpusubtype);
    Results result2;
    result2.result = macho_cpu_type_for_arch_name("x86_64h", &result2.cputype, &result2.cpusubtype);
    Results result3;
    result3.result = macho_cpu_type_for_arch_name("arm64_32", &result3.cputype, &result3.cpusubtype);
    Results result4;
    result4.result = macho_cpu_type_for_arch_name("unknown", &result4.cputype, &result4.cpusubtype);

    // Assert:
    XCTAssertTrue(result1.result);
    XCTAssert(result1.cputype    == CPU_TYPE_ARM64);
    XCTAssert(result1.cpusubtype == CPU_SUBTYPE_ARM64_ALL);
    XCTAssertTrue(result2.result);
    XCTAssert(result2.cputype    == CPU_TYPE_X86_64);
    XCTAssert(result2.cpusubtype == CPU_SUBTYPE_X86_64_H);
    XCTAssertTrue(result3.result);
    XCTAssert(result3.cputype    == CPU_TYPE_ARM64_32);
    XCTAssert(result3.cpusubtype == CPU_SUBTYPE_ARM64_32_V8);
    XCTAssertFalse(result4.result);
}


- (void)testmacho_arch_name_for_cpu_type
{
    // Arrange: nothing to set up

    // Act: call macho_arch_name_for_cpu_type() with many variants
    const char* arch1 = macho_arch_name_for_cpu_type(CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64_ALL);
    const char* arch2 = macho_arch_name_for_cpu_type(CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_H);
    const char* arch3 = macho_arch_name_for_cpu_type(CPU_TYPE_ARM64_32, CPU_SUBTYPE_ARM64_32_V8);
    const char* arch4 = macho_arch_name_for_cpu_type(CPU_TYPE_ARM64,    100);
    const char* arch5 = macho_arch_name_for_cpu_type(100,               0);

    // Assert:
    XCTAssert(strcmp(arch1, "arm64") == 0);
    XCTAssert(strcmp(arch2, "x86_64h") == 0);
    XCTAssert(strcmp(arch3, "arm64_32") == 0);
    XCTAssert(arch4 == nullptr);
    XCTAssert(arch5 == nullptr);
}




- (void)testmacho_arch_name_for_mach_header
{
    // Arrange: nothing to set up

    // Act: call macho_arch_name_for_mach_header() with many variants
    mach_header mh;
    mh.cputype = CPU_TYPE_ARM64; mh.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    const char* arch1 = macho_arch_name_for_mach_header(&mh);
    mh.cputype = CPU_TYPE_X86_64; mh.cpusubtype = CPU_SUBTYPE_X86_64_H;
    const char* arch2 = macho_arch_name_for_mach_header(&mh);
    mh.cputype = CPU_TYPE_ARM64; mh.cpusubtype = 100;
    const char* arch3 = macho_arch_name_for_mach_header(&mh);
    const char* arch4 = macho_arch_name_for_mach_header(NULL);

    // Assert:
    XCTAssert(strcmp(arch1, "arm64") == 0);
    XCTAssert(strcmp(arch2, "x86_64h") == 0);
    XCTAssert(arch3 == nullptr);
    XCTAssert(arch4 != nullptr); // is current arch
}



- (void)test_macho_for_each_slice
{
    // Arrange: make FAT file with two slices
    MockO mock1(MH_EXECUTE, "x86_64");
    MockO mock2(MH_EXECUTE, "arm64e");
    Muckle muckle;
    muckle.addMockO(&mock1);
    muckle.addMockO(&mock2);
    char savedToPath[PATH_MAX];
    muckle.save(savedToPath);

    // Act: call macho_for_each_slice() and record each arch
    const char* sliceArchArray[] = { nullptr, nullptr, nullptr, nullptr };
    __block const char** sliceArchs = sliceArchArray;
    __block int index = 0;
    int result = macho_for_each_slice(savedToPath, ^(const mach_header* slice, uint64_t sliceOffset, size_t size, bool* stop) {
        sliceArchs[index++] = macho_arch_name_for_mach_header(slice);
    });
    ::unlink(savedToPath);

    // Assert: arch names are as expected
    XCTAssert(result == 0);
    XCTAssert(index == 2);
    XCTAssert(strcmp(sliceArchs[0], "x86_64") == 0);
    XCTAssert(strcmp(sliceArchs[1], "arm64e") == 0);
}

- (void)test_macho_for_each_slice_badPath
{
    // Arrange: nothing

    // Act: call macho_for_each_slice() with bad path
    __block int index = 0;
    int result = macho_for_each_slice("/tmp/file-does-not-exist.dylib", ^(const mach_header* slice, uint64_t sliceOffset, size_t size, bool* stop) {
        index++;
    });

    // Assert: failed with ENOENT and block never called
    XCTAssert(result == ENOENT);
    XCTAssert(index == 0);
}

- (void)test_macho_for_each_slice_badPerms
{
    // Arrange: make file with perms set for it to be unreadable
    MockO mock1(MH_DYLIB, "arm64", Platform::macOS, "12.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);
    ::chmod(savedToPath, 0);

    // Act: call macho_for_each_slice() with bad path
    __block int index = 0;
    int result = macho_for_each_slice(savedToPath, ^(const mach_header* slice, uint64_t sliceOffset, size_t size, bool* stop) {
        index++;
    });
    ::chmod(savedToPath, 0777);
    ::unlink(savedToPath);

    // Assert: failed with EACCES and block never called
    XCTAssert(result == EACCES);
    XCTAssert(index == 0);
}

- (void)test_macho_for_each_slice_not_macho
{
    // Arrange: nothing

    // Act: call macho_for_each_slice() with path to a non-mach-o file
    __block int index = 0;
    int result = macho_for_each_slice("/System/Library/Frameworks/CoreFoundation.framework/Resources/Info.plist", ^(const mach_header* slice, uint64_t sliceOffset, size_t size, bool* stop) {
        index++;
    });

    // Assert: failed with EACCES and block never called
    XCTAssert(result == EFTYPE);
    XCTAssert(index == 0);
}

- (void)test_macho_for_each_slice_no_callback_not_macho
{
    // Arrange: nothing

    // Act: call macho_for_each_slice() with path to a non-mach-o file
    int result = macho_for_each_slice("/System/Library/Frameworks/CoreFoundation.framework/Resources/Info.plist", nullptr);

    // Assert: failed with EACCES and block never called
    XCTAssert(result == EFTYPE);
}

- (void)test_macho_for_each_slice_no_callback
{
    // Arrange: make FAT file with two slices
    MockO mock1(MH_EXECUTE, "x86_64");
    MockO mock2(MH_EXECUTE, "arm64e");
    Muckle muckle;
    muckle.addMockO(&mock1);
    muckle.addMockO(&mock2);
    char savedToPath[PATH_MAX];
    muckle.save(savedToPath);

    // Act: call macho_for_each_slice() and record each arch
    int result = macho_for_each_slice(savedToPath, nullptr);
    ::unlink(savedToPath);

    // Assert: arch names are as expected
    XCTAssert(result == 0);
}

- (void)test_macho_for_each_slice_fd
{
    // Arrange: make FAT file with three slices
    MockO mock1(MH_DYLIB, "x86_64");
    MockO mock2(MH_DYLIB, "arm64e");
    MockO mock3(MH_DYLIB, "arm64_32");
    Muckle muckle;
    muckle.addMockO(&mock1);
    muckle.addMockO(&mock2);
    muckle.addMockO(&mock3);
    char savedToPath[PATH_MAX];
    muckle.save(savedToPath);

    // Act: call macho_for_each_slice() and record each arch
    const char* sliceArchArray[] = { nullptr, nullptr, nullptr, nullptr };
    __block const char** sliceArchs = sliceArchArray;
    __block int index = 0;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_for_each_slice_in_fd(fd, ^(const mach_header* slice, uint64_t sliceOffset, size_t size, bool* stop) {
        sliceArchs[index++] = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: arch names are as expected
    XCTAssert(result == 0);
    XCTAssert(index == 3);
    XCTAssert(strcmp(sliceArchs[0], "x86_64") == 0);
    XCTAssert(strcmp(sliceArchs[1], "arm64e") == 0);
    XCTAssert(strcmp(sliceArchs[2], "arm64_32") == 0);
}


// test AS macOS picks arm64e over arm64 when keys are off
- (void)test_macho_best_slice_fd_fat_arm64_keys_off
{
    // Arrange: make FAT macOS file with arm64/arm64e slices
    MockO mock1(MH_DYLIB, "arm64",  Platform::macOS, "12.0");
    MockO mock2(MH_DYLIB, "arm64e", Platform::macOS, "12.0");
    Muckle muckle;
    muckle.addMockO(&mock1);
    muckle.addMockO(&mock2);
    char savedToPath[PATH_MAX];
    muckle.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::arm64e_keysoff, GradedArchs::arm64e_keysoff, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: arm64e slice found
    XCTAssert(result == 0);
    XCTAssert(strcmp(sliceArchName, "arm64e") == 0);
}

// test AS macOS picks arm64e over arm64 when keys are on
- (void)test_macho_best_slice_fd_fat_arm64_keys_on
{
    // Arrange: make FAT macOS file with arm64/arm64e slices
    MockO mock1(MH_DYLIB, "arm64", Platform::macOS, "12.0");
    MockO mock2(MH_DYLIB, "arm64e", Platform::macOS, "12.0");
    Muckle muckle;
    muckle.addMockO(&mock1);
    muckle.addMockO(&mock2);
    char savedToPath[PATH_MAX];
    muckle.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::arm64e, GradedArchs::arm64e, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: arm64e slice found
    XCTAssert(result == 0);
    XCTAssert(strcmp(sliceArchName, "arm64e") == 0);
}

// test AS macOS allows arm64e program to load arm64 dylib when keys are off
- (void)test_macho_best_slice_fd_thin_arm64_keys_off
{
    // Arrange: make macOS arm64 executable
    MockO mock1(MH_DYLIB, "arm64", Platform::macOS, "12.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::arm64e_keysoff, GradedArchs::arm64e_keysoff, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: arm64 slice found
    XCTAssert(result == 0);
    XCTAssert(strcmp(sliceArchName, "arm64") == 0);
}

// test AS macOS does not allow arm64e program to load arm64 dylib when keys are on
- (void)test_macho_best_slice_fd_thin_arm64_keys_on
{
    // Arrange: make macOS arm64 executable
    MockO mock1(MH_DYLIB, "arm64", Platform::macOS, "12.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::arm64e, GradedArchs::arm64e, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: no slice found
    XCTAssert(result == EBADARCH);
    XCTAssert(sliceArchName == nullptr);
}

// test macoOS will launch a catalyst program
- (void)test_macho_best_slice_macos_catalyst
{
    // Arrange: make catalyst main
    MockO mock1(MH_EXECUTE, "x86_64", Platform::iOSMac, "15.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::launch_Intel, GradedArchs::x86_64, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: x86_64 slice found
    XCTAssert(result == 0);
    XCTAssert(strcmp(sliceArchName, "x86_64") == 0);
}

// test macoOS will launch an almond program
- (void)test_macho_best_slice_macos_almond
{
    // Arrange: make catalyst main
    MockO mock1(MH_EXECUTE, "arm64", Platform::iOS, "15.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::launch_AS, GradedArchs::arm64, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: x86_64 slice found
    XCTAssert(result == 0);
    XCTAssert(strcmp(sliceArchName, "arm64") == 0);
}

// test macoOS will launch not launch an apple TV app
- (void)test_macho_best_slice_macos_tv
{
    // Arrange: make catalyst main
    MockO mock1(MH_EXECUTE, "arm64", Platform::tvOS, "15.0");
    char savedToPath[PATH_MAX];
    mock1.save(savedToPath);

    // Act: call macho_best_slice_fd_internal() and record best slice
    __block const char* sliceArchName = nullptr;
    int fd = ::open(savedToPath, O_RDONLY, 0);
    int result = macho_best_slice_fd_internal(fd, Platform::macOS, GradedArchs::launch_AS, GradedArchs::arm64, false, ^(const mach_header* slice, uint64_t sliceOffset, size_t size) {
        sliceArchName = macho_arch_name_for_mach_header(slice);
    });
    ::close(fd);
    ::unlink(savedToPath);

    // Assert: no slice found
    XCTAssert(result == EBADARCH);
    XCTAssert(sliceArchName == nullptr);
}

// test getting install name
- (void)test_macho_dylib_install_name
{
    // Arrange: make dylib and bundle
    MockO mock1(MH_DYLIB, "arm64", Platform::tvOS, "15.0");
    mock1.customizeInstallName("/usr/foo/bar");
    MockO mock2(MH_BUNDLE, "arm64", Platform::tvOS, "15.0");
    uint32_t badData[] = { 0x12, 0x34, 0x12, 0x78, 0x97};

    // Act: call macho_dylib_install_name()
    const char* path1 = macho_dylib_install_name(mock1.header());
    const char* path2 = macho_dylib_install_name(mock2.header());
    const char* path3 = macho_dylib_install_name((mach_header*)badData);

    // Assert: no slice found
    XCTAssert(strcmp(path1, "/usr/foo/bar") == 0);
    XCTAssert(path2 == nullptr);
    XCTAssert(path3 == nullptr);
}



@end

#pragma clang diagnostic pop

