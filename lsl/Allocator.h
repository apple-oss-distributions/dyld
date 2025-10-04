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
#include <span>
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


#if ENABLE_CRASH_REPORTER
#include <CrashReporterClient.h>
#endif

#include <new>
#include <stdio.h>

#if DYLD_FEATURE_USE_HW_TPRO
#define __ptrauth_dyld_tpro_stack           __ptrauth(ptrauth_key_process_independent_data, 1, ptrauth_string_discriminator("__ptrauth_dyld_tpro_stack"))
#endif // DYLD_FEATURE_USE_HW_TPRO

// FIXME: Copied from LibSsystemHelpers.h
#if TARGET_OS_EXCLAVEKIT
typedef int                         kern_return_t;
#endif

namespace dyld4 {
class RuntimeState;
};

namespace callback_impl
{

template <typename F>
struct return_type : public return_type<decltype(&F::operator())>
{
};

template <class ClassTy, class R, class... A>
struct return_type<R(ClassTy::*)(A...) const>
{
    typedef R type;
};

template <class R, class... A>
struct return_type<R (*)(A...)>
{
    typedef R type;
};

template <class R, class... A>
struct return_type<R (^)(A...)>
{
    typedef R type;
};

} // namespace callback_impl

namespace lsl {

struct Allocator;

//
// MARK: --- ProtectedStackReturnType ---
//
// The work() lambda called by the with*() functions may be called
// from a read-write stack, but ultimately return to a variable in the read-only TPRO-stack.
// That is ok if the result is in a register, but struct returns are a problem
// if the write is to a stack which is currently not writable.
// This wraps up all allowed values which we can guarantee fit in a register
struct ProtectedStackReturnType
{
    // Copied from LibSystemHelpers
    typedef bool (*FuncLookup)(const char* name, void** addr);

    ProtectedStackReturnType() = default;

    // convert from required type to this wrapper
    ProtectedStackReturnType(size_t val) {
        v.size = val;
    }
    ProtectedStackReturnType(kern_return_t val) {
        v.kr = val;
    }
    ProtectedStackReturnType(bool val) {
        v.boolean = val;
    }
    ProtectedStackReturnType(void* val) {
        v.voidptr = val;
    }
    ProtectedStackReturnType(char* val) {
        v.charptr = val;
    }
    ProtectedStackReturnType(const char* val) {
        v.constcharptr = val;
    }
    ProtectedStackReturnType(FuncLookup val) {
        v.funcptr = val;
    }

    // Convert back to result types
    operator size_t() const         { return v.size; }
    operator kern_return_t() const  { return v.kr; }
    operator bool() const           { return v.boolean; }
    operator void*() const          { return v.voidptr; }
    operator const char*() const    { return v.charptr; }
    operator char*() const          { return v.charptr; }
    operator FuncLookup() const     { return v.funcptr; }

private:
    union {
        size_t          size;
        kern_return_t   kr;
        bool            boolean;
        void*           voidptr;
        char*           charptr;
        const char*     constcharptr;
        FuncLookup      funcptr;
    } v;
};

static_assert(sizeof(ProtectedStackReturnType) == sizeof(uintptr_t));

//
// MARK: --- ProtectedStack ---
//

class VIS_HIDDEN ProtectedStack
{
public:
    ProtectedStack(bool isEnabledInProcess);

    // Allocates a new stack, to avoid keeping dirty memory around from a previous use
    // If there is no stack, just allocates one.  If there's an existing stack it will
    // deallocate it and allocate a new one.
    void reset();

    void withProtectedStack(void (^work)(void));
    void withNestedProtectedStack(void (^work)(void));
    ProtectedStackReturnType withNestedRegularStack(ProtectedStackReturnType (^work)(void));

    bool enabled() const;

    // Returns true if the stack is being used on the current frame
    bool onStackInCurrentFrame() const;

    // Returns true if the stack is being used on the given frame
    bool onStackInFrame(const void* frameAddr) const;

    // Returns true if the stack is being used on any frame in this thread
    bool onStackInAnyFrameInThisThread() const;

