/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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

#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <charconv>

#include "Version32.h"

namespace mach_o {


Error Version32::fromString(std::string_view versString, Version32& vers,
                            void (^ _Nullable truncationHandler)(void))
{
    // Initialize default value
    vers = Version32();

    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    const char* endPtr = versString.data() + versString.size();
    auto res = std::from_chars(versString.data(), endPtr, x);
    if ( res.ec == std::errc{} && *res.ptr == '.' ) {
        res = std::from_chars(res.ptr + 1, endPtr, y);
        if ( res.ec == std::errc{} && *res.ptr == '.' )
            res = std::from_chars(res.ptr + 1, endPtr, z);
    }
    bool valueOverflow = (x > 0xffff) || (y > 0xff) || (z > 0xff);
    if ( valueOverflow && truncationHandler ) {
        truncationHandler();
        x = std::min(x, 0xFFFFU);
        y = std::min(y, 0xFFU);
        z = std::min(z, 0xFFU);
        valueOverflow = false;
    }
    if ( res.ptr == nullptr || res.ptr != endPtr || valueOverflow ) {
        if ( valueOverflow || (truncationHandler == nullptr) || (res.ptr == versString.data()) ) {
            char errVersString[versString.size() + 1];
            memcpy(errVersString, versString.data(), versString.size());
            errVersString[versString.size()] = 0;
            return Error("malformed version number '%s' cannot fit in 32-bit xxxx.yy.zz", (const char*)errVersString);
        }
    }

    vers = Version32(x, y, z);
    return Error::none();
}


static void appendDigit(char*& s, unsigned& num, unsigned place, bool& startedPrinting)
{
    if ( num >= place ) {
        unsigned dig = (num/place);
        *s++ = '0' + dig;
        num -= (dig*place);
        startedPrinting = true;
    }
    else if ( startedPrinting ) {
        *s++ = '0';
    }
}

static void appendNumber(char*& s, unsigned num)
{
    assert(num < 99999);
    bool startedPrinting = false;
    appendDigit(s, num, 10000, startedPrinting);
    appendDigit(s, num,  1000, startedPrinting);
    appendDigit(s, num,   100, startedPrinting);
    appendDigit(s, num,    10, startedPrinting);
    appendDigit(s, num,     1, startedPrinting);
    if ( !startedPrinting )
        *s++ = '0';
}


const char* Version32::toString(char buffer[32]) const
{
    // sprintf(versionString, "%d.%d.%d", (_raw >> 16), ((_raw >> 8) & 0xFF), (_raw & 0xFF));
    char* s = buffer;
    appendNumber(s, (_raw >> 16));
    *s++ = '.';
    appendNumber(s, (_raw >> 8) & 0xFF);
    unsigned micro = (_raw & 0xFF);
    if ( micro != 0 ) {
        *s++ = '.';
        appendNumber(s, micro);
    }
    *s++ = '\0';
    return buffer;
}



} // namespace mach_o





