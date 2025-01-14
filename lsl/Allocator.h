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

#ifndef LSL_Allocator_h
#define LSL_Allocator_h

#include <TargetConditionals.h>
#include "Defines.h"
#include "BitUtils.h"
#include "AuthenticatedValue.h"

#include <bit>
#include <atomic>
#include <limits>
#include <cassert>
#include <compare>
#include <cstddef>
#include <utility>
#if !TARGET_OS_EXCLAVEKIT
#include <_simple.h>
#include <mach/vm_statistics.h>
#include <os/lock.h>
#include <pthread.h>
#include <sys/mman.h>
#endif // !TARGET_OS_EXCLAVEKIT


#include <new>
#include <stdio.h>

namespace dyld4 {
class RuntimeState;
};

namespace lsl {

struct Allocator;

#if !TARGET_OS_EXCLAVEKIT
#pragma mark -
#pragma mark Lock abstraction

// TODO: We should have a LockManager class that handles fork(), etc

struct VIS_HIDDEN Lock {
    struct Guard {
        Guard(Lock& lock) : _lock(&lock)    { _lock->lock(); }
        Guard()                             = delete;
        Guard(const Guard& other)           = delete;
        Guard(Guard&& other)                { swap(other); }
        ~Guard()                            { _lock->unlock(); }
    private:
        void swap(Guard& other) {
            if (&other == this) { return; }
            using std::swap;
            swap(_lock, other._lock);
        }
        Lock* _lock;
    };
    Lock()                          = default;
    Lock(const Lock&)               = default;
    Lock(Lock&&)                    = default;
    Lock& operator=(Lock&&)         = default;
    Lock& operator=(const Lock&)    = default;
    Lock(dyld4::RuntimeState* runtimeState, os_unfair_lock_t lock) : _runtimeState(runtimeState), _lock(lock) {}

    void assertNotOwner();
    void assertOwner();
private:
    void swap(Lock& other) {
        if (&other == this) { return; }
        using std::swap;
        swap(_runtimeState, other._runtimeState);
        swap(_lock,         other._lock);
    }
    void lock();
    void unlock();
    dyld4::RuntimeState*    _runtimeState   = nullptr;
    os_unfair_lock_t        _lock           = nullptr;
};
#endif // !TARGET_OS_EXCLAVEKIT
#pragma mark -
#pragma mark Memory Manager

#if __has_feature(ptrauth_calls)
    typedef AuthenticatedValue<void*> WriteProtectionStateData;
#else
    typedef void* WriteProtectionStateData;
#endif

struct VIS_HIDDEN MemoryManager {
    struct WriteProtectionState {
        WriteProtectionStateData    data;

        static constexpr uintptr_t readwrite = 1;
        static constexpr uintptr_t readonly = 0;
        static constexpr uintptr_t invalid = 0x00000000FFFFFFFFULL;
    };
    // a tuple of an allocated <pointer, size>
    struct Buffer {
        void*   address = nullptr;
        uint64_t  size  = 0;
        [[gnu::pure]] void*     lastAddress() const;                // end() ??
        bool                    align(uint64_t alignment, uint64_t size);

        bool                    contains(const Buffer&) const;
        bool                    valid() const;
        void                    dump() const;
        bool                    succeeds(const Buffer&) const;
        void                    remainders(const Buffer& other, Buffer& prefix) const;
        Buffer                  findSpace(uint64_t targetSize, uint64_t targetAlignment) const;
        void                    consumeSpace(uint64_t consumedSpace);
        explicit                operator bool() const;
        auto                    operator<=>(const Buffer&) const = default;
    };

    MemoryManager()                     = default;
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager(MemoryManager&& other);
    MemoryManager& operator=(const MemoryManager&) = delete;
    MemoryManager& operator=(MemoryManager&& other);
    MemoryManager(const char** envp, const char** apple, void* dyldSharedCache);

    void setDyldCacheAddr(void* sharedCache);
#if !TARGET_OS_EXCLAVEKIT
    MemoryManager(Lock&& lock);
    void adoptLock(Lock&& lock);
#endif // !TARGET_OS_EXCLAVEKIT

