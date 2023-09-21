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


#ifndef mach_o_FunctionStarts_h
#define mach_o_FunctionStarts_h

#include <span>
#include <stdint.h>

#if BUILDING_MACHO_WRITER
  #include <vector>
#endif

#include "Defines.h"
#include "Error.h"

namespace mach_o {

/*!
 * @class FunctionStarts
 *
 * @abstract
 *      Abstraction for a list of function address in TEXT
 */
class VIS_HIDDEN FunctionStarts
{
public:
                        // construct from a mach-o linkedit blob
                        FunctionStarts(const uint8_t* start, size_t size);

    Error               valid(uint64_t maxFuncOffset) const;
    void                forEachFunctionStart(uint64_t loadAddr, void (^callback)(uint64_t funcAddr)) const;

#if BUILDING_MACHO_WRITER
                        // used build a function starts blob
                        FunctionStarts(uint64_t prefLoadAddr, std::span<const uint64_t> functionAddresses);

    std::span<const uint8_t>  bytes() const { return _bytes; }
#endif

private:
#if BUILDING_MACHO_WRITER
    void                  append_uleb128(uint64_t value);
#endif

    const uint8_t*       _funcStartsBegin;
    const uint8_t*       _funcStartsEnd;
#if BUILDING_MACHO_WRITER
    std::vector<uint8_t> _bytes;
#endif
};


} // namespace mach_o

#endif // mach_o_FunctionStarts_h
