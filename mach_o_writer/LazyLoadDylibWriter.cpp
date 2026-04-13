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

#include "Defines.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>


#include "LazyLoadDylibWriter.h"

namespace mach_o {


//
//
// MARK: --- LazyLoadDylibWriter methods ---
//

LazyLoadDylibWriter::LazyLoadDylibWriter(CString loadPath, std::span<const CString> symbols, bool weakLink)
   : LazyLoadDylib({})
{
    // sanity check lazy loading is not being abused
    assert(symbols.size() < 1000);

    // compute size of linkedit blob to hold LazyLoadDylib
    size_t size = sizeof(LazyLoadDylibLinkEdit) + symbols.size()*sizeof(uint32_t) + loadPath.size()+1;
    for (CString s : symbols)
        size += s.size()+1;
    size = (size+7) & (-8);  // LINKEDIT content must be pointer size aligned

    // allocate byte vector to hold whole blob
    _buffer.resize(size);
    _bytes = _buffer;

    // fill in blob header and all entries
    LazyLoadDylibLinkEdit* p             = (LazyLoadDylibLinkEdit*)_buffer.data();
    uint32_t*              symbolsArray  = (uint32_t*)&_buffer[sizeof(LazyLoadDylibLinkEdit) + p->symbolStringArrayOffset];
    uint32_t               curStrOffset  = (uint32_t)(sizeof(LazyLoadDylibLinkEdit) + symbols.size()*sizeof(uint32_t));
    p->loadPathOffset = curStrOffset;
    memcpy(&_buffer[curStrOffset], loadPath.c_str(), loadPath.size()+1);
    curStrOffset += loadPath.size()+1;
    p->flagImageOffset        = 0;
    p->flags                  = weakLink;
    p->pointerFormat          = 0;
    p->chainStartImageOffset  = 0;
    p->symbolsCount           = (uint32_t)symbols.size();
    p->symbolStringArrayOffset= sizeof(LazyLoadDylibLinkEdit);
    for (uint32_t i=0; i < symbols.size(); ++i) {
        assert(curStrOffset < _buffer.size());
        symbolsArray[i] = curStrOffset;
        CString str = symbols[i];
        memcpy(&_buffer[curStrOffset], str.c_str(), str.size()+1);
        curStrOffset += str.size()+1;
    }
}

void LazyLoadDylibWriter::setPointerFormat(uint16_t pf)
{
    LazyLoadDylibLinkEdit* p = (LazyLoadDylibLinkEdit*)_buffer.data();
    p->pointerFormat = pf;
}

void LazyLoadDylibWriter::setImageOffsets(uint32_t flagsOffset, uint32_t chainStartOffset)
{
    LazyLoadDylibLinkEdit* p = (LazyLoadDylibLinkEdit*)_buffer.data();
    p->flagImageOffset        = flagsOffset;
    p->chainStartImageOffset  = chainStartOffset;
}


} // namespace mach_o
