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

#include <cassert>

#import "DyldTestCase.h"
#include "Vector.h"
#include "Allocator.h"
#include "OrderedSet.h"
#include "AllocatorTestSequence.h"

using namespace lsl;

@interface AllocatorTests : DyldTestCase

@end

@implementation AllocatorTests

- (void) testEphemeralAllocator {
    auto allocator = EphemeralAllocator();
    auto testSequence = TestSequence([self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()]);
    testSequence.runMallocTests(self, &allocator, true);
    void* largeAllocation = allocator.malloc(2*1024*1024);
#if __LP64__
    void* veryLargeAllocation = allocator.malloc(2*1024*1024*1024ULL);
#else
    void* veryLargeAllocation = allocator.malloc(64*1024*1024ULL);
#endif
    allocator.free(largeAllocation);
    if (veryLargeAllocation) {
        allocator.free(veryLargeAllocation);
    }
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

- (void) testPersistentAllocator {
    auto& allocator = Allocator::persistentAllocator();
    auto testSequence = TestSequence([self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()]);
    testSequence.runMallocTests(self, &allocator, true);
    void* largeAllocation = allocator.malloc(2*1024*1024);
#if __LP64__
    void* veryLargeAllocation = allocator.malloc(2*1024*1024*1024ULL);
#else
    void* veryLargeAllocation = allocator.malloc(64*1024*1024ULL);
#endif
    allocator.free(largeAllocation);
    if (veryLargeAllocation) {
        allocator.free(veryLargeAllocation);
    }
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

- (void) testPersistentAllocatorSimple {
    auto& allocator = Allocator::persistentAllocator();
    for (auto i = 0; i < 1024; ++i) {
        allocator.free(allocator.malloc(i));
    }
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

//FIXME: We need mach exception handling for this test to make sense
- (void) disabledTestWriteProtect {
    auto& allocator = Allocator::persistentAllocator();
    auto buffer = (char *)allocator.malloc(128);
    memset(buffer, 0x1f, 128);
    allocator.writeProtect(true);
    for (auto i = 0; i < 128; ++i) {
        XCTAssert(buffer[i] == 0x1f);
    }

    // FIXME: Implement mach exception handling on this to test write protection and make sure we do not regress it
    //    memset(buffer.address, 0x2e, buffer.size);
    allocator.writeProtect(false);
    allocator.free(buffer);
    XCTAssert(allocator.allocated_bytes() == 0, "allocator.allocated_bytes() = %zu\n", allocator.allocated_bytes());
}

- (void) testEphemeralAllocatorPerformance {
    self.randomSeed = 8848042ULL;
    EphemeralAllocator allocator;
    auto testSequence = TestSequence([self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()]);
    __block auto allocatorPtr = &allocator;
    __block auto& testSequenceRef = testSequence;
    [self measureBlock:^{
        testSequenceRef.runMallocTests(self, allocatorPtr, false);
    }];
}

- (void) testPersistentAllocatorPerformance {
    self.randomSeed = 8848042ULL;
    auto& allocator = Allocator::persistentAllocator();
    auto testSequence = TestSequence([self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()]);
    __block auto allocatorPtr = &allocator;
    __block auto& testSequenceRef = testSequence;
     [self measureBlock:^{
        testSequenceRef.runMallocTests(self, allocatorPtr, false);
    }];
    allocator.destroy();
}

@end
