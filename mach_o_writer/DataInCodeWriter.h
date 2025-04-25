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


#ifndef mach_o_writer_DataInCode_h
#define mach_o_writer_DataInCode_h

#include <span>
#include <stdint.h>
#include <vector>
#include <unordered_map>

// mach_o
#include "DataInCode.h"
#include "Error.h"

namespace mach_o {

using namespace mach_o;

/*!
 * @class DataInCode
 *
 * @abstract
 *      Class to encapsulate building data in code
 */
class VIS_HIDDEN DataInCodeWriter : public DataInCode
{
public:
                        // used build data in code
                        DataInCodeWriter(std::span<const Entry> entries);
    static size_t       estimateDataInCodeSize(std::span<const Entry> entries);

    std::span<const uint8_t>  bytes() const { return _bytes; }

private:
    std::vector<uint8_t> _bytes;
    Error                _buildError;
    static const bool    _verbose = false;
};


} // namespace mach_o

#endif // mach_o_writer_CompactUnwind_h
