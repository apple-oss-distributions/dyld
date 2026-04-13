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
#include <mach-o/loader.h>

// mach_o_writer
#include "DataInCodeWriter.h"

// stl
#include <span>

namespace mach_o {

DataInCodeWriter::DataInCodeWriter(std::span<const Entry> entries)
 : DataInCode(std::span<const uint8_t>())
{
    _dices.reserve(entries.size());
    for (const Entry& entry : entries) {
        if ( entry.length > 0xFFFF ) {
            _buildError = Error("length=%d too large for data-in-code", entry.length);
            break;
        }
        if ( (int)entry.kind > 5 ) {
            _buildError = Error("kind=%d not a known value", (int)entry.kind);
            break;
        }
        data_in_code_entry ee;
        ee.offset = entry.startOffset;
        ee.length = entry.length;
        ee.kind   = entry.kind;
        _dices.push_back(ee);
    }

    // set up _bytes to point into _dices
    if ( !_dices.empty() )
        this->_bytes = std::span<const uint8_t>{(uint8_t*)_dices.data(), _dices.size()*sizeof(data_in_code_entry)};
}

Error DataInCodeWriter::valid(uint32_t textEndOffset)
{
    if (_buildError.hasError())
        return std::move(_buildError);
    return DataInCode::valid(textEndOffset);
}


} // namepace mach_o
