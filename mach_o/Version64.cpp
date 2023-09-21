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


const char* Version64::toString(char buffer[64]) const
{
    assert("unimplemented");
    return NULL;
}

} // namespace mach_o