    void getRange(const void*& stackBottom, const void*& stackTop) const;

private:
    void allocateStack();

    static const void* getCurrentThreadId();

#if DYLD_FEATURE_USE_HW_TPRO
    // Worker threads gets 512KB, so match that for the dyld stack
    constexpr static uint64_t stackSize             = 512 * 1024;
    constexpr static uint64_t guardPageSize         = 16 * 1024;
    void* __ptrauth_dyld_tpro_stack topOfStack      = nullptr;
    void* __ptrauth_dyld_tpro_stack bottomOfStack   = nullptr;
    void* __ptrauth_dyld_tpro_stack stackBuffer     = nullptr;

    // We might go RW->RO->RW->...
    // These track the next stack location to push a new TPRO/regular frame
    void* __ptrauth_dyld_tpro_stack nextTPROStackAddr       = nullptr;
    void* __ptrauth_dyld_tpro_stack nextRegularStackAddr    = nullptr;

    // which thread owns the TPRO stack, ie, has the writer lock
    const void* __ptrauth_dyld_tpro_stack threadId = nullptr;
#endif // DYLD_FEATURE_USE_HW_TPRO
};

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

struct VIS_HIDDEN MemoryManager {
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

    MemoryManager()                                 = delete;
    MemoryManager(const MemoryManager&)             = delete;
    MemoryManager(MemoryManager&& other)            = delete;
    MemoryManager& operator=(const MemoryManager&)  = delete;
    MemoryManager& operator=(MemoryManager&& other) = delete;

    // Support for creating allocatos
    static Allocator&       defaultAllocator();

    static void init(const char** envp = nullptr, const char** apple = nullptr, void* dyldSharedCache = nullptr);
    static MemoryManager& memoryManager();

    void setDyldCacheAddr(void* sharedCache);
    void setProtectedStack(ProtectedStack& protectedStack);
    void clearProtectedStack();

#if !TARGET_OS_EXCLAVEKIT
    MemoryManager(Lock&& lock);
    void adoptLock(Lock&& lock);
#endif // !TARGET_OS_EXCLAVEKIT

#if DYLD_FEATURE_EMBEDDED_PAGE_ALLOCATOR
    [[nodiscard]] static void*      allocate_pages(uint64_t size);
    static void                     deallocate_pages(void* p, uint64_t size);
#endif /* DYLD_FEATURE_EMBEDDED_PAGE_ALLOCATOR */
    [[nodiscard]] Buffer            vm_allocate_bytes(uint64_t size, bool tproEnabled);
    void static                     vm_deallocate_bytes(void* p, uint64_t size);

    template<typename F>
    ALWAYS_INLINE static void withWritableMemory(F work) {
        MemoryManager& memoryManager = MemoryManager::memoryManager();
        memoryManager.withWritableMemoryInternal(work);
    }

private:
    template<typename F>
    ALWAYS_INLINE void withWritableMemoryInternal(F work) {
#if DYLD_FEATURE_USE_HW_TPRO
        // If we were on the TPRO stack in a higher frame then move back to it now
        if ( (_protectedStack != nullptr) && _protectedStack->onStackInAnyFrameInThisThread() ) {
            os_compiler_barrier();
            os_thread_self_restrict_tpro_to_rw();
            os_compiler_barrier();

            _protectedStack->withNestedProtectedStack(^() {
                work();
            });

            os_compiler_barrier();
            os_thread_self_restrict_tpro_to_ro();
            os_compiler_barrier();

            return;
        }

        if ( tproEnabled() ) {
            os_compiler_barrier();
            bool isWritable = os_thread_self_restrict_tpro_is_writable();
            os_compiler_barrier();

            if ( isWritable ) {
                // already writable, so just do the work without switching state
                work();
            } else {
                // not writable, so switch state
                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_rw();
                os_compiler_barrier();

                work();

                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_ro();
                os_compiler_barrier();
            }
            return;
        }
#endif // DYLD_FEATURE_USE_HW_TPRO

        // not tpro


#if DYLD_FEATURE_MPROTECT_ALLOCATOR
        {
            __unused auto lock = lockGuard();
            if (_writeableCount == 0) {
                writeProtect(false);
            }
            ++_writeableCount;
        }
#endif // DYLD_FEATURE_MPROTECT_ALLOCATOR

        work();

#if DYLD_FEATURE_MPROTECT_ALLOCATOR
        {
            __unused auto lock = lockGuard();
            _writeableCount -= 1;
            if (_writeableCount == 0) {
                writeProtect(true);
            }
        }
#endif // DYLD_FEATURE_MPROTECT_ALLOCATOR
    }

public:
    // Note, there is only one protected stack, so care needs to be taken
    // to avoid multiple threads using it at the same time.
    // As of writing, this is done by only taking the protected stack when
    // the loaders lock is also taken.
    template<typename F>
    ALWAYS_INLINE static void withProtectedStack(F work) {
#if DYLD_FEATURE_USE_HW_TPRO
        MemoryManager& memoryManager = MemoryManager::memoryManager();

        // We shouldn't be on the TPRO stack yet
        assert(!memoryManager._protectedStack->onStackInAnyFrameInThisThread());

        memoryManager._protectedStack->withProtectedStack(^() {
            work();
        });

        // reset the protected stack, to release its dirty memory
        memoryManager._protectedStack->reset();
#else
        work();
#endif // DYLD_FEATURE_USE_HW_TPRO
    }

