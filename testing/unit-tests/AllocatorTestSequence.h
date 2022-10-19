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

#ifndef AllocatorTestSequence_h
#define AllocatorTestSequence_h

#include "Vector.h"

namespace {

using namespace lsl;

struct AllocOperation {
    size_t size;
    size_t alignment;
    char pattern;
    AllocOperation(size_t S, size_t A, char P) : size(S), alignment(A), pattern(P) {}
};

struct TestOperation {
    size_t  liveAllocationIdx;
    size_t  testOpIdx;
    bool    isAllocation;
    TestOperation(size_t I, size_t TO, bool A) : liveAllocationIdx(I), testOpIdx(TO), isAllocation(A) {}
};

struct TestSequence {
    TestSequence() {}
    TestSequence(const TestSequence& other) {
        _testVector             = Vector<AllocOperation>(other._testVector, _ephemeralAllocator);
        _testOperations         = Vector<TestOperation>(other._testOperations, _ephemeralAllocator);
        _targetPoolSize         = other._targetPoolSize;
        _testOperationMaxSize   = other._testOperationMaxSize;
    }
    TestSequence& operator=(const TestSequence& other) {
        _testVector             = Vector<AllocOperation>(other._testVector, _ephemeralAllocator);
        _testOperations         = Vector<TestOperation>(other._testOperations, _ephemeralAllocator);
        _targetPoolSize         = other._targetPoolSize;
        _testOperationMaxSize   = other._testOperationMaxSize;
        return *this;
    }
    TestSequence(uint64_t seed) {
        std::mt19937_64 mt;
        mt.seed(seed);
        std::uniform_int_distribution<uint8_t>  biasedBool(0,2);
        std::uniform_int_distribution<uint8_t>  alignDist(4,7);
        std::uniform_int_distribution<uint8_t>  patternDist(0,255);
        std::uniform_int_distribution<size_t>   sizeDist(0, 128*1024);
        _testVector.reserve(1028);
        _testOperations.reserve(102400);
        for (auto i = 0; i < 1024; ++i) {
            size_t size = sizeDist(mt);
            char pattern = (char)patternDist(mt);
            auto alignment = 1 << alignDist(mt);
            _testVector.emplace_back(size, alignment, pattern);
        }
        _testVector.emplace_back(4096, PAGE_SIZE, (char)patternDist(mt));
        _testVector.emplace_back(16384, PAGE_SIZE, (char)patternDist(mt));
        _testVector.emplace_back(65536, PAGE_SIZE, (char)patternDist(mt));

        size_t spaceUsed = 0;
        Vector<bool>           liveIdxes(_ephemeralAllocator);
        Vector<TestOperation>  liveAllocations(_ephemeralAllocator);
        std::uniform_int_distribution<size_t>   allocationDist(0, _testVector.size()-1);
        liveIdxes.reserve(102400);
        liveAllocations.reserve(102400);

        bool growing = true;
        while(1) {
            // First we grow the pool, so bias in favor of allocations, then switch when it is time to shrink the pool
            bool isAllocation =  true;
            if (liveAllocations.size() != 0) {
                if (growing) {
                    isAllocation = (biasedBool(mt) > 0) ? true : false;
                } else {
                    isAllocation = (biasedBool(mt) > 0) ? false : true;
                }
            }
            if (isAllocation) {
                size_t allocationIndex = allocationDist(mt);
                spaceUsed += _testVector[allocationIndex].size;
                auto j = std::find(liveIdxes.begin(), liveIdxes.end(), false);
                if (j == liveIdxes.end()) {
                    j = liveIdxes.insert(liveIdxes.end(), true);
                } else {
                    *j = true;
                }
                auto op = TestOperation(j-liveIdxes.begin(), allocationIndex, true);
                _testOperations.push_back(op);
                liveAllocations.push_back(op);
            } else {
                size_t liveAllocationIndex = std::uniform_int_distribution<uint64_t>(0,liveAllocations.size()-1)(mt);
                auto op = *(liveAllocations.begin() + liveAllocationIndex);
                spaceUsed -= _testVector[op.testOpIdx].size;
                op.isAllocation = false;
                liveIdxes.begin()[op.liveAllocationIdx] = false;
                _testOperations.push_back(op);
                liveAllocations.erase(liveAllocations.begin() + liveAllocationIndex);
            }
            if (growing && (spaceUsed >= _targetPoolSize)) {
                growing = false;
            }
            if (!growing && (spaceUsed <= _targetPoolSize/2)) {
                break;
            }
        }
        _testOperationMaxSize = liveIdxes.size();
        // Drain the  pool
        while (liveAllocations.size()) {
            size_t liveAllocationIndex = std::uniform_int_distribution<uint8_t>(0,liveAllocations.size()-1)(mt);
            auto op = *(liveAllocations.begin() + liveAllocationIndex);
            op.isAllocation = false;
            liveIdxes.begin()[op.liveAllocationIdx] = false;
            _testOperations.push_back(op);
            liveAllocations.erase(liveAllocations.begin() + liveAllocationIndex);
        }
    }
    void runMallocTests(DyldTestCase* test, Allocator* allocator, bool verify) {
        auto liveAllocations = (std::pair<void*,AllocOperation*>*)_ephemeralAllocator.malloc(sizeof(std::pair<void*,uint8_t>) * _testOperationMaxSize);
        bzero((void*)liveAllocations, sizeof(std::pair<void*,AllocOperation*>) * _testOperationMaxSize);

        uint64_t count = 0;
        for (auto op : _testOperations) {
            if (op.isAllocation) {
                auto& testOp = _testVector[op.testOpIdx];
                auto buffer = allocator->aligned_alloc(testOp.alignment, testOp.size);
                liveAllocations[op.liveAllocationIdx].first = buffer;
//                fprintf(stderr, "%llu:\t(%zu) Allocated   %zu @ 0x%lx\n", count, allocator->allocated_bytes(), testOp.size, (uintptr_t)buffer);
                if (verify) {
                    memset(buffer, testOp.pattern, testOp.size);
                    liveAllocations[op.liveAllocationIdx].second = (AllocOperation*)&testOp;
                }
            } else {
                allocator->free(liveAllocations[op.liveAllocationIdx].first);
//                auto& testOp = (*_testVector)[op.testOpIdx];
//                fprintf(stderr, "%llu:\t(%zu) Deallocated %zu @ 0x%lx\n", count, allocator->allocated_bytes(), testOp.size, (uintptr_t)liveAllocations[op.liveAllocationIdx].first);
                liveAllocations[op.liveAllocationIdx] = { nullptr, (AllocOperation*)nullptr };
            }

            // We only check every 100th iteration in order to speed up test runs. If crashes happen change this to validate every
            // modification
            if ((++count%100 == 0) && verify) {
//                allocator->debugDump();
                allocator->validate();
                for (auto j = &liveAllocations[0]; j != &liveAllocations[_testOperationMaxSize]; ++j) {
                    if (j->first == nullptr) { continue; }
                    const uint8_t* charBuffer = (uint8_t*)j->first;
                    uint64_t patternBuffer = 0;
                    // Hacky manually optimization, the XCTAssert macro defeats loop unrolling,
                    // so we manually unroll this to speed up verification
                    memset((void*)&patternBuffer, (int)j->second->pattern, sizeof(uint64_t));
                    size_t i = 0;
                    for (i = 0; i+8 < j->second->size; i+=8) {
                        if (*(uint64_t*)&charBuffer[i] != patternBuffer) {
                            [test recordIssue:[[XCTIssue alloc] initWithType:XCTIssueTypeAssertionFailure compactDescription:
                                               [NSString stringWithFormat:@"failed 0x%lx[%zu] == %u\n",
                                                (uintptr_t)charBuffer, i, (unsigned char)j->second->pattern]]];
                        }
                    }
                    for (; i < j->second->size; ++i) {
                        if (charBuffer[i] != (uint8_t)j->second->pattern) {
                            [test recordIssue:[[XCTIssue alloc] initWithType:XCTIssueTypeAssertionFailure compactDescription:
                                               [NSString stringWithFormat:@"failed 0x%lx[%zu] == %u\n",
                                                (uintptr_t)charBuffer, i, (unsigned char)j->second->pattern]]];
                        }
                    }
                }
            }
        }
        _ephemeralAllocator.free((void*)liveAllocations);
    }
    EphemeralAllocator                  _ephemeralAllocator;
    Vector<AllocOperation>              _testVector             = Vector<AllocOperation>(_ephemeralAllocator);
    Vector<TestOperation>               _testOperations         = Vector<TestOperation>(_ephemeralAllocator);
    size_t                              _targetPoolSize         = (32*1024*1024);
    uint64_t                            _testOperationMaxSize;
};

};

#endif /* AllocatorTestSequence_h */
