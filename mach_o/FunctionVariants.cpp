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


#include "FunctionVariants.h"

namespace mach_o {


//
// MARK: --- FunctionVariantsRuntimeTable methods ---
//

uint32_t FunctionVariantsRuntimeTable::forEachVariant(void (^callback)(FunctionVariantsRuntimeTable::Kind kind, uint32_t impl, bool implIsTableIndex, std::span<const uint8_t> flagIndexes, bool& stop)) const
{
    bool stop = false;
    for (uint32_t i=0; i < this->count; ++i) {
        int flagCount = 0;
        const uint8_t* flagBitNums = this->entries[i].flagBitNums;
        for (int f=0; f < 4; ++f) {
            if ( flagBitNums[f] != 0 )
                flagCount = f+1;
        }
        std::span<const uint8_t> flags(flagBitNums, flagCount);
        callback(this->kind, this->entries[i].impl, this->entries[i].anotherTable, flags, stop);
        if ( stop )
            break;
    }
    return (uint32_t)offsetof(FunctionVariantsRuntimeTable, entries[this->count]);
}

uint32_t FunctionVariantsRuntimeTable::size() const
{
    return (uint32_t)offsetof(FunctionVariantsRuntimeTable, entries[this->count]);
}

Error FunctionVariantsRuntimeTable::valid(size_t length) const
{
    // verify kind is known
    switch ( this->kind ) {
        case Kind::perProcess:
        case Kind::systemWide:
        case Kind::arm64:
        case Kind::x86_64:
            break;
        default:
            return Error("unknown FunctionVariantsRuntimeTable::Kind (%d)", this->kind);
    }
    // verify length
    size_t actualSize = offsetof(FunctionVariantsRuntimeTable, entries[this->count]);
    if ( (actualSize != length) && (actualSize != length-4) ) // last entry's size may be rounded up to align linkedit blob
        return Error("invalid FunctionVariantsRuntimeTable length %lu for count=%u", length, this->count);
    // verify "default" is last
    if ( this->entries[this->count - 1].flagBitNums[0] != 0 )
        return Error("last entry in FunctionVariantsRuntimeTable entries is not 'default'");

    return Error::none();
}

//
// MARK: --- FunctionVariants methods ---
//

FunctionVariants::FunctionVariants(std::span<const uint8_t> linkeditBytes)
  : _bytes(linkeditBytes)
{
}

Error FunctionVariants::valid() const
{
    const OnDiskFormat* p = header();
    if ( p == nullptr )
        return Error("FunctionVariants is too small");

    if ( offsetof(OnDiskFormat, tableOffsets[p->tableCount]) >= _bytes.size() )
        return Error("FunctionVariants tableCount=%u is too large for size=%lu", p->tableCount, _bytes.size());

    // verify layout each entry is in increasing order and fits in byte range
    for (uint32_t i=0; i < p->tableCount; ++i) {
        uint32_t offsetInBlob = p->tableOffsets[i];
        if ( offsetInBlob > _bytes.size() )
            return Error("tableOffsets[%d]=0x%08X which is > total size 0x%08lX", i, p->tableOffsets[i], _bytes.size());
        const FunctionVariantsRuntimeTable* fvrt = (FunctionVariantsRuntimeTable*)(&_bytes[offsetInBlob]);
        if ( offsetInBlob+fvrt->size() > _bytes.size() )
            return Error("entry %d extends to 0x%08X which beyond total size 0x%08lX", i, offsetInBlob+fvrt->size(), _bytes.size());
    }

    // verify each entry 
    for (uint32_t i=0; i < p->tableCount; ++i) {
        const FunctionVariantsRuntimeTable* entry = this->entry(i);
        uint32_t length;
        if ( i < (p->tableCount - 1) )
            length = p->tableOffsets[i+1] - p->tableOffsets[i];
        else
            length = (uint32_t)_bytes.size() - p->tableOffsets[i];
        if ( Error err = entry->valid(length) )
            return err;
    }

    return Error::none();
}

uint32_t FunctionVariants::count() const
{
    if ( const OnDiskFormat* p = header() )
        return p->tableCount;
    return 0;
}

FunctionVariants::OnDiskFormat* FunctionVariants::header() const
{
    if ( _bytes.size() < sizeof(OnDiskFormat) )
        return nullptr;
    return (OnDiskFormat*)_bytes.data();
}

const FunctionVariantsRuntimeTable* FunctionVariants::entry(uint32_t index) const
{
    if ( index < count() ) {
        if ( const OnDiskFormat* p = header() ) {
            uint32_t offsetInBlob = p->tableOffsets[index];
            if ( offsetInBlob < _bytes.size() )
                return (FunctionVariantsRuntimeTable*)(&_bytes[offsetInBlob]);
        }
    }
    return nullptr;
}



//
// MARK: --- FunctionVariantFixups methods ---
//

FunctionVariantFixups::FunctionVariantFixups(std::span<const uint8_t> linkeditBytes)
{
    _fixups = std::span<const InternalFixup>((InternalFixup*)linkeditBytes.data(), linkeditBytes.size()/sizeof(InternalFixup));
}


void FunctionVariantFixups::forEachFixup(void (^callback)(InternalFixup fixupInfo)) const
{
    for ( InternalFixup fixup : _fixups )
        callback(fixup);
}


} // namespace mach_o
