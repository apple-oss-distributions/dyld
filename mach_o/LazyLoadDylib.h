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


#ifndef mach_o_LazyLoadDylib_h
#define mach_o_LazyLoadDylib_h

#include <span>
#include <stdint.h>
#include <mach-o/loader.h>

#include "MachODefines.h"
#include "CString.h"

#include "Error.h"

namespace mach_o {

/*!
 * @class LazyLoadDylib
 *
 * @abstract
 *      Parser for LINKEDIT blob that describes how to lazy load a dylib
 */
class VIS_HIDDEN LazyLoadDylib
{
public:
                        // construct from a mach-o linkedit blob
                        LazyLoadDylib(std::span<const uint8_t> linkeditBytes);

    Error               valid(uint64_t maxFuncOffset) const;
    CString             dylibLoadPath() const;
    bool                dylibMayBeMissing() const; // "weak linked"
    uint32_t*           imageLoadedFlag(const mach_header*) const;
    uint16_t            pointerFormat() const;
    uint32_t            symbolCount() const;
    CString             symbol(uint32_t index) const;
    void*               chainStart(const mach_header*) const;

protected:
    std::span<const uint8_t> _bytes;

    struct LazyLoadDylibLinkEdit
    {
        uint32_t    loadPathOffset;          // path of dylib to load
        uint32_t    flagImageOffset;         // image offset to flags global
        uint16_t    flags;                   // weak linked or not
        uint16_t    pointerFormat;           // e.g. DYLD_CHAINED_PTR_ARM64E_USERLAND
        uint32_t    chainStartImageOffset;   // image offset to fixup chain start
        uint32_t    symbolsCount;            // how many symbol names to bind
        uint32_t    symbolStringArrayOffset; // offset of each symbol name within blob
        // room for future fields here
        // path string
        // symbols strings
    };

    const LazyLoadDylibLinkEdit* info() const { return (LazyLoadDylibLinkEdit*)_bytes.data(); }
};


} // namespace mach_o

#endif // mach_o_LazyLoadDylib_h
