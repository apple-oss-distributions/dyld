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


#ifndef mach_o_DataInCode_h
#define mach_o_DataInCode_h

#include <span>
#include <stdint.h>

#include "MachODefines.h"
#include "Error.h"

namespace mach_o {

/*!
 * @class DataInCode
 *
 * @abstract
 *      Class to encapsulate accessing and building data in code
 */
class VIS_HIDDEN DataInCode
{
public:
                        // construct from a chunk of LINKEDIT
                        DataInCode(const uint8_t* start, size_t size);

    struct Entry
    {
        // TODO: Implement this
    };

    Error   valid() const;

    static uint32_t     dataInCodeSize(bool is64);

protected:

    const uint8_t*       _dataInCodeStart;
    const uint8_t*       _dataInCodeEnd;
};


} // namespace mach_o

#endif // mach_o_CompactUnwind_h
