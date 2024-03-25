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
#endif // !TARGET_OS_EXCLAVEKIT

#include <new>

namespace dyld4 {
class RuntimeState;
};

namespace lsl {

struct Allocator;
struct PersistentAllocator;

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

//    Lock(Lock&& other) {
//        swap(other);
//    }
//    Lock& operator=(const Lock&) = default;
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

struct VIS_HIDDEN MemoryManager {
    struct WriteProtectionState {
        uintptr_t   signature;
        intptr_t    data;
    };
    // a tuple of an allocated <pointer, size>
    struct Buffer {
        void*   address;
        uint64_t  size;
        [[gnu::pure]] void*     lastAddress() const;                // end() ??
        bool                    align(uint64_t alignment, uint64_t size);

        bool                    contains(const Buffer&) const;
        bool                    valid() const;
        void                    dump() const;
        bool                    succeeds(const Buffer&) const;
        void                    remainders(const Buffer& other, Buffer& prefix, Buffer& postfix) const;
        Buffer                  findSpace(uint64_t targetSize, uint64_t targetAlignment, uint64_t prefix) const;
        explicit                operator bool() const;
        auto                    operator<=>(const Buffer&) const = default;
    };

    MemoryManager()                     = default;
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager(MemoryManager&& other) {
        swap(other);
    }
    MemoryManager& operator=(const MemoryManager&) = delete;
    MemoryManager& operator=(MemoryManager&& other) {
        swap(other);
        return *this;
    }
    // TODO: Parse mlock() params out for compact info
    MemoryManager(const char** apple) {
        // Eventually we will use this to parse parameters for controlling comapct info mlock()
        // We need to do this before allocator is created
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if (_simple_getenv(apple, "dyld_hw_tpro") != nullptr) {
            // Start in a writable state to allow bootstrap
            _tproEnable = true;
        }
#endif //  BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    }

#if !TARGET_OS_EXCLAVEKIT
    MemoryManager(Lock&& lock) : _lock(std::move(lock)) {}

    void adoptLock(Lock&& lock) {
        _lock = std::move(lock);
    }
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
    friend struct PersistentAllocator;

#if !TARGET_OS_EXCLAVEKIT
    [[nodiscard]] Lock::Guard lockGuard();
#endif // !TARGET_OS_EXCLAVEKIT
    void writeProtect(bool protect);

    __attribute__((always_inline))
    void makeWriteable(WriteProtectionState& previousState) {
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if (_tproEnable) {
            os_compiler_barrier();
            // Stacks are in  memory, so it is possible to attack tpro via writing the stack vars in another thread. To protect
            // against this we create a signature that mixes the value of writable and its address then validate it later. If the
            // barriers work the state will never spill to the stack between varification and usage.
            previousState.data      = os_thread_self_restrict_tpro_is_writable();
  #if __has_feature(ptrauth_calls)
            previousState.signature = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
#endif /* __has_feature(ptrauth_calls) */
            if (!previousState.data) {
                os_thread_self_restrict_tpro_to_rw();
            }
            os_compiler_barrier();
            return;
        }
        previousState.data      = 1;
#if __has_feature(ptrauth_calls)
        previousState.signature = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
#endif /* __has_feature(ptrauth_calls) */
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
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if (_tproEnable) {
            os_compiler_barrier();
            // Stacks are in  memory, so it is possible to attack tpro via writing the stack vars in another thread. To protect
            // against this we create a signature that mixes the value of writable and its address then validate it later. If the
            // barriers work the state will never spill to the stack between varification and usage.
            previousState.data      = os_thread_self_restrict_tpro_is_writable();
#if __has_feature(ptrauth_calls)
            previousState.signature = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
#endif /* __has_feature(ptrauth_calls) */
            if (previousState.data) {
                os_thread_self_restrict_tpro_to_ro();
            }
            os_compiler_barrier();
            return;
        }
        previousState.data      = -1;
#if __has_feature(ptrauth_calls)
        previousState.signature = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
#endif /* __has_feature(ptrauth_calls) */
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
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if (_tproEnable) {
            os_compiler_barrier();
#if __has_feature(ptrauth_calls)
            uintptr_t signedWritableState = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
            if (previousState.signature != signedWritableState) {
                // Someone tampered with writableState. Process is under attack, we need to abort();
                abort();
            }
#endif /* __has_feature(ptrauth_calls) */

            uintptr_t state = os_thread_self_restrict_tpro_is_writable();
            if (state != previousState.data) {
                if (previousState.data) {
                    os_thread_self_restrict_tpro_to_rw();
                } else {
                    os_thread_self_restrict_tpro_to_ro();
                }
            }
            os_compiler_barrier();
            return;
        }
#if __has_feature(ptrauth_calls)
        uintptr_t signedWritableState = ptrauth_sign_generic_data(previousState.data, (uintptr_t)&previousState.data | (uintptr_t)this);
        if (previousState.signature != signedWritableState) {
            // Someone tampered with writable state. Process is under attack, we need to abort();
            abort();
        }
#endif /* __has_feature(ptrauth_calls) */
        {
            __unused auto lock = lockGuard();
            if (previousState.data == -1) {
                if (_writeableCount == 0) {
                    writeProtect(false);
                }
                _writeableCount += 1;
            } else if (previousState.data == 1) {
                _writeableCount -= 1;
                if (_writeableCount == 0) {
                    writeProtect(true);
                }
            }
        }
#endif // BUILD_DYLD && !TARGET_OS_EXCLAVEKIT
    }
    void swap(MemoryManager& other) {
        using std::swap;
#if !TARGET_OS_EXCLAVEKIT
        swap(_lock,                     other._lock);
#endif // !TARGET_OS_EXCLAVEKIT
        swap(_allocator,                other._allocator);
        swap(_writeableCount,           other._writeableCount);

        // We don't actually swap this because it is a process wide setting, and we may need it to be set correctly
        // even in the bootstrapMemoryProtector adfter move construction
        _tproEnable = other._tproEnable;
    }

    int vmFlags() const {
        int result = 0;
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        if (_tproEnable) {
            // add tpro for memory protection on platform that support it
            result |= VM_FLAGS_TPRO;
        }
        // Only include the dyld tag for allocations made by dyld
        result |= VM_MAKE_TAG(VM_MEMORY_DYLD);
#endif // BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
        return result;
    }

#if !TARGET_OS_EXCLAVEKIT
    Lock                    _lock;
#endif // !TARGET_OS_EXCLAVEKIT
    PersistentAllocator*    _allocator                  = nullptr;
    uint64_t                _writeableCount             = 0;
    bool                    _tproEnable                 = false;
};

// C++ does not (generally speaking) support destructive moves. In the case of heap allocated (allocator backed objects) where the
// collectoin classes explicitly call destructors we can often get the some of the same performance by tricks invoking move construction
// and eliding calls to destruct the previous location, etc.

struct VIS_HIDDEN AllocationMetadata {
    enum Type {
        NormalPtr   = 0,
        UniquePtr   = 1,
        SharedPtr   = 2
    };
    AllocationMetadata(Allocator* allocator, uint64_t size);
    static AllocationMetadata*  getForPointer(void*);
    Allocator&                  allocator() const; // allocator offset 24 bit
    uint64_t                    size() const; // 1 bit granule size : 21 granule count
    Type                        type() const;
    void                        setType(Type type);
    void freeObject();
    void incrementRefCount();
    bool decrementRefCount();
    void incrementWeakRefCount();
    bool decrementWeakRefCount();
    static uint64_t goodSize(uint64_t);
    template<typename T> static uint64_t goodSize() {
        return goodSize(sizeof(T));
    }
//private:
    //TODO: This can be packed tighter, and will need to be if we ever do weak refs
    static constexpr uint8_t granules[]     = {4, 15, 26, 37};
    uint64_t                _allocator      : 49; // 8 byte min alignment
    uint64_t                _size           : 11;
    uint64_t                _sizeClass      : 2;
    uint64_t                _type           : 2;
    //FIXME: When libc++ supports std::atomic_ref we can use those, for now due horrible tricks with casts
    uint32_t                _refCount       = 0;  // SharedPtr refCount
    uint32_t                _weakRefCount   = 0;  // WeakPtr refCount
};

template<typename T>
struct VIS_HIDDEN UniquePtr {
    UniquePtr() = default;
    constexpr UniquePtr(std::nullptr_t) : UniquePtr() {};
    template<class  U> explicit UniquePtr(U* data) : _data(data) {
        if (!_data) { return; }
        auto metadata = AllocationMetadata::getForPointer((void*)_data);
        assert(metadata->type() == AllocationMetadata::NormalPtr);
        metadata->setType(AllocationMetadata::UniquePtr);
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
            auto metadata = AllocationMetadata::getForPointer((void*)_data);
            assert(metadata->type() == AllocationMetadata::UniquePtr);
            _data->~T();
            metadata->setType(AllocationMetadata::NormalPtr);
            metadata->freeObject();
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
        if (_data) {
            auto metadata = AllocationMetadata::getForPointer((void*)_data);
            assert(metadata->type() == AllocationMetadata::UniquePtr);
            metadata->setType(AllocationMetadata::NormalPtr);
        }
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
    SharedPtr() = default;
    constexpr SharedPtr(std::nullptr_t) : SharedPtr() {};
    explicit SharedPtr(T* data) : _data(data) {
        if (!_data) { return; }
        auto metadata = AllocationMetadata::getForPointer(_data);
        if (metadata->type() == AllocationMetadata::NormalPtr) {
            metadata->setType(AllocationMetadata::SharedPtr);
            // FIXME: Do we need a barrier here?
        } else {
            metadata->incrementRefCount();
        }
    }
    SharedPtr(const SharedPtr& other) : _data(other._data) {
        if (!_data) { return; }
        auto metadata = AllocationMetadata::getForPointer(_data);
        metadata->incrementRefCount();
    };
    template<class U>
    SharedPtr(const SharedPtr<U>& other) : SharedPtr((T*)other._data) {
        if (!_data) { return; }
        auto metadata = AllocationMetadata::getForPointer(_data);
        metadata->incrementRefCount();
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
        if (!_data) { return; }
        auto metadata = AllocationMetadata::getForPointer(_data);
        if (metadata->decrementRefCount()) {
            _data->~T();
            metadata->freeObject();
        }
    }
    explicit operator bool() const {
        return (_data != nullptr);
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
    friend void swap(SharedPtr& x, SharedPtr& y) {
        x.swap(y);
    }
    //TODO: Move this to opeator<=> once C++20 imp is more complete
    bool operator<(const SharedPtr& other) const {
        return *_data < *other._data;
    }
private:
    void swap(SharedPtr& other) {
        if (&other == this) { return; }
        std::swap(_data, other._data);
    }
    template<typename U>
    void swap(SharedPtr<U>& other) {
        auto tmp = (SharedPtr*)&other;
        if (tmp == this) { return; }
        std::swap(_data, tmp->_data);
    }
    template<typename U> friend struct SharedPtr;
    T*  _data = nullptr;
};

struct VIS_HIDDEN Allocator {
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
    //   These present an interface similiar to normal malloc, and return managed pointers than can be used
    //   with the SharedPtr and UniquePtr classes
    void*                   malloc(uint64_t size);
    void*                   aligned_alloc(uint64_t alignment, uint64_t size);
    void                    free(void* ptr);
    char*                   strdup(const char*);
    virtual bool            owned(const void* p, uint64_t nbytes) const = 0;
    virtual void            destroy() = 0;
    virtual MemoryManager*  memoryManager() { return nullptr; }
    static Allocator&       persistentAllocator(MemoryManager&& memoryManager);
    static Allocator&       persistentAllocator();
#if !BUILDING_DYLD
    // Returns a shared persistent allocator
    static Allocator&       defaultAllocator();
#endif
    static void* align(uint64_t alignment, uint64_t size, void*& ptr, uint64_t& space);
#pragma mark Primitive methods to be provided by subclasses
public:
    virtual uint64_t      allocated_bytes() const = 0;
    virtual uint64_t      vm_allocated_bytes() const = 0;
    // For debugging
    virtual void        validate() const {};
    virtual void        debugDump() const {};
protected:
    template<typename T> friend struct UniquePtr;
    template<typename T> friend struct Vector;
    template<typename T, class C, bool M> friend struct BTree;
    [[nodiscard]] virtual Buffer    allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) = 0;
    // Deallocates a buffer returned from allocate_bytes
    virtual void                    deallocate_buffer(Buffer buffer) = 0;
#pragma mark Common functions for subclasses
    [[nodiscard]] Buffer            allocate_buffer(uint64_t nbytes, uint64_t alignment);
    void                            deallocate_buffer(void* p, uint64_t nbytes, uint64_t alignment);
    [[nodiscard]] static Buffer     vm_allocate_bytes(uint64_t size, int flags);
    static void                     vm_deallocate_bytes(void* p, uint64_t size);
    Allocator() = default;
    static_assert(sizeof(AllocationMetadata) <= kGranuleSize, "Granule must be large enough to hold AllocationMetadata");
    static_assert(alignof(AllocationMetadata) <= kGranuleSize, "AllocationMetadata must be naturally aligned ona granule");
};

struct __attribute__((aligned(16))) VIS_HIDDEN EphemeralAllocator : Allocator {
                        EphemeralAllocator();
                        EphemeralAllocator(MemoryManager&);
                        EphemeralAllocator(void* buffer, uint64_t size);
                        EphemeralAllocator(void* buffer, uint64_t size, MemoryManager&);
                        EphemeralAllocator(const EphemeralAllocator& other);
                        EphemeralAllocator(EphemeralAllocator&& other);
                        ~EphemeralAllocator();
    EphemeralAllocator& operator=(const EphemeralAllocator& other)          = delete;
    EphemeralAllocator& operator=(EphemeralAllocator&& other);

