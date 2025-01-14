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

#include <string.h>
#include <cstdio>
#include <algorithm>
#include <compare>
#include <TargetConditionals.h>
#include "Defines.h"
#if !TARGET_OS_EXCLAVEKIT
  #include <sys/mman.h>
  #include <mach/mach.h>
  #include <mach/mach_vm.h>
  #include <malloc/malloc.h>
#endif //  !TARGET_OS_EXCLAVEKIT
#include <sanitizer/asan_interface.h>


#include "Allocator.h"
#include "BTree.h"
#include "BitUtils.h"
#include "StringUtils.h"

#if !TARGET_OS_EXCLAVEKIT
#include "DyldRuntimeState.h"
#if BUILDING_DYLD
#include "dyld_cache_format.h"
#endif // BUILDING_DYLD
#endif // !TARGET_OS_EXCLAVEKIT

#if !BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
#include <dispatch/dispatch.h>
#endif

#if ALLOCATOR_LOGGING_ENABLED
#define ALLOCATOR_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define ALLOCATOR_LOG(...)
#endif

#if ALLOCATOR_MAKE_TRACE
#define ALLOCATOR_TRACE(...) fprintf(stderr, __VA_ARGS__)
#else
#define ALLOCATOR_TRACE(...)
#endif

#if BUILDING_DYLD
extern "C" void* __dso_handle;
#endif


// On darwin platforms PAGE_SIZE is not constant so it cannot be passed into templates.
// For our purposes we can assume 16k pages, the allocator always allocates quantities larger than that anyway, so 4k devices will not be penalized.
static const uint64_t kPageSize = 16384;

