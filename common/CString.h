/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef common_CString_h
#define common_CString_h

#include <string_view>
#include <span>
#include <string>
#include <assert.h>

// commmon
#include "Defines.h"

/*!
 * @class CString
 *
 * @abstract
 *      A type safe wrapper of a null-terminated string. It is based on std::string\_view, so it inherits string\_view's methods and can be used interchangeably with it.
 *
 *      string\_view methods that operate on the string bounds still return string\_view objects. This is the behaviour we need, since the new string\_view might no longer point to a null-terminated string.
 *      For this reason it's good to specialize certain methods where null-terminators can still be guaranteed.
 *      An example of such API specialization is the `substr(size_type pos)` method, where we know the end pointer won't change.
 *
 *      CString is also compatible with POSIX APIs that require null-terminated strings, unlike string\_view where there's no such guarantee.
 *      Compatibility with C strings isn't totally transparent and there are some limitations:
 *      - The type is only explicitly convertible to `const char*`. Implicit conversion could be supported, but it would require explicit implementation of existing string_view operators.
 *      - CString can't be used interchangeably with `const char*` in variadic APIs, such as `printf`. Instead we use `c_str` method or an explicit cast.
 *          Variadic functions don't have explicit types, so the type won't be converted to `const char*`, even if we'd have an implicit conversion operator.
 *          Compiler also warns about functions such as `printf` when trying to use CString object with the `%s` format.
 */
struct VIS_HIDDEN CString: public std::string_view
{

    constexpr CString(): std::string_view() {}
    constexpr CString(const char* cstr): std::string_view(cstr ? std::string_view(cstr) : std::string_view()) {}
              CString(const std::string& str): std::string_view(str) {}
              CString(const char* str, size_t len): std::string_view(str, len) {}

    // Allow std::string_view conversion only in a compile time context.
    // Runtime casting should be done only explicitly using ::fromSV API.
    consteval CString(std::string_view str): std::string_view(std::move(str)) {
        assert(str.data() == nullptr || *(str.data() + str.size()) == 0);
    }

    static constexpr CString fromSV(std::string_view str)
    {
        return CString(str, UnsafeSVCastTag());
    }

    constexpr const char* c_str() const { return std::string_view::data(); }
    explicit constexpr operator const char*() const { return c_str(); }

    static CString dup(std::string_view str)
    {
        char* buffer = (char*)malloc(str.size() + 1);
        memcpy(buffer, str.data(), str.size());
        buffer[str.size()] = 0;
        return fromSV(std::string_view(buffer, str.size()));
    }

    CString dup() const { return CString::dup(*this); }

    CString strcpy(char* dst) const
    {
        size_t size = this->size();
        memcpy(dst, c_str(), size);
        *(dst + size) = '\0';
        return CString(std::string_view(dst, size), UnsafeSVCastTag());
    }

    static CString strcpy(std::string_view src, char* dst)
    {
        memcpy(dst, src.data(), src.size());
        *(dst + src.size()) = '\0';
        return CString(std::string_view(dst, src.size()), UnsafeSVCastTag());
    }

    // Substring from an offset will still be a valid C string, as the end pointer doesn't change.
    CString substr(size_type pos) const
    {
        return fromSV(this->std::__1::string_view::substr(pos));
    }

    std::string_view substr(size_type pos, size_type n) const
    {
        return this->std::string_view::substr(pos, n);
    }

    CString dupSubstr(size_type pos, size_type n) const { return dup(substr(pos, n)); }

    // string_view::contains() was added in c++23, but we still build with c++20
    constexpr bool contains(std::string_view str) const {
        return (this->find(str) != std::string_view::npos);
    }

    static constexpr CString concat(std::span<const std::string_view> strs)
    {
        size_t length = 0;
        for (std::string_view s : strs) {
            length += s.size();
        }

        char* buffer = (char*)malloc(length + 1);
        char* ptr = buffer;

        for (std::string_view s : strs) {
            memcpy(ptr, s.data(), s.size());
            ptr += s.size();
        }

        *ptr = 0;
        return fromSV(std::string_view(buffer, length));
    }

    const char* data() const = delete;

    const CString leafName() const
    {
        size_t pos = this->rfind('/');
        if ( pos == npos )
            return *this;
        return substr(pos+1);
    }

private:
    // Dummy type to support a private std::string_view -> CString constructor.
    struct UnsafeSVCastTag {};

    constexpr CString(std::string_view str, UnsafeSVCastTag): std::string_view(str)
    {
        assert(str.data() == nullptr || *(str.data() + str.size()) == 0);
    }
};

namespace std
{

template<>
struct hash<CString>
{
    uint64_t operator()(CString str) const { return std::hash<std::string_view>{}(str); }
};
}

#endif // common_CString_h
