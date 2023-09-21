/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_Misc_h
#define mach_o_Misc_h

#include <stdint.h>

#include <span>

#include "Defines.h"
#include "Header.h"
#include "Error.h"

namespace mach_o {

// replacements for posix that handle EINTR and EAGAIN
int	resilient_stat(const char* path, struct stat* buf) VIS_HIDDEN;
int	resilient_open(const char* path, int flag, int other) VIS_HIDDEN;


/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
inline bool greaterThanAddOrOverflow(uint32_t addLHS, uint32_t addRHS, T b) {
    uint32_t sum;
    if (__builtin_add_overflow(addLHS, addRHS, &sum) )
        return true;
    return (sum > b);
}

/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
inline bool greaterThanAddOrOverflow(uint64_t addLHS, uint64_t addRHS, T b) {
    uint64_t sum;
    if (__builtin_add_overflow(addLHS, addRHS, &sum) )
        return true;
    return (sum > b);
}


uint64_t    read_uleb128(const uint8_t*& p, const uint8_t* end, bool& malformed);
int64_t     read_sleb128(const uint8_t*& p, const uint8_t* end, bool& malformed);
uint32_t	uleb128_size(uint64_t value);

inline void pageAlign4K(uint64_t& value)
{
    value = ((value + 0xFFF) & (-0x1000));
}

inline void pageAlign16K(uint64_t& value)
{
    value = ((value + 0x3FFF) & (-0x4000));
}

bool withReadOnlyMappedFile(const char* path, void (^handler)(std::span<const uint8_t>));

// used by command line tools to process files that may be on disk or in dyld cache
void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter,
                             void (^callback)(const char* slicePath, const Header* sliceHeader, size_t sliceLength));



} // namespace mach_o

#endif /* mach_o_Misc_h */
