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
#include "StringUtils.h"


#if !TARGET_OS_EXCLAVEKIT
#include "DyldRuntimeState.h"
#endif // !TARGET_OS_EXCLAVEKIT

// TODO: Reenable ASAN support once we have time to debug it

#if !BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
#include <dispatch/dispatch.h>
#endif

#if BUILDING_LIBDYLD || BUILDING_LIBDYLD_INTROSPECTION
extern "C" VIS_HIDDEN void __cxa_pure_virtual(void);
void __cxa_pure_virtual()
{
    abort();
}
#endif


#define ALLOCATOR_LOGGING_ENABLED (0)

#if BUILDING_DYLD && TARGET_OS_OSX && __x86_64__
extern "C" void* __dso_handle;
#endif

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

EphemeralAllocator::EphemeralAllocator() {
    MemoryManager memoryManager;
    _memoryManager = &memoryManager;
    _memoryManager = new (this->aligned_alloc(alignof(MemoryManager), sizeof(MemoryManager))) MemoryManager(std::move(memoryManager));    // Don't count the space used by the MemoryManager
    _allocatedBytes = 0;
}

EphemeralAllocator::EphemeralAllocator(MemoryManager& memoryManager) : _memoryManager(&memoryManager) {}

EphemeralAllocator::EphemeralAllocator(void* B, uint64_t S) : _freeBuffer({B,S}) {
    MemoryManager memoryManager;
    _memoryManager = &memoryManager;
    _memoryManager = new (this->aligned_alloc(alignof(MemoryManager), sizeof(MemoryManager))) MemoryManager(std::move(memoryManager));
    // Don't count the space used by the MemoryManager
    _allocatedBytes = 0;
}
EphemeralAllocator::EphemeralAllocator(void* B, uint64_t S, MemoryManager& memoryManager) : _memoryManager(&memoryManager), _freeBuffer({B,S}){}

EphemeralAllocator::EphemeralAllocator(EphemeralAllocator&& other) {
    swap(other);
}

EphemeralAllocator::~EphemeralAllocator() {
    reset();
}

EphemeralAllocator& EphemeralAllocator::operator=(EphemeralAllocator&& other) {
    swap(other);
    return *this;
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

void EphemeralAllocator::swap(EphemeralAllocator& other) {
    using std::swap;
    if (this == &other) { return; }
    swap(_memoryManager,        other._memoryManager);
    swap(_freeBuffer.size,      other._freeBuffer.size);
    swap(_freeBuffer.address,   other._freeBuffer.address);
    swap(_regionList,           other._regionList);
    swap(_allocatedBytes,       other._allocatedBytes);
}

void EphemeralAllocator::reset() {
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // HACK: The cache builder doesn't free allocations in the ProcessConfig
#else
    // FIXME: re-enable this assertion once we figure out the problem with the builders.
    // assert(_allocatedBytes == 0);
#endif
    if (_regionList != nullptr) {
        void* cleanupSpace = alloca(1024);
        EphemeralAllocator cleanupAllocator(cleanupSpace, 1024);
        Vector<Buffer> regions(cleanupAllocator);
        for (auto i = _regionList; i != nullptr; i = i->next) {
            regions.push_back(i->buffer);
        }
        for(auto& region : regions) {
            _memoryManager->vm_deallocate_bytes(region.address, region.size);
        }
        _regionList = nullptr;
        _freeBuffer = { nullptr, 0 };
    }
}

[[nodiscard]] Allocator::Buffer EphemeralAllocator::allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) {
    assert(prefix == 16 || prefix == 0);
    // First space for the prefix
    *((uint64_t*)&_freeBuffer.address) += prefix;
    _freeBuffer.size -= prefix;
    if ((_freeBuffer.size == 0 - prefix) || (!_freeBuffer.align(alignment, nbytes))) {
#if __LP64__
        uint64_t size = std::max<uint64_t>(4*nbytes, EPHEMERAL_ALLOCATOR_DEFAULT_POOL_SIZE);
#else
        uint64_t size = std::max<uint64_t>(nbytes+65536, EPHEMERAL_ALLOCATOR_DEFAULT_POOL_SIZE);
#endif
        _freeBuffer = _memoryManager->vm_allocate_bytes(size);
//        ASAN_UNPOISON_MEMORY_REGION(_freeBuffer.address, sizeof(RegionListEntry));
        _regionList = new (_freeBuffer.address) RegionListEntry({ _freeBuffer, _regionList});
        uint64_t roundedSize = prefix + ((sizeof(RegionListEntry) + 15) & (-16));
        *((uint64_t*)&_freeBuffer.address) += roundedSize;
        _freeBuffer.size -= roundedSize;
        _freeBuffer.align(alignment, nbytes);
    }
    assert((uint64_t)_freeBuffer.address%16 == 0);

    Allocator::Buffer result = { (void*)((uint64_t)_freeBuffer.address-prefix), nbytes+prefix };
    *((uint64_t*)&_freeBuffer.address) += nbytes;
    _freeBuffer.size -= nbytes;
    _allocatedBytes += (nbytes + prefix);

//    fprintf(stderr, "%llu @ 0x%lx(%llx) Allocated\n", result.size, (uint64_t)result.address, (uint64_t)this);
//    fprintf(stderr, "SPACE: %lu, 0x%lx\n", _freeBuffer.size, (uint64_t)_freeBuffer.address);
    return result;
}

