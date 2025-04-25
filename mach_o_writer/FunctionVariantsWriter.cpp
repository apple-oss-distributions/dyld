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

#include "Defines.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>


#include "FunctionVariantsWriter.h"

namespace mach_o {


//
// MARK: --- FunctionVariantsRuntimeTableWriter methods ---
//

FunctionVariantsRuntimeTableWriter* FunctionVariantsRuntimeTableWriter::make(Kind kind, size_t variantsCount)
{
    size_t size = offsetof(FunctionVariantsRuntimeTable, entries[variantsCount]);
    FunctionVariantsRuntimeTableWriter* p = (FunctionVariantsRuntimeTableWriter*)::calloc(size, 1);
    p->kind  = kind;
    p->count = (uint32_t)variantsCount;
    return p;
}

Error FunctionVariantsRuntimeTableWriter::setEntry(size_t index, uint32_t impl, bool implIsTableIndex, std::span<const uint8_t> flagIndexes)
{
    if ( index >= this->count )
        return Error("index=%lu too large (max=%d)", index, this->count);
    if ( flagIndexes.size() > 4 )
        return Error("flagIndexes too large %lu (max 4)", flagIndexes.size());
    this->entries[index] = { impl, implIsTableIndex };
    memcpy(this->entries[index].flagBitNums, flagIndexes.data(), flagIndexes.size());
    return Error::none();
}


//
// MARK: --- FunctionVariantsWriter methods ---
//


FunctionVariantsWriter::FunctionVariantsWriter(std::span<const FunctionVariantsRuntimeTable*> entries)
{
    // compute size of linkedit blob to hold all FunctionVariantsRuntimeTable
    const size_t firstOffset = sizeof(OnDiskFormat) + entries.size()*sizeof(uint32_t);
    size_t       size        = firstOffset;
    for ( const FunctionVariantsRuntimeTable* fvrt : entries )
        size += fvrt->size();
    size = (size+7) & (-8);  // LINKEDIT content must be pointer size aligned

    // allocate byte vector to hold whole blob
    _builtBytes.resize(size);
    _bytes = _builtBytes;

    // fill in blob header and all entries
    OnDiskFormat* p             = header();
    uint32_t      currentOffset = (uint32_t)firstOffset;
    for ( const FunctionVariantsRuntimeTable* fvrt : entries ) {
        p->tableOffsets[p->tableCount] = currentOffset;
        p->tableCount++;
        size_t entrySize = fvrt->size();
        assert(currentOffset+entrySize <= size);
        memcpy(&_builtBytes[currentOffset], fvrt, entrySize);
        currentOffset += entrySize;
    }
}


//
// MARK: --- FunctionVariantFixupsWriter methods ---
//

FunctionVariantFixupsWriter::FunctionVariantFixupsWriter(std::span<const InternalFixup> entries)
{
    _builtBytes.resize(entries.size() * sizeof(InternalFixup));
    memcpy(_builtBytes.data(), entries.data(), entries.size() * sizeof(InternalFixup));
    _fixups = std::span<const InternalFixup>((InternalFixup*)_builtBytes.data(), entries.size());
}


} // namespace mach_o
