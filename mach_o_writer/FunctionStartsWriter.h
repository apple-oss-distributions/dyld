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


#ifndef mach_o_writer_FunctionStarts_h
#define mach_o_writer_FunctionStarts_h

#include <span>
#include <stdint.h>

#include <vector>

// mach_o
#include "Error.h"
#include "FunctionStarts.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class FunctionStartsWriter
 *
 * @abstract
 *      Abstraction for building a list of function address in TEXT
 */
class VIS_HIDDEN FunctionStartsWriter : public FunctionStarts
{
public:
    // used build a function starts blob
    FunctionStartsWriter(uint64_t prefLoadAddr, std::span<const uint64_t> functionAddresses);

    std::span<const uint8_t>  bytes() const { return _bytes; }

private:
    void                  append_uleb128(uint64_t value);

    std::vector<uint8_t> _bytes;
};


} // namespace mach_o

#endif // mach_o_writer_FunctionStarts_h
