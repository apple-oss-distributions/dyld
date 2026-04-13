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

// OS
#include <mach-o/loader.h>

// mach_o
#include "DataInCode.h"

namespace mach_o {

DataInCode::DataInCode(std::span<const uint8_t> bytes)
    : _bytes(bytes)
{
}

Error DataInCode::valid(uint32_t textEndOffset) const
{
    // empty region marks there is no data-in-code
    if ( _bytes.empty() )
        return Error::none();

    if ( (_bytes.size() % sizeof(data_in_code_entry)) != 0 )
        return Error("invalid size for LC_DATA_IN_CODE payload");
    const uint32_t            count   = (uint32_t)(_bytes.size() / sizeof(data_in_code_entry));
    const data_in_code_entry* entries = (data_in_code_entry*)_bytes.data();
    for (uint32_t i=0; i < count; ++i) {
        if ( entries[i].offset > textEndOffset )
            return Error("Data-in-Code entry %d has offset 0x%08X that is out of text range", i, entries[i].offset);
        if ( entries[i].offset+entries[i].length > textEndOffset )
            return Error("Data-in-Code entry %d has offset+length 0x%08X that is out of text range", i, entries[i].offset);
        if ( (entries[i].kind > 5) || (entries[i].kind == 0) )
            return Error("Data-in-Code entry %d has unknown kind=%d", i, entries[i].kind);
    }
    return Error::none();
}

void DataInCode::forEachEntry(void (^callback)(const Entry&)) const
{
    const uint32_t            count   = (uint32_t)(_bytes.size() / sizeof(data_in_code_entry));
    const data_in_code_entry* entries = (data_in_code_entry*)_bytes.data();
    for (uint32_t i=0; i < count; ++i) {
        callback({ (Entry::Kind)entries[i].kind, entries[i].offset, entries[i].length});
    }
}

} // namepace mach_o

