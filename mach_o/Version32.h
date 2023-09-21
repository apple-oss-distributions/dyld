/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_Version32_h
#define mach_o_Version32_h

#include <stdint.h>
#include <compare>

// common
#include "Defines.h"
#include "CString.h"

// mach-o
#include "Error.h"

namespace mach_o {

/*!
 * @class Version32
 *
 * @abstract
 *      Type safe wrapper for version numbers packed into 32-bits
 *      example:  X.Y[.Z] as xxxxyyzz
 */
class VIS_HIDDEN Version32
{
public:
                  constexpr Version32(uint16_t major, uint8_t minor, uint8_t micro=0) : _raw((major << 16) | (minor << 8) | micro) { }
                   explicit Version32(uint32_t raw) : _raw(raw) { }
                            Version32() : _raw(0x00010000) { }

    static Error            fromString(std::string_view versString, Version32& vers,
                                       void (^ _Nullable truncationHandler)(void) = nullptr);
    const char* _Nonnull    toString(char* _Nonnull buffer) const;
    auto                    operator<=>(const Version32& other) const = default;
    uint32_t                value() const { return _raw; }
    uint32_t                major() const { return (_raw >> 16) & 0xFFFF; }
    uint32_t                minor() const { return (_raw >> 8) & 0xFF; }
private:
    uint32_t                _raw;
};

inline bool operator<(const Version32& l, const Version32& r) { return (l.value() < r.value()); }


} // namespace mach_o

#endif /* mach_o_Version32_h */
