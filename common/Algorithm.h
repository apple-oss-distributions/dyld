/*
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#ifndef Algorithm_h
#define Algorithm_h

#if BUILDING_MACHO_WRITER

#include <atomic>
#include <algorithm>
#include <optional>
#include <span>

#include <dispatch/dispatch.h>

#include "Array.h"

// A global switch to force all uses of `dispatchApply` to run sequentially instead of in parallel.
// This is useful for debugging parallel algorithms.
extern bool gSerializeDispatchApply;

template<typename T, typename Fn>
void dispatchApply(T&& container, Fn fn);

constexpr uint64_t defaultMapChunkSize = 0x2000;

// mapReduce() is a generalized way to process the entire set of elements in parallel.
// It uses a map-reduce style algorithm where the entire set of elements is broken up into
// subranges (by default 8192 elements per subrange). In parallel, the subranges are passed
// to the 'map' callback along with a T object.  The map callback should process the range
// of elements and update the results in the T object. Once all elements have been processed,
// the 'reduce' callback is called with the full set of T objects. It should then combine
// all the information from the Ts.
//
// Note: since many map callbacks are in flight at the same time, each should only store any
// state information in the T object, and not in captured variables.
//
template<typename ElementTy, typename ChunkTy>
inline void mapReduce(std::span<ElementTy> elements, size_t elementsPerChunk,
                      void (^map)(size_t, ChunkTy&, std::span<ElementTy>),
                      void (^reduce)(std::span<ChunkTy>)=nullptr)
{
    if ( elements.empty() )
        return;

    // map:
    // divvy up all elements into chunks
    // construct a T object for each chunk
    // call map(elements range) on each MAP object for a subrange of elements
    const size_t chunkCount     = (elements.size() + (elementsPerChunk - 1)) / elementsPerChunk;
    const size_t lastChunkIndex = chunkCount - 1;
    ChunkTy      chunksStorage[chunkCount];
    ChunkTy*     chunks = chunksStorage; // work around clang bug
    dispatchApply(std::span(chunks, chunkCount), ^(size_t i, ChunkTy& chunk) {
        if ( i == lastChunkIndex )
            map(i, chunk, elements.subspan(i * elementsPerChunk)); // Run the last chunk with whatever is left over
        else
            map(i, chunk, elements.subspan(i * elementsPerChunk, elementsPerChunk));
    });

    // reduce:
    if ( reduce )
        reduce(std::span<ChunkTy>(chunks, chunkCount));
}

template<typename ElementTy, typename Fn>
inline void dispatchForEach(std::span<ElementTy> elements, size_t elementsPerChunk, Fn fn)
{
    if ( elements.empty() )
        return;

    // divvy up all elements into chunks
    // call fn on each subrange of elements
    const size_t chunkCount     = (elements.size() + (elementsPerChunk - 1)) / elementsPerChunk;
    const size_t lastChunkIndex = chunkCount - 1;
    dispatchApply(chunkCount, [elements, elementsPerChunk, lastChunkIndex, fn](size_t chunkIndex) {
        std::span<ElementTy> chunkElements;
        if ( chunkIndex == lastChunkIndex )
            chunkElements = elements.subspan(chunkIndex * elementsPerChunk); // Run the last chunk with whatever is left over
        else
            chunkElements = elements.subspan(chunkIndex * elementsPerChunk, elementsPerChunk);

        size_t chunkStart = chunkIndex * elementsPerChunk;
        for ( size_t i = 0; i < chunkElements.size(); ++i ) {
            fn(chunkStart + i, chunkElements[i]);
        }
    });
}

template<typename ElementTy, typename Fn>
inline void dispatchForEach(std::span<ElementTy> elements, Fn fn)
{
    dispatchForEach(elements, defaultMapChunkSize, fn);
}

template<typename ElementTy, typename ChunkTy>
inline void mapReduce(std::span<ElementTy> elements, void (^map)(size_t, ChunkTy&, std::span<ElementTy>),
                      void (^reduce)(std::span<ChunkTy>)=nullptr)
{
    mapReduce(elements, defaultMapChunkSize, map, reduce);
}

#if 0
// Unused.  Leaving it here in case we want to use it for something in future.
template<typename ElementTy, typename ChunkTy>
inline void mapImmediateReduce(std::span<ElementTy> elements, size_t elementsPerChunk,
                               void (^map)(size_t, ChunkTy&, std::span<ElementTy>),
                               void (^reduce)(ChunkTy&))
{
    if ( elements.empty() )
        return;

    // map:
    // divvy up all elements into chunks
    // construct a T object for each chunk
    // call map(elements range) on each MAP object for a subrange of elements
    const size_t chunkCount     = (elements.size() + (elementsPerChunk - 1)) / elementsPerChunk;
    const size_t lastChunkIndex = chunkCount - 1;
    ChunkTy      chunksStorage[chunkCount];
    ChunkTy*     chunks = chunksStorage; // work around clang bug

    // Chunks are applied (reduced) immediately when they are the next chunk in order
    // We'll have a state per chunk and use atomics to compare them
    // When a chunk is ready, it will be atomically swapped to the ready state
    std::atomic_bool chunksStateStorage[chunkCount];
    std::atomic_bool* chunksState = chunksStateStorage; // work around clang bug
    for ( uint32_t i = 0; i != chunkCount; ++i )
        chunksStateStorage[i].store(false, std::memory_order_relaxed);

    // Chunk[0] is the next chunk we should apply
    chunksStateStorage[0].store(true, std::memory_order_relaxed);

    dispatchApply(std::span(chunks, chunkCount), ^(size_t i, ChunkTy& chunk) {
        if ( i == lastChunkIndex )
            map(i, chunk, elements.subspan(i * elementsPerChunk)); // Run the last chunk with whatever is left over
        else
            map(i, chunk, elements.subspan(i * elementsPerChunk, elementsPerChunk));

        // Our chunk now has data.  So we want to transition to the ready to be reduced state
        // If we have already been marked as the next chunk to be reduced, then we can immediately reduce now
        // If we are in the unset state, then just move to the ready state, and some other thread will do the reduce
        bool expected = false;
        if ( chunksState[i].compare_exchange_strong(expected, true) ) {
            // Our chunk wasn't next to be reduced, but we've at least marked it as ready
            // Some other thread will run reduce on it.
        } else {
            // This chunk is the next chunk, so we are going to reduce it
            size_t chunkIndex = i;
            while ( true ) {
                reduce(chunks[chunkIndex]);

                // Stop if we are out of chunks
                if ( ++chunkIndex == chunkCount )
                    break;

                // Try set the next chunk as being next to be reduced, from the false state
                // In that succeeds, then we are done here, and some other thread
                // will see the nextChunk state and handle it
                expected = false;
                if ( chunksState[chunkIndex].compare_exchange_strong(expected, true) ) {
                    break;
                }
            }
        }
    });
}
#endif

template<typename T, typename Fn>
inline void dispatchApply(T&& container, Fn fn)
{
    if constexpr ( std::is_convertible_v<T, size_t> ) {
        size_t count = container;
        if ( (count <= 1) || gSerializeDispatchApply ) {
            for ( size_t i = 0; i < count; ++i )
                fn(i);
        } else {
            dispatch_apply(count, DISPATCH_APPLY_AUTO, ^(size_t i) {
                fn(i);
            });
        }
    } else {
        if ( (container.size() <= 1) || gSerializeDispatchApply ) {
            for ( size_t i = 0; i < container.size(); ++i )
                fn(i, container[i]);
        } else {
            dispatch_apply(container.size(), DISPATCH_APPLY_AUTO, ^(size_t i) {
                fn(i, container[i]);
            });
        }
    }
}

template<typename ValTy>
inline void mergeVectorChunks(std::vector<ValTy>& outVec, std::span<std::vector<ValTy>> chunks)
{
    size_t totalSize = 0;
    for ( auto& chunk : chunks ) {
        totalSize += chunk.size();
    }
    if ( totalSize == 0 ) return;

    outVec.reserve(outVec.size() + totalSize);
    for ( auto& chunk : chunks ) {
        if ( !chunk.empty() )
            outVec.insert(outVec.end(), chunk.begin(), chunk.end());
    }
}

namespace details
{

// Rturns a pair of iterators of the sorted range or std::nullopt if there are less than two
// elements. Result iterators and input iterators form two sub-arrays at
// [begin, result.first] and [result.second, end] that need to be sorted by calling
// `quicksortPartTasks` recursively or with other algorithm.
template <typename It, typename Comp>
inline std::optional<std::pair<It, It>> quicksortPartTasks(It begin, It end, Comp comp) {
    size_t size = std::distance(begin, end);
    if ( size < 2 ) return std::nullopt;

    const auto pivot = *(begin + size / 2);
    auto low = begin - 1;
    auto high = end;

    while (true) {
        do {
            ++low;
        } while (comp(*low, pivot));

        do {
            --high;
        } while (comp(pivot, *high));

        if (low >= high) {
            return std::make_pair(low, high + 1);
        }

        std::swap(*low, *high);
    }
}

constexpr size_t serialThreshold = 4096;
}

inline bool shouldUseParallelSort(size_t size) { return size > details::serialThreshold; }

// NOTE: This implementation is suitable only for large ranges and when the comparison
// is expensive, e.g. strings, so that it can be done in parallel. When sorting a simple
// vector of integers the overhead of concurrency and simple quicksort implementation
// will be slower than std::sort.
//
// Parallel sort algorithm based on divide-and-conquer and quicksort algorithm.
// Regular quicksort algorithm could be written recursively as:
// quicksort(It begin, It end)
//  if ( distance(begin, end) < 2 ) return
//  auto pivot = partition(begin, end)
//  quicksort(begin, pivot)
//  quicksort(pivot + 1, end)
//
// This parallel implementation works in the same principle, but instead of
// serial recursive execution the subranges created by pivot partition
// are gathered into a worklist and processed in parallel. e.g.:
// 1. [begin, end] range is partitioned (sequentially) to create
//    two sub ranges [begin, pivot], [pivot + 1, end]
// 2. the 2 subranges are added to the worklist array
//    and then both will be partitioned concurrently to create
//    4 subranges
// 3. then the 4 subranges will create 8 new subranges etc.
// 4. this will be repeated until the number of elements in a subrange
//    exceed the details::serialThreshold limit, once the subrange
//    is smaller than the threshold it will be sorted with std::sort
//    and that will finish the recursion
template <typename It, typename Comp>
void parallelSort(It begin, It end, Comp comp)
{
    {
        size_t size = std::distance(begin, end);
        if ( size < 2 ) return;
        if ( !shouldUseParallelSort(size) ) {
            std::sort(begin, end, comp);
            return;
        }
    }

    auto startPair = details::quicksortPartTasks(begin, end, comp);
    if ( !startPair ) return;

    using TaskTy = std::pair<It, It>;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(TaskTy, nextTasks, 1024);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(TaskTy, currentTasks, 1024);
    auto* nextTasksPtr = &nextTasks;

    currentTasks.push_back(std::make_pair(begin, startPair->first));
    currentTasks.push_back(std::make_pair(startPair->second, end));

    do {
        nextTasks.resize(currentTasks.count() * 2);

        std::atomic_size_t nextTasksCount = 0;
        auto* nextTasksCountPtr = &nextTasksCount;

        auto doTask = ^(size_t i) {
            auto curTask = currentTasks[i];

            auto pair = details::quicksortPartTasks(curTask.first, curTask.second, comp);
            if ( pair ) {
                auto firstSize = std::distance(curTask.first, pair->first);
                auto secondSize = std::distance(pair->second, curTask.second);

                bool serialFirst = !shouldUseParallelSort(firstSize);
                bool serialSecond = !shouldUseParallelSort(secondSize);

                if ( serialFirst || serialSecond ) {
                    if ( serialFirst ) {
                        std::sort(curTask.first, pair->first, comp);
                    } else {
                        auto idx = nextTasksCountPtr->fetch_add(1, std::memory_order::relaxed);
                        (*nextTasksPtr)[idx] = std::make_pair(curTask.first, pair->first);
                    }

                    if ( serialSecond ) {
                        std::sort(pair->second, curTask.second, comp);
                    } else {
                        auto idx = nextTasksCountPtr->fetch_add(1, std::memory_order::relaxed);
                        (*nextTasksPtr)[idx] = std::make_pair(pair->second, curTask.second);
                    }
                } else {
                    auto idx = nextTasksCountPtr->fetch_add(2, std::memory_order::relaxed);
                    (*nextTasksPtr)[idx] = std::make_pair(curTask.first, pair->first);
                    (*nextTasksPtr)[idx + 1] = std::make_pair(pair->second, curTask.second);
                }
            }
        };
        if ( currentTasks.count() == 1 ) {
            doTask(0);
        } else {
            dispatch_apply(currentTasks.count(), DISPATCH_APPLY_AUTO, ^(size_t i) {
                doTask(i);
            });
        }

        nextTasks.resize(nextTasksCount);
        {
            currentTasks.clear();
            auto tmp = std::move(currentTasks);
            currentTasks = std::move(nextTasks);
            nextTasks = std::move(tmp);
        }
    } while ( !currentTasks.empty() );
}


template <typename It>
void parallelSort(It begin, It end)
{
    parallelSort(begin, end, std::less<std::remove_reference_t<decltype(*begin)>>{});
}

template <typename Container, typename Comp>
void parallelSort(Container& c, Comp comp)
{
    parallelSort(c.begin(), c.end(), comp);
}

template <typename Container>
void parallelSort(Container& c)
{
    parallelSort(c, std::less<std::remove_reference_t<decltype(*c.begin())>>{});
}

#endif

#endif /* Algorithm_h */
