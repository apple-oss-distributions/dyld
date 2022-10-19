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

#import "Allocator.h"
#import "OrderedMap.h"

using namespace lsl;

@interface OrderedMapTests : DyldTestCase

@end

@implementation OrderedMapTests

- (void) testOrderedMap {
    auto allocator = EphemeralAllocator();
    OrderedMap<uint64_t,uint64_t> map(allocator);
    for (uint32_t i = 0;  i <= 300000; ++i) {
        uint64_t key = [self uniformRandomFrom:0 to:10000];
        uint64_t value = [self uniformRandomFrom:0 to:10000];
        auto j = map.insert({key,value});
        if (j.second) {
            XCTAssertEqual(map[key], value);
        } else {
            XCTAssertEqual(map[key], (j.first)->second);
        }
    }
};

- (void) testOrderedMultiMap {
    auto allocator = EphemeralAllocator();
    OrderedMultiMap<uint64_t,uint64_t> map(allocator);
    auto i  = map.insert(map.end(), {100,1});   XCTAssertEqual(i->first, 100); XCTAssertEqual(i->second, 1);
    i       = map.insert(map.end(), {200, 2});  XCTAssertEqual(i->first, 200); XCTAssertEqual(i->second, 2);
    i       = map.insert(i,         {150, 3});  XCTAssertEqual(i->first, 150); XCTAssertEqual(i->second, 3);
    i       = map.insert(i,         {125, 4});  XCTAssertEqual(i->first, 125); XCTAssertEqual(i->second, 4);
    i       = map.insert(i,         {125, 5});  XCTAssertEqual(i->first, 125); XCTAssertEqual(i->second, 5);
    i       = map.insert(map.end(), {115, 6});  XCTAssertEqual(i->first, 115); XCTAssertEqual(i->second, 6);
    XCTAssertEqual(map.size(), 6);
    XCTAssertEqual(map.count(125), 2);
    i = map.lower_bound(125);
    bool fourFound = false;
    bool fiveFound = false;
    for (i = map.lower_bound(125); i->first == 125; ++i) {
        if (i->second == 4) {
            fourFound = true;
        } else if (i->second == 5) {
            fiveFound = true;
        }
    }
    XCTAssertTrue(fourFound);
    XCTAssertTrue(fiveFound);
    map.erase(125);
    XCTAssertEqual(map.size(), 4);
    XCTAssertEqual(map.count(125), 0);
};

#if 0
- (void) testOrderedMultiMapMutations {
    auto allocator = EphemeralAllocator();
    std::multiset<uint64_t,uint64_t> oldMap;
    OrderedMultiMap<uint64_t,uint64_t> newMap(allocator);
    // Do stuff
    for (uint32_t i = 0;  i <= 300000; ++i) {
        uint64_t value1 = [self uniformRandomFrom:0 to:10000];
        uint64_t value2 = [self uniformRandomFrom:0 to:10000];
        auto value = std::make_pair(value1, value2);
        newMap.insert(value);
        oldMap.insert(value);
    }
    XCTAssertEqual(oldMap.size(), newMap.size());
    Vector<std::pair<uint64_t,uint64_t>> newResults(allocator);
    Vector<std::pair<uint64_t,uint64_t>> oldResults(allocator);
    newResults.reserve(newMap.size());
    oldResults.reserve(oldMap.size());
    std::copy(newMap.begin(), newMap.end(), std::back_inserter(newResults));
    std::copy(oldMap.begin(), oldMap.end(), std::back_inserter(oldResults));
    XCTAssertEqual(oldMap.size(), newMap.size());
    std::sort(newResults.begin(), newResults.end());
    std::sort(oldResults.begin(), oldResults.end());
    for (auto i = newResults.begin(), j = oldResults.begin(); i != newResults.end(); ++i, ++j) {
        XCTAssertEqual(*i, *j);
    }
    OrderedSet<uint64_t> removals(allocator);
    for (auto i : newMap) {
        removals.insert(i.first);
    }
    for (auto i : removals) {
        XCTAssertEqual(newMap.erase(i), oldMap.erase(i));
    }
    XCTAssertTrue(newMap.empty());
}
#endif

@end