namespace lsl {

#if !TARGET_OS_EXCLAVEKIT
void Lock::lock() {
    if (!_lock) { return; }
    assertNotOwner();
#if BUILDING_DYLD
    assert(_runtimeState != nullptr);
    _runtimeState->libSystemHelpers->os_unfair_lock_lock_with_options(_lock, OS_UNFAIR_LOCK_NONE);
#else /* BUILDING_DYLD */
    os_unfair_lock_lock_with_options(_lock, OS_UNFAIR_LOCK_NONE);
#endif /* BUILDING_DYLD */
}
void Lock::unlock() {
    if (!_lock) { return; }
    assertOwner();
#if BUILDING_DYLD
    assert(_runtimeState != nullptr);
    _runtimeState->libSystemHelpers->os_unfair_lock_unlock(_lock);
#else /* BUILDING_DYLD */
    os_unfair_lock_unlock(_lock);
#endif /* BUILDING_DYLD */
}

void Lock::assertNotOwner() {
    if (!_lock) { return; }
    os_unfair_lock_assert_not_owner(_lock);
}
void Lock::assertOwner() {
    if (!_lock) { return; }
    os_unfair_lock_assert_owner(_lock);
    
}
#endif // !TARGET_OS_EXCLAVEKIT

#pragma mark -
#pragma mark MemoryManager

MemoryManager::MemoryManager(MemoryManager&& other) {
    swap(other);
}

MemoryManager& MemoryManager::operator=(MemoryManager&& other) {
    swap(other);
    return *this;
}

MemoryManager::MemoryManager(const char** envp, const char** apple, void* dyldSharedCache) {
    // Eventually we will use this to parse parameters for controlling comapct info mlock()
    // We need to do this before allocator is created
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT

#if ENABLE_HW_TPRO_SUPPORT
    // Note this is the "does the HW support TPRO bit" not the "is this process using TPRO for DATA_CONST bit".
    // We want the HW bit here as the kernel keeps the TPRO flag enabled in the TPRO_CONST mapping, even
    // if it the process doesn't support TPRO for DATA_CONST
    if ( (_simple_getenv(apple, "dyld_hw_tpro") != nullptr) && (vm_page_size == 0x4000)) {
        bool isPrivateCache = false;
        if ( const char* privateCache = _simple_getenv(envp, "DYLD_SHARED_REGION") )
            isPrivateCache = !strcmp(privateCache, "private");

        if ( !isPrivateCache ) {
            // Start in a writable state to allow bootstrap
            _tproEnable = true;
        }
    }
#endif

    _sharedCache = dyldSharedCache;
#endif //  BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
}

void MemoryManager::setDyldCacheAddr(void* sharedCache) {
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    _sharedCache = (dyld_cache_header*)sharedCache;
#endif /* BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT */
}

#if !TARGET_OS_EXCLAVEKIT
MemoryManager::MemoryManager(Lock&& lock) : _lock(std::move(lock)) {}

void MemoryManager::adoptLock(Lock&& lock) {
    _lock = std::move(lock);
}
#endif // !TARGET_OS_EXCLAVEKIT


void MemoryManager::swap(MemoryManager& other) {
    using std::swap;
#if !TARGET_OS_EXCLAVEKIT
    swap(_lock,                     other._lock);
#endif // !TARGET_OS_EXCLAVEKIT
    swap(_allocator,                other._allocator);
    swap(_writeableCount,           other._writeableCount);
#if BUILDING_DYLD
    swap(_sharedCache,              other._sharedCache);
#endif // BUILDING_DYLD

    // We don't actually swap this because it is a process wide setting, and we may need it to be set correctly
    // even in the bootstrapMemoryProtector adfter move construction
#if ENABLE_HW_TPRO_SUPPORT
    _tproEnable = other._tproEnable;
#endif // ENABLE_HW_TPRO_SUPPORT
}

int MemoryManager::vmFlags() const {
    int result = 0;

#if ENABLE_HW_TPRO_SUPPORT
    if (_tproEnable) {
        // add tpro for memory protection on platform that support it
        result |= VM_FLAGS_TPRO;
    }
#endif // ENABLE_HW_TPRO_SUPPORT

#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    // Only include the dyld tag for allocations made by dyld
    result |= VM_MAKE_TAG(VM_MEMORY_DYLD);
#endif // BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    return result;
}


bool MemoryManager::Buffer::align(uint64_t alignment, uint64_t targetSize) {
    if (targetSize > size) { return false; }
    char* p1 = static_cast<char*>(address);
    char* p2 = reinterpret_cast<char*>(reinterpret_cast<size_t>(p1 + (alignment - 1)) & -alignment);
    uint64_t d = static_cast<uint64_t>(p2 - p1);
    if (d > size - targetSize) { return false; }
    address = p2;
    size -= d;
    return true;
}

#if TARGET_OS_EXCLAVEKIT
// ExclaveKit specific page allocator - for now, let's use a fixed-size static arena.
static char page_alloc_arena[34 * 0x4000] __attribute__((aligned(kPageSize)));
static uint64_t page_alloc_arena_used = 0;

[[nodiscard]] void* MemoryManager::allocate_pages(uint64_t size) {
    uint64_t targetSize = roundToNextAligned<kPageSize>(size);
    if (page_alloc_arena_used + targetSize > sizeof(page_alloc_arena)) {
        return nullptr;
    }
    void *result = page_alloc_arena + page_alloc_arena_used;
    page_alloc_arena_used += targetSize;
    return result;
}

void MemoryManager::deallocate_pages(void* p, uint64_t size) {
    void *last = page_alloc_arena + page_alloc_arena_used - size;
    if ( p == last ) {
        bzero(p, size);
        page_alloc_arena_used -= size;
    }
}

[[nodiscard]] MemoryManager::Buffer MemoryManager::vm_allocate_bytes(uint64_t size) {
    uint64_t targetSize = roundToNextAligned<kPageSize>(size);
    void* result = MemoryManager::allocate_pages(targetSize);
    if ( !result ) {
        return {nullptr, 0};
    }
    return {result, targetSize};
}

void MemoryManager::vm_deallocate_bytes(void* p, uint64_t size) {
    MemoryManager::deallocate_pages(p, size);
}

#else
[[nodiscard]] Lock::Guard MemoryManager::lockGuard() {
    return Lock::Guard(_lock);
}

template<typename T>
static void appendHexToString(char *dst, T value, uint64_t size) {
    char buffer[130];
    bytesToHex((const uint8_t*)&value, sizeof(T), buffer);
    strlcat(dst, buffer, (size_t)size);
}

[[nodiscard]] MemoryManager::Buffer MemoryManager::vm_allocate_bytes(uint64_t size) {
    kern_return_t kr = KERN_FAILURE;
    uint64_t targetSize = roundToNextAligned<kPageSize>(size);
#if __LP64__
    mach_vm_address_t result = 0x0100000000;                    // Set to 4GB so that is the first eligible address
#else
    mach_vm_address_t result = 0;
#endif
#if !TARGET_OS_SIMULATOR
    // We allocate an extra page to use as a guard page
    kr = mach_vm_map(mach_task_self(),
                                   &result,
                                   targetSize,
                                   PAGE_MASK,                       // Page alignment
                                   VM_FLAGS_ANYWHERE | vmFlags(),
                                   MEMORY_OBJECT_NULL,              // Allocate memory instead of using an existing object
                                   0,
                                   FALSE,
                                   VM_PROT_READ | VM_PROT_WRITE,
                                   VM_PROT_ALL,                 // Needs to VM_PROT_ALL for libsyscall glue to pass via trap
                                   VM_INHERIT_DEFAULT);         // Needs to VM_INHERIT_DEFAULT for libsyscall glue to pass via trap
#endif
    if (kr != KERN_SUCCESS) {
        // Fall back to vm_allocate() if mach_vm_map() fails. That can happen due to sandbox, or when running un the simulator
        // on an older host. Technically this is not guaranteed to be above 4GB, but since it requires manually configuring a zero
        // page to be below 4GB it is safe to assume processes that need it will also setup their sandbox properly so that
        // mach_vm_map() works.
        kr = vm_allocate(mach_task_self(), (vm_address_t*)&result, (vm_size_t)targetSize, VM_FLAGS_ANYWHERE | vmFlags());
    }

    if (kr != KERN_SUCCESS) {
        char buffer[1024];
        strlcpy(&buffer[0], "Could not vm_allocate 0x", 1024);
        appendHexToString(&buffer[0], targetSize, 1024);
        strlcat(&buffer[0], "\n\tRequested size: 0x", 1024);
        appendHexToString(&buffer[0], requestedSize, 1024);
        strlcat(&buffer[0], "\n\tRequested allgnment: 0x", 1024);
        appendHexToString(&buffer[0], requestedAlignment, 1024);
        strlcat(&buffer[0], "\n\tRequested target size: 0x", 1024);
        appendHexToString(&buffer[0], requestedTargetSize, 1024);
        strlcat(&buffer[0], "\n\tRequested target allgnment: 0x", 1024);
        appendHexToString(&buffer[0], requestedTargetAlignment, 1024);
        strlcat(&buffer[0], "\n\tkern return: 0x", 1024);
        appendHexToString(&buffer[0], kr, 1024);
        CRSetCrashLogMessage2(buffer);
        assert(0 && "vm_allocate failed");
        return {nullptr, 0};
    }
    ALLOCATOR_LOG("vm_allocate_bytes: 0x%llx-0x%llx (%llu bytes)\n", result, result+targetSize, targetSize);
    return {(void*)result, targetSize};
}

void MemoryManager::vm_deallocate_bytes(void* p, uint64_t size) {
    ALLOCATOR_LOG("vm_deallocate_bytes: 0x%llx-0x%llx (%llu bytes)\n", (uint64_t)p, (uint64_t)p+size, size);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)p, (vm_size_t)size);
}
#endif // TARGET_OS_EXCLAVEKIT

