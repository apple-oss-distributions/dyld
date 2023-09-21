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

#ifndef mach_o_Version64_h
#define mach_o_Version64_h

#include <stdint.h>
#include <compare>

// common
#include "Defines.h"
#include "CString.h"

// mach-o
#include "Error.h"

namespace mach_o {

/*!
 * @class Version64
 *
 * @abstract
 *      Type safe wrapper for version numbers packed into 64-bits
 *      example: A[.B[.B[.D[.E]]]] into a uint64_t where the bits are a24.b10.c10.d10.e10
 */
class VIS_HIDDEN Version64
{
public:
                    constexpr Version64(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e){
                        assert((a <= 0xFFFFFF) && (b <= 0x3FF) && (c <= 0x3FF)
                                && (d <= 0x3FF) && (e <= 0x3FF));
                        _raw = (a << 40) | ( b << 30 ) | ( c << 20 ) | ( d << 10 ) | e;
                    }
                    explicit Version64(uint64_t raw) : _raw(raw) { }
                    Version64() : _raw(0x0) { };

    static Error            fromString(CString versString, Version64& vers);
    const char* _Nonnull    toString(char* _Nonnull buffer) const;
    auto                    operator<=>(const Version64& other) const = default;
    uint64_t                value() const { return _raw; }
private:
    uint64_t                _raw;
};

inline bool operator<(const Version64& l, const Version64& r) { return (l.value() < r.value()); }


} // namespace mach_o


#endif /* mach_o_Version64_h */
