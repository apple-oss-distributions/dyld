/*
 * Copyright (c) 2025 Apple Inc. All rights reserved.
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

#ifndef __DYLD_TOOL_UTILS__
#define __DYLD_TOOL_UTILS__

#if !BUILDING_DYLD

// stl
#include <vector>
#include <string>
#include <charconv>

// common
#include "CString.h"

// macho
#include "Error.h"

inline void parseList(CString str, char separator, std::vector<std::string_view>& tokens)
{
    size_t startPos = 0;
    auto colonPos = str.find(separator);
    while ( colonPos != std::string::npos )
    {
        size_t tokenSize = colonPos - startPos;
        if ( tokenSize != 0 ) // ignore empty entries
            tokens.push_back(str.substr(startPos, tokenSize));
        startPos = colonPos + 1;
        colonPos = str.find(separator, startPos);
    }
    std::string_view last = str.substr(startPos);
    if ( !last.empty() )
        tokens.push_back(last);
}

inline mach_o::Error parseHex(CString str, uint64_t& addr)
{
    char* endptr;
    addr = strtoull(str.c_str(), &endptr, 16);
    if ( endptr != str.end() ) {
        return mach_o::Error("not a hexadecimal number: %s", str.c_str());
    }
    return mach_o::Error::none();
}

inline mach_o::Error parseHex(std::string_view str, uint64_t& addr)
{
    if ( str.starts_with("0x") || str.starts_with("0X") )
        str = str.substr(2);
    const char* end = str.data()+str.size();
    auto res = std::from_chars(str.data(), end, addr, 16);
    if ( res.ec == std::errc() && res.ptr == end )
        return mach_o::Error::none();

    return mach_o::Error("not a hexadecimal number: %.*s", (int)str.size(), str.data());
}


inline mach_o::Error parseHexAddrList(CString vaString, char separator, CString optionName, std::vector<uint64_t>& vec)
{
    std::vector<std::string_view> tokens;
    parseList(vaString, separator, tokens);

    if ( tokens.empty() )
        return mach_o::Error("%s <va_hex>", optionName.c_str());

    for ( std::string_view token : tokens ) {
        mach_o::Error err = parseHex(token, vec.emplace_back());
        if ( err.hasError() )
            return mach_o::Error("%s %s", optionName.c_str(), err.message());
    }
    return mach_o::Error();
}

#endif
#endif
