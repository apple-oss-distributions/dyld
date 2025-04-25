/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
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

#ifndef _LD_ALLOCATOR_H_
#define _LD_ALLOCATOR_H_

#include <stdint.h>
#include <span>
#include <atomic>
#include <os/lock.h>

#include "Defines.h"

class ChunkBumpAllocatorZoneImpl;
struct ChunkBumpAllocatorChunk;
class ChunkBumpAllocatorZone;

/*!
 * @class ChunkBumpAllocator
 *
 * @abstract
 *      This is a bump allocator that manages memory in bulk by requesting
 *      chunks of memory from a zone it's been created with. Once it runs out of
 *      memory it will request a new memory chunk from the zone.
 *      When the allocator goes out of scope it will automatically give up its
 *      remaining memory back to the zone, allowing future allocators to reuse the space.
 *      It is also not thread safe, it can only be used from one thread at a time.
 *      Allocator zones on the other hand are thread-safe, so multiple allocators can be created
 *      and can request new memory chunks in parallel.
 *
 *      This allocator *does not* manage lifecycle of objects, it's only suitable for POD structures
 *      or objects that never have to be freed.
 */
class VIS_HIDDEN ChunkBumpAllocator
{
private:
    ChunkBumpAllocatorZoneImpl* _zone    = nullptr;
    ChunkBumpAllocatorChunk*    _chunk = nullptr;

public:

    ChunkBumpAllocator() {}
    ChunkBumpAllocator(ChunkBumpAllocatorZoneImpl* zone, ChunkBumpAllocatorChunk* chunk=nullptr): _zone(zone), _chunk(chunk) {}
    ChunkBumpAllocator(ChunkBumpAllocatorZone& zone, ChunkBumpAllocatorChunk* chunk=nullptr);

    ChunkBumpAllocator(const ChunkBumpAllocator&) = delete;
    ChunkBumpAllocator(ChunkBumpAllocator&& other): _zone(other._zone), _chunk(other._chunk)
    {
        other._zone = nullptr;
        other._chunk = nullptr;
    }

    ChunkBumpAllocator& operator=(const ChunkBumpAllocator&) = delete;
    ChunkBumpAllocator& operator=(ChunkBumpAllocator&& other)
    {
        std::swap(_zone, other._zone);
        std::swap(_chunk, other._chunk);
        return *this;
    }

    ~ChunkBumpAllocator();

    std::span<uint8_t> allocate(uint64_t size, uint16_t align=sizeof(uintptr_t));

    template<typename T>
    std::span<T> allocate(uint64_t count)
    {
        return std::span((T*)allocate(count * sizeof(T), alignof(T)).data(), count);
    }

    template<typename T>
    T* allocate()
    {
        return allocate<T>(1).data();
    }
};

/*!
 * @class ChunkBumpAllocatorZone
 *
 * @abstract
 *      A zone to manage a group of chunk bump allocators.
 *      This is not a general purpose allocator, so there's no default zone, but there are
 *      some global zones and a new zone can be created with the ::make factory method.
 *      Memory allocated in global zones is never freed, local zones created using the ::make
 *      method will free their memory when destroyed.
 */
class VIS_HIDDEN ChunkBumpAllocatorZone
{
private:
    ChunkBumpAllocatorZoneImpl* _zone   = nullptr;
    // global zones are never freed
    bool                        _global = false;

    constexpr ChunkBumpAllocatorZone(ChunkBumpAllocatorZoneImpl* zone, bool global): _zone(zone), _global(global) {}

public:

    // default chunk size when requesting a new allocator with the `makeAllocator` call
    // the default chunk must be large enough to for the zone to scale properly and to
    // reduce contention
    static const uint32_t defaultChunkSize  = 0x4000u * 8; // default 128kb chunk size 
 
    // a default threshold size for chunk reuse, when an allocator goes out of scope and its
    // remaining available memory is smaller than the min size then that memory will
    // be unused
    static const uint32_t defaultMinSize    = 0x1000u;

    // global zones
    static constinit ChunkBumpAllocatorZone atomsZone;
    static constinit ChunkBumpAllocatorZone symbolStringZone;

    ChunkBumpAllocatorZone(const ChunkBumpAllocatorZoneImpl&) = delete;
    ChunkBumpAllocatorZone(ChunkBumpAllocatorZone&& other)
    {
        std::swap(_zone, other._zone);
        std::swap(_global, other._global);
    }

    ChunkBumpAllocatorZone& operator=(const ChunkBumpAllocatorZone&) = delete;
    ChunkBumpAllocatorZone& operator=(ChunkBumpAllocatorZone&& other)
    {
        _zone = other._zone;
        _global = other._global;

        other._zone = nullptr;
        return *this;
    }

    ~ChunkBumpAllocatorZone();

    // Make a new allocator in this zone. By default this will create a new allocator using
    // any of already available chunk, optionally the size argument can be specified to ensure
    // a large enough chunk is allocated.
    ChunkBumpAllocator makeAllocator(size_t size=0);

    // Make a new allocator without reserving any memory, so a new chunk will be lazily requested
    // a on first allocation.
    ChunkBumpAllocator makeEmptyAllocator();

    void               printStatistics();

    static ChunkBumpAllocatorZone  make(uint32_t chunkSize = defaultChunkSize, uint32_t minSize=defaultMinSize);

    operator ChunkBumpAllocatorZoneImpl*() { return _zone; }
};

#endif // _LD_ALLOCATOR_H_
