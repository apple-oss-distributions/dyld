/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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

#ifndef PerfectHashWriter_h
#define PerfectHashWriter_h

#include "Defines.h"
#include "PerfectHash.h"

#include <span>
#include <string_view>
#include <unordered_map>

namespace objc {

// An objc string is at a certain offset in to its buffer. Eg, a selector is a given offset
// in to the selector strings buffer
typedef std::pair<std::string_view, uint32_t> ObjCString;

struct eqstr {
    bool operator()(const char* s1, const char* s2) const {
        return strcmp(s1, s2) == 0;
    }
};

struct hashstr {
    size_t operator()(const char *s) const {
        return (size_t)objc::lookup8((uint8_t *)s, strlen(s), 0);
    }
};

// cstring => cstring's vmaddress
// (used for selector names and class names)
typedef std::unordered_map<const char *, uint64_t, hashstr, eqstr> string_map;

// cstring => cstring's vmaddress
// (used for selector names and class names)
typedef std::unordered_map<const char *, uint64_t, hashstr, eqstr> string_map;

// protocol name => protocol vmaddress
typedef std::unordered_map<const char *, uint64_t, hashstr, eqstr> legacy_protocol_map;

// protocol name => (protocol vmaddress, dylib objc index)
typedef std::unordered_multimap<const char *, std::pair<uint64_t, uint16_t>, hashstr, eqstr> protocol_map;

// class name => (class vmaddress, dylib objc index)
typedef std::unordered_multimap<const char *, std::pair<uint64_t, uint16_t>, hashstr, eqstr> class_map;

struct VIS_HIDDEN PerfectHash {
    uint32_t capacity   = 0;
    uint32_t occupied   = 0;
    uint32_t shift      = 0;
    uint32_t mask       = 0;
    uint64_t salt       = 0;

    uint32_t scramble[256];
    std::vector<uint8_t> tab; // count == mask+1

    struct SwiftStrings
    {
        std::span<const uint8_t> key1;
        std::span<const uint8_t> key2;
    };

    // For the shared cache builder swift maps
    static void make_perfect(std::span<const SwiftStrings> strings, PerfectHash& result);

    // For the shared cache builder selector/class/protocol maps
    static void make_perfect(std::span<const ObjCString> strings, objc::PerfectHash& phash);
};

} // namespace objc

#endif /* PerfectHashWriter_h */
