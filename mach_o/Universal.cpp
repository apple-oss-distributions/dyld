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
#include "UnsafeHeader.h"
#include "Misc.h"
#include "Architecture.h"
#include "Archive.h"

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
    memcpy(&headerFirstFourBytes, (uint32_t*)fileStartAsFat, 4); // use memcpy to avoid UB if content (such as in static lib) is not aligned
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
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
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
    if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO({(uint8_t*)this+sliceOffset, (size_t)sliceLen}) ) {
        if ( !hdr->isDSYM() && !hdr->isObjectFile() ) {
            uint32_t pageSizeMask = (hdr->uses16KPages() && !hdr->isObjectFile()) ? 0x3FFF : 0xFFF;
            if ( (sliceOffset & pageSizeMask) != 0 ) {
                // slice not page aligned
                return Error("slice is not page aligned");
            }
        }
        Architecture machHeaderArch = hdr->arch();
        if ( machHeaderArch != sliceArch ) {
            // for historical loosely match arm64e
            if ( !machHeaderArch.usesArm64AuthPointers() || !sliceArch.usesArm64AuthPointers() )
                return Error("cpu type/subtype in slice (%s) does not match fat header (%s)", machHeaderArch.name(), sliceArch.name());
        }
    }
    return Error::none();
}


void Universal::forEachSlice(void (^callback)(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop)) const
{
    bool           stop     = false;
    const uint32_t numArchs = OSSwapBigToHostInt32(this->fh.nfat_arch);
	if ( this->fh.magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
        const fat_arch* const archs = (fat_arch*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; (i < numArchs) && !stop; ++i) {
            Architecture arch(&archs[i]);
            uint32_t sliceOffset     = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t sliceLen        = OSSwapBigToHostInt32(archs[i].size);
            uint32_t sliceAlign      = OSSwapBigToHostInt32(archs[i].align);
           if ( arch == Architecture::arm64e_old ) {
                // FIXME: hack libtool built fat headers are missing ABI info for arm64e slices
                arch = Architecture::arm64e;
            }
            callback(arch, sliceOffset, sliceLen, sliceAlign, stop);
        }
        // Look for one more slice for arm64ageddon
        Architecture arch(&archs[numArchs]);
        if ( arch == Architecture::arm64 ) {
            uint32_t sliceOffset = OSSwapBigToHostInt32(archs[numArchs].offset);
            uint32_t sliceLen    = OSSwapBigToHostInt32(archs[numArchs].size);
            uint32_t sliceAlign  = OSSwapBigToHostInt32(archs[numArchs].align);
            callback(arch, sliceOffset, sliceLen, sliceAlign, stop);
        }
    }
    else if ( this->fh.magic == OSSwapBigToHostInt32(FAT_MAGIC_64) ) {
        const fat_arch_64* const archs = (fat_arch_64*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; (i < numArchs) && !stop; ++i) {
            Architecture arch(&archs[i]);
            uint64_t sliceOffset     = OSSwapBigToHostInt64(archs[i].offset);
            uint64_t sliceLen        = OSSwapBigToHostInt64(archs[i].size);
            uint32_t sliceAlign      = OSSwapBigToHostInt32(archs[i].align);
            callback(arch, sliceOffset, sliceLen, sliceAlign, stop);
        }
    }
}

void Universal::forEachSlice(void (^callback)(Slice slice, bool& stop)) const
{
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
        callback(Slice{sliceArch, std::span((uint8_t*)this+sliceOffset, (size_t)sliceSize), sliceOffset, sliceAlignment}, stop);
    });
}

// construct string describing slices in file like:  "x86-64,arm64,arm64e"
const char* Universal::archNames(char strBuf[256]) const
{
    strBuf[0] = '\0';
    __block bool  needComma = false;
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
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
    this->forEachSlice(^(Architecture sliceArch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
        if ( needComma )
            strlcat(strBuf, ",", 512);
        strlcat(strBuf, sliceArch.name(), 512);
        const UnsafeHeader* mh = (UnsafeHeader*)((uint8_t*)this+sliceOffset);
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

uint32_t Universal::sliceCount() const
{
    __block uint32_t count = 0;
    this->forEachSlice(^(Architecture arch, uint64_t sliceOffset, uint64_t sliceSize, uint8_t sliceAlignment, bool& stop) {
        ++count;
    });
    return count;
}

uint8_t Universal::defaultAlignment(std::span<const uint8_t> fileContent)
{
    if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(fileContent) ) {
        // .o slices only need to be 8-byte align in fat .o files
        if ( hdr->isObjectFile() )
            return 3; // 8-byte aligned

        // dSYM slices only need to be 32-byte align in fat .dSYM files
        if ( hdr->isDSYM() )
            return 5; // 32-byte aligned

        // all old final images are 4KB aligned
        if ( hdr->arch().usesx86_64Instructions() )
            return 12; // 4KB aligned
        if ( hdr->arch().usesArm32Instructions() || hdr->arch().usesThumbInstructions() )
            return 12; // 4KB aligned
        if ( hdr->arch() == Architecture::i386 )
            return 12; // 4KB aligned
    }
    else if ( UnsafeHeader::isBitCodeHeader(fileContent) ) {
        // .o bit codes slices only need to be 8-byte align in fat .o files
        return 3; // 8-byte aligned
    }
#if !TARGET_OS_EXCLAVEKIT
    else if ( Archive::isArchive(fileContent) ) {
        return 3; // 8-byte aligned
    }
#endif
    // everything thing else is 16KB aligned slices in fat files
    return 14; // 16KB aligned
}


} // namespace mach_o