    template<typename F>
    ALWAYS_INLINE static void withReadOnlyMemory(F work) {
        MemoryManager& memoryManager = MemoryManager::memoryManager();
        memoryManager.withReadOnlyMemoryInternal(work);
    }

private:
    template<typename F>
    ALWAYS_INLINE void withReadOnlyMemoryInternal(F work) {
#if DYLD_FEATURE_USE_HW_TPRO
        // If we're on the protected stack then we need to move back to the regular one
        // before we go RO
        if ( (_protectedStack != nullptr) && _protectedStack->onStackInCurrentFrame() ) {
            _protectedStack->withNestedRegularStack(^{
                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_ro();
                os_compiler_barrier();

                work();

                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_rw();
                os_compiler_barrier();

                return ProtectedStackReturnType();
            });
            return;
        }

        if ( tproEnabled() ) {
            os_compiler_barrier();
            bool isReadOnly = !os_thread_self_restrict_tpro_is_writable();
            os_compiler_barrier();

            if ( isReadOnly ) {
                // already read-only, so just do the work without switching state
                work();
            } else {
                // not read-only, so switch state
                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_ro();
                os_compiler_barrier();

                work();

                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_rw();
                os_compiler_barrier();
            }
            return;
        }
#endif // DYLD_FEATURE_USE_HW_TPRO

        // not tpro


#if DYLD_FEATURE_MPROTECT_ALLOCATOR
        {
            __unused auto lock = lockGuard();
            --_writeableCount;
            if (_writeableCount == 0) {
                writeProtect(true);
            }
        }
#endif // DYLD_FEATURE_MPROTECT_ALLOCATOR

        work();

#if DYLD_FEATURE_MPROTECT_ALLOCATOR
        {
            __unused auto lock = lockGuard();
            if (_writeableCount == 0) {
                writeProtect(false);
            }
            _writeableCount += 1;
        }
#endif //  DYLD_FEATURE_MPROTECT_ALLOCATOR
    }

public:
    template<typename F>
    ALWAYS_INLINE static auto withReadOnlyTPROMemory(F work) -> callback_impl::return_type<decltype(&F::operator())>::type
    {
        typedef typename callback_impl::return_type<decltype(&F::operator())>::type RetTy;

#if DYLD_FEATURE_USE_HW_TPRO
        MemoryManager& memoryManager = MemoryManager::memoryManager();

        // If we're on the protected stack then we need to move back to the regular one
        // before we go RO
        if ( (memoryManager._protectedStack != nullptr) && memoryManager._protectedStack->onStackInCurrentFrame() ) {
            ProtectedStackReturnType result = memoryManager._protectedStack->withNestedRegularStack(^{
                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_ro();
                os_compiler_barrier();

                ProtectedStackReturnType workResult = work();

                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_rw();
                os_compiler_barrier();

                return workResult;
            });

            return result;
        }

        if ( memoryManager.tproEnabled() ) {
            os_compiler_barrier();
            bool isReadOnly = !os_thread_self_restrict_tpro_is_writable();
            os_compiler_barrier();

            RetTy result;
            if ( isReadOnly ) {
                // already read-only, so just do the work without switching state
                result = work();
            } else {
                // not read-only, so switch state
                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_ro();
                os_compiler_barrier();

                result = work();

                os_compiler_barrier();
                os_thread_self_restrict_tpro_to_rw();
                os_compiler_barrier();
            }
            return result;
        }
#endif // DYLD_FEATURE_USE_HW_TPRO

        // not tpro, don't switch state, but do the work
        RetTy result = work();
        return result;
    }

#if DYLD_FEATURE_USE_HW_TPRO
    bool tproEnabled() const { return _tproEnable; }
#endif

#if SUPPORT_ROSETTA
    bool isTranslated() const { return _translated; }
#endif


private:
    friend struct Allocator;
#if !TARGET_OS_EXCLAVEKIT
    [[nodiscard]] Lock::Guard lockGuard();
#endif // !TARGET_OS_EXCLAVEKIT
    MemoryManager(const char** envp, const char** apple, void* dyldSharedCache, bool didInitialProtCopy);
    void writeProtect(bool protect);
    int vmFlags(bool tproEnabled) const;

#if !TARGET_OS_EXCLAVEKIT
    Lock                    _lock;
#endif // !TARGET_OS_EXCLAVEKIT
    Allocator*              _defaultAllocator           = nullptr;
    uint64_t                _writeableCount             = 0;
    bool                    _didInitialProtCopy         = false;

#if DYLD_FEATURE_USE_HW_TPRO
    bool                    _tproEnable                 = false;
#endif // DYLD_FEATURE_USE_HW_TPRO
    bool                    _translated                 = false;
#if BUILDING_DYLD
    void*                   _sharedCache                = nullptr;
#endif // BUILDING_DYLD

#if DYLD_FEATURE_USE_HW_TPRO
    ProtectedStack*         _protectedStack             = nullptr;
#endif

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
    static uint64_t         size(const void* p);

#if DYLD_FEATURE_USE_INTERNAL_ALLOCATOR
    struct Pool;
    Allocator(MemoryManager& memoryManager, Pool& pool);
    Allocator(MemoryManager& memoryManager);
    ~Allocator();