extern void* tproConstStart   __asm("segment$start$__TPRO_CONST");
extern void* tproConstEnd     __asm("segment$end$__TPRO_CONST");

void MemoryManager::writeProtect(bool protect) {
#if !TARGET_OS_EXCLAVEKIT && BUILDING_DYLD
    // First (un)lock dyld's __TPRO_CONST segment if it is not part of the shared cache
    const mach_header* dyldMH = (const mach_header*)&__dso_handle;
    if (!(dyldMH->flags & MH_DYLIB_IN_CACHE)) {
        size_t tproConstSize = (size_t)&tproConstEnd - (size_t)&tproConstStart;
        kern_return_t kr = ::vm_protect(mach_task_self(), (vm_address_t)&tproConstStart, (vm_size_t)tproConstSize, false,
                                        VM_PROT_READ | (protect ? 0 : (VM_PROT_WRITE | VM_PROT_COPY)));
        if (kr != KERN_SUCCESS) {
            // fprintf(stderr, "FAILED: %d", kr);
        }
    }
    // Next if there is a configured shared cache (un)lock it's __TPRO_CONST segment
    if (_sharedCache && ((dyld_cache_header*)_sharedCache)->mappingOffset > __offsetof(dyld_cache_header, tproMappingsCount)) {
        uint8_t* cacheBuffer = (uint8_t*)_sharedCache;
        dyld_cache_header* cacheHeader = (dyld_cache_header*)_sharedCache;
        dyld_cache_tpro_mapping_info* mappings = (dyld_cache_tpro_mapping_info*)&cacheBuffer[cacheHeader->tproMappingsOffset];
        uint64_t    slide = (uint64_t)_sharedCache - cacheHeader->sharedRegionStart;
        for (auto i = 0; i < cacheHeader->tproMappingsCount; ++i) {
            void* addr = (void*)(mappings[i].unslidAddress + slide);
            kern_return_t kr = ::vm_protect(mach_task_self(), (vm_address_t)addr, (vm_size_t)mappings[i].size, false,
                                            VM_PROT_READ | (protect ? 0 : (VM_PROT_WRITE | VM_PROT_COPY)));
            if (kr != KERN_SUCCESS) {
                // fprintf(stderr, "FAILED: %d", kr);
            }
        }
    }
    // Finally if there are any vm_allocated tpro protected regions (un)lock them
    if (!_allocator) { return; }
    _allocator->forEachVMAllocatedBuffer(^(const Buffer& buffer) {
        kern_return_t kr = ::vm_protect(mach_task_self(), (vm_address_t)buffer.address, (vm_size_t)buffer.size, false,
                                        VM_PROT_READ | (protect ? 0 : VM_PROT_WRITE ));
        if (kr != KERN_SUCCESS) {
            // fprintf(stderr, "FAILED: %d", kr);
        }
    });
#endif // !TARGET_OS_EXCLAVEKIT && BUILDING_DYLD
}

#pragma mark -
#pragma mark Common Utility functionality for allocators

void* Allocator::Buffer::lastAddress() const {
    return (void*)((uint64_t)address + size);
}

bool Allocator::Buffer::contains(const Buffer& region) const {
    if (region.address < address) { return false; }
    if (region.lastAddress() > lastAddress()) { return false; }
    return true;
}

bool Allocator::Buffer::valid() const {
    return (address != nullptr);
}

Allocator::Buffer Allocator::Buffer::findSpace(uint64_t targetSize, uint64_t targetAlignment) const {
    Buffer result = *this;
    result.address = (void*)((uint64_t)result.address);
    if (result.align(targetAlignment, targetSize)) {
        result.address = (void*)((uint64_t)result.address);
        result.size = targetSize;
        return result;
    }
    return {nullptr , 0};
}

void Allocator::Buffer::consumeSpace(uint64_t consumedSpace) {
    assert(consumedSpace <= size);
    assert(consumedSpace%16==0);
    address = (void*)((uint64_t)address+consumedSpace);
    size -= consumedSpace;
}

Allocator::Buffer::operator bool() const {
    if (address != nullptr) { return true; }
    if (size != 0) { return true; }
    return false;
}

bool Allocator::Buffer::succeeds(const Buffer& other) const {
    if (((uint64_t)address + size) == ((uint64_t)other.address)) { return true; }
    if (((uint64_t)other.address + other.size) == ((uint64_t)address)) { return true; }
    return false;
}


void Allocator::Buffer::dump() const {
    fprintf(stderr, "\t%llu @ 0x%llx - 0x%llx\n", size, (uint64_t)address, (uint64_t)address+size);
}

#pragma mark -
#pragma mark Allocator