    uint64_t              allocated_bytes() const override;
    uint64_t              vm_allocated_bytes() const override; // Only for testing, should be private but no way to make an ObjC friend
    bool                owned(const void* p, uint64_t nbytes) const override;
    void                destroy() override;
    void                reset();
    friend              void swap(EphemeralAllocator& x, EphemeralAllocator& y) {
        x.swap(y);
    }
protected:
    [[nodiscard]] Buffer    allocate_buffer(uint64_t nbytes, uint64_t alignment, uint64_t prefix) override;
    void                    deallocate_buffer(Buffer buffer) override;
private:
    void                    swap(EphemeralAllocator& other);
    struct RegionListEntry {
        Buffer              buffer;
        RegionListEntry     *next   = nullptr;
        RegionListEntry(Buffer& B, RegionListEntry* N) : buffer(B), next(N) {}
    };
    MemoryManager*      _memoryManager  = nullptr;
    Buffer              _freeBuffer     = { nullptr, 0 };
    RegionListEntry*    _regionList     = nullptr;
    uint64_t            _allocatedBytes = 0;
};

} // namespace lsl


// These are should never be used. To prevent accidental usage, the prototypes exist, but using will cause a link error
VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator* allocator);
VIS_HIDDEN void* operator new(std::size_t count, std::align_val_t al, lsl::Allocator* allocator);

#endif /*  LSL_Allocator_h */
