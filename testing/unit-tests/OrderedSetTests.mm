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

#include <map>
#include <set>
#include <string>
#include <cstdint>

#import "DyldTestCase.h"

#include "Allocator.h"
#include "OrderedSet.h"
#include "OrderedMap.h"

using namespace lsl;

@interface OrderedSetTests : DyldTestCase {}

@end

@implementation OrderedSetTests

- (void) testOrderedSetCreation {
    auto allocator = EphemeralAllocator();
    OrderedSet<uint64_t> set(allocator);
    for (uint32_t i = 0;  i <= 3000; ++i) {
        set.insert([self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()]);
    }
    auto set2 = OrderedSet(set, allocator);
    XCTAssertEqual(set.size(), set2.size());
    for(auto i = set.begin(), j = set2.begin(); i != set.end();  ++i, ++j) {
        XCTAssertEqual(*i, *j);
    }
    auto set3 = std::move(set2);
    XCTAssertEqual(set2.size(), 0);
    XCTAssertEqual(set.size(), set3.size());
    for(auto i = set.begin(), j = set3.begin(); i != set.end();  ++i, ++j) {
        XCTAssertEqual(*i, *j);
    }

    set2 = OrderedSet<uint64_t>(allocator);
    XCTAssertEqual(set2.size(), 0);
    auto k = set2.end();
    for (auto i :  set) {
        k = set2.insert(set2.end(), i);
        XCTAssertNotEqual(set2.end(), k);
    }
    set2.clear();
    XCTAssertEqual(set2.size(), 0);
    auto i = set.end();
    auto j = set3.end();
    k = set2.end();
    while (i != set.begin()) {
        XCTAssertEqual(*(--i), *(--j));
        k = set2.insert(k, *i);
        XCTAssertNotEqual(set2.end(), k);
    }
}

- (void) testOrderedSetHintedInsertion {
    auto allocator = EphemeralAllocator();
    OrderedSet<uint64_t> set(allocator);
    auto  result    = set.insert(set.end(), 100);   XCTAssertEqual(*result, 100);
    result          = set.insert(set.end(), 200);   XCTAssertEqual(*result, 200);
    result          = set.insert(result,    150);   XCTAssertEqual(*result, 150);
    result          = set.insert(result,    125);   XCTAssertEqual(*result, 125);
    result          = set.insert(result,    125);   XCTAssertEqual(*result, 125);
    result          = set.insert(set.end(), 115);   XCTAssertEqual(*result, 115);
}

- (void) testOrderedInsertionCStr {
    auto allocator = EphemeralAllocator();
    OrderedSet<const char*, ConstCharStarCompare> set(allocator);
    auto  result    = set.insert(std::string("ABC").c_str());   XCTAssertTrue(result.second);
    result          = set.insert(std::string("DEF").c_str());   XCTAssertTrue(result.second);
    result          = set.insert(std::string("ABC").c_str());   XCTAssertFalse(result.second);
    result          = set.insert(std::string("ABCDE").c_str()); XCTAssertTrue(result.second);
    result          = set.insert(std::string("G").c_str());   XCTAssertTrue(result.second);
    XCTAssertEqual(set.size(), 4);
}

- (void) testOreredMultiSet {
    auto allocator = EphemeralAllocator();
    OrderedMultiSet<uint64_t> set(allocator);
    auto i  = set.insert(set.end(), 100);   XCTAssertEqual(*i, 100);
    i       = set.insert(set.end(), 200);   XCTAssertEqual(*i, 200);
    i       = set.insert(i,         150);   XCTAssertEqual(*i, 150);
    i       = set.insert(i,         125);   XCTAssertEqual(*i, 125);
    i       = set.insert(i,         125);   XCTAssertEqual(*i, 125);
    i       = set.insert(set.end(), 115);   XCTAssertEqual(*i, 115);
    XCTAssertEqual(set.size(), 6);
    XCTAssertEqual(set.count(125), 2);
    set.erase(125);
    XCTAssertEqual(set.size(), 4);
    XCTAssertEqual(set.count(125), 0);
};

- (void) testOrderedMultiSetMutations {
    self.randomSeed = 8097896182481744822ULL;
    auto allocator = EphemeralAllocator();
    std::multiset<uint64_t> oldSet;
    OrderedMultiSet<uint64_t> newSet(allocator);
    // Do stuff
    for (uint32_t i = 0;  i <= 300000; ++i) {
        uint64_t value = [self uniformRandomFrom:0 to:10000];
        if ([self randomBool]) {
            XCTAssertEqual(*newSet.insert(value), value);
            XCTAssertEqual(*oldSet.insert(value), value);
        } else {
            XCTAssertEqual(newSet.erase(value), oldSet.erase(value), "Erase success state should be equal");
        }
    }
    auto w = oldSet.begin();
    for (auto j : newSet) {
        XCTAssert(*(w++) == j);
    }
    XCTAssertEqual(w, oldSet.end());
    OrderedSet<uint64_t> removals(newSet.begin(), newSet.end(), allocator);

    for (auto i : removals) {
        XCTAssertEqual(newSet.erase(i), oldSet.erase(i));
    }
    XCTAssertTrue(newSet.empty());
}

- (void) testOrderedSetMutations {
    auto allocator = EphemeralAllocator();
    std::set<uint64_t> oldSet;
    OrderedSet<uint64_t> newSet(allocator);
    // Do stuff
    for (uint32_t i = 0;  i <= 300000; ++i) {
        uint64_t value = [self uniformRandomFrom:0 to:10000];
        if ([self randomBool]) {
            auto w = newSet.insert(value);
            auto x = oldSet.insert(value);
            XCTAssertEqual(w.second, x.second, "Insertion success state should be equal");
            if (w.first == newSet.end()) {
                XCTAssertEqual(x.first, oldSet.end());
            } else {
                XCTAssertEqual(*w.first, *x.first, "Insertion success state should be equal");
            }
        } else {
            XCTAssertEqual(newSet.erase(value), oldSet.erase(value), "Erase success state should be equal");
        }
    }
    auto w = oldSet.begin();
    for (auto j : newSet) {
        XCTAssert(*(w++) == j);
    }
    XCTAssert(w == oldSet.end());
    Vector<uint64_t> removals(allocator);
    removals.reserve(newSet.size());
    std::copy(newSet.begin(), newSet.end(), std::back_inserter(removals));
    for (auto i = 0; i < 5*newSet.size(); ++i) {
        auto target1 = [self uniformRandomFrom:0 to:newSet.size()-1];
        auto target2 = [self uniformRandomFrom:0 to:newSet.size()-1];
        std::swap(removals[target1], removals[target2]);
    }
    for (auto i : removals) {
        auto j = newSet.find(i);
        auto k = std::next(j);
        bool y = (k != newSet.end());
        uint64_t x = 0;
        if (y) { x = *k; }
        XCTAssertNotEqual(j, newSet.end());
        auto l = newSet.erase(j);
        if (y) {
            XCTAssertEqual(x,*l);
        }
    }
    XCTAssertTrue(newSet.empty());
}

- (void) testOrderedSetStress {
    dispatch_apply([self uniformRandomFrom:10 to:40], DISPATCH_APPLY_AUTO, ^(size_t n) {
        [self testOrderedSetMutations];
    });
}

@end