void Allocator::swap(Allocator& other) {
    using std::swap;
    if (this == &other) { return; }
    swap(_memoryManager,    other._memoryManager);
    swap(_firstPool,        other._firstPool);
    swap(_currentPool,      other._currentPool);
    swap(_allocatedBytes,   other._allocatedBytes);
    swap(_bestFit,          other._bestFit);
}

Allocator& Allocator::createAllocator() {
    MemoryManager memoryManager;
    Buffer buffer = memoryManager.vm_allocate_bytes(256*1024);
    AllocatorLayout* layout = new (buffer.address) AllocatorLayout();
    layout->init(256*1024);
    return layout->allocator();
}

Allocator& Allocator::stackAllocatorInternal(void* buffer, uint64_t size) {
    assert(buffer != nullptr);
    assert(size != 0);
    Buffer stackPool{ buffer, size };
    if (!stackPool.align(alignof(AllocatorLayout), sizeof(AllocatorLayout))) {
        assert(0);
    }
    AllocatorLayout* layout = new (stackPool.address) AllocatorLayout();
    layout->init(stackPool.size);
    return layout->allocator();
}

Allocator& Allocator::operator=(Allocator&& other) {
    _memoryManager  =   other._memoryManager;
    _firstPool      =   other._firstPool;
    _currentPool    =   other._currentPool;
    _allocatedBytes =   other._allocatedBytes;

    return *this;
}

void* Allocator::malloc(uint64_t size) {
    return this->aligned_alloc(kGranuleSize, size);
}

void* Allocator::aligned_alloc(uint64_t alignment, uint64_t size) {
#if !TARGET_OS_EXCLAVEKIT
    __unused auto lock = _memoryManager->lockGuard();
#endif
    assert(std::popcount(alignment) == 1); // Power of 2
    const uint64_t targetAlignment  = std::max<uint64_t>(16ULL, alignment);
    const uint64_t targetSize       = roundToNextAligned(targetAlignment, std::max<uint64_t>(size, 16ULL));
    void* result = nullptr;
    _memoryManager->requestedSize               = size;
    _memoryManager->requestedAlignment          = alignment;
    _memoryManager->requestedTargetSize         = targetSize;
    _memoryManager->requestedTargetAlignment    = targetAlignment;

    if (_bestFit) {
        result = _currentPool->aligned_alloc_best_fit(targetAlignment, targetSize);
    } else {
        result = _currentPool->aligned_alloc(targetAlignment, targetSize);
    }

    // No pools had enough space, allocate another pool
    if (!result) {
        uint64_t minPoolSize = roundToNextAligned<kPageSize>(2*sizeof(AllocationMetadata) + sizeof(Pool) + targetSize + targetAlignment);
        _currentPool->makeNextPool(this, std::max<uint64_t>(minPoolSize, 256*1024));
        _currentPool->nextPool()->validate();
        _currentPool = _currentPool->nextPool();
        result = _currentPool->aligned_alloc(targetAlignment, targetSize);
    }
    assert(result);
    _allocatedBytes += targetSize;
    ALLOCATOR_LOG("ALLOCATOR(0x%llx/%llu)\taligned_alloc: (%llu %% %llu) -> 0x%llx\n",
                  (uint64_t)this, _logID++, targetSize, targetAlignment, (uint64_t)result);
    ALLOCATOR_TRACE("void* alloc%llu = allocator.aligned_alloc(%llu, %llu);\n", (uint64_t)result, targetAlignment, targetSize);
    validate();
    return result;
}

void Allocator::freeObject(void* ptr) {
    if (!ptr) { return; }
    AllocationMetadata* metadata = AllocationMetadata::forPtr(ptr);
    metadata->pool()->allocator()->free(ptr);
}

void Allocator::free(void* ptr) {
#if !TARGET_OS_EXCLAVEKIT
    __unused auto lock = _memoryManager->lockGuard();
#endif
    if (!ptr) { return; }
    ALLOCATOR_LOG("ALLOCATOR(0x%llx/%llu)\tfree:          (0x%llx)\n", (uint64_t)this, +_logID++, (uint64_t)ptr);
    ALLOCATOR_TRACE("allocator.free(alloc%llu);\n", (uint64_t)ptr);
    AllocationMetadata* metadata = AllocationMetadata::forPtr(ptr);
    _allocatedBytes -= metadata->size();
    metadata->deallocate();
    validate();
}

void Allocator::dump() const {
    for (auto pool = _firstPool;; pool = pool->nextPool()) {
        ALLOCATOR_LOG("DUMP:\t\tPOOL(0x%llx)\n", (uint64_t)pool);
        pool->dump();
        if (pool == _currentPool) { break; }
    }
}

bool Allocator::realloc(void* ptr, uint64_t size) {
#if !TARGET_OS_EXCLAVEKIT
    __unused auto lock = _memoryManager->lockGuard();
#endif
    if (!ptr) { return false; }
    AllocationMetadata* metadata = AllocationMetadata::forPtr(ptr);
    const uint64_t targetSize = (std::max<uint64_t>(size, 16ULL) + (15ULL) & -16ULL);
    const uint64_t currentSize = metadata->size();
    bool result = true;
    if (currentSize < targetSize) {
        result = metadata->consumeFromNext(targetSize);
    } else if (currentSize > targetSize) {
        metadata->returnToNext(targetSize);
    }
    if (result) {
        _allocatedBytes += (targetSize - currentSize);
    }
    ALLOCATOR_LOG("ALLOCATOR(0x%llx/%llu)\trealloc:       (0x%llx):  %llu -> %s)\n",
                  (uint64_t)this, _logID++, (uint64_t)ptr, targetSize, result ? "true" : "false");
    ALLOCATOR_TRACE("allocator.realloc(alloc%llu, %llu);\n", (uint64_t)ptr, targetSize);
    validate();
    return result;
}

