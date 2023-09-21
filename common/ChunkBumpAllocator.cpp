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

#include <mach/mach_vm.h>

#include "ChunkBumpAllocator.h"

#if BUILDING_MACHO_WRITER
#include "DynamicAtom.h"
#endif

// An allocated memory chunk and its size, this is always located
// at the beginning of an allocated memory chunk, which allows to maintain
// a free/used lists without extra misc allocations.
struct ChunkBumpAllocatorChunk
{
    ChunkBumpAllocatorChunk* next  = nullptr;
    uint32_t                 size  = 0;
    uint32_t                 pos   = 0;

    uint32_t available() { return size - pos; }
    uint8_t* begin() { return (uint8_t*)this + pos; }
};

using Entry = ChunkBumpAllocatorChunk;

class ChunkBumpAllocatorZoneImpl
{
public:
    friend ChunkBumpAllocator;

    static const uint32_t headerSize = sizeof(Entry);

    ChunkBumpAllocatorZoneImpl(uint32_t chunkSize, uint32_t minReuseSize): _chunkSize(chunkSize), _minReuseSize(minReuseSize)
    {
        assert(chunkSize >= 512 && "too small chunk size?");
        assert(chunkSize > _minReuseSize);
        assert(_minReuseSize > 0);
    }

    ~ChunkBumpAllocatorZoneImpl();

    ChunkBumpAllocatorZoneImpl(const ChunkBumpAllocatorZoneImpl&) = delete;
    ChunkBumpAllocatorZoneImpl(ChunkBumpAllocatorZoneImpl&&) = delete;

    ChunkBumpAllocatorZoneImpl& operator=(const ChunkBumpAllocatorZoneImpl&) = delete;
    ChunkBumpAllocatorZoneImpl& operator=(ChunkBumpAllocatorZoneImpl&&) = delete;

    // Get a next available memory chunk that's large enough to serve 
    Entry* nextFreeChunk(uint64_t size=0);

    void printStatistics();

private:

    size_t  allocationSizeForRequestedSize(uint64_t);

    // chunk factory methods
    Entry*  getFreeEntryLocked(uint64_t size);
    Entry*  nextFreeChunkReclaimOld(uint64_t size, Entry* entry);
    // reclaim entry methods - either moves the entry to the free list or it will be retired and moved to the used list
    void    reclaimEntry(Entry* entry);
    void    reclaimFreeEntryLocked(Entry* entry);

    bool    retireEntryIfSmall(Entry* entry);
    void    retireEntry(Entry* entry);

    // Entry chunks allocation helpers
    Entry*  makeNewEntry(uint64_t size);
    void    free(Entry*);

    Entry*              _freeList = nullptr;
    std::atomic<Entry*> _usedList = nullptr;
    // default chunk allocation size
    uint32_t            _chunkSize = 0;
    // threshold size for chunk reuse, if an entry has less available memory
    // it will be retired into the used list
    uint32_t            _minReuseSize = 0;
    os_unfair_lock_s    _lock = OS_UNFAIR_LOCK_INIT;
};

ChunkBumpAllocatorZoneImpl::~ChunkBumpAllocatorZoneImpl()
{
    Entry* entry = _freeList;
    while ( entry ) {
        Entry* next = entry->next;
        free(entry);
        entry = next;
    }
    _freeList = nullptr;

    entry = _usedList;
    while ( entry ) {
        Entry* next = entry->next;
        free(entry);
        entry = next;
    }
    _usedList = nullptr;
}

Entry* ChunkBumpAllocatorZoneImpl::makeNewEntry(uint64_t size)
{
    assert(size > sizeof(Entry));
    Entry* entry = (Entry*)malloc(size);
    new (entry) Entry();

    entry->size = (uint32_t)size;
    assert(entry->size == size && "size exceeds 32-bit allocation size limit");
    entry->pos = sizeof(Entry);
    return entry;
}

void ChunkBumpAllocatorZoneImpl::free(Entry* entry)
{
    ::free(entry);
}

