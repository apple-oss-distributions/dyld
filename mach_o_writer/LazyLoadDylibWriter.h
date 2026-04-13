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


#ifndef mach_o_writer_LazyLoadDylib_h
#define mach_o_writer_LazyLoadDylib_h

#include <span>
#include <stdint.h>

#include <vector>

// mach_o
#include "Error.h"
#include "LazyLoadDylib.h"

namespace mach_o {

/*!
 * @class LazyLoadDylibWriter
 *
 * @abstract
 *      Abstraction for building a LazyLoadDylib LinkEdit structure
 */
class VIS_HIDDEN LazyLoadDylibWriter : public LazyLoadDylib
{
public:
                                // used build a lazy load dylib info blob
                                LazyLoadDylibWriter(CString loadPath, std::span<const CString> symbols, bool weakLink=false);
    void                        setPointerFormat(uint16_t pf);
    void                        setImageOffsets(uint32_t flagsOffset, uint32_t chainStartOffset);

    // resulting bytes
    std::span<const uint8_t>    bytes() const { return _bytes; }

private:
    std::vector<uint8_t> _buffer;
};


} // namespace mach_o

#endif // mach_o_writer_LazyLoadDylib_h
