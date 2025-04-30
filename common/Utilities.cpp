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
#include <assert.h>
#if __has_feature(ptrauth_calls)
    #include <ptrauth.h>
#endif

#include "Utilities.h"

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

void escapeCStringLiteral(const char* s, char* b, size_t bufferLength, char** end)
{
    char* e = b + bufferLength - 1; // reserve one character for null terminator
    while (b < e) {
        char c = *s++;
        if ( c == '\n' ) {
            *b++ = '\\';
            *b++ = 'n';
        }
        else if ( c == '\r' ) {
            *b++ = '\\';
            *b++ = 'r';
        }
        else if ( c == '\t' ) {
            *b++ = '\\';
            *b++ = 't';
        }
        else if ( c == '\"' ) {
            *b++ = '\\';
            *b++ = '\"';
        }
        else if ( c == '\0' ) {
            break;
        }
        else {
            *b++ = c;
        }
    }
    *b = '\0';
    if ( end )
        *end = b;
}


#if __has_feature(ptrauth_calls)
uint64_t signPointer(uint64_t unsignedAddr, void* loc, bool addrDiv, uint16_t diversity, ptrauth_key key)
{
    // don't sign NULL
    if ( unsignedAddr == 0 )
        return 0;

    uint64_t extendedDiscriminator = diversity;
    if ( addrDiv )
        extendedDiscriminator = __builtin_ptrauth_blend_discriminator(loc, extendedDiscriminator);
    switch ( key ) {
        case ptrauth_key_asia:
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 0, extendedDiscriminator);
        case ptrauth_key_asib:
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 1, extendedDiscriminator);
        case ptrauth_key_asda:
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 2, extendedDiscriminator);
        case ptrauth_key_asdb:
            return (uint64_t)__builtin_ptrauth_sign_unauthenticated((void*)unsignedAddr, 3, extendedDiscriminator);
        default:
            assert(0 && "invalid signing key");
    }
}
#endif

