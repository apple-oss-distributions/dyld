/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
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

// FIXME: Move to std::endian when libc++ implements it

#ifndef ByteStream_h
#define ByteStream_h

#include <cstdint>
#include <cstring>
#include <string_view>
//#include <libkern/OSByteOrder.h>

#include "Vector.h"

/* This provides a thin wrapper over Vector<std::byte>. The primary feature it includes is the ability to handle endian swaps.
 * This is useful when a stream contains both big and little endian data, for example, when AppleArchive (little endian) wraps
 * a binary plist (big endian). It is implemented by overloading push_back for all integer types (where swaps are handled), as well
 * as for non-integer types that do not need swapping such as string_views.
 */

struct ByteStream {
    enum Endian {
        Little  = 0,
        Big     = 1,
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        Native  = Little
#else
        Native  = Big
#endif
    };
    template<typename T> using Vector = lsl::Vector<T>;
    using Allocator = lsl::Allocator;
    using value_type = std::byte;

    ByteStream(Allocator& allocator) : _bytes(allocator) {}
    using iterator = Vector<std::byte>::iterator;
    using const_iterator = Vector<std::byte>::const_iterator;
    iterator begin() {
        return _bytes.begin();
    }
    iterator end() {
        return _bytes.end();
    }
    const_iterator begin() const {
        return _bytes.begin();
    }
    const_iterator end() const {
        return _bytes.end();
    }
    const_iterator cbegin() const {
        return _bytes.begin();
    }
    const_iterator cend() const {
        return _bytes.end();
    }

    template<typename T>
    void push_back(T value) {
        if (_endian != Endian::Native) {
            if constexpr(sizeof(T) == 2) {
                value = __builtin_bswap16(value);
            } else if constexpr(sizeof(T) == 4) {
                value = __builtin_bswap32(value);
            } else if constexpr(sizeof(T) == 8) {
                value = __builtin_bswap64(value);
            }
        }
        std::byte* swappedValueBytes = reinterpret_cast<std::byte*>(&value);
        std::copy(swappedValueBytes, swappedValueBytes+sizeof(T), std::back_inserter(_bytes));
    }

    template<>
    void push_back<std::byte>(std::byte value) {
        _bytes.push_back(value);
    }

    template<>
    void push_back<uint8_t>(uint8_t value) {
        _bytes.push_back(std::byte{value});
    }

    template<>
    void push_back<std::string_view>(std::string_view value) {
        (void)_bytes.insert(_bytes.end(), (std::byte*)&*value.begin(), (std::byte*)&*value.end());
    }

    template<>
    void push_back<const char*>(const char* value) {
       (void)_bytes.insert(_bytes.end(), (std::byte*)value, (std::byte*)value+strlen(value));
    }

    void push_back(uint8_t size, uint64_t value) {
        switch (size) {
            case 1: push_back((uint8_t)value); break;
            case 2: push_back((uint16_t)value); break;
            case 4: push_back((uint32_t)value); break;
            case 8: push_back((uint64_t)value); break;
        }
    }

    uint64_t size() const {
        return _bytes.size();
    }
    void resize(uint64_t newCapacity) {
        _bytes.resize(newCapacity);
    }
    void clear() {
        _bytes.clear();
    }
    Allocator& allocator() {
        return *_bytes.allocator();
    }
    void setEndian(Endian endian) {
        _endian = endian;
    }
    std::byte* bytes() {
        return _bytes.data();
    }
    const std::byte* bytes() const {
        return _bytes.data();
    }
    std::byte&          operator[](uint64_t pos)       { return _bytes[pos]; }
    const std::byte&    operator[](uint64_t pos) const { return _bytes[pos]; }

    template< class InputIt >
    iterator insert(const_iterator pos, InputIt first, InputIt last ) {
        return _bytes.insert(pos, first, last);
    }
    const std::span<std::byte> span() const {
        if (_bytes.empty()) { return std::span<std::byte>(); }
        return std::span((std::byte*)_bytes.data(), (size_t)_bytes.size());
    }
private:
    Vector<std::byte>   _bytes;
    Endian              _endian = Endian::Little;
};


#endif /* ByteStream_h */
