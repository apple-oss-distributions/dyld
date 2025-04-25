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
#include <string.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT
  #include <mach/mach.h>
  #include <unistd.h>
#else
  #define OSSwapBigToHostInt32 __builtin_bswap32
  #define OSSwapBigToHostInt64 __builtin_bswap64
  #define htonl                __builtin_bswap32
#endif // !TARGET_OS_EXCLAVEKIT

// mach_o
#include "Header.h"
#include "Misc.h"
#include "Architecture.h"

// mach_o_writer
#include "UniversalWriter.h"

namespace mach_o {

//
// MARK: --- methods when creating a fat file ---
//

// FIXME: fill out align field of fat header
// FIXME: compute slice alignment based on mach_header type and cpu type
// FIXME: sort slices by alignment
const UniversalWriter* UniversalWriter::make(std::span<const Header*> mhs, bool forceFat64, bool arm64offEnd)
{
    Slice slices[mhs.size()];
    for ( size_t i = 0; i < mhs.size(); ++i ) {
        const Header* header = mhs[i];
        Slice& slice = slices[i];
        slice.arch = header->arch();
        slice.buffer = std::span((const uint8_t*)header, header->fileSize());
    }

    return make(std::span(slices, mhs.size()), forceFat64, arm64offEnd);
}

const UniversalWriter* UniversalWriter::make(std::span<const Universal::Slice> slices, bool forceFat64, bool arm64offEnd)
{
    // compute number of slices and total size
    uint64_t totalSize = 0x4000;
    int32_t  count     = 0;
    for (const Universal::Slice& slice : slices) {
        ++count;
        totalSize += slice.buffer.size();
        pageAlign16K(totalSize);
    }

    // allocate buffer
    vm_address_t newAllocationAddr;
    if ( ::vm_allocate(mach_task_self(), &newAllocationAddr, (size_t)totalSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS )
        return nullptr;

    // make fat header
    UniversalWriter* result = (UniversalWriter*)newAllocationAddr;
    bool fat64 = forceFat64 || (totalSize > 0x100000000ULL);
    if ( fat64 ) {
        result->fh.magic     = OSSwapHostToBigInt32(FAT_MAGIC_64);
        result->fh.nfat_arch = OSSwapHostToBigInt32(count);
    }
    else {
        result->fh.magic     = OSSwapHostToBigInt32(FAT_MAGIC);
        if ( arm64offEnd && (slices[count-1].arch == Architecture::arm64) )
            result->fh.nfat_arch = OSSwapHostToBigInt32(count-1); // hide arm64 slice off end of array
        else
            result->fh.nfat_arch = OSSwapHostToBigInt32(count);
    }

    // add entry and copy each slice into buffer
    fat_arch*    entry32 = (fat_arch*)   ((uint8_t*)result + sizeof(fat_header));
    fat_arch_64* entry64 = (fat_arch_64*)((uint8_t*)result + sizeof(fat_header));
    uint64_t currentOffset = 0x4000;
    for (const Universal::Slice& slice : slices) {
        uint64_t sliceSize = slice.buffer.size();
        if ( fat64 ) {
            slice.arch.set(*entry64);
            entry64->offset    = OSSwapHostToBigInt64(currentOffset);
            entry64->size      = OSSwapHostToBigInt64(sliceSize);
            entry64->align     = OSSwapHostToBigInt32(0x4000);
            entry64->reserved  = 0;
            ++entry64;
        }
        else {
            slice.arch.set(*entry32);
            entry32->offset = OSSwapHostToBigInt32((uint32_t)currentOffset);
            entry32->size   = OSSwapHostToBigInt32((uint32_t)sliceSize);
            entry32->align  = OSSwapHostToBigInt32(0x4000);
            ++entry32;
        }
        memcpy((uint8_t*)newAllocationAddr+currentOffset, slice.buffer.data(), slice.buffer.size());
        currentOffset += sliceSize;
        pageAlign16K(currentOffset);
    }
    return result;
}

uint64_t UniversalWriter::size() const
{
    int currSliceCount = OSSwapBigToHostInt32(fh.nfat_arch);
    if ( currSliceCount == 0 )
        return 0x4000;

    __block uint64_t endOffset = 0;
    this->forEachSlice(^(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop) {
        endOffset = sliceOffset + sliceSize;
    });
    pageAlign16K(endOffset);
    return endOffset;
}

void UniversalWriter::save(char savedPath[PATH_MAX]) const
{
    ::strcpy(savedPath, "/tmp/universal-XXXXXX");
    int fd = ::mkstemp(savedPath);
    if ( fd != -1 ) {
        ::pwrite(fd, this, (size_t)size(), 0);
        ::close(fd);
    }
}

void UniversalWriter::free() const
{
    ::vm_deallocate(mach_task_self(), (vm_address_t)this, (vm_size_t)size());
}


} // namespace mach_o





