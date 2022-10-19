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
#include "AllocatorTestSequence.h"

using namespace lsl;

@interface ConcurrentAllocatorTests : DyldTestCase

@end

@implementation ConcurrentAllocatorTests

- (void) testConcurrentAllocator {
    auto& allocator = Allocator::concurrentAllocator();
    __block auto allocatorPtr = &allocator;
    __block std::array<TestSequence,16> sequences;
    uint64_t seedBase = [self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()];
    dispatch_apply(16, DISPATCH_APPLY_AUTO, ^(size_t iteration) {
        sequences[iteration] = std::move(TestSequence(seedBase+iteration));
    });
    dispatch_apply(16, DISPATCH_APPLY_AUTO, ^(size_t iteration) {
        sequences[iteration].runMallocTests(self, allocatorPtr, true);
    });
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

struct StackNode {
    size_t size;
    void* buffer;
    char pattern;
    StackNode* next;
};

static StackNode*           threadedAllocatorTestStack = nullptr;
static os_unfair_lock_s     threadedAllocatorTestStackLock = OS_UNFAIR_LOCK_INIT;

static
bool validate(StackNode* record) {
    const uint8_t* charBuffer = (uint8_t*)record->buffer;
    uint64_t patternBuffer = 0;
    memset((void*)&patternBuffer, (int)record->pattern, sizeof(uint64_t));
    size_t j = 0;
    for (j = 0; j+8 < record->size; j+=8) {
        if (*(uint64_t*)&charBuffer[j] != patternBuffer) {
            return false;
        }
    }
    for (; j < record->size; ++j) {
        if (charBuffer[j] != (uint8_t)record->pattern) {
            return false;
        }
    }
    return true;
}

static void* concurrentllocatorTestFunc(void* ctx) {
    auto allocator = (Allocator*)ctx;
    std::mt19937_64 mt;
    mt.seed((uintptr_t)pthread_self());
    std::uniform_int_distribution<uint8_t>  biasedBool(0,2);
    std::uniform_int_distribution<uint8_t>  patternDist(0,255);
    std::uniform_int_distribution<size_t>   sizeDist(0, 65536);
    size_t allocated = 0;

    while (allocated <= 1024*1024) {
        bool allocation = biasedBool(mt) > 0 ? true : false;
        if (allocation) {
            auto record = (StackNode*)allocator->malloc(sizeof(StackNode));
            record->size = sizeDist(mt);
            record->pattern = (char)patternDist(mt);
            record->buffer = allocator->malloc(record->size);
            memset(record->buffer, record->pattern, record->size);
            allocated += record->size;
            os_unfair_lock_lock(&threadedAllocatorTestStackLock);
            record->next = threadedAllocatorTestStack;
            threadedAllocatorTestStack = record;
            os_unfair_lock_unlock(&threadedAllocatorTestStackLock);
        } else {
            StackNode* record = nullptr;
            os_unfair_lock_lock(&threadedAllocatorTestStackLock);
            if (threadedAllocatorTestStack != nullptr) {
                record = threadedAllocatorTestStack;
                threadedAllocatorTestStack = record->next;
            }
            os_unfair_lock_unlock(&threadedAllocatorTestStackLock);
            if (record == nullptr) { continue; }
            if (!validate(record)) {
                return (void*)0x01;
            }
            allocator->free(record->buffer);
            allocator->free(record);
        }
    }

    while(1) {
        StackNode* record = nullptr;
        os_unfair_lock_lock(&threadedAllocatorTestStackLock);
        if (threadedAllocatorTestStack != nullptr) {
            record = threadedAllocatorTestStack;
            threadedAllocatorTestStack = record->next;
        }
        os_unfair_lock_unlock(&threadedAllocatorTestStackLock);
        if (record == nullptr) { return nullptr; }
        if (!validate(record)) {
            return (void*)0x01;
        }
        allocator->free(record->buffer);
        allocator->free(record);
    }
}

- (void) testConcurrentAllocatorCrossThread {
    auto& allocator = Allocator::concurrentAllocator();
    __block auto& allocatorRef = allocator;
    pthread_t threads[32];
    for (auto i = 0; i < 32; ++i) {
        pthread_create(&threads[i], nullptr, concurrentllocatorTestFunc, (void*)&allocatorRef);
    }
    for (auto i = 0; i < 32; ++i) {
        void* status = nullptr;
        pthread_join(threads[i], &status);
        XCTAssertEqual(status, nullptr);
    }
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

static StackNode*           currentAllocatorTestDestructStack = nullptr;

static void* currentAllocatorTestDestructFunc(void* ctx) {
    auto allocator = (Allocator*)ctx;
    std::mt19937_64 mt;
    mt.seed((uintptr_t)pthread_self());
    std::uniform_int_distribution<uint8_t>  biasedBool(0,2);
    std::uniform_int_distribution<uint8_t>  patternDist(0,255);
    std::uniform_int_distribution<size_t>   sizeDist(0, 65536);
    auto record = (StackNode*)allocator->malloc(sizeof(StackNode));
    record->size = sizeDist(mt);
    record->pattern = (char)patternDist(mt);
    record->buffer = allocator->malloc(record->size);
    record->next = currentAllocatorTestDestructStack;
    currentAllocatorTestDestructStack = record;
    return nullptr;
}

- (void) testConcurrentAllocatorThreadDestruction {
    auto& allocator = Allocator::concurrentAllocator();
    __block auto& allocatorRef = allocator;
    for (auto i = 0; i < 1024; ++i) {
        pthread_t thread;
        void* status = nullptr;
        pthread_create(&thread, nullptr, currentAllocatorTestDestructFunc, (void*)&allocatorRef);
        pthread_join(thread, &status);
        XCTAssertEqual(status, nullptr);
    }
    while(1) {
        StackNode* record = nullptr;
        if (currentAllocatorTestDestructStack != nullptr) {
            record = currentAllocatorTestDestructStack;
            currentAllocatorTestDestructStack = record->next;
        }
        if (record == nullptr) { break; }
        allocator.free(record->buffer);
        allocator.free(record);
    }
    XCTAssertEqual(allocator.allocated_bytes(), 0);
    allocator.destroy();
}


- (void) testThreadSafeAllocatorPerformance {
    self.randomSeed = 8848042ULL;
    auto& allocator = Allocator::concurrentAllocator();
    __block std::array<TestSequence,10> sequences;
    __block auto allocatorPtr = &allocator;
    uint64_t seedBase = [self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()];
    for (auto i = 0; i < 10; ++i) {
        sequences[i] = std::move(TestSequence(seedBase+i));
    }
    [self measureBlock:^{
        dispatch_apply(10, DISPATCH_APPLY_AUTO, ^(size_t iteration) {
            sequences[iteration].runMallocTests(self, allocatorPtr, false);
        });
    }];
    XCTAssert(allocator.allocated_bytes() == 0);
    allocator.destroy();
}

@end
