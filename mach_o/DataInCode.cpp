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
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "DataInCode.h"

#if BUILDING_MACHO_WRITER
#include <map>
#endif

namespace mach_o {

DataInCode::DataInCode(const uint8_t* start, size_t size)
    : _dataInCodeStart(start), _dataInCodeEnd(start + size)
{
}

Error DataInCode::valid() const
{
    return Error::none();
}

#if BUILDING_MACHO_WRITER

DataInCode::DataInCode(std::span<const Entry> entries)
{
    assert(entries.empty());

    this->_bytes.reserve(1);

    // set up buffer
    this->_dataInCodeStart = &this->_bytes.front();
    this->_dataInCodeEnd   = &this->_bytes.back();
}

#endif // BUILDING_MACHO_WRITER

} // namepace mach_o