    [[nodiscard]] static void*      allocate_pages(uint64_t size);
    static void                     deallocate_pages(void* p, uint64_t size);
    [[nodiscard]] Buffer            vm_allocate_bytes(uint64_t size);
    void static                     vm_deallocate_bytes(void* p, uint64_t size);
    template<typename F>
    ALWAYS_INLINE void          withWritableMemory(F work) {
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        // Purposefully spill into a signed stack slot to prevent attackers replacing the this pointer
        MemoryManager* __ptrauth_dyld_tpro0 memoryManager = this;
        WriteProtectionState previousState;
        // Barrier to prevent optimizing away memoryManager-> to this->
        os_compiler_barrier();
        memoryManager->makeWriteable(previousState);
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
        work();
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        memoryManager->restorePreviousState(previousState);
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }

    template<typename F>
    ALWAYS_INLINE void          withReadOnlyMemory(F work) {
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        // Purposefully spill into a signed stack slot to prevent attackers replacing the this pointer
        MemoryManager* __ptrauth_dyld_tpro1 memoryManager = this;
        WriteProtectionState previousState;
        // Barrier to prevent optimizing away memoryManager-> to this->
        os_compiler_barrier();
        memoryManager->makeReadOnly(previousState);
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
        work();
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        memoryManager->restorePreviousState(previousState);
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }
private:
    friend struct Allocator;
    template<uint64_t SIZE> friend struct PreallocatedAllocatorLayout;
#if !TARGET_OS_EXCLAVEKIT
    [[nodiscard]] Lock::Guard lockGuard();
#endif // !TARGET_OS_EXCLAVEKIT
    void writeProtect(bool protect);

    __attribute__((always_inline))
    void makeWriteable(WriteProtectionState& previousState) {
#if ENABLE_HW_TPRO_SUPPORT
        if (_tproEnable) {
            os_compiler_barrier();
            // Stacks are in  memory, so it is possible to attack tpro via writing the stack vars in another thread. To protect
            // against this we create a signature that mixes the value of writable and its address then validate it later. If the
            // barriers work the state will never spill to the stack between varification and usage.
            previousState.data = os_thread_self_restrict_tpro_is_writable() ? (void*)WriteProtectionState::readwrite : (void*)WriteProtectionState::readonly;
            if ( previousState.data == (void*)WriteProtectionState::readonly ) {
                os_thread_self_restrict_tpro_to_rw();
            }
            os_compiler_barrier();
            return;
        }
#endif // ENABLE_HW_TPRO_SUPPORT

#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        previousState.data = (void*)WriteProtectionState::readwrite;
        {
            __unused auto lock = lockGuard();
            if (_writeableCount == 0) {
                writeProtect(false);
            }
            ++_writeableCount;
        }
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }
    __attribute__((always_inline))
    void makeReadOnly(WriteProtectionState& previousState) {
#if ENABLE_HW_TPRO_SUPPORT
        if (_tproEnable) {
            os_compiler_barrier();
            // Stacks are in  memory, so it is possible to attack tpro via writing the stack vars in another thread. To protect
            // against this we create a signature that mixes the value of writable and its address then validate it later. If the
            // barriers work the state will never spill to the stack between varification and usage.
            previousState.data = os_thread_self_restrict_tpro_is_writable() ? (void*)WriteProtectionState::readwrite : (void*)WriteProtectionState::readonly;
            if (previousState.data == (void*)WriteProtectionState::readwrite) {
                os_thread_self_restrict_tpro_to_ro();
            }
            os_compiler_barrier();
            return;
        }
#endif // ENABLE_HW_TPRO_SUPPORT

#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        previousState.data = (void*)WriteProtectionState::invalid;
        {
            __unused auto lock = lockGuard();
            --_writeableCount;
            if (_writeableCount == 0) {
                writeProtect(true);
            }
        }
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }
    __attribute__((always_inline))
    void restorePreviousState(WriteProtectionState& previousState) {
#if ENABLE_HW_TPRO_SUPPORT
        if (_tproEnable) {
            os_compiler_barrier();
            uintptr_t state = os_thread_self_restrict_tpro_is_writable() ? WriteProtectionState::readwrite : WriteProtectionState::readonly;
            if ((void*)state != previousState.data) {
                if ( previousState.data == (void*)WriteProtectionState::readwrite ) {
                    os_thread_self_restrict_tpro_to_rw();
                } else {
                    os_thread_self_restrict_tpro_to_ro();
                }
            }
            os_compiler_barrier();
            return;
        }
#endif // ENABLE_HW_TPRO_SUPPORT

#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        {
            __unused auto lock = lockGuard();
            if ( previousState.data == (void*)WriteProtectionState::invalid ) {
                if (_writeableCount == 0) {
                    writeProtect(false);
                }
                _writeableCount += 1;
            } else if ( previousState.data == (void*)WriteProtectionState::readwrite ) {
                _writeableCount -= 1;
                if (_writeableCount == 0) {
                    writeProtect(true);
                }
            }
        }
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }
    void swap(MemoryManager& other);
    int vmFlags() const;

#if !TARGET_OS_EXCLAVEKIT
    Lock                    _lock;
#endif // !TARGET_OS_EXCLAVEKIT
    Allocator*              _allocator                  = nullptr;
    uint64_t                _writeableCount             = 0;

#if ENABLE_HW_TPRO_SUPPORT
    bool                    _tproEnable                 = false;
#endif // ENABLE_HW_TPRO_SUPPORT

#if BUILDING_DYLD
    void*                   _sharedCache                = nullptr;
#endif // BUILDING_DYLD
    // This is info we are stashing for CRSetCrashLogMessage2 later. They are just for debugging.
    uint64_t                requestedAlignment          = 0;
    uint64_t                requestedSize               = 0;
    uint64_t                requestedTargetAlignment    = 0;
    uint64_t                requestedTargetSize         = 0;
};

