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

#include <cstdio>
#include <algorithm>
#include <sys/mman.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sanitizer/asan_interface.h>
#include <compare>
#include "Defines.h"
#include "Allocator.h"
#include "BTree.h"
#include "StringUtils.h"

// TODO: Reenable ASAN support once we have time to debug it

#if !BUILDING_DYLD
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

extern "C" void* __dso_handle;

namespace lsl {

EphemeralAllocator::EphemeralAllocator(void* B, size_t S) : _freeBuffer({B,S}) {}

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

void* Allocator::align(size_t alignment, size_t size, void*& ptr, size_t& space) {
    void* r = nullptr;
    if (size <= space) {
        char* p1 = static_cast<char*>(ptr);
        char* p2 = reinterpret_cast<char*>(reinterpret_cast<size_t>(p1 + (alignment - 1)) & -alignment);
        size_t d = static_cast<size_t>(p2 - p1);
        if (d <= space - size) {
            r = p2;
            ptr = r;
            space -= d;
        }
    }
    return r;
}

void EphemeralAllocator::swap(EphemeralAllocator& other) {
    using std::swap;
    if (this == &other) { return; }
    swap(_freeBuffer.size,     other._freeBuffer.size);
    swap(_freeBuffer.address,  other._freeBuffer.address);
    swap(_regionList,          other._regionList);
    swap(_allocatedBytes,      other._allocatedBytes);
}

void EphemeralAllocator::reset() {
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // HACK: The cache builder doesn't free allocations in the ProcessConfig
#else
    contract(_allocatedBytes == 0);
#endif
    if (_regionList != nullptr) {
        void* cleanupSpace = alloca(1024);
        EphemeralAllocator cleanupAllocator(cleanupSpace, 1024);
        Vector<Buffer> regions(cleanupAllocator);
        for (auto i = _regionList; i != nullptr; i = i->next) {
            regions.push_back(i->buffer);
        }
        for(auto& region : regions) {
            vm_deallocate_bytes(region.address, region.size);
        }
        _regionList = nullptr;
        _freeBuffer = { nullptr, 0 };
    }
}

[[nodiscard]] Allocator::Buffer EphemeralAllocator::allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) {
    contract(prefix == 16 || prefix == 0);
    *deallocator = this;
    // First space for the prefix
    *((uintptr_t*)&_freeBuffer.address) += prefix;
    _freeBuffer.size -= prefix;
    if ((_freeBuffer.size == 0 - prefix) || (!align(alignment, nbytes, _freeBuffer.address, _freeBuffer.size))) {
#if __LP64__
        size_t size = std::max<size_t>(4*nbytes, ALLOCATOR_DEFAULT_POOL_SIZE);
#else
        size_t size = std::max<size_t>(nbytes+65536, ALLOCATOR_DEFAULT_POOL_SIZE);
#endif
        _freeBuffer = vm_allocate_bytes(size, vm_allocate_flags());
//        ASAN_UNPOISON_MEMORY_REGION(_freeBuffer.address, sizeof(RegionListEntry));
        _regionList = new (_freeBuffer.address) RegionListEntry({ _freeBuffer, _regionList});
        size_t roundedSize = prefix + ((sizeof(RegionListEntry) + 15) & (-16));
        *((uintptr_t*)&_freeBuffer.address) += roundedSize;
        _freeBuffer.size -= roundedSize;
        (void)align(alignment, nbytes, _freeBuffer.address, _freeBuffer.size);
    }

    Allocator::Buffer result = { (void*)((uintptr_t)_freeBuffer.address-prefix), nbytes+prefix };
    *((uintptr_t*)&_freeBuffer.address) += nbytes;
    _freeBuffer.size -= nbytes;
    _allocatedBytes += (nbytes + prefix);

//    fprintf(stderr, "  Allocated %zu @ 0x%lx\n", result.size, result.address);
//    fprintf(stderr, "SPACE: %lu, 0x%lx\n", _freeBuffer.size, (uintptr_t)_freeBuffer.address);
    return result;
}

void EphemeralAllocator::deallocate_buffer(Buffer buffer) {
//    fprintf(stderr, "Deallocated %zu @ 0x%lx\n", buffer.size, buffer.address);
    _allocatedBytes -= buffer.size;
}

std::size_t EphemeralAllocator::allocated_bytes() const {
    return _allocatedBytes;
}

std::size_t EphemeralAllocator::vm_allocated_bytes() const {
    std::size_t result = 0;
    for (auto i = _regionList; i != nullptr; i = i->next) {
        result += i->buffer.size;
    }
    return result;
}

bool EphemeralAllocator::owned(const void* p, std::size_t nbytes) const {
    for (auto i = _regionList; i != nullptr; i = i->next) {
        if (i->buffer.contains({(void*)p,nbytes})) { return true; }
    }
    return false;
}

void EphemeralAllocator::destroy() {
    contract(_allocatedBytes == 0);
}