Entry* ChunkBumpAllocatorZoneImpl::getFreeEntryLocked(uint64_t size)
{
    Entry* rootEntry = _freeList;

    // free entries are sorted by size, so use root if it's big enough
    if ( rootEntry && rootEntry->available() >= size ) {
        _freeList = rootEntry->next;
        rootEntry->next = nullptr;
        return rootEntry;
    }
    // no free entry
    return nullptr;
}

size_t ChunkBumpAllocatorZoneImpl::allocationSizeForRequestedSize(uint64_t size)
{
    uint64_t baseSize = std::max((uint64_t)_chunkSize, size + sizeof(Entry));

    // align allocation size to a power of 2
    uint64_t leadingZeros = __builtin_clzll(baseSize);
    uint64_t bufferSize = 1 << (64 - leadingZeros - 1);
    if ( bufferSize != baseSize ) {
        // for allocations smaller than 2mb align to a power of 2
        if ( bufferSize < (1 << 21) )
            bufferSize = bufferSize << 1;
        else // otherwise round up to a page size
            bufferSize = mach_vm_round_page(baseSize);
    }
    return bufferSize;
}

Entry* ChunkBumpAllocatorZoneImpl::nextFreeChunk(uint64_t size)
{
    return nextFreeChunkReclaimOld(size, /* entry to reclaim */ nullptr);
}

Entry* ChunkBumpAllocatorZoneImpl::nextFreeChunkReclaimOld(uint64_t size, Entry* old)
{
    uint64_t bufferSize = allocationSizeForRequestedSize(size);
    // quick check if top of the free list is set, we won't be using the actual value
    // so no lock is required
    if ( _freeList == nullptr ) {
        return (Entry*)makeNewEntry(bufferSize);
    }

    os_unfair_lock_lock(&_lock);
    Entry* freeEntry = getFreeEntryLocked(size);
    if ( old && !retireEntryIfSmall(old) ) {
        reclaimFreeEntryLocked(old);
    }
    os_unfair_lock_unlock(&_lock);
    if ( freeEntry == nullptr ) {
        // no free entry, make a new one
        freeEntry = (Entry*)makeNewEntry(bufferSize);
    }
    return freeEntry;
}

void ChunkBumpAllocatorZoneImpl::retireEntry(Entry* entry)
{
    assert(entry->next == nullptr && "free entries shouldn't have the next entry set");
    Entry* currentRoot = _usedList.load(std::memory_order_relaxed);
    entry->next = currentRoot;
    while ( !_usedList.compare_exchange_weak(currentRoot, entry, std::memory_order_release, std::memory_order_relaxed) )
        entry->next = currentRoot;
}

bool ChunkBumpAllocatorZoneImpl::retireEntryIfSmall(Entry* entry)
{
    bool reuse = entry->available() >= _minReuseSize;
    if ( reuse )
        return false;

    retireEntry(entry);
    return true;
}

void ChunkBumpAllocatorZoneImpl::reclaimEntry(Entry* entry)
{
    if ( retireEntryIfSmall(entry) )
        return;
    os_unfair_lock_lock(&_lock);
    reclaimFreeEntryLocked(entry);
    os_unfair_lock_unlock(&_lock);
}

void ChunkBumpAllocatorZoneImpl::reclaimFreeEntryLocked(Entry* entry)
{
    if ( _freeList == nullptr ) {
        _freeList = entry;
        return;
    }

    // free list is ordered by the available space, add as a root if larger or equal
    size_t newEntryAvailable = entry->available();
    assert(newEntryAvailable >= _minReuseSize && "used entry should have been already reclaimed");
    if ( newEntryAvailable >= _freeList->available() ) {
        entry->next = _freeList;
        _freeList = entry;
        return;
    }

    // otherwise walk the list to find the last entry that has more available space
    // than this new entry, but limit the depth of search for free entries
    // this is to prevent contention by ensuring we don't create a big
    // list with small available space
    int freeListCap = 15;
    Entry* previousEntry = _freeList;
    Entry* currentEntry = previousEntry->next;
    while ( currentEntry && currentEntry->available() > newEntryAvailable ) {
        previousEntry = currentEntry;
        currentEntry = currentEntry->next;

        if ( --freeListCap < 1 ) {
            // depth limit reached, retire this entry and add it to the used list
            retireEntry(entry);
            return;
        }
    }
    // current entry has the same or less available space
    // so add this new entry after the previous one
    entry->next = previousEntry->next;
    previousEntry->next = entry;
}