template<typename T>
struct VIS_HIDDEN UniquePtr;

template<typename T>
struct VIS_HIDDEN SharedPtr;



struct __attribute__((aligned(16))) VIS_HIDDEN Allocator {
    using Buffer = MemoryManager::Buffer;

    static const uint64_t    kGranuleSize                    = (16);

    // smart pointers
    template< class T, class... Args >
    NO_DEBUG UniquePtr<T> makeUnique(Args&&... args ) {
        void* storage = aligned_alloc(alignof(T), sizeof(T));
        return UniquePtr<T>(new (storage) T(std::forward<Args>(args)...));
    }

    template< class T, class... Args >
    NO_DEBUG SharedPtr<T> makeShared(Args&&... args ) {
        void* storage = aligned_alloc(alignof(T), sizeof(T));
        return SharedPtr<T>(new (storage) T(std::forward<Args>(args)...));
    }

    // Simple interfaces
    void*                   malloc(uint64_t size);
    void*                   aligned_alloc(uint64_t alignment, uint64_t size);
    void                    free(void* ptr);
    // realloc() does not follow posix semantics. If it cannnot realloc in place it returns false
    bool                    realloc(void* ptr, uint64_t size);
    static void             freeObject(void* ptr);
    char*                   strdup(const char*);
    bool                    owned(const void* p, uint64_t nbytes) const;
    uint64_t                size(const void* p) const;
    MemoryManager*          memoryManager() const;

    // Support for creating allocatos
    static Allocator&       createAllocator();
#if !BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    static Allocator&       defaultAllocator();
#endif
    static Allocator&       stackAllocatorInternal(void* buffer, uint64_t);

public:
    struct Pool;
    friend struct AllocatorLayout;
    Allocator(MemoryManager& memoryManager, Pool& pool);
    ~Allocator();

    void setBestFit(bool);
    void validate() const;
    void dump() const;
    void reset();
    void forEachVMAllocatedBuffer(void (^callback)(const Buffer&));

