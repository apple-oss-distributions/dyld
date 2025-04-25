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
    using size_type         = uint64_t;
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
        std::copy(other.begin(), other.end(), _buffer);
    }
    Vector(Vector&& other, Allocator& allocator) : _allocator(&allocator) {
        std::swap(_size,        other._size);
        if (_allocator == other._allocator) {
            std::swap(_capacity,    other._capacity);
            std::swap(_buffer,      other._buffer);
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
    iterator begin()                                { return _buffer; }
    iterator end()                                  { return _buffer + _size; }
    const_iterator begin() const                    { return _buffer; }
    const_iterator end() const                      { return _buffer + _size; }
    const_iterator cbegin() const noexcept          { return _buffer; }
    const_iterator cend() const noexcept            { return _buffer + _size; }

    reference at(size_type pos)                     { return *(_buffer + pos); }
    const_reference at(size_type pos) const         { return *(_buffer + pos); }
    reference       operator[](size_type pos)       { return *(_buffer + pos); }
    const_reference operator[](size_type pos) const { return *(_buffer + pos); }
    
    reference       front()                         { return *_buffer; }
    const_reference front() const                   { return *_buffer; }
    reference       back()                          { return *(_buffer + _size - 1); }
    const_reference back() const                    { return *(_buffer + _size - 1); }
#pragma mark -
    constexpr pointer data()                        { return _buffer; }
    constexpr const_pointer data() const            { return _buffer; }
    
    [[nodiscard]] constexpr bool empty() const      { return (_size == 0); }
    size_type size() const                          { return _size; }
    size_type capacity() const                      { return _capacity; }
    void clear() {
        deleteElements(0, _size);
        _size = 0;
    }
    void resize(size_type newCapacity) {
        if (newCapacity > capacity()) {
            reserve(newCapacity);
            _size = newCapacity;
            _capacity = newCapacity;
        } else if (newCapacity == 0) {
            deleteElements(0, _size);
            if (_buffer) {
                _allocator->free((void*)_buffer);
            }
            _buffer = nullptr;
            _size = 0;
            _capacity = 0;
        } else {
            deleteElements(newCapacity, _size);
            _size = newCapacity;
            _capacity = newCapacity;
            (void)_allocator->realloc((void*)_buffer, sizeof(T)*newCapacity);
        }
    }
    // This function exists purely to support stack allocated arrays
    void reserveExact(size_type newCapacity) {
        if (newCapacity <= capacity()) { return; }
        if (_allocator->realloc((void*)_buffer, sizeof(T)*newCapacity)) {
            _capacity = newCapacity;
            return;
        }
        auto buffer = _allocator->aligned_alloc(std::max(16UL, alignof(T)), sizeof(T)*newCapacity);
        for (auto i = 0; i < _size; ++i) {
            (void)new ((void*)((uintptr_t)buffer+(i*sizeof(T)))) T(std::move(_buffer[i]));
        }
        deleteElements(0, _size);
        if (_buffer) {
            _allocator->free((void*)_buffer);
        }
        _buffer = (value_type *)buffer;
        _size = std::min(newCapacity, _size);
        _capacity = newCapacity;
        assert(_capacity >= newCapacity);
    }
    void reserve(size_type newCapacity) {
        if (newCapacity <= _capacity) { return; }
        if (newCapacity < 16) {
            newCapacity = 16;
        } else {
            newCapacity = (uint64_t)lsl::bit_ceil(newCapacity);
        }
        reserveExact(newCapacity);
    }
    iterator insert( const_iterator pos, const T& value ) {
        auto offset = pos-begin();
        reserve(_size+1);
        std::copy_backward(_buffer + offset, _buffer + _size, _buffer + _size + 1);
        ++_size;
        _buffer[offset] = value;
        return &_buffer[offset];
    }
    iterator insert( const_iterator pos, T&& value ) {
        auto offset = pos-begin();
        reserve(_size+1);
        std::move_backward(_buffer + offset , _buffer + _size, _buffer + _size + 1);
        ++_size;
        std::swap(_buffer[offset], value);
        return &_buffer[offset];
    }
    iterator insert( const_iterator pos, size_type count, const T& value ) {
        auto offset = pos-begin();
        reserve(_size+count);
        std::move_backward(_buffer + offset, _buffer + _size, _buffer + _size + count);
        for(auto i = 0; i < count; ++i) {
            _buffer[offset+i] = value;
        }
        _size += count;
        return &_buffer[offset];
    }
    template< class InputIt >
    iterator insert( const_iterator pos, InputIt first, InputIt last ) {
        auto offset = pos-begin();
        auto count = last-first;
        reserve(_size+count);
        std::copy_backward(_buffer + offset, _buffer + _size, _buffer + _size + count);
        std::copy(first, last, _buffer + offset );
        _size += count;
        return _buffer + offset;
    }
    iterator erase(iterator pos) {
        if (pos == end()) {
            return end();
        }
        return erase(pos, pos + 1);
    }
    iterator erase(const_iterator pos) {
        if (pos == end()) {
            return end();
        }
        return erase(pos, pos + 1);
    }
    iterator erase(iterator first, iterator last) {
        if (first == last) {
            return end();
        }
        uint64_t firstIdx   = first - _buffer;
        uint64_t count      = last - first;
        std::move(last, end(), first);
        deleteElements(_size-count, _size);
        _size -= count;
        return _buffer + std::min(firstIdx, _size);
    }
    iterator erase(const_iterator first, const_iterator last) {
        if (first == last) {
            return end();
        }
        uint64_t firstIdx   = first - _buffer;
        uint64_t count      = last - first;
        std::move(last, cend(), (iterator)first);
        deleteElements(_size-count, _size);
        _size -= count;
        return _buffer + std::min(firstIdx, _size);
        
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
        (void)new((void*)(_buffer + _size)) value_type(std::forward<Args>(args)...);
        return _buffer[_size++];
    }
    void pop_back() {
        assert(_size > 0);
        deleteElements(_size-1, _size);
        _size--;
    }
    Allocator* allocator() const {
        return _allocator;
    }
private:
    Vector(const Vector& other): _allocator(other._allocator) {
        assert(_allocator != nullptr);
        resize(0);
        reserve(other._size);
        _size = other._size;
        std::copy(other.begin(), other.end(), _buffer);
    };
    void swap(Vector& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_allocator,    other._allocator);
        swap(_size,         other._size);
        swap(_capacity,     other._capacity);
        swap(_buffer,       other._buffer);
    }
    void deleteElements(uint64_t startIdx, uint64_t endIdx) {
        if constexpr(!std::is_trivially_destructible<value_type>::value) {
            for (auto i = _buffer + startIdx; i != _buffer + endIdx; ++i) {
                i->~value_type();
            }
        }
    }
    Allocator*  _allocator      = nullptr;
    value_type* _buffer         = nullptr;
    uint64_t    _size           = 0;
    uint64_t    _capacity       = 0;
};

} // namespace lsl

#define STACK_ALLOC_VECTOR_BYTE_SIZE(_type, _count) (16UL   + alignof(_type)              + sizeof(_type)*_count        \
                                                            + alignof(lsl::Vector<_type>) + sizeof(lsl::Vector<_type>))

#define STACK_ALLOC_VECTOR(_type, _name, _count)                                        \
    STACK_ALLOCATOR(__##_name##_allocator, STACK_ALLOC_VECTOR_BYTE_SIZE(_type,_count))  \
    lsl::Vector<_type> _name(__##_name##_allocator);                                    \
    _name.reserveExact(_count);

#define BLOCK_STACK_ALLOC_VECTOR(_type, _name, _count)                                  \
    STACK_ALLOCATOR(__##_name##_allocator, STACK_ALLOC_VECTOR_BYTE_SIZE(_type,_count))  \
    __block lsl::Vector<_type> _name(__##_name##_allocator);                            \
    _name.reserveExact(_count);

#endif /*  LSL_Vector_h */
