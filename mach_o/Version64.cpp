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

#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "Version64.h"

namespace mach_o {


Error Version64::fromString(CString versString, Version64& vers)
{
    // Initialize default value
    vers = Version64();
    
    uint64_t a = 0;
    uint64_t b = 0;
    uint64_t c = 0;
    uint64_t d = 0;
    uint64_t e = 0;
    char* end;
    a = strtoul(versString.c_str(), &end, 10);
    if ( *end == '.' ) {
        b = strtoul(&end[1], &end, 10);
        if ( *end == '.' ) {
            c = strtoul(&end[1], &end, 10);
            if ( *end == '.' ) {
                d = strtoul(&end[1], &end, 10);
                if ( *end == '.' ) {
                    e = strtoul(&end[1], &end, 10);
                }
            }
        }
    }
    if ( (*end != '\0') || (a > 0xFFFFFF) || (b > 0x3FF) || (c > 0x3FF) || (d > 0x3FF)  || (e > 0x3FF) )
        return Error("malformed 64-bit a.b.c.d.e version number: %s", versString.c_str());
    
    vers = Version64(a, b, c, d, e);
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
    assert(num < 9999999);
    bool startedPrinting = false;
    appendDigit(s, num, 10000000, startedPrinting);
    appendDigit(s, num,  1000000, startedPrinting);
    appendDigit(s, num,   100000, startedPrinting);
    appendDigit(s, num,    10000, startedPrinting);
    appendDigit(s, num,     1000, startedPrinting);
    appendDigit(s, num,      100, startedPrinting);
    appendDigit(s, num,       10, startedPrinting);
    appendDigit(s, num,        1, startedPrinting);
    if ( !startedPrinting )
        *s++ = '0';
}

/* A.B.C.D.E packed as a24.b10.c10.d10.e10 */
const char* Version64::toString(char buffer[64]) const
{
    char* s = buffer;
    appendNumber(s, (_raw >> 40));
    *s++ = '.';
    appendNumber(s, (_raw >> 30) & 0x3FF);
    unsigned c = (_raw >> 20) & 0x3FF;
    if ( c != 0 ) {
        *s++ = '.';
        appendNumber(s, c);
    }
    unsigned d = (_raw >> 10) & 0x3FF;
    if ( d != 0 ) {
        *s++ = '.';
        appendNumber(s, d);
    }
    unsigned e = _raw & 0x3FF;
    if ( e != 0 ) {
        *s++ = '.';
        appendNumber(s, e);
    }
    *s++ = '\0';
    return buffer;
}

} // namespace mach_o