void ChunkBumpAllocatorZoneImpl::printStatistics()
{
    size_t usedEntries = 0;
    size_t wastedSpace = 0;
    size_t usedSpace   = 0;
    Entry* entry = _usedList;
    while ( entry ) {
        ++usedEntries;
        wastedSpace += entry->available();
        usedSpace   += entry->size - entry->available();
        entry = entry->next;
    }

    size_t freeEntries = 0;
    size_t freeSpace = 0;
    entry = _freeList;
    while ( entry ) {
        ++freeEntries;
        freeSpace += entry->available();
        usedSpace += entry->size - entry->available();
        entry = entry->next;
    }

    printf("used space: %lu, free entries: %lu, free space: %lu, used entries: %lu, wasted space: %lu\n", usedSpace, freeEntries, freeSpace, usedEntries, wastedSpace);
}

ChunkBumpAllocator::ChunkBumpAllocator(ChunkBumpAllocatorZone& zone, ChunkBumpAllocatorChunk* chunk): _zone(zone), _chunk(chunk)
{}

std::span<uint8_t> ChunkBumpAllocator::allocate(uint64_t size, uint16_t align)
{
    if ( !_chunk ) {
        assert(_zone);
        this->_chunk = _zone->nextFreeChunk(size);
    }
    uint8_t* begin = _chunk->begin();
    size_t alignOffset = 0;
    size_t alignBytes = (uint64_t)begin % align;
    if ( alignBytes != 0 )
        alignOffset = align - alignBytes;

    size_t totalSize = alignOffset + size;
    if ( _chunk->available() < totalSize ) {
        _chunk = _zone->nextFreeChunkReclaimOld(size, _chunk);
        // call allocate() recursively, as the alignment offset might be
        // different
        return allocate(size, align);
    }

    std::span<uint8_t> buffer(_chunk->begin() + alignOffset, size);
    _chunk->pos += totalSize;
    return buffer;
}

ChunkBumpAllocator::~ChunkBumpAllocator()
{
    if ( _chunk && _zone ) {
        _zone->reclaimEntry(_chunk);
    }
}

ChunkBumpAllocatorZone ChunkBumpAllocatorZone::make(uint32_t chunkSize, uint32_t minSize)
{
    return ChunkBumpAllocatorZone(new ChunkBumpAllocatorZoneImpl(chunkSize, minSize), /* global */ false);
}

ChunkBumpAllocatorZone::~ChunkBumpAllocatorZone()
{
    if ( !_global && _zone ) {
        delete _zone;
        _zone = nullptr;
    }
}

void ChunkBumpAllocatorZone::printStatistics()
{
    _zone->printStatistics();
}

ChunkBumpAllocator ChunkBumpAllocatorZone::makeAllocator(size_t size)
{
    return ChunkBumpAllocator(_zone, _zone->nextFreeChunk(size));
}

ChunkBumpAllocator ChunkBumpAllocatorZone::makeEmptyAllocator()
{
    return ChunkBumpAllocator(*this);
}

#if BUILDING_MACHO_WRITER

const size_t sAtomAllocatorMinReuseSize = 0x1000 / sizeof(ld::DynamicAtom);

static ChunkBumpAllocatorZoneImpl gAtomsZone(ChunkBumpAllocatorZone::defaultChunkSize, sAtomAllocatorMinReuseSize);
static ChunkBumpAllocatorZoneImpl gSymbolStringZone(ChunkBumpAllocatorZone::defaultChunkSize, ChunkBumpAllocatorZone::defaultMinSize);

constinit ChunkBumpAllocatorZone ChunkBumpAllocatorZone::atomsZone         = ChunkBumpAllocatorZone(&gAtomsZone, /* global */ true);
constinit ChunkBumpAllocatorZone ChunkBumpAllocatorZone::symbolStringZone  = ChunkBumpAllocatorZone(&gSymbolStringZone, /* global */ true);
#endif
