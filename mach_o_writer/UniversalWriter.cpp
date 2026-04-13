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
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
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
#include "UnsafeHeader.h"
#include "Misc.h"
#include "Architecture.h"

// mach_o_writer
#include "UniversalWriter.h"

#include "Utilities.h"

namespace mach_o {

//
// MARK: --- methods when creating a fat file ---
//

// FIXME: fill out align field of fat header
// FIXME: compute slice alignment based on mach_header type and cpu type
// FIXME: sort slices by alignment
// FIXME: optimize layout by sorting slices by alignment
const UniversalWriter* UniversalWriter::make(std::span<const UnsafeHeader*> mhs, bool forceFat64, bool arm64offEnd)
{
    Slice slices[mhs.size()];
    for ( size_t i = 0; i < mhs.size(); ++i ) {
        const UnsafeHeader* hdr = mhs[i];
        Slice& slice = slices[i];
        slice.arch       = hdr->arch();
        slice.buffer     = std::span((const uint8_t*)hdr, hdr->fileSize());
        slice.fileOffset = 0;
        slice.alignment  = Universal::defaultAlignment(slice.buffer);
    }

    return make(std::span(slices, mhs.size()), forceFat64, arm64offEnd);
}

const UniversalWriter* UniversalWriter::make(std::span<const Universal::Slice> inputSlices, bool forceFat64, bool arm64offEnd)
{
    // Make a copy so that we can update the alignment
    std::vector<Universal::Slice> slices{ inputSlices.begin(), inputSlices.end() };
    for ( Universal::Slice& slice : slices ) {
        if ( slice.alignment == 0 )
            slice.alignment = Universal::defaultAlignment(slice.buffer);
    }

    // compute worst case header size
    uint64_t headerSize = sizeof(fat_header) + slices.size()*(forceFat64 ? sizeof(fat_arch_64) : sizeof(fat_arch));
    uint64_t firstAlign = (1 << slices[0].alignment);
    headerSize = (headerSize + firstAlign - 1) & (-firstAlign);

    // compute slice offsets and total size
    uint64_t totalSize          = headerSize;
    uint64_t largestSliceOffset = 0; // for checking if this fits into classic fat file format
    int32_t  count              = 0;
    uint64_t offsets[slices.size()];
    for (const Universal::Slice& slice : slices) {
        uint64_t align = (1 << slice.alignment);
        totalSize = (totalSize + align - 1) & (-align);
        offsets[count] = totalSize;
        if ( offsets[count] > largestSliceOffset )
            largestSliceOffset = offsets[count];
        totalSize += slice.buffer.size();
        ++count;
    }
    // if some slice will start > 4GB into the file, switch to fat64
    if ( !forceFat64 && (largestSliceOffset > 0x100000000ULL) )
        return make(slices, true, arm64offEnd);

    // allocate buffer
    vm_address_t newAllocationAddr;
    if ( ::vm_allocate(mach_task_self(), &newAllocationAddr, (size_t)totalSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS )
        return nullptr;

    // make fat header
    UniversalWriter* result = (UniversalWriter*)newAllocationAddr;
    if ( forceFat64 ) {
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
    int index = 0;
    for (const Universal::Slice& slice : slices) {
        if ( forceFat64 ) {
            slice.arch.set(*entry64);
            entry64->offset    = OSSwapHostToBigInt64(offsets[index]);
            entry64->size      = OSSwapHostToBigInt64(slice.buffer.size());
            entry64->align     = OSSwapHostToBigInt32(slice.alignment);
            entry64->reserved  = 0;
            ++entry64;
        }
        else {
            slice.arch.set(*entry32);
            entry32->offset = OSSwapHostToBigInt32((uint32_t)offsets[index]);
            entry32->size   = OSSwapHostToBigInt32((uint32_t)slice.buffer.size());
            entry32->align  = OSSwapHostToBigInt32(slice.alignment);
            ++entry32;
        }
        memcpy((uint8_t*)newAllocationAddr+offsets[index], slice.buffer.data(), slice.buffer.size());
        ++index;
    }
    return result;
}

uint64_t UniversalWriter::size() const
{
    int currSliceCount = OSSwapBigToHostInt32(fh.nfat_arch);
    if ( currSliceCount == 0 )
        return 0x4000;

    __block uint64_t endOffset = 0;
    this->forEachSlice(^(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
        endOffset = sliceOffset + sliceSize;
    });
    return endOffset;
}

std::span<const uint8_t> UniversalWriter::content() const
{
    return std::span<const uint8_t>((uint8_t*)this, (size_t)size());
}

void UniversalWriter::save(char savedPath[PATH_MAX]) const
{
    ::strcpy(savedPath, "/tmp/universal-XXXXXX");
    int fd = ::mkstemp(savedPath);
    if ( fd != -1 ) {
        write64(fd, this, (size_t)size());
        ::close(fd);
    }
}

bool UniversalWriter::saveToPath(const char* path, uint32_t permissions) const
{
    mode_t umask = ::umask(0);
    ::umask(umask); // put back the original umask
    permissions &= ~umask;

    int fd = ::open(path, O_WRONLY | O_CREAT, permissions);
    if ( fd != -1 ) {
        ::ftruncate(fd, 0);
        size_t sz     = (size_t)size();
        bool   result = (write64(fd, this, sz) == sz);
        ::close(fd);
        return result;
    }
    return false;
}

void UniversalWriter::free() const
{
    ::vm_deallocate(mach_task_self(), (vm_address_t)this, (vm_size_t)size());
}


} // namespace mach_o





