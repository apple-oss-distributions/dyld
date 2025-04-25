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


#ifndef mach_o_writer_SplitSeg_h
#define mach_o_writer_SplitSeg_h

// mach_o
#include "SplitSeg.h"
#include "Error.h"

#include <span>
#include <vector>
#include <unordered_map>

namespace mach_o {

using namespace mach_o;

/*!
 * @class SplitSegInfo
 *
 * @abstract
 *      Class to encapsulate building split seg info
 */
class VIS_HIDDEN SplitSegInfoWriter : public SplitSegInfo
{
public:

                        // used build split seg info
                        // Note: entries so not need to be sorted
                        SplitSegInfoWriter(std::span<const Entry> entries);

    static size_t       estimateSplitSegInfoSize(std::span<const Entry> entries);

    std::span<const uint8_t>  bytes() const { return _bytes; }

private:
    std::vector<uint8_t> _bytes;
    Error                _buildError;
    static const bool    _verbose = false;
};


} // namespace mach_o

#endif // mach_o_writer_CompactUnwind_h
