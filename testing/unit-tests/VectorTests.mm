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

#import "DyldTestCase.h"

#include <memory>
#include <vector>

#include "Vector.h"

using namespace lsl;

@interface VectorTests : DyldTestCase {
    std::vector<uint64_t>   _testVector;
    bool                    _initialized;
}

@end

@implementation VectorTests

- (void)setUp {
    if (_initialized) { return; }
    _initialized = true;
    uint64_t count = [self uniformRandomFrom:0 to:10000];
    for (auto i = 0; i < count; ++i) {
        _testVector.push_back([self uniformRandomFrom:0 to:10000]);
    }
}

- (void)checkVector:(const Vector<uint64_t>&)vec {
    XCTAssert(vec.size() == _testVector.size());
    std::span<uint64_t> integers = _testVector;
    for (auto i = 0; i < vec.size(); ++i) {
        XCTAssert(integers[i] == vec[i]);
    }
}

- (void)testPushBack {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.push_back(i);
    }
    [self checkVector:ints];
}

- (void)testEmplaceBack {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.emplace_back(i);
    }
    [self checkVector:ints];
}

- (void)testInsert {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    [self checkVector:ints];
}

- (void)testCopyConstructor {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    Vector<uint64_t> ints2(ints, allocator);
    [self checkVector:ints2];
}

- (void)testMoveConstructor {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    Vector<uint64_t> ints2(ints, allocator);
    [self checkVector:ints2];
}


- (void)testVectorInsertRValue {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator), ints2(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    for (auto& i : ints) {
        ints2.insert(ints2.cend(), std::move(i));
    }
    // The values in ints
    XCTAssertTrue(ints.size() == ints2.size());
    [self checkVector:ints2];
}

- (void)testCopyAssignment {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator), ints2(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    ints2 = ints;
    [self checkVector:ints2];
}

- (void) testVectorInsertRange {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator), ints2(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    ints2.insert(ints2.begin(), ints.begin(), ints.end());
    [self checkVector:ints2];
}

- (void) testVectorConstructRange {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    Vector<uint64_t> ints2(ints.begin(), ints.end(), allocator);
    [self checkVector:ints2];
}

- (void) testVectorPushBackRValue {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator), ints2(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    for (auto& i : ints) {
        ints2.push_back(std::move(i));
    }
    [self checkVector:ints2];
}

- (void)testVectorMisc {
    auto allocator = EphemeralAllocator();
    Vector<uint64_t> ints(allocator), ints2(allocator);
    for (const auto& i : _testVector) {
        ints.insert(ints.end(), i);
    }
    ints2 = ints;
    ints.erase(ints.begin(), ints.end());
    XCTAssert(ints.size() == 0);
    ints = ints2;
    ints.erase(ints.cbegin(), ints.cend());
    XCTAssert(ints.size() == 0);
    ints = ints2;
    while(ints.size()) {
        ints.erase(ints.begin());
    }
    XCTAssert(ints.size() == 0);
    ints = ints2;
    while(ints.size()) {
        ints.erase(ints.cbegin());
    }
    XCTAssert(ints.size() == 0);
    ints = ints2;
    while(ints.size()) {
        ints.erase(ints.end()-1);
    }
    XCTAssert(ints.size() == 0);
    ints = ints2;
    while(ints.size()) {
        ints.erase(ints.cend()-1);
    }
    XCTAssert(ints.size() == 0);
    ints = std::move(ints2);
    while(ints.size()) {
        auto pos = ints.begin() + [self uniformRandomFrom:0 to:(ints.size()-1)];
        ints.erase(pos);
    }
}

// A struct with move semantics
struct MovedInteger
{
    MovedInteger() = default;
    MovedInteger(int v) : value(v) { }
    ~MovedInteger() = default;

    MovedInteger(const MovedInteger&) = delete;
    MovedInteger& operator=(const MovedInteger&) = delete;

    MovedInteger(MovedInteger&& other) {
        this->value = other.value;
        other.value = 0;
    }
    MovedInteger& operator=(MovedInteger&& other) {
        this->value = other.value;
        other.value = 0;
        return *this;
    }

    int value = 0;
};

-(void)testVectorEraseMovedElements {
    auto allocator = EphemeralAllocator();
    // Arrange: make a vector and remove an element
    Vector<MovedInteger> ints(allocator);
    ints.push_back(1);
    ints.push_back(2);
    ints.push_back(3);

    ints.erase(ints.begin());

    // Act: test the vector contents
    size_t size = ints.size();
    int e0 = ints[0].value;
    int e1 = ints[1].value;

    // Assert: vector is the correct size and the elements are as expected
    XCTAssertTrue(size == 2);
    XCTAssertTrue(e0 == 2);
    XCTAssertTrue(e1 == 3);
}

- (void) testStackAllocatedVectors {
    STACK_ALLOC_VECTOR(uint64_t, ints, 8);
    XCTAssertEqual(__ints_vector_allocator.vm_allocated_bytes(), 0, "No vm allocations should be necessary");
    for (auto i = 0; i < 8; ++i) {
        ints.push_back(i);
        XCTAssertEqual(__ints_vector_allocator.vm_allocated_bytes(), 0, "No vm allocations should be necessary");
    }
}

@end
