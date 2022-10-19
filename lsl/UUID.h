/*
* Copyright (c) 2020 Apple Inc. All rights reserved.
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
#ifndef UUID_h
#define UUID_h

#include <algorithm>
#include <array>
#include <string>
#include <cstdint>
#include <functional>

namespace lsl {
struct UUID {
    UUID() {}
    UUID(const uint8_t* uuid) {
        std::copy((std::byte*)&uuid[0], (std::byte*)&uuid[16], &_data[0]);
    }
    UUID(const std::byte* uuid) {
        std::copy(&uuid[0], &uuid[16], &_data[0]);
    }

    bool operator==(const UUID& other) const {
        return std::equal(_data.begin(), _data.end(), other._data.begin(), other._data.end());
    }
    bool operator!=(const UUID& other) const {
        return !(other == *this);
    }
    bool operator<(const UUID& other) const {
        return std::lexicographical_compare(_data.begin(), _data.end(), other._data.begin(), other._data.end());
    }

    explicit operator bool() const {
        return std::any_of(_data.begin(), _data.end(), [](const std::byte& x) {
            return x != (std::byte)0;
        });
    }
    void dumpStr(char uuidStr[64]) const {
        char* p = uuidStr;
        appendHexByte(_data[0], p);
        appendHexByte(_data[1], p);
        appendHexByte(_data[2], p);
        appendHexByte(_data[3], p);
        *p++ = '-';
        appendHexByte(_data[4], p);
        appendHexByte(_data[5], p);
        *p++ = '-';
        appendHexByte(_data[6], p);
        appendHexByte(_data[7], p);
        *p++ = '-';
        appendHexByte(_data[8], p);
        appendHexByte(_data[9], p);
        *p++ = '-';
        appendHexByte(_data[10], p);
        appendHexByte(_data[11], p);
        appendHexByte(_data[12], p);
        appendHexByte(_data[13], p);
        appendHexByte(_data[14], p);
        appendHexByte(_data[15], p);
        *p = '\0';
    }
    bool empty() const {
        for (auto i = 0; i < 16; ++i) {
            if (_data[i] != (std::byte)0) {
                return false;
            }
        }
        return true;
    }
    std::byte* begin() { return _data.begin(); }
    std::byte* end() { return _data.end(); }
    const std::byte* begin() const { return _data.begin(); }
    const std::byte* end() const { return _data.end(); }
    const std::byte* cbegin() const { return _data.begin(); }
    const std::byte* cend() const { return _data.end(); }
private:
    void appendHexNibble(uint8_t value, char*& p) const {
        if ( value < 10 )
            *p++ = '0' + value;
        else
            *p++ = 'A' + value - 10;
    }

    void appendHexByte(std::byte value, char*& p) const {
        uint8_t intValue = ((uint8_t)value & 0xff);
        appendHexNibble(intValue >> 4, p);
        appendHexNibble(intValue & 0x0F, p);
    }

    friend std::hash<lsl::UUID>;
    std::array<std::byte, 16> _data = { (std::byte)0 };
};
};

namespace std {
    template<> struct hash<lsl::UUID>
    {
        std::size_t operator()(lsl::UUID const& U) const noexcept {
            size_t result = 0;
            for (uint16_t i = 0; i < 16/sizeof(size_t); ++i) {
                size_t fragment = *((size_t *)&U._data[i*sizeof(size_t)]);
                result ^= fragment;
            }
            return result;
        }
    };
};

#endif /* UUID_h */
