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

#include "MetadataVisitor.h"

using namespace metadata_visitor;

@interface MetadataVisitorTests : XCTestCase

@end

@implementation MetadataVisitorTests
{
    dyld3::MachOFile MachO_x86_64;
    dyld3::MachOFile MachO_arm64;
    dyld3::MachOFile MachO_arm64e;
    dyld3::MachOFile MachO_arm64_32;

    // Parameters Visitor
    CacheVMAddress              cacheBaseAddress;
    std::vector<Segment>        segments;
    std::optional<VMAddress>    selectorStringsBaseAddress;
    std::vector<uint64_t>       bindTargets;
}

static void addSegment(std::vector<Segment>& segments,
                       uint64_t startVMAddr, uint64_t endVMAddr)
{
    Segment segment;
    segment.startVMAddr                     = VMAddress(startVMAddr);
    segment.endVMAddr                       = VMAddress(endVMAddr);
    segment.bufferStart                     = (uint8_t*)malloc(endVMAddr - startVMAddr);
    segment.segIndex                        = (uint32_t)segments.size();
    segment.onDiskDylibChainedPointerFormat = std::nullopt;

    segments.push_back(std::move(segment));
}

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
    MachO_x86_64.magic      = MH_MAGIC_64;
    MachO_x86_64.cputype    = CPU_TYPE_X86_64;
    MachO_x86_64.cpusubtype = CPU_SUBTYPE_X86_ALL;
    MachO_x86_64.filetype   = MH_EXECUTE;
    MachO_x86_64.ncmds      = 0;
    MachO_x86_64.sizeofcmds = 0;
    MachO_x86_64.flags      = 0;

    MachO_arm64.magic      = MH_MAGIC_64;
    MachO_arm64.cputype    = CPU_TYPE_ARM64;
    MachO_arm64.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    MachO_arm64.filetype   = MH_EXECUTE;
    MachO_arm64.ncmds      = 0;
    MachO_arm64.sizeofcmds = 0;
    MachO_arm64.flags      = 0;

    MachO_arm64e.magic      = MH_MAGIC_64;
    MachO_arm64e.cputype    = CPU_TYPE_ARM64;
    MachO_arm64e.cpusubtype = CPU_SUBTYPE_ARM64E;
    MachO_arm64e.filetype   = MH_EXECUTE;
    MachO_arm64e.ncmds      = 0;
    MachO_arm64e.sizeofcmds = 0;
    MachO_arm64e.flags      = 0;

    MachO_arm64_32.magic      = MH_MAGIC_64;
    MachO_arm64_32.cputype    = CPU_TYPE_ARM64_32;
    MachO_arm64_32.cpusubtype = CPU_SUBTYPE_ARM64_V8;
    MachO_arm64_32.filetype   = MH_EXECUTE;
    MachO_arm64_32.ncmds      = 0;
    MachO_arm64_32.sizeofcmds = 0;
    MachO_arm64_32.flags      = 0;
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
    for ( Segment& segment : segments )
        free(segment.bufferStart);

    cacheBaseAddress            = CacheVMAddress();
    selectorStringsBaseAddress  = std::nullopt;
    segments.clear();
    bindTargets.clear();
}

- (void)testConstructor32 {
    // Arrange: mock up visitor
    Visitor visitor(cacheBaseAddress, &MachO_arm64_32, std::move(segments),
                    selectorStringsBaseAddress, std::move(bindTargets));

    // Act:
    // n/a

    // Assert:
    // n/a
}

- (void)testConstructor64 {
    // Arrange: mock up visitor
    Visitor visitor(cacheBaseAddress, &MachO_x86_64, std::move(segments),
                    selectorStringsBaseAddress, std::move(bindTargets));

    // Act:
    // n/a

    // Assert:
    // n/a
}

// resolveVMAddress walks the segments to find the segment for a given VMAddress
- (void)testResolveVMAddress {
    // Arrange: mock up visitor
    addSegment(segments, 0, 0x1000);
    addSegment(segments, 0x1000, 0x2000);
    Visitor visitor(cacheBaseAddress, &MachO_x86_64, std::move(segments),
                    selectorStringsBaseAddress, std::move(bindTargets));

    // Act:
    VMAddress inputVMAddr1(0x100ULL);
    ResolvedValue value1 = visitor.getValueFor(inputVMAddr1);
    VMAddress resultVMAddr1 = value1.vmAddress();

    VMAddress inputVMAddr2(0x000ULL);
    ResolvedValue value2 = visitor.getValueFor(inputVMAddr2);
    VMAddress resultVMAddr2 = value2.vmAddress();

    VMAddress inputVMAddr3(0x1000ULL);
    ResolvedValue value3 = visitor.getValueFor(inputVMAddr3);
    VMAddress resultVMAddr3 = value3.vmAddress();

    VMAddress inputVMAddr4(0x1FFFULL);
    ResolvedValue value4 = visitor.getValueFor(inputVMAddr4);
    VMAddress resultVMAddr4 = value4.vmAddress();

    // Assert:
    XCTAssertTrue(resultVMAddr1 == inputVMAddr1);
    XCTAssertTrue(resultVMAddr2 == inputVMAddr2);
    XCTAssertTrue(resultVMAddr3 == inputVMAddr3);
    XCTAssertTrue(resultVMAddr4 == inputVMAddr4);
}


@end