    // For by stack allocators
    void setInitialPool(Pool& pool);

    void setBestFit(bool);
    void validate() const;
    void dump() const;
    void reset();
    void forEachPool(void (^callback)(const Pool&));
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
        Pool()              = default;
        Pool(const Pool&)   = delete;
        Pool(Pool&&)        = delete;
        Pool(Allocator* allocator, Pool* prevPool, uint64_t size, bool tproEnabled);
        Pool(Allocator* allocator, Pool* prevPool, Buffer region, bool tproEnabled, bool asanEnabled);
        Pool(Allocator* allocator, Pool* prevPool, Buffer region, Buffer freeRegion, bool tproEnabled, bool asanEnabled);
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
        bool vmAllocated() const {
            return _vmAllocated;
        }
        void* highWaterMark() const {
            return _highWaterMark;
        }
    private:
        friend struct AllocationMetadata;
        Allocator*          _allocator          = nullptr;
        Pool*               _nextPool           = nullptr;
        Pool*               _prevPool           = nullptr;
        AllocationMetadata* _lastFreeMetadata   = nullptr;
        const Buffer        _poolBuffer;
        void*               _highWaterMark      = 0;
        bool                _vmAllocated        = false;

#if DYLD_FEATURE_USE_HW_TPRO
    bool                    _tproEnabled        = false;
#endif // DYLD_FEATURE_USE_HW_TPRO
    };
    Pool*               _firstPool      = nullptr;
    Pool*               _currentPool    = nullptr;
    uint64_t            _allocatedBytes = 0;
    uint64_t            _logID          = 0;
    bool                _bestFit        = false;
#endif /* DYLD_FEATURE_USE_INTERNAL_ALLOCATOR */
private:
    friend MemoryManager;
    Allocator() = default;
};

