/*
* Copyright (c) 2019 Apple Inc. All rights reserved.
*
* @APPLE_LICENSE_HEADER_START@
*
* "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
* Reserved.  This file contains Original Code and/or Modifications of
* Original Code as defined in and that are subject to the Apple Public
* Source License Version 1.0 (the 'License').  You may not use this file
* except in compliance with the License.  Please obtain a copy of the
* License at http://www.apple.com/publicsource and read it before using
* this file.
*
* The Original Code and all software distributed under the License are
* distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
* INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
* License for the specific language governing rights and limitations
* under the License."
*
* @APPLE_LICENSE_HEADER_END@
*/

// This is a mostly complete reimplementation of std::vector that can be safely used in dyld
// It does not support a methods we don't use like max_capacity

//FIXME: All the erase functions are broken

#ifndef  LSL_Vector_h
#define  LSL_Vector_h

#include <span>
#include <cassert>
#include <cstdlib>
#include <cstddef>
#include <algorithm>

#include "Defines.h"
#include "BitUtils.h"
#include "Allocator.h"


namespace lsl {

template<typename T>
struct TRIVIAL_ABI Vector {
#pragma mark -
#pragma mark Typedefs
    using value_type        = T;
    using size_type         = std::size_t;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using const_reference   = const value_type&;
    using pointer           = value_type*;
    using const_pointer     = const value_type*;
    using iterator          = value_type*;
    using const_iterator    = const value_type*;
#pragma mark -
#pragma mark Constructors / Destructors / Assignment Operators / swap
    Vector() = delete;
    //Vector(const Vector& other); Implementation is private
    Vector& operator=(const Vector& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    Vector(Vector&& other) {
        swap(other);
    }
    Vector& operator=(Vector&& other) {
        swap(other);
        return *this;
    }
    ~Vector() {
        if (_buffer) {
            resize(0);
        }
    }
    explicit Vector(Allocator& allocator) : _allocator(&allocator) {}
    Vector(const Vector& other, Allocator& allocator) :
            _allocator(&allocator),
            _size(0) {
        contract(_allocator != nullptr);
        reserve(other._size);
        _size = other._size;
        std::copy(other.begin(), other.end(), &_buffer[0]);
    }
    Vector(Vector&& other, Allocator& allocator) : _allocator(&allocator) {
        std::swap(_size,        other._size);
        if (_allocator == other._allocator) {
            std::swap(_deallocator,     other._deallocator);
            std::swap(_allocationSize,  other._allocationSize);
            std::swap(_buffer,          other._buffer);
        } else {
            reserve(_size);
            for(auto&& i : other) {
                push_back(i);
            }
            other.resize(0);
        }
    }
    template< class InputIt >
    Vector(InputIt first, InputIt last, Allocator& allocator) : _allocator(&allocator) {
        using category = typename std::iterator_traits<InputIt>::iterator_category;
        if constexpr (std::is_same_v<category, std::random_access_iterator_tag>) {
            reserve(last-first);
            std::copy(first, last, begin());
            _size = last-first;
        } else {
            for (auto i = first; i != last; ++i) {
                push_back(*i);
            }
        }
    }
    static Vector<T>* make(Allocator& allocator) {
        void* storage = allocator.malloc(sizeof(Vector<T>));
        return new (storage) Vector<T>(allocator);
    }
    Vector(std::initializer_list<T> ilist, Allocator& allocator) : _allocator(&allocator) {
        auto first = ilist.begin();
        auto last = ilist.end();
        reserve(last-first);
        std::copy(first, last, begin());
        _size = last-first;
    }
    explicit operator std::span<T>() {
        return std::span(begin(), end());
    }
    friend void swap(Vector<T> x, Vector<T> y) {
        return x.swap(y);
    }
#pragma mark -
#pragma mark Iterator support
    iterator begin()                                { return &_buffer[0]; }
    iterator end()                                  { return &_buffer[_size]; }
    const_iterator begin() const                    { return &_buffer[0]; }
    const_iterator end() const                      { return &_buffer[_size]; }
    const_iterator cbegin() const noexcept          { return &_buffer[0]; }
    const_iterator cend() const noexcept            { return &_buffer[_size]; }

    reference at(size_type pos)                     { return _buffer[pos]; }
    const_reference at(size_type pos) const         { return _buffer[pos]; }
    reference       operator[](size_type pos)       { return _buffer[pos]; }
    const_reference operator[](size_type pos) const { return _buffer[pos]; }
    
    reference       front()                         { return _buffer[0]; }
    const_reference front() const                   { return _buffer[0]; }
    reference       back()                          { return _buffer[_size-1]; }
    const_reference back() const                    { return _buffer[_size-1]; }
#pragma mark -
    constexpr pointer data()                        { return &_buffer[0]; }
    constexpr const_pointer data() const            { return &_buffer[0]; }
    
    [[nodiscard]] constexpr bool empty() const      { return (_size == 0); }
    size_type size() const                          { return _size; }
    size_type capacity() const                      { return _allocationSize/sizeof(T); }
    void clear() {
        if constexpr(!std::is_trivially_destructible<value_type>::value) {
            for (auto i = begin(); i != end(); ++i) {
                i->~value_type();
            }
        }
        _size = 0;
    }
    void resize(size_type newCapacity) {
        if (newCapacity > capacity()) {
            reserve(newCapacity);
        } else if (newCapacity == 0) {
            if constexpr(!std::is_trivially_destructible<value_type>::value) {
                for (auto i = &_buffer[0]; i != &_buffer[_size]; ++i) {
                    i->~value_type();
                }
            }
            if (_buffer) {
                _deallocator->deallocate_buffer((void*)_buffer, _allocationSize, std::max(16UL, alignof(T)));
            }
            _buffer = nullptr;
            _size = 0;
            _allocationSize = 0;
        } else {
            Allocator* deallocator = nullptr;
            auto [newBuffer, newBufferSize] = _allocator->allocate_buffer(sizeof(T)*newCapacity, std::max(16UL, alignof(T)), &deallocator);
            std::move(&_buffer[0], &_buffer[newCapacity], (value_type *)newBuffer);
            for (auto i = &_buffer[newCapacity]; i != &_buffer[_size]; ++i) {
                i->~value_type();
            }
            if (_buffer) {
                _deallocator->deallocate_buffer((void*)_buffer, _allocationSize, std::max(16UL, alignof(T)));
            }
            _deallocator = deallocator;
            _buffer = (value_type *)newBuffer;
            _size = newCapacity;
            _allocationSize = newBufferSize;
        }
    }
    // This function exists purely to support stack allocated arrays
    void reserveExact(size_type newCapacity) {
        if (newCapacity <= capacity()) { return; }
        Allocator* deallocator = nullptr;
        auto buffer = _allocator->allocate_buffer(sizeof(T)*newCapacity, std::max(16UL, alignof(T)), &deallocator);
        for (auto i = 0; i < _size; ++i) {
            (void)new ((void*)((uintptr_t)buffer.address+(i*sizeof(T)))) T(std::move(_buffer[i]));
        }
        if constexpr(!std::is_trivially_destructible<value_type>::value) {
            for (auto i = &_buffer[0]; i != &_buffer[_size]; ++i) {
                i->~value_type();
            }
        }
        if (_buffer) {
            _deallocator->deallocate_buffer((void*)_buffer, _allocationSize, std::max(16UL, alignof(T)));
        }
        _deallocator = deallocator;
        _buffer = (value_type *)buffer.address;
        _size = std::min(newCapacity, _size);
        _allocationSize = buffer.size;
        assert(capacity() >= newCapacity);
    }
    void reserve(size_type newCapacity) {
        if (newCapacity <= capacity()) { return; }
        if (newCapacity < 16) {
            newCapacity = 16;
        } else {
            newCapacity = (size_t)lsl::bit_ceil(newCapacity);
        }
        reserveExact(newCapacity);
    }
    iterator insert( const_iterator pos, const T& value ) {
        auto offset = pos-begin();
        reserve(_size+1);
        std::move_backward(&_buffer[offset], &_buffer[_size], &_buffer[_size+1]);
        ++_size;
        _buffer[offset] = value;
        return &_buffer[offset];
    }
    iterator insert( const_iterator pos, T&& value ) {
        auto offset = pos-begin();
        reserve(_size+1);
        std::move_backward(&_buffer[offset], &_buffer[_size], &_buffer[_size+1]);
        ++_size;
        std::swap(_buffer[offset], value);
        return &_buffer[offset];
    }
    iterator insert( const_iterator pos, size_type count, const T& value ) {
        auto offset = pos-begin();
        reserve(_size+count);
        std::move_backward(&_buffer[offset], &_buffer[_size], &_buffer[_size+count]);
        for(auto i = 0; i < count; ++i) {
            _buffer[offset+i] = value;
        }
        return &_buffer[offset];
    }
    template< class InputIt >
    iterator insert( const_iterator pos, InputIt first, InputIt last ) {
        auto offset = pos-begin();
        auto count = last-first;
        reserve(_size+count);
        std::move_backward(&_buffer[offset], &_buffer[_size], &_buffer[_size+count]);
        std::move(first, last, &_buffer[offset]);
        _size += count;
        return &_buffer[offset];
    }
    iterator erase(iterator pos) {
        contract(_size > 0);
        std::move(pos+1, end(), pos);
        --_size;
        return &_buffer[pos-begin()];
    }
    iterator erase(const_iterator pos) {
        contract(_size > 0);
        std::move(pos+1, cend(), (iterator)pos);
        --_size;
        return &_buffer[pos-cbegin()];
    }
    iterator erase(iterator first, iterator last) {
        uint64_t count = (last-first);
        std::move(last, end(), first);
        _size -= count;
        return &_buffer[first-begin()];

    }
    iterator erase(const_iterator first, const_iterator last) {
        uint64_t count = (last-first);
        std::move(last, cend(), (iterator)first);
        _size -= count;
        return &_buffer[first-cbegin()];
        
    }
    void push_back(const T& value) {
        reserve(_size+1);
        _buffer[_size++] = value;
    }
    void push_back(T&& value) {
        reserve(_size+1);
        _buffer[_size++] = std::move(value);
    }
    template< class... Args >
    reference emplace_back( Args&&... args ) {
        reserve(_size+1);
        (void)new((void*)&_buffer[_size]) value_type(std::forward<Args>(args)...);
        return _buffer[_size++];
    }
    
    void pop_back() {
        contract(_size > 0);
        if constexpr(!std::is_trivially_destructible<value_type>::value) {
            _buffer[_size].~value_type();
        }
        _size--;
    }
    
    Allocator* allocator() const {
        return _allocator;
    }
private:
    Vector(const Vector& other): _allocator(other._allocator) {
        contract(_allocator != nullptr);
        resize(0);
        reserve(other._size);
        _size = other._size;
        std::copy(other.begin(), other.end(), &_buffer[0]);
    };
    void swap(Vector& other) {
        using std::swap;

        if (this == &other) { return; }
        swap(_allocator,        other._allocator);
        swap(_deallocator,      other._deallocator);
        swap(_size,             other._size);
        swap(_allocationSize,   other._allocationSize);
        swap(_buffer,           other._buffer);
    }
    Allocator*  _allocator      = nullptr;
    Allocator*  _deallocator    = nullptr;
    value_type* _buffer         = nullptr;
    size_t      _size           = 0;
    size_t      _allocationSize = 0;
};

} // namespace lsl

#define STACK_ALLOC_VECTOR(_type, _name, _count)                                                                        \
    void* __##_name##_vector_storage = alloca(std::min(16UL,alignof(_type))+(sizeof(_type)*_count));                    \
    EphemeralAllocator __##_name##_vector_allocator(__##_name##_vector_storage, alignof(_type)+(sizeof(_type)*_count)); \
    lsl::Vector<_type> _name(__##_name##_vector_allocator);                                                             \
    _name.reserveExact(_count);

#define BLOCK_STACK_ALLOC_VECTOR(_type, _name, _count)                                                                        \
    void* __##_name##_vector_storage = alloca(std::min(16UL,alignof(_type))+(sizeof(_type)*_count));                    \
    EphemeralAllocator __##_name##_vector_allocator(__##_name##_vector_storage, alignof(_type)+(sizeof(_type)*_count)); \
    __block lsl::Vector<_type> _name(__##_name##_vector_allocator);                                                             \
    _name.reserveExact(_count);

#endif /*  LSL_Vector_h */
