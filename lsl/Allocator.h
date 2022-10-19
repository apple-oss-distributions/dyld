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

#include <limits>
#include <cassert>
#include <compare>
#include <cstddef>

#include <os/lock.h>
#include <sys/_pthread/_pthread_types.h>

#include <new>

#include "Defines.h"

//TODO: Implement UniquePtr <-> SharedPtr adoption
//TODO: WeakPtr support (since the allocator supports partial returns we can support very efficient zeroing weak refs)
//TODO: MallocStackLogging support

namespace lsl {

struct Allocator;

// C++ does not (generally speaking) support destructive moves. In the case of heap allocated (allocator backed objects) where the
// collectoin classes explicitly call destructors we can often get the some of the same performance by tricks invoking move construction
// and eliding calls to destruct the previous location, etc.

struct VIS_HIDDEN AllocationMetadata {
    enum Type {
        NormalPtr   = 0,
        UniquePtr   = 1,
        SharedPtr   = 2
    };
    AllocationMetadata(Allocator* allocator, size_t size);
    static AllocationMetadata*  getForPointer(void*);
    Allocator&                  allocator() const; // allocator offset 24 bit
    size_t                      size() const; // 1 bit granule size : 21 granule count
    Type                        type() const;
    void                        setType(Type type);
    void freeObject();
    void incrementRefCount();
    bool decrementRefCount();
    void incrementWeakRefCount();
    bool decrementWeakRefCount();
    static size_t goodSize(size_t);
    template<typename T> static size_t goodSize() {
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
        contract(metadata->type() == AllocationMetadata::NormalPtr);
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
            contract(metadata->type() == AllocationMetadata::UniquePtr);
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
            contract(metadata->type() == AllocationMetadata::UniquePtr);
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
    static const std::size_t    kGranuleSize                    = (16);

    // a tuple of an allocated <pointer, size>
    struct Buffer {
        void*   address;
        size_t  size;
        [[gnu::pure]] void*     lastAddress() const;                // end() ??
        bool                    contains(const Buffer&) const;
        bool                    valid() const;
        void                    dump() const;
        bool                    succeeds(const Buffer&) const;
        void                    remainders(const Buffer& other, Buffer& prefix, Buffer& postfix) const;
        Buffer                  findSpace(size_t targetSize, size_t targetAlignment, size_t prefix) const;
        explicit                operator bool() const;
        auto                    operator<=>(const Buffer&) const = default;
    };

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
    void*                   malloc(size_t size);
    void*                   aligned_alloc(size_t alignment, size_t size);
    void                    free(void* ptr);
    char*                   strdup(const char*);
    virtual bool            owned(const void* p, std::size_t nbytes) const = 0;
    virtual void            destroy() = 0;
    //FIXME: Remove once we land APRR
    virtual void            writeProtect(bool protect) {};
    static Allocator&       persistentAllocator(bool useHWTPro = false);
#if CONCURRENT_ALLOCATOR_SUPPORT
    static Allocator&       concurrentAllocator();
    static Allocator&       defaultAllocator();
#endif
    static void* align(size_t alignment, size_t size, void*& ptr, size_t& space);
#pragma mark Primitive methods to be provided by subclasses
public:
    friend struct ConcurrentAllocator;
    virtual size_t      allocated_bytes() const = 0;
    virtual size_t      vm_allocated_bytes() const = 0;
    // For debugging
    virtual void        validate() const {};
    virtual void        debugDump() const {};
protected:
    template<typename T> friend struct UniquePtr;
    template<typename T> friend struct Vector;
    template<typename T, class C, bool M> friend struct BTree;
    [[nodiscard]] virtual Buffer    allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) = 0;
    // Deallocates a buffer returned from allocate_bytes
    virtual void                    deallocate_buffer(Buffer buffer) = 0;
    virtual int                     vm_allocate_flags() const;
#pragma mark Common functions for subclasses
    [[nodiscard]] Buffer            allocate_buffer(std::size_t nbytes, std::size_t alignment, Allocator** deallocator);
    void                            deallocate_buffer(void* p, std::size_t nbytes, std::size_t alignment);
    [[nodiscard]] static Buffer     vm_allocate_bytes(std::size_t size, int flags);
    static void                     vm_deallocate_bytes(void* p, std::size_t size);
    Allocator() = default;
    static_assert(sizeof(AllocationMetadata) <= kGranuleSize, "Granule must be large enough to hold AllocationMetadata");
    static_assert(alignof(AllocationMetadata) <= kGranuleSize, "AllocationMetadata must be naturally aligned ona granule");
};

struct __attribute__((aligned(16))) VIS_HIDDEN EphemeralAllocator : Allocator {
                        EphemeralAllocator() = default;
                        EphemeralAllocator(void* buffer, size_t size);
                        EphemeralAllocator(const EphemeralAllocator& other);
                        EphemeralAllocator(EphemeralAllocator&& other);
                        ~EphemeralAllocator();
    EphemeralAllocator& operator=(const EphemeralAllocator& other)          = delete;
    EphemeralAllocator& operator=(EphemeralAllocator&& other);

    size_t              allocated_bytes() const override;
    size_t              vm_allocated_bytes() const override; // Only for testing, should be private but no way to make an ObjC friend
    bool                owned(const void* p, std::size_t nbytes) const override;
    void                destroy() override;
    void                reset();
    friend              void swap(EphemeralAllocator& x, EphemeralAllocator& y) {
        x.swap(y);
    }
protected:
    [[nodiscard]] Buffer    allocate_buffer(std::size_t nbytes, std::size_t alignment, std::size_t prefix, Allocator** deallocator) override;
    void                    deallocate_buffer(Buffer buffer) override;
private:
    void                    swap(EphemeralAllocator& other);
    struct RegionListEntry {
        Buffer              buffer;
        RegionListEntry     *next   = nullptr;
        RegionListEntry(Buffer& B, RegionListEntry* N) : buffer(B), next(N) {}
    };
    Buffer              _freeBuffer = { nullptr, 0 };
    RegionListEntry*    _regionList = nullptr;
    size_t              _allocatedBytes = 0;
};

} // namespace lsl

// These are should never be used. To prevent accidental usage, the prototypes exist, but using will cause a link error
VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator* allocator);
VIS_HIDDEN void* operator new(std::size_t count, lsl::Allocator& allocator);
VIS_HIDDEN void* operator new(std::size_t count, std::align_val_t al, lsl::Allocator* allocator);

#endif /*  LSL_Allocator_h */
