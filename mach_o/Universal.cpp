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

#include "Universal.h"
#include "Header.h"
#include "Misc.h"
#include "Architecture.h"

namespace mach_o {

//
// MARK: --- methods for inspecting a fat file ---
//

const Universal* Universal::isUniversal(std::span<const uint8_t> fileContent)
{
    if ( fileContent.size() < sizeof(fat_header) )
        return nullptr;

    const Universal* fileStartAsFat = (Universal*)fileContent.data();
    uint32_t headerFirstFourBytes;
    memcpy(&headerFirstFourBytes, fileStartAsFat, 4); // use memcpy to avoid UB if content (such as in static lib) is not aligned
    if ( (headerFirstFourBytes == OSSwapBigToHostInt32(FAT_MAGIC)) || (headerFirstFourBytes == OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return fileStartAsFat;
    else
        return nullptr;
}

Error Universal::valid(uint64_t fileSize) const
{
    if ( (this->fh.magic != OSSwapBigToHostInt32(FAT_MAGIC)) && (this->fh.magic != OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return Error("file does not start with FAT_MAGIC");

    const bool      isFat64 = this->fh.magic == OSSwapBigToHostInt32(FAT_MAGIC_64);
    const uint32_t  minFileSize = sizeof(fat_header) + (isFat64 ? sizeof(fat_arch_64) : sizeof(fat_arch));
    if ( fileSize < minFileSize )
        return Error("fat file too short");

    const uint32_t sliceCount = OSSwapBigToHostInt32(this->fh.nfat_arch);
    if ( sliceCount > kMaxSliceCount )
        return Error("fat file has too many slices (%d)", sliceCount);

    // 32-bit FAT file must fit n+1 slice headers to possbily account for the past-end arm64 slice.
    // Theoretically a 32-bit FAT file that fits only n slice headers could be valid too, but that'd be a file with empty slices, so we can ignore that.
    const uint32_t archHeadersSize = (isFat64 ? (sliceCount * sizeof(fat_arch_64)) : ((sliceCount + 1) * sizeof(fat_arch)));
    if ( greaterThanAddOrOverflow(sizeof(fat_header), archHeadersSize, fileSize) )
        return Error("slice headers extend beyond end of file");

    struct SliceRange { uint64_t start; uint64_t end; };
    SliceRange             sliceRanges[kMaxSliceCount];
    Architecture           archsBuffer[kMaxSliceCount];
    Architecture*          archsStart    = archsBuffer;
    __block Error          sliceError;
    __block uint64_t       lastSliceEnd  = minFileSize;
    __block Architecture*  archsCurrent  = archsBuffer;
    __block SliceRange*    slicesCurrent = sliceRanges;
    __block bool           strictLayout  = true;
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop) {
        *slicesCurrent++ = {sliceOffset, sliceOffset+sliceSize};
        if ( greaterThanAddOrOverflow(sliceOffset, sliceSize, fileSize) ) {
            sliceError = Error("%s slice extends beyond end of file", sliceArch.name());
            stop = true;
            return;
        }
        if ( sliceOffset < lastSliceEnd ) {
            strictLayout = false;
        }
        for (Architecture* a=archsStart; a < archsCurrent; ++a) {
            if ( sliceArch == *a ) {
                sliceError = Error("duplicate %s slices", sliceArch.name());
                stop = true;
                return;
            }
        }
        *archsCurrent++ = sliceArch;
        sliceError = validSlice(sliceArch, sliceOffset, sliceSize);
        if ( sliceError.hasError() )
            stop = true;
        lastSliceEnd = sliceOffset + sliceSize;
    });
    if ( sliceError.hasError() )
        return std::move(sliceError);

    if ( !strictLayout ) {
        // slices either overlap or are not in order
        size_t count = slicesCurrent - sliceRanges;
        for (int i=0; i < count; ++i) {
            for (int j=0; j < count; ++j) {
                if ( i == j )
                    continue;
                if ((sliceRanges[j].start < sliceRanges[i].end) && (sliceRanges[j].end > sliceRanges[i].start)) {
                    return Error("overlapping slices");
                }
            }
        }
    }
    return Error::none();
}

Error Universal::validSlice(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceLen) const
{
    if ( const Header* mh = Header::isMachO({(uint8_t*)this+sliceOffset, (size_t)sliceLen}) ) {
        uint32_t pageSizeMask = (mh->uses16KPages() && !mh->isObjectFile()) ? 0x3FFF : 0xFFF;
        if ( (sliceOffset & pageSizeMask) != 0 ) {
            // slice not page aligned
            return Error("slice is not page aligned");
        }
        Architecture machHeaderArch = mh->arch();
        if ( machHeaderArch != sliceArch )
            return Error("cpu type/subtype in slice (%s) does not match fat header (%s)", machHeaderArch.name(), sliceArch.name());
    }
    return Error::none();
}


void Universal::forEachSlice(void (^callback)(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop)) const
{
    bool           stop     = false;
    const uint32_t numArchs = OSSwapBigToHostInt32(this->fh.nfat_arch);
	if ( this->fh.magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
        const fat_arch* const archs = (fat_arch*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; (i < numArchs) && !stop; ++i) {
            Architecture arch(&archs[i]);
            uint32_t sliceOffset     = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t sliceLen        = OSSwapBigToHostInt32(archs[i].size);
            if ( arch == Architecture::arm64e_old ) {
                // FIXME: hack libtool built fat headers are missing ABI info for arm64e slices
                arch = Architecture::arm64e;
            }
            callback(arch, sliceOffset, sliceLen, stop);
        }
        // Look for one more slice for arm64ageddon
        Architecture arch(&archs[numArchs]);
        if ( arch == Architecture::arm64 ) {
            uint32_t sliceOffset = OSSwapBigToHostInt32(archs[numArchs].offset);
            uint32_t sliceLen    = OSSwapBigToHostInt32(archs[numArchs].size);
            callback(arch, sliceOffset, sliceLen, stop);
        }
    }
    else if ( this->fh.magic == OSSwapBigToHostInt32(FAT_MAGIC_64) ) {
        const fat_arch_64* const archs = (fat_arch_64*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; (i < numArchs) && !stop; ++i) {
            Architecture arch(&archs[i]);
            uint64_t sliceOffset     = OSSwapBigToHostInt64(archs[i].offset);
            uint64_t sliceLen        = OSSwapBigToHostInt64(archs[i].size);
            callback(arch, sliceOffset, sliceLen, stop);
        }
    }
}

void Universal::forEachSlice(void (^callback)(Slice slice, bool& stop)) const
{
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop) {
        callback(Slice{sliceArch, std::span((uint8_t*)this+sliceOffset, (size_t)sliceSize)}, stop);
    });
}

// construct string describing slices in file like:  "x86-64,arm64,arm64e"
const char* Universal::archNames(char strBuf[256]) const
{
    strBuf[0] = '\0';
    __block bool  needComma = false;
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop) {
        if ( needComma )
            strlcat(strBuf, ",", 256);
        strlcat(strBuf, sliceArch.name(), 256);
        needComma = true;
    });
    return strBuf;
}

