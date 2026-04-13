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

#include <limits.h>
#include <stdlib.h>

#include "LazyLoadDylib.h"


namespace mach_o {

LazyLoadDylib::LazyLoadDylib(std::span<const uint8_t> linkeditBytes)
   : _bytes(linkeditBytes)
{
}

Error LazyLoadDylib::valid(uint64_t maxVmOffset) const
{
    if ( _bytes.size() < sizeof(LazyLoadDylibLinkEdit) )
        return Error("LazyLoadDylib info size (%lu) too small", _bytes.size());

    // sanity check loadPathOffset
    if ( _bytes.size() < info()->loadPathOffset )
        return Error("LazyLoadDylib.loadPathOffset (%d) out of range (max %lu)", info()->loadPathOffset, _bytes.size());
    const uint8_t* p = (_bytes.data()+(info()->loadPathOffset));
    bool foundPathEnd = false;
    while (p < &_bytes.back()) {
        if ( *p == '\0' ) {
            foundPathEnd = true;
            break;
        }
        ++p;
    }
    if ( !foundPathEnd )
        return Error("LazyLoadDylib.loadPathOffset string not zero terminated");

    // sanity check flagImageOffset
    if ( info()->flagImageOffset > maxVmOffset )
        return Error("LazyLoadDylib.flagImageOffset (0x%08X) beyond max vmOffset (0x%08llX)", info()->flagImageOffset, maxVmOffset);

    // sanity check chainStartImageOffset
    if ( info()->chainStartImageOffset > maxVmOffset )
        return Error("LazyLoadDylib.chainStartImageOffset (0x%08X) beyond max vmOffset (0x%08llX)", info()->flagImageOffset, maxVmOffset);

    // sanity check symbolsCount
    if ( info()->symbolStringArrayOffset + sizeof(uint32_t)*info()->symbolsCount > _bytes.size() )
        return Error("LazyLoadDylib.symbolsCount (%u) too large to fit in size (%lu)", info()->symbolsCount, _bytes.size());

    // sanity check each symbolStringOffsets
    const uint32_t* symbolsOffsetsArray = (uint32_t*)&_bytes[info()->symbolStringArrayOffset];
    for (uint32_t i=0; i < info()->symbolsCount; ++i) {
        uint32_t offset = symbolsOffsetsArray[i];
        if ( offset > _bytes.size() )
            return Error("LazyLoadDylib.symbolStringOffsets[%i] (%u) too large to fit in size (%lu)", i, offset, _bytes.size());
    }

    return Error::none();
}

CString LazyLoadDylib::dylibLoadPath() const
{
    return (char*)_bytes.data()+info()->loadPathOffset;
}

uint32_t* LazyLoadDylib::imageLoadedFlag(const mach_header* mh) const
{
    return (uint32_t*)((uint8_t*)mh + info()->flagImageOffset);
}

uint32_t LazyLoadDylib::symbolCount() const
{
   return info()->symbolsCount;
}

uint16_t LazyLoadDylib::pointerFormat() const
{
    return info()->pointerFormat;
}

CString LazyLoadDylib::symbol(uint32_t index) const
{
    const uint32_t* symbolsOffsetsArray = (uint32_t*)&_bytes[info()->symbolStringArrayOffset];
    return (const char*)(_bytes.data()+symbolsOffsetsArray[index]);
}

void* LazyLoadDylib::chainStart(const mach_header* mh) const
{
    return (void*)((uint8_t*)mh+info()->chainStartImageOffset);
}

bool LazyLoadDylib::dylibMayBeMissing() const
{
    return (info()->flags & 0x0001);
}


} // namespace mach_o