    Allocator& operator=(Allocator&& other);
    
    uint64_t      allocated_bytes() const;
    // For debugging
//    virtual void        validate() const {};
//    virtual void        debugDump() const {};
    struct __attribute__((aligned(16))) AllocationMetadata {
        static const uint64_t kNextBlockAllocatedFlag       = 0x01ULL;
        static const uint64_t kNextBlockLastBlockFlag       = 0x02ULL;
        static const uint64_t kNextBlockAddressMask         = ~(kNextBlockAllocatedFlag | kNextBlockLastBlockFlag);
        static const uint64_t kPreviousBlockIsAllocatorFlag = 0x01ULL;
        static const uint64_t kPreviousBlockAddressMask      = ~(kPreviousBlockIsAllocatorFlag);
        AllocationMetadata() = delete;
        AllocationMetadata(Pool* pool, uint64_t size);
        AllocationMetadata(AllocationMetadata *prev, uint64_t size, uint64_t flags, uint64_t prevFlags);
        void* firstAddress() const;
        void* lastAddress() const;
        uint64_t size() const;
        void reserve(uint64_t size, bool allocated);
        void coalesce(Pool* pool);
        bool allocated() const;
        bool free() const;
        AllocationMetadata* previous() const;
        AllocationMetadata* next() const;
        bool last() const;
        Pool* pool(bool useHints = true) const;

        void deallocate();
        static AllocationMetadata* forPtr(void* ptr);
        void markAllocated();
        void returnToNext(uint64_t size);
        bool consumeFromNext(uint64_t size);


        void validate() const;
        void logAddressSpace(const char* prefix) const;
    private:
        void setPoolHint(Pool* pool);
        // We use the low bit of previous to indicate if the pointer points to another metadata, or the pool
        // 0: metadata
        // 1: pool
        uint64_t    _prev = 0;
        // We use the low bit of next to indicate if the space between this and the next metadata is free or used
        // 0: free
        // 1: allocated
        // and the next bit to indicate if it is the last metadata in the pool
        // 0: normal metadata
        // 2: last metadata
        uint64_t    _next = 0;
    };
    struct __attribute__((aligned(16))) Pool {
        Pool() = default;
        Pool(Allocator* allocator, Pool* prevPool, uint64_t size);
        Pool(Allocator* allocator, Pool* prevPool, Buffer region);
        Pool(Allocator* allocator, Pool* prevPool, Buffer region, Buffer freeRegion);
        void* aligned_alloc(uint64_t alignment, uint64_t size);
        void* aligned_alloc_best_fit(uint64_t alignment, uint64_t size);
        void free(void* ptr);
        void makeNextPool(Allocator* allocator, uint64_t newPoolSize);
        Pool* nextPool() const;
        Pool* prevPool() const;
        static Pool* forPtr(void* ptr);
        const Buffer& poolBuffer() const;
        Allocator* allocator() const;
        void validate() const;
        void dump() const;
    private:
        friend struct AllocationMetadata;
        Allocator*          _allocator          = nullptr;
        Pool*               _nextPool           = nullptr;
        Pool*               _prevPool           = nullptr;
        AllocationMetadata* _lastFreeMetadata   = nullptr;
        Buffer              _poolBuffer;
    };
    void swap(Allocator& other);
    MemoryManager*      _memoryManager  = nullptr;
    Pool*               _firstPool      = nullptr;
    Pool*               _currentPool    = nullptr;
    uint64_t            _allocatedBytes = 0;
    uint64_t            _logID          = 0;
    bool                _bestFit        = false;
private:
    friend struct AllocatorLayout;
    Allocator() = default;
};

struct VIS_HIDDEN AllocatorGuard {
    AllocatorGuard(Allocator& allocator) : _allocator(allocator) {}
    ~AllocatorGuard() {
        _allocator.~Allocator();
    }
private:
    Allocator& _allocator;
};

