/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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

#include "ObjC.h"

namespace mach_o {

//
// MARK: --- ObjCMethodList methods ---
//

uint32_t ObjCMethodList::getMethodSize() const
{
    const uint32_t* header = (const uint32_t*)this;
    return header[0] & methodListSizeMask;
}

uint32_t ObjCMethodList::getMethodCount() const
{
    const uint32_t* header = (const uint32_t*)this;
    return header[1];
}

bool ObjCMethodList::usesRelativeOffsets() const
{
    const uint32_t* header = (const uint32_t*)this;
    return (header[0] & methodListIsRelative) != 0;
}

//
// MARK: --- ObjCProtocolList methods ---
//

uint32_t ObjCProtocolList::count(bool is64) const
{
    if ( is64 ) {
        const uint64_t* header = (const uint64_t*)this;
        return (uint32_t)header[0];
    }
    else {
        const uint32_t* header = (const uint32_t*)this;
        return header[0];
    }
}

//
// MARK: --- ObjCPropertyList methods ---
//

uint32_t ObjCPropertyList::getPropertySize() const
{
    const uint32_t* header = (const uint32_t*)this;
    return header[0];
}

uint32_t ObjCPropertyList::getPropertyCount() const
{
    const uint32_t* header = (const uint32_t*)this;
    return header[1];
}


} // namespace mach_o