void EphemeralAllocator::deallocate_buffer(Buffer buffer) {
//    fprintf(stderr, "%llu @ 0x%lx(%llx) Deallocated\n", buffer.size, (uint64_t)buffer.address, (uint64_t)this);
    _allocatedBytes -= buffer.size;
}

uint64_t EphemeralAllocator::allocated_bytes() const {
    return _allocatedBytes;
}

uint64_t EphemeralAllocator::vm_allocated_bytes() const {
    std::uint64_t result = 0;
    for (auto i = _regionList; i != nullptr; i = i->next) {
        result += i->buffer.size;
    }
    return result;
}

bool EphemeralAllocator::owned(const void* p, uint64_t nbytes) const {
    for (auto i = _regionList; i != nullptr; i = i->next) {
        if (i->buffer.contains({(void*)p,nbytes})) { return true; }
    }
    return false;
}

void EphemeralAllocator::destroy() {
    contract(_allocatedBytes == 0);
}

AllocationMetadata::AllocationMetadata(Allocator* A, uint64_t S) : _type(NormalPtr) {
    _allocator = (uint64_t)A>>3;
    for (const auto& granule : granules) {
        uint64_t nextGranuleSize = 1ULL<<(granule+11);
        if (S < nextGranuleSize) {
            _sizeClass = &granule - &granules[0];
            _size = (uint32_t)(S >> granule);
            contract(_size < 1ULL<<11);
            break;
        }
    }
}

uint64_t AllocationMetadata::goodSize(uint64_t S) {
    uint64_t sizeClass = 0;
    for (const auto& granule : granules) {
        uint64_t nextGranuleSize = 1ULL<<(granule+11);
        if (S <= nextGranuleSize) {
            sizeClass = &granule - &granules[0];
            break;
        }
    }
    uint64_t result = (uint64_t)(S + ((1ULL<<granules[sizeClass])-1)) & (-1*(1ULL<<granules[sizeClass]));
    return result;
}

Allocator& AllocationMetadata::allocator() const {
    auto result = (Allocator*)(_allocator<<3);
//    fprintf(stderr, "0x%lx\tgot\t0x%lx\n", (uint64_t)this, (uint64_t)result);
    return *result;
}

uint64_t AllocationMetadata::size() const {
    return _size<<granules[_sizeClass];
}

AllocationMetadata::Type AllocationMetadata::type() const {
    return (Type)_type;
}

void AllocationMetadata::setType(Type type) {
    _type = (uint64_t)type;
}

AllocationMetadata* AllocationMetadata::getForPointer(void* data) {
    contract(data != nullptr);
    return (AllocationMetadata*)((uint64_t)data-Allocator::kGranuleSize);
}

void AllocationMetadata::freeObject() {
    void* object = (void*)&this[1];
    allocator().free(object);
};

void AllocationMetadata::incrementRefCount() {
    contract(type() == SharedPtr);
    //TODO: replace with std::atomic_ref
    // THe issue here is that we don't want to declare _refCount as a std::atomic<uint32_t> because then we will pay the cost
    // of the atomic intializer for every malloc(), even though we only care about it in the rare case where there is a SharedPtr.
    // Ideally we would just cast the fields to atomics, but that does not work and std::atomic_ref is not supported yet. We also
    // can't use C atomics because <atomic> and <stdatomic.h> are not compatible, so that leaves us with going straight down to the
    // clang intrinsics for now.
    __c11_atomic_fetch_add((_Atomic uint32_t*)&_refCount, 1, __ATOMIC_RELAXED);
}

bool AllocationMetadata::decrementRefCount() {
    contract(type() == SharedPtr);
    if (__c11_atomic_fetch_sub((_Atomic uint32_t*)&_refCount, 1, __ATOMIC_ACQ_REL) == 0) {
        return true;
    }
    return false;
}


void AllocationMetadata::incrementWeakRefCount() {
    contract(type() == SharedPtr);
    __c11_atomic_fetch_add((_Atomic uint32_t*)&_weakRefCount, 1, __ATOMIC_RELAXED);
}