#if DYLD_FEATURE_USE_INTERNAL_ALLOCATOR
struct VIS_HIDDEN AllocatorGuard {
    AllocatorGuard(Allocator& allocator) : _allocator(allocator) {}
    ~AllocatorGuard() {
        _allocator.~Allocator();
    }
private:
    Allocator& _allocator;
};
#endif /* !DYLD_FEATURE_USE_INTERNAL_ALLOCATOR */

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
    uint64_t size() const {
        return Allocator::size((void*)_data);
    }
    std::span<std::byte> bytes() const {
        if (_data) {
            return std::span<std::byte>((std::byte*)_data, size());
        }
        return std::span<std::byte>();
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
#if !DYLD_FEATURE_USE_INTERNAL_ALLOCATOR
        void* ctrlData = ::aligned_alloc(alignof(Ctrl),sizeof(Ctrl));
#else
        auto metadata = Allocator::AllocationMetadata::forPtr((void*)data);
        auto allocator = metadata->pool()->allocator();
        void* ctrlData = allocator->aligned_alloc(alignof(Ctrl),sizeof(Ctrl));
#endif
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

#if DYLD_FEATURE_USE_INTERNAL_ALLOCATOR
// We do this in a macro without creating a scope so we can have the stack allocated stroage
// That forces us to do this all in a macro with variables exposed into the scope, so we prefix them

static inline
lsl::Allocator& __stackAllocatorInternal(void* storage, uint64_t count) {
    lsl::MemoryManager::Buffer buffer{ storage, count };
    if (!buffer.align(alignof(lsl::Allocator), sizeof(lsl::Allocator))) {
        assert(0 && "Count not create aligned buffer");
    }
    void *allocatorAddress = buffer.address;
    buffer.consumeSpace(sizeof(lsl::Allocator));

    if (!buffer.align(alignof(lsl::Allocator::Pool), sizeof(lsl::Allocator::Pool))) {
        assert(0 && "Count not create aligned buffer");
    }
    void *poolAddress = buffer.address;
    buffer.consumeSpace(sizeof(lsl::Allocator::Pool));

    if (!buffer.align(16, buffer.size-16)) {
        assert(0 && "Count not create aligned buffer");
    }

    auto result = new (allocatorAddress) lsl::Allocator(lsl::MemoryManager::memoryManager());

    // Disable TPRO on ephemeral allocators to avoid exhausting virtual address space
    auto pool = new (poolAddress) lsl::Allocator::Pool(result, nullptr, buffer, buffer, false /* tproEnable */, false /* asanEnable */);
    result->setInitialPool(*pool);
    return *result;
}

#define STACK_ALLOCATOR(_name, _count)                                                                                                  \
    uint64_t        __ ## _name ## _Size =  2 * (sizeof(lsl::Allocator::Pool) + alignof(lsl::Allocator::Pool)) + _count + 16            \
                                            + alignof(lsl::Allocator) + sizeof(lsl::Allocator)                                          \
                                            + alignof(lsl::Allocator::AllocationMetadata) + sizeof(lsl::Allocator::AllocationMetadata); \
    void* __ ## _name ## _Storage = alloca(__ ## _name ## _Size);                                                                       \
    auto& _name = __stackAllocatorInternal(__ ## _name ## _Storage, __ ## _name ## _Size);                                              \
    lsl::AllocatorGuard __##_name##_gaurd(_name);
#else
#define STACK_ALLOCATOR(_name, _count) \
    lsl::Allocator& _name = lsl::MemoryManager::defaultAllocator();
#endif

// These are should never be used. To prevent accidental usage, the prototypes exist, but using will cause a link error
VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator* allocator);
VIS_HIDDEN void* operator new(std::size_t count, std::align_val_t al, lsl::Allocator* allocator);

#endif /*  LSL_Allocator_h */