struct VIS_HIDDEN AllocatorLayout {
    AllocatorLayout() = default;
    void init(uint64_t size, const char** envp = nullptr, const char** apple = nullptr, void* dyldSharedCache = nullptr);
    Allocator& allocator();
    static uint64_t minSize();
private:
    void configure(const char** envp, const char** apple, void* dyldSharedCache);
    MemoryManager   _memoryManager;
    Allocator       _allocator;
    Allocator::Pool _pool;
};

template<uint64_t SIZE>
struct PreallocatedAllocatorLayout {
    void init(const char** envp = nullptr, const char** apple = nullptr, void* dyldSharedCache = nullptr) {
        MemoryManager memoryManager(envp, apple, dyldSharedCache);
        memoryManager.withWritableMemory([&] {
            AllocatorLayout* layout = new ((void*)&_poolBytes[0]) AllocatorLayout();
            layout->init(SIZE, envp, apple, dyldSharedCache);
        });
    }
    Allocator& allocator() {
        AllocatorLayout* layout = (AllocatorLayout*)&_poolBytes[0];
        return layout->allocator();
    }
private:
    uint8_t _poolBytes[SIZE];
} __attribute__((aligned(16)));

template<typename T>
struct VIS_HIDDEN UniquePtr {
    UniquePtr() = default;
    constexpr UniquePtr(std::nullptr_t) : UniquePtr() {};
    template<class  U> explicit UniquePtr(U* data) : _data(data) {
        if (!_data) { return; }
    }
    UniquePtr(const UniquePtr&) = delete;
    UniquePtr(UniquePtr&& other) {
        swap(other);
    }
    template<class U>
    UniquePtr(UniquePtr<U>&& other) {
        swap(other);
    }
    UniquePtr& operator=(const UniquePtr&) = delete;
    UniquePtr& operator=(UniquePtr&& other) {
        swap(other);
        return *this;
    };
    template<class U>
    UniquePtr& operator=(UniquePtr&& other) {
        swap(other);
        return *this;
    };
    ~UniquePtr() {
        if (_data) {
            _data->~T();
            Allocator::freeObject((void*)_data);
        }
    }
    explicit operator bool() const {
        return (_data!= nullptr);
    }
    T& operator*() {
        return *_data;
    }
    T* operator->() {
        return _data;
    }
    const T& operator*() const {
        return *((const T*)_data);
    }
    const T* operator->() const {
        return (const T*)_data;
    }
    template<typename F>
    auto withUnsafe(F f) {
        return f(_data);
    }
    template<typename F>
    auto withUnsafe(const F f) const {
        return f(_data);
    }
    T* release() {
        auto result = _data;
        _data = nullptr;
        return result;
    }
    friend void swap(UniquePtr& x, UniquePtr& y) {
        x.swap(y);
    }
    //TODO: Move this to opeator<=> once C++20 imp is more complete
    bool operator<(const UniquePtr& other) const {
        return *_data < *other._data;
    }
private:
    void swap(UniquePtr& other) {
        if (&other == this) { return; }
        std::swap(_data, other._data);
    }
    template<typename U>
    void swap(UniquePtr<U>& other) {
        auto tmp = (UniquePtr*)&other;
        if (tmp == this) { return; }
        std::swap(_data, tmp->_data);
    }
    template<typename U> friend struct UniquePtr;
    T*  _data = nullptr;
};


template<typename T>
struct VIS_HIDDEN SharedPtr {
    struct Ctrl {
        Ctrl() = delete;
        Ctrl(T* data) : _data(data) {}
        void incrementRefCount() {
            //__c11_atomic_fetch_add((_Atomic uint32_t*)&_refCount, 1, __ATOMIC_RELAXED);
            _refCount.fetch_add(1, std::memory_order_relaxed);
        }
        void decrementRefCount() {
            //__c11_atomic_fetch_sub((_Atomic uint32_t*)&_refCount, 1, __ATOMIC_ACQ_REL)
            if (_refCount.fetch_sub(1, std::memory_order_acq_rel) == 0) {
                if (_data) {
                    _data->~T();
                    Allocator::freeObject((void*)_data);
                }
                Allocator::freeObject((void*)this);
            }
        }
        T* data() const {
            return _data;
        }
        std::atomic<uint32_t>  _refCount{0};
        T*  _data = nullptr;
    };
    SharedPtr() = default;
    constexpr SharedPtr(std::nullptr_t) : _ctrl(nullptr) {}
    explicit SharedPtr(T* data) : _ctrl(nullptr) {
        auto metadata = Allocator::AllocationMetadata::forPtr((void*)data);
        auto allocator = metadata->pool()->allocator();
        void* ctrlData = allocator->aligned_alloc(alignof(Ctrl),sizeof(Ctrl));
        _ctrl = new (ctrlData) Ctrl(data);
    }