bool AllocationMetadata::decrementWeakRefCount() {
    contract(type() == SharedPtr);
    if (__c11_atomic_fetch_sub((_Atomic uint32_t*)&_weakRefCount, 1, __ATOMIC_ACQ_REL) == 0) {
        return true;
    }
    return false;
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

void Allocator::Buffer::remainders(const Buffer& other, Buffer& prolog, Buffer& epilog) const {
    contract(contains(other));
    if (((uint64_t)address) < (uint64_t)other.address) {
        prolog.address = address;
        prolog.size = (uint64_t)other.address - ((uint64_t)address);
    }
    if (((uint64_t)address+size) > (uint64_t)other.address+other.size) {
        epilog.address = (void*)((uint64_t)other.address+other.size);
        epilog.size = ((uint64_t)address+size) - ((uint64_t)other.address+other.size);
    }
}

Allocator::Buffer Allocator::Buffer::findSpace(uint64_t targetSize, uint64_t targetAlignment, uint64_t prefix) const {
    Buffer result = *this;
    result.address = (void*)((uint64_t)result.address + prefix);
    result.size -= prefix;
    if (result.align(targetAlignment, targetSize)) {
        result.address = (void*)((uint64_t)result.address - prefix);
        result.size = (targetSize + prefix);
        return result;
    }
    return {nullptr , 0};
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
    printf("\t%llu @ 0x%llx - 0x%llx\n", size, (uint64_t)address, (uint64_t)address+size);
}

#pragma mark -
#pragma mark Primitive allocator implementations

#if TARGET_OS_EXCLAVEKIT
// ExclaveKit specific page allocator - for now, let's use a fixed-size static arena.
static char page_alloc_arena[34 * 0x4000] __attribute__((aligned(PAGE_SIZE)));
static uint64_t page_alloc_arena_used = 0;

[[nodiscard]] void* MemoryManager::allocate_pages(uint64_t size) {

    uint64_t targetSize = (size + (PAGE_SIZE-1)) & (-1*PAGE_SIZE);
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
    uint64_t targetSize = (size + (PAGE_SIZE-1)) & (-1*PAGE_SIZE);
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

#if TARGET_OS_OSX && BUILDING_DYLD && __x86_64__
[[nodiscard]] MemoryManager::Buffer MemoryManager::vm_allocate_bytes(uint64_t size) {
    // Only do this on macOS for now due to qualification issue in embedded simulators
    static const uint64_t kMOneMegabyte = 0x0100000;
    // We allocate an extra page to use as a guard page
    uint64_t targetSize = ((size + (PAGE_SIZE-1)) & (-1*PAGE_SIZE)) + PAGE_SIZE;
#if __LP64__
    mach_vm_address_t result = 0x0100000000;                    // Set to 4GB so that is the first eligible address
#else
    mach_vm_address_t result = 0;
#endif
    kern_return_t kr = mach_vm_map(mach_task_self(),
                                   &result,
                                   targetSize,
                                   kMOneMegabyte - 1,                  // This mask guarantees 1MB alignment
                                   VM_FLAGS_ANYWHERE | vmFlags(),
                                   MEMORY_OBJECT_NULL,          // Allocate memory instead of using an existing object
                                   0,
                                   FALSE,
                                   VM_PROT_READ | VM_PROT_WRITE,
                                   VM_PROT_ALL,                 // Needs to VM_PROT_ALL for libsyscall glue to pass via trap
                                   VM_INHERIT_DEFAULT);         // Needs to VM_INHERIT_DEFAULT for libsyscall glue to pass via trap
    if (kr != KERN_SUCCESS) {
        // Fall back to vm_allocate() if mach_vm_map() fails. That can happen due to sandbox, or when running un the simulator
        // on an older host. Technically this is not guaranteed to be above 4GB, but since it requires manually configuring a zero
        // page to be below 4GB it is safe to assume processes that need it will also setup their sandbox properly so that
        // mach_vm_map() works.

        // We also need to allocate an extra 1MB so we can align it to 1MB
        kr = vm_allocate(mach_task_self(), (vm_address_t*)&result, targetSize + kMOneMegabyte, VM_FLAGS_ANYWHERE | vmFlags());
        if (kr == KERN_SUCCESS) {
            mach_vm_address_t alignedResult = (result + kMOneMegabyte - 1) & -1*(kMOneMegabyte);
            if (alignedResult != result) {
                (void)vm_deallocate(mach_task_self(), (vm_address_t)result,
                                    (vm_size_t)(alignedResult - result));
            }
            (void)vm_deallocate(mach_task_self(), (vm_address_t)(alignedResult+targetSize),
                                (vm_size_t)((result+targetSize+kMOneMegabyte) - (alignedResult+targetSize)));
            result = alignedResult;
        }
    }

    if (kr != KERN_SUCCESS) {
        return {nullptr, 0};
    }

    // Remove the guard page
    targetSize -= PAGE_SIZE;

    // Force accesses to the guard page to fault
    (void)vm_protect(mach_task_self(), (vm_address_t)result+targetSize, PAGE_SIZE, true, VM_PROT_NONE);

//    ASAN_POISON_MEMORY_REGION((void*)result, targetSize);
//    fprintf(stderr, "0x%lx - 0x%lx\t  VM_ALLOCATED\n", (uint64_t)result, (uint64_t)result+targetSize);
    return {(void*)result, targetSize};
}
#else /* TARGET_OS_OSX && BUILDING_DYLD && __x86_64__ */
[[nodiscard]] MemoryManager::Buffer MemoryManager::vm_allocate_bytes(uint64_t size) {
    uint64_t targetSize = ((size + (PAGE_SIZE-1)) & (-1*PAGE_SIZE)) + PAGE_SIZE;
    vm_address_t    result;
    // We allocate an extra page to use as a guard page
    kern_return_t kr = vm_allocate(mach_task_self(), &result, (vm_size_t)targetSize, VM_FLAGS_ANYWHERE | vmFlags());
#if BUILDING_DYLD && TARGET_OS_OSX && __x86_64__
    // rdar://79214654 support wine games that need low mem.  Move dyld heap out of low mem
    if ( (kr == KERN_SUCCESS) && (result < 0x100000000ULL) ) {
        vm_address_t result2 = (long)&__dso_handle + 0x00200000; // look for vm range after dyld
        kern_return_t kr2 = vm_allocate(mach_task_self(), &result2, targetSize, VM_FLAGS_FIXED | vmFlags());
        if ( kr2 == KERN_SUCCESS ) {
            (void)vm_deallocate(mach_task_self(), result, targetSize);
            result = result2;
        }
    }
#endif /* BUILDING_DYLD && TARGET_OS_OSX && __x86_64__ */

    // Remove the guard page
    targetSize -= PAGE_SIZE;

    if (kr != KERN_SUCCESS) {
        char buffer[1024];
        char intStrBuffer[130];
        bytesToHex((const uint8_t*)&size, sizeof(uint64_t ), intStrBuffer);
        strlcpy(&buffer[0], "Could not vm_allocate 0x", 1024);
        strlcat(&buffer[0], intStrBuffer, 1024);
        strlcat(&buffer[0], " bytes (kr: 0x", 1024);
        bytesToHex((const uint8_t*)&kr, sizeof(kern_return_t), intStrBuffer);
        strlcat(&buffer[0], intStrBuffer, 1024);
        strlcat(&buffer[0], ")", 1024);
        CRSetCrashLogMessage2(buffer);
        assert(0 && "vm_allocate failed");
        return {nullptr, 0};
    }

    // Force accesses to the guard page to fault
    (void)vm_protect(mach_task_self(), (vm_address_t)(result+targetSize), PAGE_SIZE, true, VM_PROT_NONE);

    ASAN_POISON_MEMORY_REGION((void*)result, targetSize);
//    fprintf(stderr, "0x%lx - 0x%lx\t  VM_ALLOCATED\n", (uint64_t)result, (uint64_t)result+targetSize);
    return {(void*)result, targetSize};
}
#endif /* TARGET_OS_OSX && BUILDING_DYLD && __x86_64__ */

void MemoryManager::vm_deallocate_bytes(void* p, uint64_t size) {
//    fprintf(stderr, "0x%lx - 0x%lx\tVM_DEALLOCATED\n", (uint64_t)p, (uint64_t)p+size);
    //FIXME: We need to unpoison memory here because the same addresses can be allocated by libraries and passed back to us later
    //FIXME: We can remove this hack if we do somehting like interpose vm_allocate and track allocations there
    ASAN_UNPOISON_MEMORY_REGION(p, size);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)p, (vm_size_t)size + PAGE_SIZE);
}
#endif // TARGET_OS_EXCLAVEKIT