// construct string describing slices in file like:  "x86-64:macOS,arm64:macOS"
const char* Universal::archAndPlatformNames(char strBuf[512]) const
{
    strBuf[0] = '\0';
    __block bool  needComma = false;
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, bool& stop) {
        if ( needComma )
            strlcat(strBuf, ",", 512);
        strlcat(strBuf, sliceArch.name(), 512);
        const Header* mh = (Header*)((uint8_t*)this+sliceOffset);
        strlcat(strBuf, ":", 512);
        strlcat(strBuf, mh->platformAndVersions().platform.name().c_str(), 512);
        needComma = true;
    });
    return strBuf;
}

bool Universal::bestSlice(const GradedArchitectures& gradedArchs, bool isOSBinary, Slice& sliceOut) const
{
    Slice           sliceBuffer[kMaxSliceCount];
    Architecture    archBuffer[kMaxSliceCount];
    Slice*          allSlices    = sliceBuffer;
    Architecture*   allArchs    = archBuffer;
    __block uint32_t sliceCount = 0;
    this->forEachSlice(^(Slice slice, bool& stop) {
        allArchs[sliceCount]    = slice.arch;
        allSlices[sliceCount]   = slice;
        ++sliceCount;
    });

    uint32_t bestSliceIndex;
    if ( gradedArchs.hasCompatibleSlice(std::span<Architecture>(allArchs, sliceCount), isOSBinary, bestSliceIndex) ) {
        sliceOut = allSlices[bestSliceIndex];
        return true;
    }

    return false;
}

//
// MARK: --- methods when creating a fat file ---
//

#if BUILDING_MACHO_WRITER

// FIXME: fill out align field of fat header
// FIXME: compute slice alignment based on mach_header type and cpu type
// FIXME: sort slices by alignment
const Universal* Universal::make(std::span<const Header*> mhs, bool forceFat64, bool arm64offEnd)
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

const Universal* Universal::make(std::span<const Universal::Slice> slices, bool forceFat64, bool arm64offEnd)
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
    Universal* result = (Universal*)newAllocationAddr;
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

uint64_t Universal::size() const
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

void Universal::save(char savedPath[PATH_MAX]) const
{
    ::strcpy(savedPath, "/tmp/universal-XXXXXX");
    int fd = ::mkstemp(savedPath);
    if ( fd != -1 ) {
        ::pwrite(fd, this, (size_t)size(), 0);
        ::close(fd);
    }
}

void Universal::free() const
{
    ::vm_deallocate(mach_task_self(), (vm_address_t)this, (vm_size_t)size());
}

#endif // BUILDING_MACHO_WRITER


} // namespace mach_o