    SharedPtr(const SharedPtr& other) : _ctrl(other._ctrl) {
        if (!_ctrl) { return; }
        _ctrl->incrementRefCount();
    };
    template<class U>
    SharedPtr(const SharedPtr<U>& other) : SharedPtr((T*)other._data) {
        if (!_ctrl) { return; }
        _ctrl->incrementRefCount();
    }
    SharedPtr(SharedPtr&& other) {
        swap(other);
    }
    template<class U>
    SharedPtr(SharedPtr<U>&& other) {
        swap(other);
    }
    SharedPtr& operator=(const SharedPtr& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    template<class U>
    SharedPtr& operator=(const SharedPtr<U>& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    SharedPtr& operator=(SharedPtr&& other) {
        swap(other);
        return *this;
    };
    template<class U>
    SharedPtr& operator=(SharedPtr<U>&& other) {
        swap(other);
        return *this;
    };
    ~SharedPtr() {
        if (!_ctrl) { return; }
        _ctrl->decrementRefCount();
    }
    explicit operator bool() const {
        if (!_ctrl) { return false; }
        return (_ctrl->data() != nullptr);
    }
    T& operator*() {
        assert(_ctrl != nullptr);
        return *_ctrl->data();
    }
    T* operator->() {
        if (!_ctrl) { return nullptr; }
        return _ctrl->data();
    }
    const T& operator*() const {
        assert(_ctrl != nullptr);
        return *((const T*)_ctrl->data());
    }
    const T* operator->() const {
        assert(_ctrl != nullptr);
        return (const T*)_ctrl->data();
    }
    template<typename F>
    auto withUnsafe(F f) {
        assert(_ctrl != nullptr);
        return f(_ctrl->data());
    }
    template<typename F>
    auto withUnsafe(const F f) const {
        assert(_ctrl != nullptr);
        return f(_ctrl->data());
    }
    friend void swap(SharedPtr& x, SharedPtr& y) {
        x.swap(y);
    }
    //TODO: Move this to opeator<=> once C++20 imp is more complete
    bool operator<(const SharedPtr& other) const {
        return *_ctrl->data() < *other._ctrl->data();
    }
private:
    void swap(SharedPtr& other) {
        if (&other == this) { return; }
        std::swap(_ctrl, other._ctrl);
    }
    template<typename U>
    void swap(SharedPtr<U>& other) {
        auto tmp = (SharedPtr*)&other;
        if (tmp == this) { return; }
        std::swap(_ctrl, tmp->_ctrl);
    }
    template<typename U> friend struct SharedPtr;
    Ctrl*    _ctrl = nullptr;
};

} // namespace lsl

#define STACK_ALLOCATOR(_name, _count)                                                                                          \
    void* __##_name##_storage = alloca((size_t)(_count+lsl::AllocatorLayout::minSize()));                                       \
    lsl::Allocator& _name = lsl::Allocator::stackAllocatorInternal(__##_name##_storage,_count+lsl::AllocatorLayout::minSize()); \
    lsl::AllocatorGuard __##_name##_gaurd(_name);





// These are should never be used. To prevent accidental usage, the prototypes exist, but using will cause a link error
VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator* allocator);
VIS_HIDDEN void* operator new(std::size_t count, std::align_val_t al, lsl::Allocator* allocator);

#endif /*  LSL_Allocator_h */