char* Allocator::strdup(const char* str)
{
    size_t len    = strlen(str);
    char*  result = (char*)this->malloc(len+1);
    strlcpy(result, str, len+1);
    return result;
}

bool Allocator::owned(const void* p, uint64_t nbytes) const {
    for (auto pool = _currentPool; pool != nullptr; pool = pool->prevPool()) {
        Buffer objectBuffer{ (void*)p, nbytes };
        if (pool->poolBuffer().contains(objectBuffer)) {
            return true;
        }
    }
    return false;
}

uint64_t Allocator::size(const void* ptr) const {
    if (!ptr) { return 0; }
    AllocationMetadata* metadata = AllocationMetadata::forPtr((void*)ptr);
    return metadata->size();
}


#if !BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
Allocator& Allocator::defaultAllocator() {
    static os_unfair_lock_s unfairLock = OS_UNFAIR_LOCK_INIT;
    static Allocator* allocator = nullptr;
    static dispatch_once_t onceToken;

    dispatch_once(&onceToken, ^{
        Lock lock(nullptr, &unfairLock);
        allocator = &Allocator::createAllocator();
        allocator->memoryManager()->adoptLock(std::move(lock));
    });
    return *allocator;
}
#endif

uint64_t Allocator::allocated_bytes() const {
    return _allocatedBytes;
}

Allocator::Allocator(MemoryManager& memoryManager, Pool& pool) :
    _memoryManager(&memoryManager), _firstPool(&pool), _currentPool(&pool), _allocatedBytes(0) {}

Allocator::~Allocator() {
    forEachVMAllocatedBuffer(^(const Buffer& buffer) {
        memoryManager()->vm_deallocate_bytes((void*)buffer.address, buffer.size);
    });
}

void Allocator::forEachVMAllocatedBuffer(void (^callback)(const Buffer&)) {
    for (auto pool = _currentPool; pool != nullptr; pool = pool->prevPool()) {
        Buffer poolObjectBuffer{ (void*)pool, sizeof(Pool) };
        if (!pool->poolBuffer().contains(poolObjectBuffer)) {
            callback({(void*)pool->poolBuffer().address, pool->poolBuffer().size});
        }
    }
}

MemoryManager* Allocator::memoryManager() const {
    return _memoryManager;
}

#pragma mark -
#pragma mark Allocator Pool

Allocator::Pool::Pool(Allocator* allocator, Pool* prevPool, uint64_t size)
: Pool(allocator, prevPool, allocator->memoryManager()->vm_allocate_bytes(size)) {}
Allocator::Pool::Pool(Allocator* allocator, Pool* prevPool, Buffer region) : Pool(allocator, prevPool, region, region) {}
Allocator::Pool::Pool(Allocator* allocator, Pool* prevPool, Buffer region, Buffer freeRegion)
: _allocator(allocator), _prevPool(prevPool), _poolBuffer(region) {
    assert(region.contains(freeRegion));
    freeRegion.size = freeRegion.size & ~0x0fUL;
    // Setup the metadata for the pool
    _lastFreeMetadata = new (freeRegion.address) AllocationMetadata(this, freeRegion.size);
    // Preallocate space for the next pool. This won't fail because the pool is new and large enough
    _nextPool = new (this->aligned_alloc(alignof(Pool), sizeof(Pool))) Pool();
}

void* Allocator::Pool::aligned_alloc(uint64_t alignment, uint64_t size) {
    assert(_lastFreeMetadata->pool() == this);
    // ALLOCATOR_LOG("aligned_alloc:\t\tPOOL(0x%llx) (%llu %% %llu)\n", (uint64_t)this, size, alignment);
    static_assert(sizeof(AllocationMetadata) <= kGranuleSize, "Ensure we can fit all metadata in a granule");
    Buffer freeBuffer = Buffer{ _lastFreeMetadata->firstAddress(), _lastFreeMetadata->size() };
    _lastFreeMetadata->validate();
    // _lastFreeMetadata->logAddressSpace("aligned_alloc");
    //ALLOCATOR_LOG("aligned_alloc:\t\t\t====================================================\n");
    // See if there is enough align the allocation and store a new metadata entry after it
    if (!freeBuffer.align(alignment, size+sizeof(AllocationMetadata))) {
        //ALLOCATOR_LOG("aligned_alloc:\t\t\t\tRETURN nullptr\n");
        return nullptr;
    }

    // We need to reserve some space to align the buffer,
    if (_lastFreeMetadata->firstAddress() != freeBuffer.address) {
        uint16_t alignmentSize = (uint64_t)freeBuffer.address - (uint64_t)_lastFreeMetadata->firstAddress() - kGranuleSize;
        _lastFreeMetadata->reserve(alignmentSize, false);
        // _lastFreeMetadata->logAddressSpace("aligned_alloc");
    }

    AllocationMetadata* reservedMetadata = _lastFreeMetadata;
    void* result = reservedMetadata->firstAddress();

    // Reserve the space
    _lastFreeMetadata->reserve(size, true);
    _lastFreeMetadata->validate();

    // Move the free space pointer to the new freespace's metadata
    //reservedMetadata->logAddressSpace("aligned_alloc");
    //_lastFreeMetadata->logAddressSpace("aligned_alloc");

    assert((uint64_t)result != (uint64_t)this);
    // ALLOCATOR_LOG("aligned_alloc:\t\t\t\tRETURN 0x%llx\n", (uint64_t)result);
    return result;
}