[[nodiscard]] Allocator::Buffer Allocator::allocate_buffer(uint64_t nbytes, uint64_t alignment) {
    uint64_t targetAlignment = std::max<uint64_t >(16ULL, alignment);
    uint64_t targetSize = (std::max<uint64_t >(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
    targetSize = AllocationMetadata::goodSize(targetSize);
    auto result = allocate_buffer(targetSize, targetAlignment, 0);
//    fprintf(stderr, "0x%lx - 0x%lx\t     ALLOCATED (tid: %u)\n", (uint64_t)result.address, (uint64_t)result.address+result.size, mach_thread_self());
//    ASAN_UNPOISON_MEMORY_REGION(result.address, result.size);
    return result;
}

void Allocator::deallocate_buffer(void* p, uint64_t nbytes, uint64_t alignment) {
    const uint64_t targetAlignment = std::max<uint64_t >(16ULL, alignment);
    uint64_t targetSize = (std::max<uint64_t >(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
//    ASAN_POISON_MEMORY_REGION(p, targetSize);
//    fprintf(stderr, "0x%lx - 0x%lx\t   DEALLOCATED (tid: %u)\n", (uint64_t)p, (uint64_t)p+targetSize, mach_thread_self());
    deallocate_buffer({p, targetSize});
}

void* Allocator::malloc(uint64_t size) {
    void* result = this->aligned_alloc(kGranuleSize, size);
//    fprintf(stderr, "MALLOC(0x%lx)\n", (uint64_t)result);
    return result;
}

void* Allocator::aligned_alloc(uint64_t alignment, uint64_t size) {
    static_assert(kGranuleSize >= (sizeof(uint64_t) == sizeof(Allocator*)), "Ensure we can fit all metadata in a granule");
    const uint64_t targetAlignment = std::max<uint64_t>(16ULL, alignment);
    uint64_t targetSize = (std::max<uint64_t>(size, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
    targetSize = AllocationMetadata::goodSize(targetSize);
    
    auto buffer = allocate_buffer(targetSize, targetAlignment, 16);
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, buffer.size);
    contract(buffer.address != nullptr);
    // We are guaranteed a 1 granule managed we can use for storage;
    //    fprintf(stderr, "(tid 0x%lx)\t0x%lx\tstashing\t0x%lx\n", mach_thread_self(), (uint64_t)buffer.address, (uint64_t)this);
    (void)new (buffer.address) AllocationMetadata(this, buffer.size-kGranuleSize);
//    fprintf(stderr, "aligned_alloc\t0x%lx\t%lu\t%lu\n", (uint64_t)buffer.address+kGranuleSize, size, alignment);
//    fprintf(stderr, "ALIGNED_ALLOC(0x%lx): %llu\n", (uint64_t)buffer.address+kGranuleSize, buffer.size-kGranuleSize);
    return (void*)((uint64_t)buffer.address+kGranuleSize);
}

void Allocator::free(void* ptr) {
//    fprintf(stderr, "FREE(0x%lx)\n", (uint64_t)ptr);
    contract((uint64_t)ptr%16==0);
    if (!ptr) { return; }
    // We are guaranteed a 1 granule prefix we can use for storage
    auto metadata = AllocationMetadata::getForPointer(ptr);
//    fprintf(stderr, "free\t0x%lx\t%lu\n", (uint64_t)ptr, metadata->size());
    metadata->allocator().deallocate_buffer((void*)((uint64_t)ptr-kGranuleSize), (uint64_t)metadata->size()+kGranuleSize, kGranuleSize);
}

char* Allocator::strdup(const char* str)
{
    size_t len    = strlen(str);
    char*  result = (char*)this->malloc(len+1);
    strlcpy(result, str, len+1);
    return result;
}

#pragma mark -
#pragma mark Persistent Allocator

struct VIS_HIDDEN PersistentAllocator : Allocator {
    PersistentAllocator(const Buffer& B, MemoryManager&& memoryManager);
    bool            owned(const void* p, uint64_t nbytes) const override;
    void            debugDump() const override;
    void            validate() const override;
    void            destroy() override;
    uint64_t        allocated_bytes() const override;
    uint64_t        vm_allocated_bytes() const override;
    MemoryManager*  memoryManager() override;

//    void operator delete  ( void* ptr, std::align_val_t al ) {
//        // Do nothing here, deletion is handled manually
//    }
    friend void swap(PersistentAllocator& x, PersistentAllocator& y) {
        x.swap(y);
    }
protected:
    Buffer  allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) override;
    void    deallocate_buffer(Buffer buffer) override;
    void    deallocate_buffer_safe(Buffer buffer, bool internal);
private:
    friend struct Allocator;
    friend struct MemoryManager;

    void    processDeallocations(Buffer* begin, Buffer* end);
    void    swap(PersistentAllocator& other);
    void    addToFreeBlockTrees(Buffer buffer);
    PersistentAllocator& operator=(PersistentAllocator&& other);
    struct RegionSizeCompare {
        bool operator() (const Buffer& x, const Buffer& y) const {
            if (x.size == y.size) { return x.address < y.address; }
            return x.size < y.size;
        }
    };
    struct RegionAddressCompare {
        bool operator() (const Buffer& x, const Buffer& y) const {
            if (x.address == y.address) { return x.size < y.size; }
            return x.address < y.address;
        }
    };

    void reloadMagazine();
    void reserveRange(BTree<Buffer, RegionSizeCompare>::iterator& i, Buffer buffer);
    // This is a special private allocator used for the collection classes used in the longterm allocator. It is a refillable magazine
    // that we pass into those collections so they never reenter the allocator and then themselves. It is the responsbility of the
    // longterm allocator to make sure the magazine always has enough allocations available to get to the next point where it is safe to
    // refill it
    template<uint32_t S, uint32_t A>
    struct MagazineAllocator : Allocator {
        MagazineAllocator(PersistentAllocator& allocator) : _persistentAllocator(allocator) {}
        Buffer allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) override {
            assert(_magazine[0].size != 0);
            contract(nbytes == S);
            contract(alignment == A);
            auto result = _magazine[0];
            result.size = S;
            _magazine[0].size -= S;
            _magazine[0].address = (void*)((uint64_t)_magazine[0].address + S);
            if (_magazine[0].size == 0) {
                std::copy(&_magazine[1], _magazine.end(), &_magazine[0]);
                _magazine[3] = {nullptr, 0};
                --_magazineDepth;
            }
            return result;
        }
        bool    owned(const void* p, uint64_t nbytes) const override { return false; }
        void    deallocate_buffer(Buffer buffer) override {
            _persistentAllocator.deallocate_buffer_safe(buffer, true);
        }
        uint64_t  allocated_bytes() const override { return 0; }
        uint64_t  vm_allocated_bytes() const override { return 0; }
        void    destroy() override {}

        void refill(Buffer buffer) {
            assert(buffer.size > 0);
            assert((uint64_t)buffer.address > 0);
            contract(buffer.size%S == 0);
            contract(((uint64_t)buffer.address)%A == 0);
            contract(_magazineDepth != 4);
            _magazine[_magazineDepth++] = buffer;
        }
        size_t size() const {
            size_t result = 0;
            for (auto i = 0; i < _magazineDepth; ++i) {
                result += _magazine[i].size/S;
            }
            return result;
        }
        friend void swap(MagazineAllocator& x, MagazineAllocator& y) {
            x.swap(y);
        }
    private:
        void swap(MagazineAllocator& other) {
            using std::swap;
            swap(_magazine,        other._magazine);
            swap(_magazineDepth,   other._magazineDepth);
        }
        // We can prove the maximumn number of entries necessary for the magazine is 4 and
        std::array<Buffer, 4>   _magazine = {};
        uint8_t                 _magazineDepth = 0;
        PersistentAllocator&    _persistentAllocator;
    };
    struct DeallocationRecord {
        DeallocationRecord(uint64_t S) : size(S) {}
        uint64_t            size;
        DeallocationRecord* next;
    };
    BTree<Buffer, RegionAddressCompare>         _regionList             = BTree<Buffer, RegionAddressCompare>(_magazine);
    BTree<Buffer, RegionAddressCompare>         _freeAddressHash        = BTree<Buffer, RegionAddressCompare>(_magazine);
    BTree<Buffer, RegionSizeCompare>            _freeSizeHash           = BTree<Buffer, RegionSizeCompare>(_magazine);
    MagazineAllocator<256,256>                  _magazine               = MagazineAllocator<256,256>(*this);
    std::atomic<uint64_t>                       _allocatedBytes         = 0;
    DeallocationRecord*                         _deallocationChian      = nullptr;
    bool                                        _useHWTPro              = false;
    MemoryManager*                              _memoryManager          = nullptr;
};

#if !BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
Allocator& Allocator::defaultAllocator() {
    static os_unfair_lock_s unfairLock = OS_UNFAIR_LOCK_INIT;
    static Allocator* allocator = nullptr;
    static dispatch_once_t onceToken;

    dispatch_once(&onceToken, ^{
        Lock lock(nullptr, &unfairLock);
        MemoryManager bootStapMemoryManager(std::move(lock));
        allocator = &Allocator::persistentAllocator(std::move(bootStapMemoryManager));
    });
    return *allocator;
}
#endif

uint64_t PersistentAllocator::allocated_bytes() const {
    return _allocatedBytes;
}

uint64_t PersistentAllocator::vm_allocated_bytes() const {
    uint64_t result = 0;
    for (auto& region : _regionList) {
        result +=  region.size;
    }
    return result;
}

MemoryManager* PersistentAllocator::memoryManager() {
    return _memoryManager;
}

PersistentAllocator::PersistentAllocator(const Buffer& buffer, MemoryManager&& memoryManager) {
    static_assert(sizeof(BTree<Buffer, std::less<Buffer>>::Node) == 256, "Nodes for btrees used in allocators must be 256 bytes");
    static_assert(sizeof(BTree<Buffer, RegionSizeCompare>::Node) == 256, "Nodes for btrees used in allocators must be 256 bytes");

    // First set the memoryManager via a pointer so it can be used during boostrap
    _memoryManager = &memoryManager;

    // Round and align the free space appropriate
    auto roundedSize = ((sizeof(PersistentAllocator) + 255) & (-256));
    size_t magazineSize = 12*256;
    Buffer magazineStorage = { (void*)((uint64_t)buffer.address + roundedSize), magazineSize};
    _magazine.refill(magazineStorage);
    Buffer freespace = { (void*)((uint64_t)buffer.address + roundedSize + magazineSize), buffer.size - (roundedSize + magazineSize)};

    // Insert the freesapce into the allocator
    _regionList.insert(buffer);
    _freeSizeHash.insert(freespace);
    _freeAddressHash.insert(freespace);

    // Next reassign it via move construction so the lock manager lives in the persistent allocator
    _memoryManager = new (this->aligned_alloc(alignof(MemoryManager), sizeof(MemoryManager))) MemoryManager(std::move(memoryManager));
    _memoryManager->_allocator = this;

    // Reset the allocated bytes so the embedded MemoryManager is not counted against destroying the zone
    _allocatedBytes = 0;
}


void PersistentAllocator::debugDump() const {
    fprintf(stderr, "_regionList\n");
    for (const auto& region : _regionList) {
        fprintf(stderr, "\t%llu @ 0x%llx\n", region.size, (uint64_t)region.address);
    }
    fprintf(stderr, "_freeSizeHash\n");
    for (const auto& region : _freeSizeHash) {
        fprintf(stderr, "\t%llu @ 0x%llx\n", region.size, (uint64_t)region.address);
    }
    fprintf(stderr, "_freeAddressHash\n");
    for (const auto& region : _freeAddressHash) {
        fprintf(stderr, "\t0x%llx: %llu\n", (uint64_t)region.address, region.size);
    }
}

void PersistentAllocator::validate() const {
#if PERSISTENT_ALLOCATOR_VALIDATION
    _regionList.validate();
    _freeSizeHash.validate();
    _freeAddressHash.validate();
    for (const auto& region : _freeSizeHash) {
        if (_freeAddressHash.find(region) == _freeAddressHash.end()) {
            fprintf(stdout, "REGION MISSING(addr) %llu, 0x%llx\n", region.size, (uint64_t)region.address);
            debugDump();
            abort();
        }
    }
    Buffer last = { nullptr, 0 };
    for (const auto& region : _freeAddressHash) {
        if (last) {
            if (((uint64_t)last.address + last.size) >= (uint64_t)region.address) {
                fprintf(stdout, "OVERLAP\t0x%llx-0x%llx\t0x%llx-0x%llx\n", (uint64_t)last.address, (uint64_t)last.address + last.size, (uint64_t)region.address, (uint64_t)region.address+region.size);
                debugDump();
                abort();
            }
        }
        last = region;
        if (_freeSizeHash.find(region) == _freeSizeHash.end()) {
            fprintf(stdout, "REGION MISSING(size) %llu, 0x%llx\n", region.size, (uint64_t)region.address);
            debugDump();
            abort();
        }
    }
#endif
}

void PersistentAllocator::swap(PersistentAllocator& other) {
    using std::swap;
    if (this == &other) { return; }
    swap(_magazine,         other._magazine);
    swap(_regionList,       other._regionList);
    swap(_freeSizeHash,     other._freeSizeHash);
    swap(_freeAddressHash,  other._freeAddressHash);
}

PersistentAllocator& PersistentAllocator::operator=(PersistentAllocator&& other) {
    swap(other);
    return *this;
}

void PersistentAllocator::reserveRange(BTree<Buffer, RegionSizeCompare>::iterator& i, Buffer buffer) {
    auto j = _freeAddressHash.find(*i);
    contract(j != _freeAddressHash.end());
    // We are passed in a buffer representing the allocation we are about to return, and iterator to _freeSizeHash that
    // contains that buffer. We need to update the state to mark that range in use by:
    // 1. Finding the same buffer in the _freeAddressHash
    // 2. Figuring out if there is any portion of the buffer represent by the iterators before or after the allocated buffer
    // 3. Make new regions for the bits we are not allocating
    // 4. Deleting the iterators from both sets
    // 5. Inserting the new buffers if they exist
    // This can be done very efficiently by using the hinted insert methods carefully to avoid spurious B+Tree searches
    Buffer prolog = { nullptr, 0 };
    Buffer epilog = { nullptr, 0 };
    i->remainders(buffer, prolog, epilog);
    i = _freeSizeHash.erase(i);
    j = _freeAddressHash.erase(j);
    if (epilog) {
        _freeSizeHash.insert(epilog);
        auto insert_op =_freeAddressHash.insert(j, epilog);
        assert(insert_op.second == true);
        j = insert_op.first;
        j.validate();
    }
    if (prolog) {
        (void)_freeSizeHash.insert(prolog);
        (void)_freeAddressHash.insert(j, prolog);
    }
}

Allocator::Buffer PersistentAllocator::allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) {
#if !TARGET_OS_EXCLAVEKIT
    __unused auto lock = _memoryManager->lockGuard();
#endif // !TARGET_OS_EXCLAVEKIT
    while (1) {
        contract(_freeSizeHash.size() == _freeAddressHash.size());
        const uint64_t targetAlignment = std::max<uint64_t>(16ULL, alignment);
        uint64_t targetSize = (std::max<uint64_t>(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
        Buffer result = { nullptr, 0 };
        auto i = _freeSizeHash.lower_bound({ nullptr, targetSize + prefix });
        for(; i != _freeSizeHash.end(); ++i) {
            result = i->findSpace(targetSize, alignment, prefix);
            _allocatedBytes += result.size;
            if (result) {
                reserveRange(i, result);
                reloadMagazine();
                validate();
                return result;
            }
        }
        // We did not find enough space, vm_allocate a new region and then loop back around to try again
        Buffer newRegion;
        if (targetSize+targetAlignment+kGranuleSize < PERSISTENT_ALLOCATOR_DEFAULT_POOL_SIZE) {
            newRegion = _memoryManager->vm_allocate_bytes(PERSISTENT_ALLOCATOR_DEFAULT_POOL_SIZE);
        } else {
            newRegion = _memoryManager->vm_allocate_bytes(targetSize+targetAlignment+kGranuleSize);
        }
        _regionList.insert(newRegion);
        _freeSizeHash.insert(newRegion);
        _freeAddressHash.insert(newRegion);
        reloadMagazine();
    }
}

// This is an alternate deallocation mechanism that creates a link list of deallocated buffers. It is intened to be used when
// it is not safe to alter the B+Trees, either because:
//
// 1) It is return memory allocated via the embedded magazine back to the allocator, which means we are in the middle of
//    updating the B+Tree already and would corrupt it if we reenter
// 2) The memory is being returned from a different thread that cannot safely access the B+Trees
//
// When the allocator later performs an operation and it is in a safe state it will walk through the chain and add the elements
// back to the B+Tree
void PersistentAllocator::deallocate_buffer_safe(Buffer buffer, bool internal) {
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, sizeof(DeallocationRecord));
    auto newRecord = new (buffer.address) DeallocationRecord(buffer.size);
    newRecord->next = _deallocationChian;
    _deallocationChian = newRecord;
    if (!internal) {
        _allocatedBytes -= buffer.size;
    }
}

void PersistentAllocator::processDeallocations(Buffer* begin, Buffer* end) {
    Buffer currentDeallocation = { nullptr, 0};

    // Walk thrugh and free the allocations, coalescing as we go to reduce operations
    for (auto i = begin; i != end; ++i) {
        if (!currentDeallocation) {
            currentDeallocation = *i;
        } else if (i->succeeds(currentDeallocation)) {
            currentDeallocation.size += i->size;
        } else  {
            addToFreeBlockTrees(currentDeallocation);
            currentDeallocation = *i;
        }
        //            ASAN_POISON_MEMORY_REGION(i.address, i.size);
    }
    if (currentDeallocation) {
        addToFreeBlockTrees(currentDeallocation);
    }
}

void PersistentAllocator::deallocate_buffer(Buffer buffer) {
#if !TARGET_OS_EXCLAVEKIT
    __unused auto lock = _memoryManager->lockGuard();
#endif // !TARGET_OS_EXCLAVEKIT
    std::array<Buffer, 20>  deallocations;
    size_t                  deallocationCount = 0;
    // First add the thing we are actually deallocating
    deallocations[deallocationCount++] = buffer;

    // Run in a loop to in case the magazine returns more buffers from this operation
    while (_deallocationChian != nullptr) {
        // Now add any pending deallocation from the magazine
        for (auto i = _deallocationChian; i != nullptr; i = i->next) {
            // Add the element into the array such that the array remains sorted
            Buffer freedRegion = {(void*)i, i->size};
            auto insertion_point = std::lower_bound(deallocations.begin(), deallocations.begin()+deallocationCount, freedRegion);
            std::copy_backward(insertion_point, deallocations.begin()+deallocationCount, deallocations.begin()+deallocationCount+1);
            *insertion_point = freedRegion;
            deallocationCount++;
            if (deallocationCount == 20) {
                processDeallocations(deallocations.begin(), deallocations.end());
                deallocationCount = 0;
            }
        }
        _deallocationChian = nullptr;
    }
    processDeallocations(deallocations.begin(), deallocations.begin()+deallocationCount);
    _allocatedBytes -= buffer.size;
    validate();
}

void PersistentAllocator::addToFreeBlockTrees(Buffer buffer) {
    contract(_freeSizeHash.size() == _freeAddressHash.size());
    auto i = _freeAddressHash.lower_bound(buffer);
    if (i != _freeAddressHash.end() && i->succeeds(buffer)) {
        // i is immediately adjacent to buffer. Erase it, and i will be the next (implcitly non-contiguous buffer)
        buffer.size += i->size;
        _freeSizeHash.erase(*i);
        i = _freeAddressHash.erase(i);
    }
    contract((i == _freeAddressHash.end()) || !i->succeeds(buffer));
    if (i != _freeAddressHash.begin()) {
        auto j = std::prev(i);
        if (buffer.succeeds(*j)) {
            buffer.address = j->address;
            buffer.size += j->size;
            _freeSizeHash.erase(*j);
            i = _freeAddressHash.erase(j);
        }
    }
    auto k = _freeSizeHash.insert(buffer);
    _freeAddressHash.insert(i, buffer);

    // We only need to check if a region has been freed if the contiguous size of the buffer is greater than the minimum region size
    if (buffer.size >= PERSISTENT_ALLOCATOR_DEFAULT_POOL_SIZE) {
        // Since there are guard pages between vm_allocates we know there is at most one freed region
        auto j = _regionList.find(buffer);
        if (j != _regionList.end()) {
            _memoryManager->vm_deallocate_bytes(j->address, j->size);
            reserveRange(k.first, buffer);
            _regionList.erase(j);
        }
    }
    reloadMagazine();
}

void PersistentAllocator::destroy() {
    contract(_allocatedBytes == 0);
    STACK_ALLOC_VECTOR(Buffer, regions, _regionList.size());
    std::copy(_regionList.begin(), _regionList.end(), std::back_inserter(regions));
    for (auto region : regions) {
//        fprintf(stderr, "PersistentAllocator2: ");
        _memoryManager->vm_deallocate_bytes(region.address, region.size);
    }
}


void MemoryManager::writeProtect(bool protect) {
#if !TARGET_OS_EXCLAVEKIT
    if (!_allocator) { return; }
    if (protect) {
        // fprintf(stderr, "writeProtect(true) called 0x%u -> 0x%u\n", _writeableCount, _writeableCount-1);
        for (const auto& region : _allocator->_regionList) {
            // fprintf(stderr, "0x%lx - 0x%lx\t  PROTECT\n", (uint64_t)region.address, (uint64_t)region.address+region.size);
            if (mprotect(region.address, (size_t)region.size, PROT_READ) == -1) {
                // printf("FAILED: %d", errno);
            }
        }
    } else {
        // fprintf(stderr, "writeProtect(false) called 0x%u -> 0x%u\n", _writeableCount, _writeableCount+1);
        for (const auto& region : _allocator->_regionList) {
            // fprintf(stderr, "0x%lx - 0x%lx\t  UNPROTECT\n", (uint64_t)region.address, (uint64_t)region.address+region.size);
            if (mprotect(region.address, (size_t)region.size, (PROT_READ | PROT_WRITE)) == -1) {
                // printf("FAILED: %d", errno);
            }
        }
    }
#endif // !TARGET_OS_EXCLAVEKIT
}

// In order to prevent reentrancy issues the BTrees used to implement this allocator cannot make any calls that would recursively mutate
// themselves. We solve that by preloading a magazine of appropriately sized allocations we can hand out without updating the B+Trees, then
// refill it when it would not cause reentrancy
void PersistentAllocator::reloadMagazine() {
    static_assert(sizeof(BTree<Buffer, std::less<Buffer>>::Node) == 256);
    static_assert(sizeof(BTree<Buffer, RegionSizeCompare>::Node) == 256);
    static_assert(alignof(BTree<Buffer, std::less<Buffer>>::Node) == 256);
    static_assert(alignof(BTree<Buffer, RegionSizeCompare>::Node) == 256);
    size_t requiredMagazineSlots = 2*(_freeSizeHash.depth()+_freeAddressHash.depth()+_regionList.depth())+3;
    if (requiredMagazineSlots <= _magazine.size()) { return; }
    size_t size = 256*((2*requiredMagazineSlots)-_magazine.size());
    for(auto i = _freeSizeHash.lower_bound({ nullptr, size }); i != _freeSizeHash.end(); ++i) {
        auto space = i->findSpace(size, 256, 0);
        if (space) {
            _magazine.refill(space);
            reserveRange(i, space);
            return;
        }
    }
    // We did not find enough space, vm_allocate a new region directly, and increase the required slots
    Buffer newRegion = _memoryManager->vm_allocate_bytes(std::max<size_t>(size, PERSISTENT_ALLOCATOR_DEFAULT_POOL_SIZE));
    assert(newRegion.address != nullptr);
    Buffer space = { newRegion.address, size};
    _magazine.refill(space);
    // Safe to call becuase we just refilled and and any misses are guaranteed to be serviced by that
    _regionList.insert(newRegion);
    newRegion.address = (void*)((uint64_t)newRegion.address + size);
    newRegion.size -= size;
    _freeSizeHash.insert(newRegion);
    _freeAddressHash.insert(newRegion);
}


Allocator& Allocator::persistentAllocator(MemoryManager&& memoryManager) {
    Buffer buffer       = memoryManager.vm_allocate_bytes(PERSISTENT_ALLOCATOR_DEFAULT_POOL_SIZE);
    return *new (buffer.address) PersistentAllocator(buffer, std::move(memoryManager));
}

Allocator& Allocator::persistentAllocator() {
    MemoryManager memoryManager;
    return persistentAllocator(std::move(memoryManager));
}

bool PersistentAllocator::owned(const void* p, uint64_t nbytes) const {
    Buffer allocation = { (void*)p, nbytes};
    for (const auto& region : _regionList) {
        if (region.contains(allocation)) { return true; }
    }
    return false;
}
} // namespace lsl

//VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator& allocator) {
//    return allocator.malloc(count);
//}
//VIS_HIDDEN void* operator new(std::size_t count, std::align_val_t al, lsl::Allocator& allocator) {
//    return allocator.aligned_alloc((size_t)al, count);
//}


