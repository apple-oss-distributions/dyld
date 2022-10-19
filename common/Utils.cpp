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


#include <string.h>

#include "Utils.h"

namespace dyld4 {
// based on ANSI-C strstr()
const char* Utils::strrstr(const char* str, const char* sub)
{
    const size_t sublen = strlen(sub);
    for (const char* p = &str[strlen(str)]; p != str; --p) {
        if ( ::strncmp(p, sub, sublen) == 0 )
            return p;
    }
    return nullptr;
}

size_t Utils::concatenatePaths(char *path, const char *suffix, size_t pathsize)
{
    if ( (path[strlen(path) - 1] == '/') && (suffix[0] == '/') )
        return strlcat(path, &suffix[1], pathsize); // avoid double slash when combining path
    else
        return strlcat(path, suffix, pathsize);
}
}; /* namespace dyld4 */