// An alternate aligned_alloc implementation for use in persistent pools where memory density matters
// The goal is for this algorithm to be very simple and reuse other parts of the allocator. As such it works like this:
// 1. It only workse with 16 byte aligned granules, anything that requires greater alignment goes to the normal path
// 2. It finds a slice of metadata which can hold the allocation, with as little extras space as possible
// 3. It marks the whole allocation as allocated
// 4. It reuses the code from realloc() (returnToNext()) to return the excess capacity back to the pool
void* Allocator::Pool::aligned_alloc_best_fit(uint64_t alignment, uint64_t size) {
    // This is only used for the persistent allocator to keep arrays from growing unbounded during dlopen(). No need to handle
    // complex cases
    if (alignment != kGranuleSize) {
        return aligned_alloc(alignment, size);
    }
    AllocationMetadata* candidateMetadata = nullptr;
    uint64_t candidateMetadataWastedBytes = ~0ULL;
    for (auto metadata = _lastFreeMetadata->previous(); metadata != nullptr; metadata = metadata->previous()) {
        if (metadata->allocated()) { continue; }
        if (metadata->size() < size) { continue; }
        uint64_t waste = metadata->size() - size;
        if (waste == 0) {
            candidateMetadata = metadata;
            break;
        } else if (waste < candidateMetadataWastedBytes) {
            candidateMetadata = metadata;
            candidateMetadataWastedBytes = waste;
        }
    }

    if (!candidateMetadata) {
        // We do not check the last metadata, which is what the default allocation policy uses, so call that
        return aligned_alloc(alignment,  size);
    }

    void* result = candidateMetadata->firstAddress();
    candidateMetadata->markAllocated();
    candidateMetadata->validate();
    if (candidateMetadata->size() > size) {
        candidateMetadata->returnToNext(size);
    }
    candidateMetadata->validate();

    // Move the free space pointer to the new freespace's metadata
    //reservedMetadata->logAddressSpace("aligned_alloc");
    //_lastFreeMetadata->logAddressSpace("aligned_alloc");

    assert((uint64_t)result != (uint64_t)this);
    // ALLOCATOR_LOG("aligned_alloc:\t\t\t\tRETURN 0x%llx\n", (uint64_t)result);
    return result;
}

void Allocator::Pool::free(void* ptr) {
    AllocationMetadata* metadata = AllocationMetadata::forPtr(ptr);
    metadata->deallocate();
}

void Allocator::Pool::makeNextPool(Allocator* allocator, uint64_t newPoolSize) {
    _nextPool = new (_nextPool) Pool(allocator, this, newPoolSize);
}

Allocator::Pool* Allocator::Pool::nextPool() const {
    return _nextPool;
}

Allocator::Pool* Allocator::Pool::prevPool() const {
    return _prevPool;
}
const MemoryManager::Buffer& Allocator::Pool::poolBuffer() const {
    return _poolBuffer;
}

Allocator* Allocator::Pool::allocator() const {
    return _allocator;
}

Allocator::Pool* Allocator::Pool::forPtr(void* ptr) {
    AllocationMetadata* metadata = AllocationMetadata::forPtr(ptr);
    return metadata->pool();
}

void Allocator::setBestFit(bool bestFit) {
    _bestFit = bestFit;
}


void Allocator::validate() const {
#if ALLOCATOR_VALIDATION
    for (auto pool = _firstPool; pool != _currentPool->nextPool(); pool = pool->nextPool()) {
        pool->validate();
    }
#endif
}

void Allocator::Pool::validate() const {
#if ALLOCATOR_VALIDATION
    bool shouldBeFree       = true;
    bool shouldBeAllocated  = false;
//    for (auto metadata = _lastFreeMetadata; metadata != nullptr; metadata = metadata->previous()) {
//        metadata->logAddressSpace("DUMP");
//    }
    for (auto metadata = _lastFreeMetadata; metadata != nullptr; metadata = metadata->previous()) {
        assert(this == metadata->pool());
        if (shouldBeFree) {
            assert(metadata->free());
            shouldBeFree = false;
            shouldBeAllocated = true;
        } else if (shouldBeAllocated) {
            assert(metadata->allocated());
            shouldBeAllocated = false;
        }
        if (metadata->free()) {
            shouldBeAllocated = true;
        }
        metadata->validate();
    }
#endif
}

void Allocator::Pool::dump() const {
    // Find the first free block. This is expensive, but only used in the debug path
    auto metadata = _lastFreeMetadata;
    while (metadata->previous() != nullptr) {
        metadata = metadata->previous();
    }
    while (metadata->next() != nullptr) {
        metadata->logAddressSpace("DUMP");
        metadata = metadata->next();
    }
    _lastFreeMetadata->logAddressSpace("DUMP");
}

#pragma mark -
#pragma mark Allocator Metadata


// This create a single metadata covering the entire space allocated for the pool, including the nmetadata tage itself
Allocator::AllocationMetadata::AllocationMetadata(Pool* pool, uint64_t size) {
    _prev = (uint64_t)pool | kPreviousBlockIsAllocatorFlag;
    _next = ((uint64_t)this + size) | kNextBlockLastBlockFlag;
}