AllocationMetadata::AllocationMetadata(Allocator* A, size_t S) : _type(NormalPtr) {
    _allocator = (uintptr_t)A>>3;
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

size_t AllocationMetadata::goodSize(size_t S) {
    size_t sizeClass = 0;
    for (const auto& granule : granules) {
        uint64_t nextGranuleSize = 1ULL<<(granule+11);
        if (S <= nextGranuleSize) {
            sizeClass = &granule - &granules[0];
            break;
        }
    }
    size_t result = (size_t)(S + ((1ULL<<granules[sizeClass])-1)) & (-1*(1ULL<<granules[sizeClass]));
    return result;
}

Allocator& AllocationMetadata::allocator() const {
    auto result = (Allocator*)(_allocator<<3);
//    fprintf(stderr, "0x%lx\tgot\t0x%lx\n", (uintptr_t)this, (uintptr_t)result);
    return *result;
}

size_t AllocationMetadata::size() const {
    return (size_t)_size<<granules[_sizeClass];
}

AllocationMetadata::Type AllocationMetadata::type() const {
    return (Type)_type;
}

void AllocationMetadata::setType(Type type) {
    _type = (uint64_t)type;
}

AllocationMetadata* AllocationMetadata::getForPointer(void* data) {
    contract(data != nullptr);
    return (AllocationMetadata*)((uintptr_t)data-Allocator::kGranuleSize);
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
    return (void*)((uintptr_t)address + size);
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
    if (((uintptr_t)address) < (uintptr_t)other.address) {
        prolog.address = address;
        prolog.size = (uintptr_t)other.address - ((uintptr_t)address);
    }
    if (((uintptr_t)address+size) > (uintptr_t)other.address+other.size) {
        epilog.address = (void*)((uintptr_t)other.address+other.size);
        epilog.size = ((uintptr_t)address+size) - ((uintptr_t)other.address+other.size);
    }
}

Allocator::Buffer Allocator::Buffer::findSpace(size_t targetSize, size_t targetAlignment, size_t prefix) const {
    Buffer result = *this;
    result.address = (void*)((uintptr_t)result.address + prefix);
    result.size -= prefix;
    if (align(targetAlignment, targetSize, result.address, result.size)) {
        result.address = (void*)((uintptr_t)result.address - prefix);
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
    if (((uintptr_t)address + size) == ((uintptr_t)other.address)) { return true; }
    if (((uintptr_t)other.address + other.size) == ((uintptr_t)address)) { return true; }
    return false;
}


void Allocator::Buffer::dump() const {
    printf("\t%zu @ 0x%lx - 0x%lx\n", size, (uintptr_t)address, (uintptr_t)address+size);
}

#pragma mark -
#pragma mark Primitive allocator implementations

//TODO: Remove this once it lands in XNU
#ifndef VM_FLAGS_TPRO
#define VM_FLAGS_TPRO (0) //Set it to zero since if xnu does not export the flag it probably does not support it yet
#endif

int Allocator::vm_allocate_flags() const {
    int result = 0;
#if BUILDING_DYLD
    // Only include the dyld tag for allocations made by dyld
    result |= VM_MAKE_TAG(VM_MEMORY_DYLD);
#endif /* BUILDING_DYLD */
    return result;
}

[[nodiscard]] Allocator::Buffer Allocator::vm_allocate_bytes(std::size_t size, int flags) {
    size_t targetSize = (size + (PAGE_SIZE-1)) & (-1*PAGE_SIZE);
    vm_address_t    result;
    kern_return_t kr = vm_allocate(mach_task_self(), &result, targetSize, VM_FLAGS_ANYWHERE | flags);
#if BUILDING_DYLD && TARGET_OS_OSX && __x86_64__
    // rdar://79214654 support wine games that need low mem.  Move dyld heap out of low mem
    if ( (kr == KERN_SUCCESS) && (result < 0x100000000ULL) ) {
        vm_address_t result2 = (long)&__dso_handle + 0x00200000; // look for vm range after dyld
        kern_return_t kr2 = vm_allocate(mach_task_self(), &result2, targetSize, VM_FLAGS_FIXED | flags);
        if ( kr2 == KERN_SUCCESS ) {
            (void)vm_deallocate(mach_task_self(), result, targetSize);
            result = result2;
        }
    }
#endif /* BUILDING_DYLD && TARGET_OS_OSX && __x86_64__ */

    if (kr != KERN_SUCCESS) {
        char buffer[1024];
        char intStrBuffer[130];
        bytesToHex((const uint8_t*)&size, sizeof(std::size_t), intStrBuffer);
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
//    ASAN_POISON_MEMORY_REGION((void*)result, targetSize);
//    fprintf(stderr, "0x%lx - 0x%lx\t  VM_ALLOCATED\n", (uintptr_t)result, (uintptr_t)result+targetSize);
    return {(void*)result, targetSize};
}

void Allocator::vm_deallocate_bytes(void* p, std::size_t size) {
//    fprintf(stderr, "0x%lx - 0x%lx\tVM_DEALLOCATED\n", (uintptr_t)p, (uintptr_t)p+size);
    (void)vm_deallocate(mach_task_self(), (vm_address_t)p, size);
}

[[nodiscard]] Allocator::Buffer Allocator::allocate_buffer(std::size_t nbytes, std::size_t alignment, Allocator** deallocator) {
    size_t targetAlignment = std::max<size_t>(16ULL, alignment);
    size_t targetSize = (std::max<size_t>(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
    targetSize = AllocationMetadata::goodSize(targetSize);
    auto result = allocate_buffer(targetSize, targetAlignment, 0, deallocator);
//    fprintf(stderr, "0x%lx - 0x%lx\t     ALLOCATED\n", (uintptr_t)result.address, (uintptr_t)result.address+result.size);
//    ASAN_UNPOISON_MEMORY_REGION(result.address, result.size);
    return result;
}

void Allocator::deallocate_buffer(void* p, std::size_t nbytes, std::size_t alignment) {
    const size_t targetAlignment = std::max<size_t>(16ULL, alignment);
    size_t targetSize = (std::max<size_t>(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
//    ASAN_POISON_MEMORY_REGION(p, targetSize);
//    fprintf(stderr, "0x%lx - 0x%lx\t   DEALLOCATED\n", (uintptr_t)p, (uintptr_t)p+targetSize);
    deallocate_buffer({p, targetSize});
}

void* Allocator::malloc(size_t size) {
    return this->aligned_alloc(kGranuleSize, size);
}

void* Allocator::aligned_alloc(size_t alignment, size_t size) {
    static_assert(sizeof(size_t) == sizeof(Allocator*), "Ensure size_t is pointer sized");
    static_assert(kGranuleSize >= (sizeof(size_t) == sizeof(Allocator*)), "Ensure we can fit all metadata in a granule");
//    fprintf(stderr, "0x%lx\taligned_alloc\t%lu\t%lu\n", (uintptr_t)this, size, alignment);
    const size_t targetAlignment = std::max<size_t>(16ULL, alignment);
    size_t targetSize = (std::max<size_t>(size, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
    targetSize = AllocationMetadata::goodSize(targetSize);
    Allocator *deallocator = nullptr;
    
    auto buffer = allocate_buffer(targetSize, targetAlignment, 16, &deallocator);
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, buffer.size);
    contract(buffer.address != nullptr);
    // We are guaranteed a 1 granule managed we can use for storage;
    //    fprintf(stderr, "(tid 0x%lx)\t0x%lx\tstashing\t0x%lx\n", foo, (uintptr_t)buffer.address, (uintptr_t)this);
    (void)new (buffer.address) AllocationMetadata(deallocator, buffer.size-kGranuleSize);
    return (void*)((uintptr_t)buffer.address+kGranuleSize);
}

void Allocator::free(void* ptr) {
    contract((uintptr_t)ptr%16==0);
    if (!ptr) { return; }
    // We are guaranteed a 1 granule prefeix we can use for storage
    auto metadata = AllocationMetadata::getForPointer(ptr);
    metadata->allocator().deallocate_buffer((void*)((uintptr_t)ptr-kGranuleSize), (uintptr_t)metadata->size()+kGranuleSize, kGranuleSize);
}

char* Allocator::strdup(const char* str)
{
    auto result = (char*)this->malloc(strlen(str)+1);
    strcpy(result, str);
    return result;
}

#if CONCURRENT_ALLOCATOR_SUPPORT
Allocator& Allocator::defaultAllocator() {
    static Allocator* allocator = nullptr;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        allocator = &Allocator::concurrentAllocator();
    });
    return *allocator;
}
#endif

#pragma mark -
#pragma mark Persistent Allocator

#if CONCURRENT_ALLOCATOR_SUPPORT
struct ConcurrentAllocator;
#endif

struct VIS_HIDDEN PersistentAllocator : Allocator {
    PersistentAllocator(const Buffer& B, bool useHWTPro = false);
    void    writeProtect(bool protect) override;
    bool    owned(const void* p, std::size_t nbytes) const override;
    void    debugDump() const override;
    void    validate() const override;
    void    destroy() override;
    size_t  allocated_bytes() const override;
    size_t  vm_allocated_bytes() const override;

//    void operator delete  ( void* ptr, std::align_val_t al ) {
//        // Do nothing here, deletion is handled manually
//    }
    friend void swap(PersistentAllocator& x, PersistentAllocator& y) {
        x.swap(y);
    }
protected:
    Buffer  allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) override;
    void    deallocate_buffer(Buffer buffer) override;
    void    deallocate_buffer_safe(Buffer buffer, bool internal);
    int     vm_allocate_flags() const override;
private:
    friend struct Allocator;
    friend struct ConcurrentAllocator;
    void    swap(PersistentAllocator& other);
    void    addToFreeBlockTrees(Buffer buffer, bool freeRegions);
    PersistentAllocator& operator=(PersistentAllocator&& other);
    struct RegionSizeCompare {
        bool operator() (const Buffer& x, const Buffer& y) const {
            if (x.size == y.size) { return x.address < y.address; }
            return (uintptr_t)x.size < (uintptr_t)y.size;
        }
    };
    void reloadMagazine();
    void reserveRange(BTree<Buffer, RegionSizeCompare>::iterator& i, Buffer buffer);
    void processDeallocationChain();
    // This is a special private allocator used for the collection classes used in the longterm allocator. It is a refillable magazine
    // that we pass into those collections so they never reenter the allocator and then themselves. It is the responsbility of the
    // longterm allocator to make sure the magazine always has enough allocations available to get to the next point where it is safe to
    // refill it
    template<uint32_t S, uint32_t A>
    struct MagazineAllocator : Allocator {
        MagazineAllocator(PersistentAllocator& allocator) : _longtermAlloactor(allocator) {}
        Buffer allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) override {
            *deallocator = this;
            contract(nbytes == S);
            contract(alignment == A);
            auto result = _magazine[0];
            result.size = S;
            _magazine[0].size -= S;
            _magazine[0].address = (void*)((uintptr_t)_magazine[0].address + S);
            if (_magazine[0].size == 0) {
                std::copy(&_magazine[1], &_magazine[_magazineDepth--], &_magazine[0]);
            }
            return result;
        }
        void    writeProtect(bool protect) override {};
        bool    owned(const void* p, std::size_t nbytes) const override { return false; }
        void    deallocate_buffer(Buffer buffer) override { _longtermAlloactor.deallocate_buffer_safe(buffer, true); }
        size_t  allocated_bytes() const override { return 0; }
        size_t  vm_allocated_bytes() const override { return 0; }
        void    destroy() override {}

        void refill(Buffer buffer) {
            contract(buffer.size%S == 0);
            contract(((uintptr_t)buffer.address)%A == 0);
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
        std::array<Buffer, 4>   _magazine;
        uint8_t                 _magazineDepth = 0;
        PersistentAllocator&  _longtermAlloactor;
    };
    struct DeallocationRecord {
        DeallocationRecord(size_t S) : size(S) {}
        size_t              size;
        DeallocationRecord* next;
    };
    BTree<Buffer>::Node                     _regionListRoot         = BTree<Buffer>::Node(0x01, &_magazine);
    BTree<Buffer>::Node                     _freeAddressHashRoot    = BTree<Buffer>::Node(0x01, &_magazine);
    BTree<Buffer, RegionSizeCompare>::Node  _freeSizeHashRoot       = BTree<Buffer, RegionSizeCompare>::Node (0x01, &_magazine);
    BTree<Buffer>                           _regionList             = BTree<Buffer>(_magazine, &_regionListRoot);
    BTree<Buffer>                           _freeAddressHash        = BTree<Buffer>(_magazine, &_freeAddressHashRoot);
    BTree<Buffer, RegionSizeCompare>        _freeSizeHash           = BTree<Buffer, RegionSizeCompare>(_magazine, &_freeSizeHashRoot);
    MagazineAllocator<256,256>              _magazine               = MagazineAllocator<256,256>(*this);
    std::atomic<std::size_t>                _allocatedBytes         = 0;
    std::atomic<DeallocationRecord*>        _deallocationChian      = nullptr;
#if CONCURRENT_ALLOCATOR_SUPPORT
    ConcurrentAllocator*                    _concurrentAllocator    = nullptr;
    os_unfair_lock_s                        _abandonedAllocatorLock = OS_UNFAIR_LOCK_INIT;
    std::atomic<bool>                       _abandoned              = false;
    bool                                    _main                   = false;
#endif
};

#if CONCURRENT_ALLOCATOR_SUPPORT
struct VIS_HIDDEN ConcurrentAllocator : Allocator {
                                    ConcurrentAllocator(PersistentAllocator& A, size_t F);
    void                            destroy() override;
    bool                            owned(const void* p, std::size_t nbytes) const override;
    size_t                          allocated_bytes() const override;
    size_t                          vm_allocated_bytes() const override;
    static  ConcurrentAllocator&    bootstrap(); // Initializes a pool and hosts the Allocator within that pool
protected:
    Buffer  allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) override;
    void    deallocate_buffer(Buffer buffer) override;
private:
    friend struct Allocator;
    friend struct PersistentAllocator;
    static void                         threadDestructor(void*);
    PersistentAllocator&                getThreadLocalAllocator();
    PersistentAllocator**               _threadAllocators           = nullptr;
    PersistentAllocator*                _threadAllocatorsOwner      = nullptr;
    size_t                              _threadAllocatorsSize       = 0;
    size_t                              _threadAllocatorsCapacity   = 0;
    pthread_key_t                       _key                        = 0;
    mutable os_unfair_lock_s            _lock                       = OS_UNFAIR_LOCK_INIT;
    std::atomic<size_t>                 _allocatedBytes             = 0;
};
#endif

size_t PersistentAllocator::allocated_bytes() const {
    return _allocatedBytes;
}

size_t PersistentAllocator::vm_allocated_bytes() const {
    size_t result = 0;
    for (auto& region : _regionList) {
        result +=  region.size;
    }
    return result;
}

PersistentAllocator::PersistentAllocator(const Buffer& B, bool useHWTPro)
{
    static_assert(sizeof(BTree<Buffer, std::less<Buffer>>::Node) == 256, "Nodes for btrees used in allocators must be 256 bytes");
    static_assert(sizeof(BTree<Buffer, RegionSizeCompare>::Node) == 256, "Nodes for btrees used in allocators must be 256 bytes");
    auto roundedSize = ((sizeof(PersistentAllocator) + 15) & (-16));
    Buffer freespace = B;
    freespace.address = (void*)((uintptr_t)freespace.address + roundedSize);
    freespace.size -= roundedSize;
    _regionList.insert(B);
    _freeSizeHash.insert(freespace);
    _freeAddressHash.insert(freespace);
}

void PersistentAllocator::debugDump() const {
    fprintf(stderr, "_freeSizeHash\n");
    for (const auto& region : _freeSizeHash) {
        fprintf(stderr, "\t%zu @ 0x%lx\n", region.size, (uintptr_t)region.address);
    }
    fprintf(stderr, "_freeAddressHash\n");
    for (const auto& region : _freeAddressHash) {
        fprintf(stderr, "\t0x%lx: %zu\n", (uintptr_t)region.address, region.size);
    }
}

void PersistentAllocator::validate() const {
#if BUILDING_UNIT_TESTS
    for (const auto& region : _freeSizeHash) {
        if (_freeAddressHash.find(region) == _freeAddressHash.end()) {
            fprintf(stderr, "REGION MISSING(addr) %zu, 0x%lx\n", region.size, (uintptr_t)region.address);
        }
    }
    Buffer last = { nullptr, 0 };
    for (const auto& region : _freeAddressHash) {
        if (last) {
            if (((uintptr_t)last.address + last.size) >= (uintptr_t)region.address) {
                fprintf(stderr, "OVERLAP\t0x%lx-0x%lx\t0x%lx-0x%lx\n", (uintptr_t)last.address, (uintptr_t)last.address + last.size, (uintptr_t)region.address, (uintptr_t)region.address+region.size);
            }
        }
        last = region;
        if (_freeSizeHash.find(region) == _freeSizeHash.end()) {
            fprintf(stderr, "REGION MISSING(size) %zu, 0x%lx\n", region.size, (uintptr_t)region.address);
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
    _freeSizeHash.erase(i);
    j = _freeAddressHash.erase(j);
    if (epilog) {
        _freeSizeHash.insert(epilog);
        auto insert_op =_freeAddressHash.insert(j, epilog);
        contract(insert_op.second == true);
        j = insert_op.first;
    }
    if (prolog) {
        (void)_freeSizeHash.insert(prolog);
        (void)_freeAddressHash.insert(j, prolog);
    }
}

Allocator::Buffer PersistentAllocator::allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) {
    *deallocator = this;
    contract(_freeSizeHash.size() == _freeAddressHash.size());
    const size_t targetAlignment = std::max<size_t>(16ULL, alignment);
    size_t targetSize = (std::max<size_t>(nbytes, 16ULL) + (targetAlignment-1)) & (-1*targetAlignment);
    Buffer result = { nullptr, 0 };
    auto i = _freeSizeHash.lower_bound({ nullptr, targetSize + prefix });
    for(; i != _freeSizeHash.end(); ++i) {
        result = i->findSpace(targetSize, alignment, prefix);
        _allocatedBytes += result.size;
        if (result) {
            reserveRange(i, result);
            reloadMagazine();
            return result;
        }
    }
    // We did not find enough space, vm_allocate a new region and then try again
    Buffer newRegion;
    if (targetSize+targetAlignment+kGranuleSize < ALLOCATOR_DEFAULT_POOL_SIZE) {
        newRegion = vm_allocate_bytes(ALLOCATOR_DEFAULT_POOL_SIZE, vm_allocate_flags());
    } else {
        newRegion = vm_allocate_bytes(targetSize+targetAlignment+kGranuleSize, vm_allocate_flags());
    }
    _regionList.insert(newRegion);
    addToFreeBlockTrees(newRegion, false);
    return allocate_buffer(nbytes, alignment, prefix, deallocator);
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
    newRecord->next = _deallocationChian.load(std::memory_order_relaxed);
    while(!_deallocationChian.compare_exchange_weak(newRecord->next, newRecord,
                                      std::memory_order_release,
                                      std::memory_order_relaxed)) {}
    if (!internal) {
        _allocatedBytes -= buffer.size;
    }
}

void PersistentAllocator::processDeallocationChain() {
    // First check if there is a deallocation chain
    for (auto deallocationRecord = _deallocationChian.load(std::memory_order_relaxed);
         deallocationRecord != nullptr;
         deallocationRecord = _deallocationChian.load(std::memory_order_relaxed)) {
        // If there is CAS it with null
        while(!_deallocationChian.compare_exchange_weak(deallocationRecord, nullptr,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {}
        // We have a chain, walk through it add all the entries to a set backed by an ephemeral allocator
        contract(deallocationRecord != nullptr);
        auto tempAllocator = EphemeralAllocator(alloca(1024), 1024);
        auto deallocations = BTree<Buffer>(tempAllocator);
        while (deallocationRecord != nullptr) {
//            ASAN_UNPOISON_MEMORY_REGION(deallocationRecord, sizeof(DeallocationRecord));
            deallocations.insert({(void*)deallocationRecord, deallocationRecord->size});
            deallocationRecord = deallocationRecord->next;
        }
        // Walk the set and coalesce adjacent buffers
        Buffer currentDeallocation = { nullptr, 0};
        for (auto i : deallocations ) {
            if (!currentDeallocation) {
                currentDeallocation = i;
            } else if (i.succeeds(currentDeallocation)) {
                currentDeallocation.size += i.size;
            } else  {
                addToFreeBlockTrees(currentDeallocation, true);
                currentDeallocation = i;
            }
//            ASAN_POISON_MEMORY_REGION(i.address, i.size);
        }
        if (currentDeallocation) {
            addToFreeBlockTrees(currentDeallocation, true);
        }
    }
}

void PersistentAllocator::deallocate_buffer(Buffer buffer) {
#if CONCURRENT_ALLOCATOR_SUPPORT
    if (_concurrentAllocator) {
        _concurrentAllocator->_allocatedBytes.fetch_sub(buffer.size, std::memory_order_release);
        auto& allocator = _concurrentAllocator->getThreadLocalAllocator();
        if (&allocator != this) {
            if (_abandoned.load(std::memory_order_acquire) == true) {
                // Allocator is abandonned, take a lock and do its work
                os_unfair_lock_lock(&_abandonedAllocatorLock);
                processDeallocationChain();
                _allocatedBytes -= buffer.size;
                addToFreeBlockTrees(buffer, true);
                auto CA = _concurrentAllocator;
                if (_allocatedBytes == 0) {
                    os_unfair_lock_lock(&CA->_lock);
                    std::remove(&CA->_threadAllocators[0],
                                &CA->_threadAllocators[CA->_threadAllocatorsSize--], this);
                    destroy();
                    os_unfair_lock_unlock(&CA->_lock);
                    // We can't unlock _abandonedAllocatorLock because we just deallocated it
                    return;
                }
                os_unfair_lock_unlock(&_abandonedAllocatorLock);
            } else {
                deallocate_buffer_safe(buffer, false);
            }
            return;
        }
    }
#endif
    processDeallocationChain();
    _allocatedBytes -= buffer.size;
    addToFreeBlockTrees(buffer, true);
}

void PersistentAllocator::addToFreeBlockTrees(Buffer buffer, bool freeRegions) {
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
//    assert(*k.first == buffer);
//    validate();
//    debugDump();

    if (freeRegions && (buffer.size >= ALLOCATOR_DEFAULT_POOL_SIZE)) {
        auto first = _regionList.end();
        auto last = _regionList.end();
        for (auto j = _regionList.begin(); j != _regionList.end(); ++j) {
            if (buffer.contains(*j)) {
                if (first == _regionList.end()) {
                    first = j;
                }
                last = j;
            } else if (j->address > buffer.address) {
                break;
            }
        }

        if (first != _regionList.end()) {
            Buffer deallocatedBuffer = { first->address, (uintptr_t)last->address+last->size-(uintptr_t)first->address };
            vm_deallocate_bytes(deallocatedBuffer.address, deallocatedBuffer.size);
            reserveRange(k.first, deallocatedBuffer);
            _regionList.erase(first);
        }
    }
//    validate();
    reloadMagazine();
}

void PersistentAllocator::destroy() {
    contract(_allocatedBytes == 0);
    STACK_ALLOC_VECTOR(Buffer, regions, _regionList.size());
    std::copy(_regionList.begin(), _regionList.end(), std::back_inserter(regions));
    for (auto region : regions) {
        Allocator::vm_deallocate_bytes(region.address, region.size);
    }
}


void PersistentAllocator::writeProtect(bool protect) {
    for (const auto& region : _regionList) {
//        if (protect) {
//            fprintf(stderr, "0x%lx - 0x%lx\t  PROTECT\n", (uintptr_t)region.address, (uintptr_t)region.address+region.size);
//        } else {
//            fprintf(stderr, "0x%lx - 0x%lx\t  UNPROTECT\n", (uintptr_t)region.address, (uintptr_t)region.address+region.size);
//        }
        if (mprotect(region.address, region.size, protect ? PROT_READ : (PROT_READ | PROT_WRITE)) == -1) {
            //printf("FAILED: %d", errno);
        }
    }
}

int PersistentAllocator::vm_allocate_flags() const {
    int result = Allocator::vm_allocate_flags();
    return result;
}

// In order to prevent reentrancy issues the BTrees used to implement this allocator cannot make any calls that would recursively mutate
// themselves. We solve that by preloading a magazine of appropriately sized allocations we can hand out without updating the B+Trees, then
// refill it when it would not cause reentrancy
void PersistentAllocator::reloadMagazine() {
    static_assert(sizeof(BTree<Buffer, std::less<Buffer>>::Node) == 256);
    static_assert(alignof(BTree<Buffer, std::less<Buffer>>::Node) == 256);
    static_assert(sizeof(BTree<Buffer, RegionSizeCompare>::Node) == 256);
    static_assert(alignof(BTree<Buffer, RegionSizeCompare>::Node) == 256);
    size_t requiredMagazineSlots = 2*((_freeSizeHash.depth()+1)+(_freeAddressHash.depth()+1))+(_regionList.depth()+1);
    if (requiredMagazineSlots <= _magazine.size()) { return; }
    size_t size = 256*((2*requiredMagazineSlots)-_magazine.size());
    for(auto i = _freeSizeHash.lower_bound({ nullptr, size }); i != _freeSizeHash.end(); ++i) {
        auto space = i->findSpace(size, 256, 0);
        if (space) {
            reserveRange(i, space);
//            ASAN_UNPOISON_MEMORY_REGION(space.address, space.size);
            _magazine.refill(space);
            break;
        }
    }
}

Allocator& Allocator::persistentAllocator(bool useHWTPro) {
    int flags = 0;
    #if BUILDING_DYLD
    // Only include the dyld tag for allocations made by dyld
    flags |= VM_MAKE_TAG(VM_MEMORY_DYLD);
    #endif /* BUILDING_DYLD */

    Buffer buffer = Allocator::vm_allocate_bytes(ALLOCATOR_DEFAULT_POOL_SIZE, flags);
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, sizeof(PersistentAllocator));
    return *new (buffer.address) PersistentAllocator(buffer, useHWTPro);
}

bool PersistentAllocator::owned(const void* p, std::size_t nbytes) const {
    Buffer allocation = { (void*)p, nbytes};
    for (const auto& region : _regionList) {
        if (region.contains(allocation)) { return true; }
    }
    return false;
}

#if CONCURRENT_ALLOCATOR_SUPPORT

void ConcurrentAllocator::threadDestructor(void* key) {
    auto threadLocalAllocator = (PersistentAllocator*)key;
    auto CA = threadLocalAllocator->_concurrentAllocator;
    bool deallocate = false;
    os_unfair_lock_lock(&threadLocalAllocator->_abandonedAllocatorLock);
    threadLocalAllocator->_abandoned.store(true, std::memory_order_release);
    threadLocalAllocator->processDeallocationChain();
    if (threadLocalAllocator->allocated_bytes() == 0) {
        deallocate = true;
    }
    os_unfair_lock_unlock(&threadLocalAllocator->_abandonedAllocatorLock);
    if (deallocate) {
        os_unfair_lock_lock(&CA->_lock);
        std::remove(&CA->_threadAllocators[0],
                    &CA->_threadAllocators[CA->_threadAllocatorsSize--], threadLocalAllocator);
        threadLocalAllocator->destroy();
        os_unfair_lock_unlock(&CA->_lock);
    }
}

Allocator& Allocator::concurrentAllocator() {
    Buffer threadLocalBuffer = Allocator::vm_allocate_bytes(ALLOCATOR_DEFAULT_POOL_SIZE, 0);
//    ASAN_UNPOISON_MEMORY_REGION(threadLocalBuffer.address, sizeof(PersistentAllocator));
    auto threadLocalAllocator = new (threadLocalBuffer.address) PersistentAllocator(threadLocalBuffer);
    threadLocalAllocator->_main = true;
    Allocator* deallocator = nullptr;
    Buffer buffer = threadLocalAllocator->allocate_buffer(sizeof(ConcurrentAllocator), alignof(ConcurrentAllocator), 0, &deallocator);
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, sizeof(PersistentAllocator));
    return *new (buffer.address) ConcurrentAllocator(*threadLocalAllocator, buffer.size);
}

ConcurrentAllocator::ConcurrentAllocator(PersistentAllocator& allocator, size_t F) {
    int err = pthread_key_create(&_key, ConcurrentAllocator::threadDestructor);
    contract(err == 0);
    allocator._concurrentAllocator  = this;

//    fprintf(stderr, "(tid 0x%lx)\tCreated allocator: 0x%lx\n", (uintptr_t)pthread_self(),(uintptr_t)&A);
    pthread_setspecific(_key, (const void*)&allocator);
    Allocator* deallocator = nullptr;
    Buffer buffer               = allocator.allocate_buffer(32*sizeof(PersistentAllocator*), alignof(PersistentAllocator*), 0, &deallocator);
//    ASAN_UNPOISON_MEMORY_REGION(buffer.address, 32*sizeof(PersistentAllocator));
    _threadAllocators           = (PersistentAllocator**)buffer.address;
    _threadAllocators[0]        = &allocator;
    _threadAllocatorsSize       = 1;
    _threadAllocatorsCapacity   = buffer.size/sizeof(PersistentAllocator*);
    _threadAllocatorsOwner      = &allocator;
}

void ConcurrentAllocator::destroy() {
    contract(_allocatedBytes.load(std::memory_order_acquire) == 0);
    os_unfair_lock_lock(&_lock);
    pthread_key_delete(_key);
    size_t  allocatorsSize          = _threadAllocatorsSize;
    size_t  allocatorsCapacity      = _threadAllocatorsCapacity;
    void*   allocatorsAllocation    = (void*)_threadAllocators;
    PersistentAllocator* allocators[allocatorsSize];

    std::copy(&_threadAllocators[0], &_threadAllocators[allocatorsSize], &allocators[0]);
    for(auto i = 0; i < allocatorsSize; ++i) {
        allocators[i]->_concurrentAllocator = nullptr;
    }
    for(auto i = 0; i < allocatorsSize; ++i) {
        if (allocators[i]->_main) {
            allocators[i]->deallocate_buffer({ (void*)this, AllocationMetadata::goodSize<ConcurrentAllocator>()});
            break;
        }
    }
    for(auto i = 0; i < allocatorsSize; ++i) {
        if (allocators[i]->owned(allocatorsAllocation, sizeof(PersistentAllocator*)*allocatorsCapacity)) {
            allocators[i]->deallocate_buffer({allocatorsAllocation, sizeof(PersistentAllocator*)*allocatorsCapacity});
            break;
        }
    }
    for(auto i = 0; i < allocatorsSize; ++i) {
        allocators[i]->destroy();
    }
    // Do not unlock _lock, it has been deallocated
}

bool ConcurrentAllocator::owned(const void* p, std::size_t nbytes) const {
    bool result = false;
    os_unfair_lock_lock(&_lock);
    for (auto i = 0; i < _threadAllocatorsSize; ++i) {
        if (_threadAllocators[i]->owned(p, nbytes)) {
            result = true;
            break;
        }
    }
    os_unfair_lock_unlock(&_lock);
    return result;
}

size_t ConcurrentAllocator::vm_allocated_bytes() const {
    size_t result = 0;
    // This is racy, but without locking all the allocators there is no way to make this totally accurate
    // we only use it for debugging and to validate the allocator is zeroed out anyway
    os_unfair_lock_lock(&_lock);
    for (auto i = 0; i < _threadAllocatorsSize; ++i) {
        result += _threadAllocators[i]->vm_allocated_bytes();
    }
    os_unfair_lock_unlock(&_lock);
    return result;
}

size_t ConcurrentAllocator::allocated_bytes() const {
    return _allocatedBytes.load(std::memory_order_relaxed);
}

PersistentAllocator& ConcurrentAllocator::getThreadLocalAllocator() {
    auto result = (PersistentAllocator*)pthread_getspecific(_key);
    if (result == nullptr) {
        result = (PersistentAllocator*)&Allocator::persistentAllocator();
        result->_concurrentAllocator = this;
//        fprintf(stderr, "Created allocator: 0x%lx\n", (uintptr_t)result);
        pthread_setspecific(_key, (const void*)result);
        os_unfair_lock_lock(&_lock);
        if (_threadAllocatorsSize == _threadAllocatorsCapacity) {
            Allocator* deallocator      = nullptr;
            Buffer buffer               = result->allocate_buffer(_threadAllocatorsCapacity*2*sizeof(PersistentAllocator*),
                                                                  alignof(PersistentAllocator*), 0, &deallocator);
//            ASAN_UNPOISON_MEMORY_REGION(buffer.address, _threadAllocatorsCapacity*2*sizeof(PersistentAllocator));
            std::copy(&_threadAllocators[0], &_threadAllocators[_threadAllocatorsSize], (PersistentAllocator**)buffer.address);
            _allocatedBytes.fetch_add(_threadAllocatorsCapacity*sizeof(PersistentAllocator*), std::memory_order_relaxed);
            _threadAllocatorsOwner->deallocate_buffer({(void*)_threadAllocators, _threadAllocatorsCapacity*sizeof(PersistentAllocator*)});
//            ASAN_POISON_MEMORY_REGION((void*)_threadAllocators, _threadAllocatorsCapacity*sizeof(PersistentAllocator));
            _threadAllocatorsCapacity   = _threadAllocatorsCapacity * 2;
            _threadAllocators           = (PersistentAllocator**)buffer.address;
            _threadAllocatorsCapacity   = buffer.size/sizeof(PersistentAllocator*);
            _threadAllocatorsOwner      = result;
        }
        _threadAllocators[_threadAllocatorsSize++] = result;
        os_unfair_lock_unlock(&_lock);
    }
//    fprintf(stderr, "Returning allocator: 0x%lx\n", (uintptr_t)result);
    return *result;
}

Allocator::Buffer ConcurrentAllocator::allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) {
    auto& allocator = getThreadLocalAllocator();
    auto result     = allocator.allocate_buffer(nbytes, alignment, prefix, deallocator);
    _allocatedBytes.fetch_add(result.size, std::memory_order_relaxed);
    return result;
}

void ConcurrentAllocator::deallocate_buffer(Allocator::Buffer buffer) {
    // This should never be called because the deallocations should be handled by the thread local allocators
    assert(false);
}
#endif

} // namespace lsl