// Unlike the previous method, this method accounts for the size of the metadata tag. That is is because when dealing with blocks
// in an already allocated zone that is much more natural
Allocator::AllocationMetadata::AllocationMetadata(AllocationMetadata *prev, uint64_t size, uint64_t flags, uint64_t prevFlags) {
    Pool* pool = prev->pool();
    assert(pool);

    // Point at the previous block
    _prev = (uint64_t)prev;

    if (flags & kNextBlockLastBlockFlag) {
        // There is no block after the new one, update the pool to indicate this is the new last metadata
        pool->_lastFreeMetadata = this;
    } else {
        // This is not the last block, update the next metadata's previous pointer to point to this metadata
        prev->next()->_prev = (uint64_t)this;
    }

    // Point the prvious block at this new block
    prev->_next = (uint64_t)this | prevFlags;
    _next = ((uint64_t)this + size + sizeof(AllocationMetadata)) | flags;
    setPoolHint(pool);

    if (!last()) {
        next()->_prev = (uint64_t)this;
    }

}

void Allocator::AllocationMetadata::setPoolHint(Pool* pool) {
    if (allocated()) { return; }
    if (this->size() < sizeof(Pool*)) { return; }

    // If there is enough room leave a pool reference so subseqeunt calls to Allocator::AllocationMetadata::pool can use it
    if (!pool) {
        pool = previous()->pool();
    }
    assert(pool);
//        fprintf(stderr, "SET HINT: 0x%lx -> 0x%lx\n", (uint64_t)this, (uint64_t)pool);
    Pool** poolHint = (Pool**)this->firstAddress();
    *poolHint = pool;
}

void* Allocator::AllocationMetadata::firstAddress() const {
    void* result = (void*)((uint64_t)this+sizeof(AllocationMetadata));
    return result;
}

void* Allocator::AllocationMetadata::lastAddress() const {
    return (void*)((uint64_t)firstAddress()+size());
}

uint64_t Allocator::AllocationMetadata::size() const {
    return ((_next & kNextBlockAddressMask) - ((uint64_t)this + sizeof(AllocationMetadata)));
}

void Allocator::AllocationMetadata::reserve(uint64_t size, bool allocated) {
    assert(free());
    uint64_t nextSize = (this->size()-(size+sizeof(AllocationMetadata)));
    void*   nextAddr = (void*)((uint64_t)this+sizeof(AllocationMetadata)+size);
    new (nextAddr) AllocationMetadata(this, nextSize, kNextBlockLastBlockFlag, (allocated ? kNextBlockAllocatedFlag : 0));
}

bool Allocator::AllocationMetadata::allocated() const {
    return (_next & kNextBlockAllocatedFlag);
}

bool Allocator::AllocationMetadata::free() const {
    return !allocated();
}

Allocator::AllocationMetadata* Allocator::AllocationMetadata::previous() const {
    if (_prev & kPreviousBlockIsAllocatorFlag) {
        // Low bit is one, this points to an allocator, not a metadata
        return nullptr;
    }
    return (AllocationMetadata*)_prev;
}

Allocator::AllocationMetadata* Allocator::AllocationMetadata::next() const {
    if (_next & kNextBlockLastBlockFlag) {
        return nullptr;
    }
    return (AllocationMetadata*)(_next & kNextBlockAddressMask);
}

bool Allocator::AllocationMetadata::last() const {
    return (AllocationMetadata*)(_next & kNextBlockLastBlockFlag);
}


Allocator::Pool* Allocator::AllocationMetadata::pool(bool useHints) const {
    auto metadata = this;
    for (; metadata->previous();  metadata = metadata->previous()) {
        if (useHints && metadata->free() && metadata->size() >= sizeof(Pool*)) {
            // This a free metadata large enough to hold a Pool*, there should be a hint waiting for us here. The one exception is
            // if we are in the middle of realign a block, in which case we may have overwritten it, in which case it will be null
            // and we need to continue searching.
            auto result = *(Pool**)metadata->firstAddress();
            if (result != nullptr) {
                return result;
            }
        }
    }
    return (Pool*)(metadata->_prev & kPreviousBlockAddressMask);
}

void Allocator::AllocationMetadata::coalesce(Pool* pool) {
    AllocationMetadata* currentMetadata = this;
    if (next() && next()->free()) {
        _next = next()->_next;
        // We only need to (and only can) update the previous entry in the next metadata if this is not the last free block. If it
        // is the last free block then trying to read the metadata past it will fault
        if (!currentMetadata->last()) {
            next()->_prev = (uint64_t)currentMetadata;
        }
    }
    // Next try to consolidate with the block immediately before this one if it is exists
    if (previous() && previous()->free()) {
        previous()->_next = _next;
        currentMetadata = previous();
        // We only need to (and only can) update the previous entry in the next metadata if this is not the last free block. If it
        // is the last free block then trying to read the metadata past it will fault
        if (!currentMetadata->last()) {
            next()->_prev = (uint64_t)currentMetadata;
        }
    }
    currentMetadata->setPoolHint(pool);

    // Finally update the free region if this was the last entry in the pool to reflect the new free memory available
    if (currentMetadata->last()) {
        // The last address of the consolidated metadata is the same as the last address of the free space, which means it
        // was consolidate with the end space, so lower the pools current free space pointer

        //uint64_t oldSize = (uint64_t)pool->_lastFreeMetadata-(uint64_t)pool->_poolBuffer.address;
        pool->_lastFreeMetadata = currentMetadata;
        //ALLOCATOR_LOG("NEW POOL SIZE: %llu -> %llu\n", oldSize, (uint64_t)currentMetadata-(uint64_t)pool->_poolBuffer.address);
    }
}

void Allocator::AllocationMetadata::deallocate() {
    assert(allocated());
    Pool* pool = this->pool();
    _next = (_next & kNextBlockAddressMask);
    // First try to consolidate with the block immediately after this one if it is exists
    coalesce(pool);
}

void Allocator::AllocationMetadata::markAllocated() {
    assert(!allocated());
    _next |= kNextBlockAllocatedFlag;
}

void Allocator::AllocationMetadata::returnToNext(uint64_t size) {
    Pool* pool = this->pool();
    uint64_t sizeReduction = this->size()-size;

    // Create a new block
    uint64_t nextSize = sizeReduction-sizeof(AllocationMetadata);
    void*   nextAddr = (void*)((uint64_t)this+sizeof(AllocationMetadata)+(this->size()-sizeReduction));
    new (nextAddr) AllocationMetadata(this, nextSize, 0, _next & ~kNextBlockAddressMask);
    next()->coalesce(pool);
}

bool Allocator::AllocationMetadata::consumeFromNext(uint64_t size) {
    if (next()->allocated()) {
        // No free space
        return false;
    }
    uint64_t requiredSize   = size-this->size();
    uint64_t nextSize       = next()->size();
    
    if (requiredSize <= nextSize) {
        // If the size we need is less than the size of the next block we can realloc() by moving the next metadata within the
        // the block.
        void* nextAddr = (void*)((uint64_t)this+sizeof(AllocationMetadata)+size);
        new (nextAddr) AllocationMetadata(this, nextSize-requiredSize, next()->_next & ~kNextBlockAddressMask, _next & ~kNextBlockAddressMask);
        return true;
    } else if (!next()->last() && (requiredSize == nextSize + sizeof(AllocationMetadata))) {
        // if we are not reallocating into the last entry we can get an extra sizeof(AllocationMetadata) by deleting the block
        // entirely and using the space from its metadata tag
        _next = next()->_next | kNextBlockAllocatedFlag;
        next()->_prev = (uint64_t)this;
        return true;
    }
    

    // TODO: handle the case where there is exactly enough space
    return false;
}

Allocator::AllocationMetadata* Allocator::AllocationMetadata::forPtr(void* ptr) {
    AllocationMetadata* castPtr = static_cast<AllocationMetadata*>(ptr);
    return castPtr-1;
}


void Allocator::AllocationMetadata::validate() const {
#if ALLOCATOR_VALIDATION
    assert(pool(true) == pool(false));
    if (!last()) {
        assert(next()->previous() == this);
    }
    if (previous()) {
        assert(previous()->next() == this);
    }
#endif
}

void Allocator::AllocationMetadata::logAddressSpace(const char* prefix) const {
    ALLOCATOR_LOG("%s:\t\t\tMETADATA(0x%llx) 0x%llx-0x%llx (%s%s)\n",
                  prefix, (uint64_t)this, (uint64_t)this, (uint64_t)this+sizeof(AllocationMetadata),
                  free() ? "free" : "allocated", last() ? "/last" : "");
    ALLOCATOR_LOG("%s:\t\t\t    DATA(0x%llx) 0x%llx-0x%llx (%lld bytes)",
                  prefix, (uint64_t)this, (uint64_t)firstAddress(), (uint64_t)this->lastAddress(), size());
    if (this->free() && !this->last() && this->size() >= kGranuleSize) {
        ALLOCATOR_LOG(" (pool hint: 0x%llx)\n", *(uint64_t*)firstAddress());
    } else {
        ALLOCATOR_LOG("\n");
    }

}

#pragma mark -
#pragma mark Allocator Layout

void lsl::AllocatorLayout::init(uint64_t size, const char** envp, const char** apple, void* dyldSharedCache) {
    MemoryManager::Buffer buffer{(void*)this, size};
    MemoryManager::Buffer freeSpace = buffer;
    freeSpace.consumeSpace(sizeof(AllocatorLayout));
    configure(envp, apple, dyldSharedCache);
    new ((void*)&_pool) Allocator::Pool(&_allocator, nullptr, buffer, freeSpace);
    new ((void*)&_allocator) Allocator(_memoryManager, _pool);
}

void lsl::AllocatorLayout::configure(const char** envp, const char** apple, void* dyldSharedCache) {
    if (envp && apple) {
        new (&_memoryManager) MemoryManager(envp, apple, dyldSharedCache);
    } else {
        const char* emptyApple[1];
        emptyApple[0] = nullptr;
        new ((void*)&_memoryManager) MemoryManager(emptyApple, emptyApple, dyldSharedCache);
    }
}

Allocator& lsl::AllocatorLayout::allocator() {
    return _allocator;
}

uint64_t lsl::AllocatorLayout::minSize() {
    // Returns the minimum size necessary to alloca for this struct. Includes:
    // 1 the struct
    // 2 the alignment padding
    // 3 Space for the initial overflow pool
    // 4 Space for the metadata to trace the allocation for that pool
    return sizeof(lsl::AllocatorLayout) + alignof(lsl::AllocatorLayout)
                + sizeof(Allocator::Pool) + sizeof(Allocator::AllocationMetadata);
}

};

