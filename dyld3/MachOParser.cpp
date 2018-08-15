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
#include <assert.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <rootless.h>
#include <dirent.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <mach-o/reloc.h>
#include <mach-o/dyld_priv.h>
#include <CommonCrypto/CommonDigest.h>

#if !DYLD_IN_PROCESS
#include <dlfcn.h>
#endif

#include "MachOParser.h"
#include "Logging.h"
#include "CodeSigningTypes.h"
#include "DyldSharedCache.h"
#include "Trie.hpp"

#if DYLD_IN_PROCESS
    #include "APIs.h"
#else
    #include "StringUtils.h"
#endif



#ifndef EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
    #define EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE 0x02
#endif

#ifndef CPU_SUBTYPE_ARM64_E
    #define CPU_SUBTYPE_ARM64_E    2
#endif

#ifndef LC_BUILD_VERSION
    #define LC_BUILD_VERSION 0x32 /* build for platform min OS version */

    /*
     * The build_version_command contains the min OS version on which this
     * binary was built to run for its platform.  The list of known platforms and
     * tool values following it.
     */
    struct build_version_command {
        uint32_t    cmd;        /* LC_BUILD_VERSION */
        uint32_t    cmdsize;    /* sizeof(struct build_version_command) plus */
        /* ntools * sizeof(struct build_tool_version) */
        uint32_t    platform;   /* platform */
        uint32_t    minos;      /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
        uint32_t    sdk;        /* X.Y.Z is encoded in nibbles xxxx.yy.zz */
        uint32_t    ntools;     /* number of tool entries following this */
    };

    struct build_tool_version {
        uint32_t    tool;       /* enum for the tool */
        uint32_t    version;    /* version number of the tool */
    };

    /* Known values for the platform field above. */
    #define PLATFORM_MACOS      1
    #define PLATFORM_IOS        2
    #define PLATFORM_TVOS       3
    #define PLATFORM_WATCHOS    4
    #define PLATFORM_BRIDGEOS   5

    /* Known values for the tool field above. */
    #define TOOL_CLANG    1
    #define TOOL_SWIFT    2
    #define TOOL_LD       3
#endif


namespace dyld3 {


bool FatUtil::isFatFile(const void* fileStart)
{
    const fat_header* fileStartAsFat = (fat_header*)fileStart;
    return ( fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC) );
}

/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
static bool greaterThanAddOrOverflow(uint32_t addLHS, uint32_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}

/// Returns true if (addLHS + addRHS) > b, or if the add overflowed
template<typename T>
static bool greaterThanAddOrOverflow(uint64_t addLHS, uint64_t addRHS, T b) {
    return (addLHS > b) || (addRHS > (b-addLHS));
}

void FatUtil::forEachSlice(Diagnostics& diag, const void* fileContent, size_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, size_t sliceSize, bool& stop))
{
    const fat_header* fh = (fat_header*)fileContent;
    if ( fh->magic != OSSwapBigToHostInt32(FAT_MAGIC) ) {
        diag.error("not a fat file");
        return;
    }

    if ( OSSwapBigToHostInt32(fh->nfat_arch) > ((4096 - sizeof(fat_header)) / sizeof(fat_arch)) ) {
        diag.error("fat header too large: %u entries", OSSwapBigToHostInt32(fh->nfat_arch));
    }
    const fat_arch* const archs = (fat_arch*)(((char*)fh)+sizeof(fat_header));
    bool stop = false;
    for (uint32_t i=0; i < OSSwapBigToHostInt32(fh->nfat_arch); ++i) {
        uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
        uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
        uint32_t offset     = OSSwapBigToHostInt32(archs[i].offset);
        uint32_t len        = OSSwapBigToHostInt32(archs[i].size);
        if (greaterThanAddOrOverflow(offset, len, fileLen)) {
            diag.error("slice %d extends beyond end of file", i);
            return;
        }
        callback(cpuType, cpuSubType, (uint8_t*)fileContent+offset, len, stop);
        if ( stop )
            break;
    }
}

#if !DYLD_IN_PROCESS
bool FatUtil::isFatFileWithSlice(Diagnostics& diag, const void* fileContent, size_t fileLen, const std::string& archName, size_t& sliceOffset, size_t& sliceLen, bool& missingSlice)
{
    missingSlice = false;
    if ( !isFatFile(fileContent) )
        return false;

    __block bool found = false;
    forEachSlice(diag, fileContent, fileLen, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, size_t sliceSize, bool& stop) {
        std::string sliceArchName = MachOParser::archName(sliceCpuType, sliceCpuSubType);
        if ( sliceArchName == archName ) {
            sliceOffset = (char*)sliceStart - (char*)fileContent;
            sliceLen    = sliceSize;
            found       = true;
            stop        = true;
        }
    });
    if ( diag.hasError() )
        return false;

    if ( !found )
        missingSlice = true;

    // when looking for x86_64h fallback to x86_64
    if ( !found && (archName == "x86_64h") )
        return isFatFileWithSlice(diag, fileContent, fileLen, "x86_64", sliceOffset, sliceLen, missingSlice);

    return found;
}

#endif

MachOParser::MachOParser(const mach_header* mh, bool dyldCacheIsRaw)
{
#if DYLD_IN_PROCESS
    // assume all in-process mach_headers are real loaded images
    _data = (long)mh;
#else
    if (mh == nullptr)
        return;
    _data = (long)mh;
    if ( (mh->flags & 0x80000000) == 0 ) {
        // asssume out-of-process mach_header not in a dyld cache are raw mapped files
        _data |= 1;
    }
    // out-of-process mach_header in a dyld cache are not raw, but cache may be raw
    if ( dyldCacheIsRaw )
        _data |= 2;
#endif
}

const mach_header* MachOParser::header() const
{
    return (mach_header*)(_data & -4);
}

// "raw" means the whole mach-o file was mapped as one contiguous region
// not-raw means the the mach-o file was mapped like dyld does - with zero fill expansion
bool MachOParser::isRaw() const
{
    return (_data & 1);
}

// A raw dyld cache is when the whole dyld cache file is mapped in one contiguous region
// not-raw manes the dyld cache was mapped as it is at runtime with padding between regions
bool MachOParser::inRawCache() const
{
    return (_data & 2);
}

uint32_t MachOParser::fileType() const
{
    return header()->filetype;
}

bool MachOParser::inDyldCache() const
{
    return (header()->flags & 0x80000000);
}

bool MachOParser::hasThreadLocalVariables() const
{
    return (header()->flags & MH_HAS_TLV_DESCRIPTORS);
}

Platform MachOParser::platform() const
{
    Platform platform;
    uint32_t minOS;
    uint32_t sdk;
    if ( getPlatformAndVersion(&platform, &minOS, &sdk) )
        return platform;

    // old binary with no explict load command to mark platform, look at arch
    switch ( header()->cputype ) {
        case CPU_TYPE_X86_64:
        case CPU_TYPE_I386:
            return Platform::macOS;
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM:
            return Platform::iOS;
    }
    return Platform::macOS;
}


#if !DYLD_IN_PROCESS

const MachOParser::ArchInfo MachOParser::_s_archInfos[] = {
    { "x86_64", CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_ALL },
    { "x86_64h", CPU_TYPE_X86_64, CPU_SUBTYPE_X86_64_H },
    { "i386", CPU_TYPE_I386, CPU_SUBTYPE_I386_ALL },
    { "arm64", CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL },
    { "arm64e", CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_E },
    { "armv7k", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7K },
    { "armv7s", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7S },
    { "armv7", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7 }
};

bool MachOParser::isValidMachO(Diagnostics& diag, const std::string& archName, Platform platform, const void* fileContent, size_t fileLength, const std::string& pathOpened, bool ignoreMainExecutables)
{
    // must start with mach-o magic value
    const mach_header* mh = (const mach_header*)fileContent;
    if ( (mh->magic != MH_MAGIC) && (mh->magic != MH_MAGIC_64) ) {
        diag.warning("could not use '%s' because it is not a mach-o file", pathOpened.c_str());
        return false;
    }

    // must match requested architecture if specified
    if (!archName.empty() && !isArch(mh, archName)) {
        // except when looking for x86_64h, fallback to x86_64
        if ( (archName != "x86_64h") || !isArch(mh, "x86_64") ) {
            diag.warning("could not use '%s' because it does not contain required architecture %s", pathOpened.c_str(), archName.c_str());
            return false;
        }
    }

    // must be a filetype dyld can load
    switch ( mh->filetype ) {
        case MH_EXECUTE:
            if ( ignoreMainExecutables )
                return false;
            break;
        case MH_DYLIB:
        case MH_BUNDLE:
            break;
        default:
            diag.warning("could not use '%s' because it is not a dylib, bundle, or executable", pathOpened.c_str());
            return false;
    }

    // must be from a file - not in the dyld shared cache
    if ( mh->flags & 0x80000000 ) {
        diag.warning("could not use '%s' because the high bit of mach_header flags is reserved for images in dyld cache", pathOpened.c_str());
        return false;
    }

    // validate load commands structure
    MachOParser parser(mh);
    if ( !parser.validLoadCommands(diag, fileLength) )
        return false;

    // must match requested platform
    if ( parser.platform() != platform ) {
        diag.warning("could not use '%s' because it was built for a different platform", pathOpened.c_str());
        return false;
    }

    // cannot be a static executable
    if ( (mh->filetype == MH_EXECUTE) && !parser.isDynamicExecutable() ) {
        diag.warning("could not use '%s' because it is a static executable", pathOpened.c_str());
        return false;
    }

    // validate dylib loads
    if ( !parser.validEmbeddedPaths(diag) )
        return false;

    // validate segments
    if ( !parser.validSegments(diag, fileLength) )
        return false;

    // validate LINKEDIT layout
    if ( !parser.validLinkeditLayout(diag) )
        return false;

     return true;
}


bool MachOParser::validLoadCommands(Diagnostics& diag, size_t fileLen)
{
    // check load command don't exceed file length
    if ( header()->sizeofcmds + sizeof(mach_header_64) > fileLen ) {
        diag.warning("load commands exceed length of file");
        return false;
    }
    // walk all load commands and sanity check them
    Diagnostics walkDiag;
    LinkEditInfo lePointers;
    getLinkEditLoadCommands(walkDiag, lePointers);
    if ( walkDiag.hasError() ) {
        diag.warning("%s", walkDiag.errorMessage().c_str());
        return false;
    }

    // check load commands fit in TEXT segment
    __block bool overflowText = false;
    forEachSegment(^(const char* segName, uint32_t segFileOffset, uint32_t segFileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            if ( header()->sizeofcmds + sizeof(mach_header_64) > segFileSize ) {
                diag.warning("load commands exceed length of __TEXT segment");
                overflowText = true;
            }
            stop = true;
        }
    });
    if ( overflowText )
        return false;

    return true;
}

bool MachOParser::validEmbeddedPaths(Diagnostics& diag)
{
    __block int index = 1;
    __block bool allGood = true;
    __block bool foundInstallName = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const dylib_command* dylibCmd;
        const rpath_command* rpathCmd;
        switch ( cmd->cmd ) {
            case LC_ID_DYLIB:
                foundInstallName = true;
                // fall through
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                if ( dylibCmd->dylib.name.offset > cmd->cmdsize ) {
                    diag.warning("load command #%d name offset (%u) outside its size (%u)", index, dylibCmd->dylib.name.offset, cmd->cmdsize);
                    stop = true;
                    allGood = false;
                }
                else {
                    bool foundEnd = false;
                    const char* start = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                    const char* end   = (char*)dylibCmd + cmd->cmdsize;
                    for (const char* s=start; s < end; ++s) {
                        if ( *s == '\0' ) {
                            foundEnd = true;
                            break;
                        }
                    }
                    if ( !foundEnd ) {
                        diag.warning("load command #%d string extends beyond end of load command", index);
                        stop = true;
                        allGood = false;
                    }
                }
                break;
            case LC_RPATH:
                rpathCmd = (rpath_command*)cmd;
                if ( rpathCmd->path.offset > cmd->cmdsize ) {
                    diag.warning("load command #%d path offset (%u) outside its size (%u)", index, rpathCmd->path.offset, cmd->cmdsize);
                    stop = true;
                    allGood = false;
                }
                else {
                    bool foundEnd = false;
                    const char* start = (char*)rpathCmd + rpathCmd->path.offset;
                    const char* end   = (char*)rpathCmd + cmd->cmdsize;
                    for (const char* s=start; s < end; ++s) {
                        if ( *s == '\0' ) {
                            foundEnd = true;
                            break;
                        }
                    }
                    if ( !foundEnd ) {
                        diag.warning("load command #%d string extends beyond end of load command", index);
                        stop = true;
                        allGood = false;
                    }
                }
                break;
        }
        ++index;
    });

    if ( header()->filetype == MH_DYLIB ) {
        if ( !foundInstallName ) {
            diag.warning("MH_DYLIB is missing LC_ID_DYLIB");
            allGood = false;
        }
    }
    else {
        if ( foundInstallName ) {
            diag.warning("LC_ID_DYLIB found in non-MH_DYLIB");
            allGood = false;
        }
    }

    return allGood;
}

bool MachOParser::validSegments(Diagnostics& diag, size_t fileLen)
{
    // check segment load command size
    __block bool badSegmentLoadCommand = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command_64);
            if ( sectionsSpace < 0 ) {
               diag.warning("load command size too small for LC_SEGMENT_64");
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section_64)) != 0 ) {
               diag.warning("segment load command size 0x%X will not fit whole number of sections", cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (seg->nsects * sizeof(section_64)) ) {
               diag.warning("load command size 0x%X does not match nsects %d", cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            } else if (greaterThanAddOrOverflow(seg->fileoff, seg->filesize, fileLen)) {
                diag.warning("segment load command content extends beyond end of file");
                badSegmentLoadCommand = true;
                stop = true;
            } else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.warning("segment filesize exceeds vmsize");
                badSegmentLoadCommand = true;
                stop = true;
            }
       }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command);
            if ( sectionsSpace < 0 ) {
               diag.warning("load command size too small for LC_SEGMENT");
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section)) != 0 ) {
               diag.warning("segment load command size 0x%X will not fit whole number of sections", cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (seg->nsects * sizeof(section)) ) {
               diag.warning("load command size 0x%X does not match nsects %d", cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            } else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.warning("segment filesize exceeds vmsize");
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
    });
     if ( badSegmentLoadCommand )
         return false;

    // check mapping permissions of segments
    __block bool badPermissions = false;
    __block bool badSize        = false;
    __block bool hasTEXT        = false;
    __block bool hasLINKEDIT    = false;
    forEachSegment(^(const char* segName, uint32_t segFileOffset, uint32_t segFileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            if ( protections != (VM_PROT_READ|VM_PROT_EXECUTE) ) {
                diag.warning("__TEXT segment permissions is not 'r-x'");
                badPermissions = true;
                stop = true;
            }
            hasTEXT = true;
        }
        else if ( strcmp(segName, "__LINKEDIT") == 0 ) {
            if ( protections != VM_PROT_READ ) {
                diag.warning("__LINKEDIT segment permissions is not 'r--'");
                badPermissions = true;
                stop = true;
            }
            hasLINKEDIT = true;
        }
        else if ( (protections & 0xFFFFFFF8) != 0 ) {
            diag.warning("%s segment permissions has invalid bits set", segName);
            badPermissions = true;
            stop = true;
        }
        if (greaterThanAddOrOverflow(segFileOffset, segFileSize, fileLen)) {
            diag.warning("%s segment content extends beyond end of file", segName);
            badSize = true;
            stop = true;
        }
        if ( is64() ) {
            if ( vmAddr+vmSize < vmAddr ) {
                diag.warning("%s segment vm range wraps", segName);
                badSize = true;
                stop = true;
            }
       }
       else {
            if ( (uint32_t)(vmAddr+vmSize) < (uint32_t)(vmAddr) ) {
                diag.warning("%s segment vm range wraps", segName);
                badSize = true;
                stop = true;
            }
       }
    });
    if ( badPermissions || badSize )
        return false;
    if ( !hasTEXT ) {
       diag.warning("missing __TEXT segment");
       return false;
    }
    if ( !hasLINKEDIT ) {
       diag.warning("missing __LINKEDIT segment");
       return false;
    }

    // check for overlapping segments
    __block bool badSegments = false;
    forEachSegment(^(const char* seg1Name, uint32_t seg1FileOffset, uint32_t seg1FileSize, uint64_t seg1vmAddr, uint64_t seg1vmSize, uint8_t seg1Protections, uint32_t seg1Index, uint64_t seg1SizeOfSections, uint8_t seg1Align, bool& stop1) {
        uint64_t seg1vmEnd   = seg1vmAddr + seg1vmSize;
        uint32_t seg1FileEnd = seg1FileOffset + seg1FileSize;
        forEachSegment(^(const char* seg2Name, uint32_t seg2FileOffset, uint32_t seg2FileSize, uint64_t seg2vmAddr, uint64_t seg2vmSize, uint8_t seg2Protections, uint32_t seg2Index, uint64_t seg2SizeOfSections, uint8_t seg2Align, bool& stop2) {
            if ( seg1Index == seg2Index )
                return;
            uint64_t seg2vmEnd   = seg2vmAddr + seg2vmSize;
            uint32_t seg2FileEnd = seg2FileOffset + seg2FileSize;
            if ( ((seg2vmAddr <= seg1vmAddr) && (seg2vmEnd > seg1vmAddr) && (seg1vmEnd > seg1vmAddr)) || ((seg2vmAddr >= seg1vmAddr) && (seg2vmAddr < seg1vmEnd) && (seg2vmEnd > seg2vmAddr)) ) {
                diag.warning("segment %s vm range overlaps segment %s", seg1Name, seg2Name);
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
             if ( ((seg2FileOffset <= seg1FileOffset) && (seg2FileEnd > seg1FileOffset) && (seg1FileEnd > seg1FileOffset)) || ((seg2FileOffset >= seg1FileOffset) && (seg2FileOffset < seg1FileEnd) && (seg2FileEnd > seg2FileOffset)) ) {
                diag.warning("segment %s file content overlaps segment %s", seg1Name, seg2Name);
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
            // check for out of order segments
            if ( (seg1Index < seg2Index) && !stop1 ) {
                if ( (seg1vmAddr > seg2vmAddr) || ((seg1FileOffset > seg2FileOffset) && (seg1FileOffset != 0) && (seg2FileOffset != 0)) ){
                    diag.warning("segment load commands out of order with respect to layout for %s and %s", seg1Name, seg2Name);
                    badSegments = true;
                    stop1 = true;
                    stop2 = true;
                }
            }
        });
    });
    if ( badSegments )
        return false;

    // check sections are within segment
    __block bool badSections = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            const section_64* const sectionsStart = (section_64*)((char*)seg + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section_64* sect=sectionsStart; (sect < sectionsEnd); ++sect) {
                if ( (int64_t)(sect->size) < 0 ) {
                    diag.warning("section %s size too large 0x%llX", sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.warning("section %s start address 0x%llX is before containing segment's address 0x%0llX",  sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.warning("section %s end address 0x%llX is beyond containing segment's end address 0x%0llX", sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            const section* const sectionsStart = (section*)((char*)seg + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
               if ( (int64_t)(sect->size) < 0 ) {
                    diag.warning("section %s size too large 0x%X", sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.warning("section %s start address 0x%X is before containing segment's address 0x%0X",  sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.warning("section %s end address 0x%X is beyond containing segment's end address 0x%0X", sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
    });

    return !badSections;
}

struct LinkEditContent
{
    const char* name;
    uint32_t    stdOrder;
    uint32_t    fileOffsetStart;
    uint32_t    size;
};



bool MachOParser::validLinkeditLayout(Diagnostics& diag)
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    const bool     is64Bit = is64();
    const uint32_t pointerSize = (is64Bit ? 8 : 4);

    // build vector of all blobs in LINKEDIT
    std::vector<LinkEditContent> blobs;
    if ( leInfo.dyldInfo != nullptr ) {
        if ( leInfo.dyldInfo->rebase_size != 0 )
            blobs.push_back({"rebase opcodes",         1, leInfo.dyldInfo->rebase_off, leInfo.dyldInfo->rebase_size});
        if ( leInfo.dyldInfo->bind_size != 0 )
            blobs.push_back({"bind opcodes",           2, leInfo.dyldInfo->bind_off, leInfo.dyldInfo->bind_size});
        if ( leInfo.dyldInfo->weak_bind_size != 0 )
            blobs.push_back({"weak bind opcodes",      3, leInfo.dyldInfo->weak_bind_off, leInfo.dyldInfo->weak_bind_size});
        if ( leInfo.dyldInfo->lazy_bind_size != 0 )
            blobs.push_back({"lazy bind opcodes",      4, leInfo.dyldInfo->lazy_bind_off, leInfo.dyldInfo->lazy_bind_size});
        if ( leInfo.dyldInfo->export_size!= 0 )
            blobs.push_back({"exports trie",           5, leInfo.dyldInfo->export_off, leInfo.dyldInfo->export_size});
    }
    if ( leInfo.dynSymTab != nullptr ) {
        if ( leInfo.dynSymTab->nlocrel != 0 )
            blobs.push_back({"local relocations",      6, leInfo.dynSymTab->locreloff, static_cast<uint32_t>(leInfo.dynSymTab->nlocrel*sizeof(relocation_info))});
        if ( leInfo.dynSymTab->nextrel != 0 )
            blobs.push_back({"external relocations",  11, leInfo.dynSymTab->extreloff, static_cast<uint32_t>(leInfo.dynSymTab->nextrel*sizeof(relocation_info))});
        if ( leInfo.dynSymTab->nindirectsyms != 0 )
            blobs.push_back({"indirect symbol table", 12, leInfo.dynSymTab->indirectsymoff, leInfo.dynSymTab->nindirectsyms*4});
    }
    if ( leInfo.splitSegInfo != nullptr ) {
        if ( leInfo.splitSegInfo->datasize != 0 )
            blobs.push_back({"shared cache info",      6, leInfo.splitSegInfo->dataoff, leInfo.splitSegInfo->datasize});
    }
    if ( leInfo.functionStarts != nullptr ) {
        if ( leInfo.functionStarts->datasize != 0 )
            blobs.push_back({"function starts",        7, leInfo.functionStarts->dataoff, leInfo.functionStarts->datasize});
    }
    if ( leInfo.dataInCode != nullptr ) {
        if ( leInfo.dataInCode->datasize != 0 )
            blobs.push_back({"data in code",           8, leInfo.dataInCode->dataoff, leInfo.dataInCode->datasize});
    }
    if ( leInfo.symTab != nullptr ) {
        if ( leInfo.symTab->nsyms != 0 )
            blobs.push_back({"symbol table",         10, leInfo.symTab->symoff, static_cast<uint32_t>(leInfo.symTab->nsyms*(is64Bit ? sizeof(nlist_64) : sizeof(struct nlist)))});
        if ( leInfo.symTab->strsize != 0 )
            blobs.push_back({"symbol table strings", 20, leInfo.symTab->stroff, leInfo.symTab->strsize});
    }
    if ( leInfo.codeSig != nullptr ) {
        if ( leInfo.codeSig->datasize != 0 )
            blobs.push_back({"code signature",       21, leInfo.codeSig->dataoff, leInfo.codeSig->datasize});
    }

    // check for bad combinations
    if ( (leInfo.dyldInfo != nullptr) && (leInfo.dyldInfo->cmd == LC_DYLD_INFO_ONLY) && (leInfo.dynSymTab != nullptr) ) {
        if ( leInfo.dynSymTab->nlocrel != 0 ) {
            diag.error("malformed mach-o contains LC_DYLD_INFO_ONLY and local relocations");
            return false;
        }
        if ( leInfo.dynSymTab->nextrel != 0 ) {
            diag.error("malformed mach-o contains LC_DYLD_INFO_ONLY and external relocations");
            return false;
        }
    }
    if ( (leInfo.dyldInfo == nullptr) && (leInfo.dynSymTab == nullptr) ) {
        diag.error("malformed mach-o misssing LC_DYLD_INFO and LC_DYSYMTAB");
        return false;
    }
    if ( blobs.empty() ) {
        diag.error("malformed mach-o misssing LINKEDIT");
        return false;
    }

    // sort vector by file offset and error on overlaps
    std::sort(blobs.begin(), blobs.end(), [&](const LinkEditContent& a, const LinkEditContent& b) {
        return a.fileOffsetStart < b.fileOffsetStart;
    });
    uint32_t     prevEnd = (uint32_t)(leInfo.layout.segments[leInfo.layout.linkeditSegIndex].fileOffset);
    const char*  prevName = "start of LINKEDIT";
    for (const LinkEditContent& blob : blobs) {
        if ( blob.fileOffsetStart < prevEnd ) {
            diag.error("LINKEDIT overlap of %s and %s", prevName, blob.name);
            return false;
        }
        prevEnd  = blob.fileOffsetStart + blob.size;
        prevName = blob.name;
    }
    const LinkEditContent& lastBlob = blobs.back();
    uint32_t linkeditFileEnd = (uint32_t)(leInfo.layout.segments[leInfo.layout.linkeditSegIndex].fileOffset + leInfo.layout.segments[leInfo.layout.linkeditSegIndex].fileSize);
    if (greaterThanAddOrOverflow(lastBlob.fileOffsetStart, lastBlob.size, linkeditFileEnd)) {
        diag.error("LINKEDIT content '%s' extends beyond end of segment", lastBlob.name);
        return false;
    }

    // sort vector by order and warn on non standard order or mis-alignment
    std::sort(blobs.begin(), blobs.end(), [&](const LinkEditContent& a, const LinkEditContent& b) {
        return a.stdOrder < b.stdOrder;
    });
    prevEnd = (uint32_t)(leInfo.layout.segments[leInfo.layout.linkeditSegIndex].fileOffset);
    prevName = "start of LINKEDIT";
    for (const LinkEditContent& blob : blobs) {
        if ( ((blob.fileOffsetStart & (pointerSize-1)) != 0) && (blob.stdOrder != 20) )  // ok for "symbol table strings" to be mis-aligned
            diag.warning("mis-aligned LINKEDIT content '%s'", blob.name);
        if ( blob.fileOffsetStart < prevEnd ) {
            diag.warning("LINKEDIT out of order %s", blob.name);
        }
        prevEnd  = blob.fileOffsetStart;
        prevName = blob.name;
    }

    // Check for invalid symbol table sizes
    if ( leInfo.symTab != nullptr ) {
        if ( leInfo.symTab->nsyms > 0x10000000 ) {
            diag.error("malformed mach-o image: symbol table too large");
            return false;
        }
        if ( leInfo.dynSymTab != nullptr ) {
            // validate indirect symbol table
            if ( leInfo.dynSymTab->nindirectsyms != 0 ) {
                if ( leInfo.dynSymTab->nindirectsyms > 0x10000000 ) {
                    diag.error("malformed mach-o image: indirect symbol table too large");
                    return false;
                }
            }
            if ( (leInfo.dynSymTab->nlocalsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->ilocalsym > leInfo.symTab->nsyms) ) {
                diag.error("malformed mach-o image: indirect symbol table local symbol count exceeds total symbols");
                return false;
            }
            if ( leInfo.dynSymTab->ilocalsym + leInfo.dynSymTab->nlocalsym < leInfo.dynSymTab->ilocalsym  ) {
                diag.error("malformed mach-o image: indirect symbol table local symbol count wraps");
                return false;
            }
            if ( (leInfo.dynSymTab->nextdefsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->iextdefsym > leInfo.symTab->nsyms) ) {
                diag.error("malformed mach-o image: indirect symbol table extern symbol count exceeds total symbols");
                return false;
            }
            if ( leInfo.dynSymTab->iextdefsym + leInfo.dynSymTab->nextdefsym < leInfo.dynSymTab->iextdefsym  ) {
                diag.error("malformed mach-o image: indirect symbol table extern symbol count wraps");
                return false;
            }
            if ( (leInfo.dynSymTab->nundefsym > leInfo.symTab->nsyms) || (leInfo.dynSymTab->iundefsym > leInfo.symTab->nsyms) ) {
                diag.error("malformed mach-o image: indirect symbol table undefined symbol count exceeds total symbols");
                return false;
            }
            if ( leInfo.dynSymTab->iundefsym + leInfo.dynSymTab->nundefsym < leInfo.dynSymTab->iundefsym  ) {
                diag.error("malformed mach-o image: indirect symbol table undefined symbol count wraps");
                return false;
            }
        }
    }

    return true;
}

bool MachOParser::isArch(const mach_header* mh, const std::string& archName)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( archName == info.name ) {
            return ( (mh->cputype == info.cputype) && ((mh->cpusubtype & ~CPU_SUBTYPE_MASK) == info.cpusubtype) );
        }
    }
    return false;
}


std::string MachOParser::archName(uint32_t cputype, uint32_t cpusubtype)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( (cputype == info.cputype) && ((cpusubtype & ~CPU_SUBTYPE_MASK) == info.cpusubtype) ) {
            return info.name;
        }
    }
    return "unknown";
}

uint32_t MachOParser::cpuTypeFromArchName(const std::string& archName)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( archName == info.name ) {
            return info.cputype;
        }
    }
    return 0;
}

uint32_t MachOParser::cpuSubtypeFromArchName(const std::string& archName)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( archName == info.name ) {
            return info.cpusubtype;
        }
    }
    return 0;
}

std::string MachOParser::archName() const
{
    return archName(header()->cputype, header()->cpusubtype);
}

std::string MachOParser::platformName(Platform platform)
{
    switch ( platform ) {
        case Platform::unknown:
            return "unknown";
        case Platform::macOS:
            return "macOS";
        case Platform::iOS:
            return "iOS";
        case Platform::tvOS:
            return "tvOS";
        case Platform::watchOS:
            return "watchOS";
        case Platform::bridgeOS:
            return "bridgeOS";
    }
    return "unknown platform";
}

std::string MachOParser::versionString(uint32_t packedVersion)
{
    char buff[64];
    sprintf(buff, "%d.%d.%d", (packedVersion >> 16), ((packedVersion >> 8) & 0xFF), (packedVersion & 0xFF));
    return buff;
}

#else

bool MachOParser::isMachO(Diagnostics& diag, const void* fileContent, size_t mappedLength)
{
    // sanity check length
    if ( mappedLength < 4096 ) {
        diag.error("file too short");
        return false;
    }

    // must start with mach-o magic value
    const mach_header* mh = (const mach_header*)fileContent;
#if __LP64__
    const uint32_t requiredMagic = MH_MAGIC_64;
#else
    const uint32_t requiredMagic = MH_MAGIC;
#endif
   if ( mh->magic != requiredMagic ) {
        diag.error("not a mach-o file");
        return false;
    }

#if __x86_64__
    const uint32_t requiredCPU = CPU_TYPE_X86_64;
#elif __i386__
    const uint32_t requiredCPU = CPU_TYPE_I386;
#elif __arm__
    const uint32_t requiredCPU = CPU_TYPE_ARM;
#elif __arm64__
    const uint32_t requiredCPU = CPU_TYPE_ARM64;
#else
    #error unsupported architecture
#endif
    if ( mh->cputype != requiredCPU ) {
        diag.error("wrong cpu type");
        return false;
    }

    return true;
}

bool MachOParser::wellFormedMachHeaderAndLoadCommands(const mach_header* mh)
{
    const load_command* startCmds = nullptr;
    if ( mh->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)mh + sizeof(mach_header_64));
    else if ( mh->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)mh + sizeof(mach_header));
    else
        return false;  // not a mach-o file, or wrong endianness

    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + mh->sizeofcmds);
    const load_command* cmd = startCmds;
    for(uint32_t i = 0; i < mh->ncmds; ++i) {
        const load_command* nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( (cmd->cmdsize < 8) || (nextCmd > cmdsEnd) || (nextCmd < startCmds)) {
            return false;
        }
        cmd = nextCmd;
    }
    return true;
}

#endif

Platform MachOParser::currentPlatform()
{
#if TARGET_OS_BRIDGE
    return Platform::bridgeOS;
#elif TARGET_OS_WATCH
    return Platform::watchOS;
#elif TARGET_OS_TV
    return Platform::tvOS;
#elif TARGET_OS_IOS
    return Platform::iOS;
#elif TARGET_OS_MAC
    return Platform::macOS;
#else
    #error unknown platform
#endif
}


bool MachOParser::valid(Diagnostics& diag)
{
#if DYLD_IN_PROCESS
    // only images loaded by dyld to be parsed
    const mach_header* inImage = dyld3::dyld_image_header_containing_address(header());
    if ( inImage != header() ) {
        diag.error("only dyld loaded images can be parsed by MachOParser");
        return false;
    }
#else

#endif
    return true;
}


void MachOParser::forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( header()->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)header() + sizeof(mach_header_64));
    else if ( header()->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)header() + sizeof(mach_header));
    else {
        diag.error("file does not start with MH_MAGIC[_64]");
        return;  // not a mach-o file, or wrong endianness
    }
    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + header()->sizeofcmds);
    const load_command* cmd = startCmds;
    for(uint32_t i = 0; i < header()->ncmds; ++i) {
        const load_command* nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( cmd->cmdsize < 8 ) {
            diag.error("malformed load command #%d, size too small %d", i, cmd->cmdsize);
            return;
        }
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%d, size too large 0x%X", i, cmd->cmdsize);
            return;
        }
        callback(cmd, stop);
        if ( stop )
            return;
        cmd = nextCmd;
    }
}

UUID MachOParser::uuid() const
{
    uuid_t uuid;
    getUuid(uuid);
    return uuid;
}

bool MachOParser::getUuid(uuid_t uuid) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UUID ) {
            const uuid_command* uc = (const uuid_command*)cmd;
            memcpy(uuid, uc->uuid, sizeof(uuid_t));
            found = true;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    if ( !found )
        bzero(uuid, sizeof(uuid_t));
    return found;
}

uint64_t MachOParser::preferredLoadAddress() const
{
    __block uint64_t result = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            result = vmAddr;
            stop = true;
        }
    });
    return result;
}

bool MachOParser::getPlatformAndVersion(Platform* platform, uint32_t* minOS, uint32_t* sdk) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const version_min_command* versCmd;
        switch ( cmd->cmd ) {
            case LC_VERSION_MIN_IPHONEOS:
                versCmd       = (version_min_command*)cmd;
                *platform     = Platform::iOS;
                *minOS        = versCmd->version;
                *sdk          = versCmd->sdk;
                found = true;
                stop = true;
                break;
           case LC_VERSION_MIN_MACOSX:
                 versCmd       = (version_min_command*)cmd;
                *platform     = Platform::macOS;
                *minOS        = versCmd->version;
                *sdk          = versCmd->sdk;
                found = true;
                stop = true;
                break;
           case LC_VERSION_MIN_TVOS:
                 versCmd       = (version_min_command*)cmd;
                *platform     = Platform::tvOS;
                *minOS        = versCmd->version;
                *sdk          = versCmd->sdk;
                found = true;
                stop = true;
                break;
           case LC_VERSION_MIN_WATCHOS:
                versCmd       = (version_min_command*)cmd;
                *platform     = Platform::watchOS;
                *minOS        = versCmd->version;
                *sdk          = versCmd->sdk;
                found = true;
                stop = true;
                break;
            case LC_BUILD_VERSION: {
                const build_version_command* buildCmd = (build_version_command *)cmd;
                *minOS        = buildCmd->minos;
                *sdk          = buildCmd->sdk;

                switch(buildCmd->platform) {
                        /* Known values for the platform field above. */
                    case PLATFORM_MACOS:
                        *platform = Platform::macOS;
                        break;
                    case PLATFORM_IOS:
                        *platform = Platform::iOS;
                        break;
                    case PLATFORM_TVOS:
                        *platform = Platform::tvOS;
                        break;
                    case PLATFORM_WATCHOS:
                        *platform = Platform::watchOS;
                        break;
                    case PLATFORM_BRIDGEOS:
                        *platform = Platform::bridgeOS;
                        break;
                }
                found = true;
                stop = true;
            } break;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return found;
}


bool MachOParser::isSimulatorBinary() const
{
    Platform platform;
    uint32_t minOS;
    uint32_t sdk;
    switch ( header()->cputype ) {
        case CPU_TYPE_I386:
        case CPU_TYPE_X86_64:
            if ( getPlatformAndVersion(&platform, &minOS, &sdk) ) {
                return (platform != Platform::macOS);
            }
            break;
    }
    return false;
}


bool MachOParser::getDylibInstallName(const char** installName, uint32_t* compatVersion, uint32_t* currentVersion) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ID_DYLIB ) {
            const dylib_command*  dylibCmd = (dylib_command*)cmd;
            *compatVersion  = dylibCmd->dylib.compatibility_version;
            *currentVersion = dylibCmd->dylib.current_version;
            *installName    = (char*)dylibCmd + dylibCmd->dylib.name.offset;
            found = true;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return found;
}

const char* MachOParser::installName() const
{
    assert(header()->filetype == MH_DYLIB);
    const char* result;
    uint32_t    ignoreVersion;
    assert(getDylibInstallName(&result, &ignoreVersion, &ignoreVersion));
    return result;
}


uint32_t MachOParser::dependentDylibCount() const
{
    __block uint32_t count = 0;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        ++count;
    });
    return count;
}

const char* MachOParser::dependentDylibLoadPath(uint32_t depIndex) const
{
    __block const char* foundLoadPath = nullptr;
    __block uint32_t curDepIndex = 0;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( curDepIndex == depIndex ) {
            foundLoadPath = loadPath;
            stop = true;
        }
        ++curDepIndex;
    });
    return foundLoadPath;
}


void MachOParser::forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                assert(dylibCmd->dylib.name.offset < cmd->cmdsize);
                const char* loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                callback(loadPath, (cmd->cmd == LC_LOAD_WEAK_DYLIB), (cmd->cmd == LC_REEXPORT_DYLIB), (cmd->cmd == LC_LOAD_UPWARD_DYLIB),
                                    dylibCmd->dylib.compatibility_version, dylibCmd->dylib.current_version, stop);
            }
            break;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOParser::forEachRPath(void (^callback)(const char* rPath, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( cmd->cmd == LC_RPATH ) {
            const char* rpath = (char*)cmd + ((struct rpath_command*)cmd)->path.offset;
            callback(rpath, stop);
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

/*
   struct LayoutInfo {
#if DYLD_IN_PROCESS
        uintptr_t    slide;
        uintptr_t    textUnslidVMAddr;
        uintptr_t    linkeditUnslidVMAddr;
        uint32_t     linkeditFileOffset;
#else
        uint32_t     segmentCount;
        uint32_t     linkeditSegIndex;
        struct {
            uint64_t    mappingOffset;
            uint64_t    fileOffset;
            uint64_t    segUnslidAddress;
            uint64_t    segSize;
        }            segments[16];
#endif
    };
*/

#if !DYLD_IN_PROCESS
const uint8_t* MachOParser::getContentForVMAddr(const LayoutInfo& info, uint64_t addr) const
{
    for (uint32_t i=0; i < info.segmentCount; ++i) {
        if ( (addr >= info.segments[i].segUnslidAddress) && (addr < (info.segments[i].segUnslidAddress+info.segments[i].segSize)) )
            return (uint8_t*)header() + info.segments[i].mappingOffset + (addr - info.segments[i].segUnslidAddress);
    }
    // value is outside this image.  could be pointer into another image
    if ( inDyldCache() ) {
        return (uint8_t*)header() + info.segments[0].mappingOffset + (addr - info.segments[0].segUnslidAddress);
    }
    assert(0 && "address not found in segment");
    return nullptr;
}
#endif

const uint8_t* MachOParser::getLinkEditContent(const LayoutInfo& info, uint32_t fileOffset) const
{
#if DYLD_IN_PROCESS
    uint32_t offsetInLinkedit   = fileOffset - info.linkeditFileOffset;
    uintptr_t linkeditStartAddr = info.linkeditUnslidVMAddr + info.slide;
    return (uint8_t*)(linkeditStartAddr + offsetInLinkedit);
#else
    uint32_t offsetInLinkedit    = fileOffset - (uint32_t)(info.segments[info.linkeditSegIndex].fileOffset);
    const uint8_t* linkeditStart = (uint8_t*)header() + info.segments[info.linkeditSegIndex].mappingOffset;
    return linkeditStart + offsetInLinkedit;
#endif
}


void MachOParser::getLayoutInfo(LayoutInfo& result) const
{
#if DYLD_IN_PROCESS
    // image loaded by dyld, just record the addr and file offset of TEXT and LINKEDIT segments
    result.slide = getSlide();
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            result.textUnslidVMAddr = (uintptr_t)vmAddr;
        }
        else if ( strcmp(segName, "__LINKEDIT") == 0 ) {
            result.linkeditUnslidVMAddr = (uintptr_t)vmAddr;
            result.linkeditFileOffset   = fileOffset;
        }
    });
#else
    bool inCache = inDyldCache();
    bool intel32 = (header()->cputype == CPU_TYPE_I386);
    result.segmentCount = 0;
    result.linkeditSegIndex = 0xFFFFFFFF;
    __block uint64_t textSegAddr = 0;
    __block uint64_t textSegFileOffset = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        auto& segInfo = result.segments[result.segmentCount];
        if ( strcmp(segName, "__TEXT") == 0 ) {
            textSegAddr       = vmAddr;
            textSegFileOffset = fileOffset;
        }
        __block bool textRelocsAllowed = false;
        if ( intel32 ) {
            forEachSection(^(const char* curSegName, uint32_t segIndex, uint64_t segVMAddr, const char* sectionName, uint32_t sectFlags,
                             uint64_t sectAddr, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& sectStop) {
                if ( strcmp(curSegName, segName) == 0 ) {
                    if ( sectFlags & (S_ATTR_EXT_RELOC|S_ATTR_LOC_RELOC) ) {
                        textRelocsAllowed = true;
                        sectStop = true;
                    }
                }
            });
        }
        if ( inCache ) {
            if ( inRawCache() ) {
                // whole cache file mapped somewhere (padding not expanded)
                // vmaddrs are useless. only file offset make sense
                segInfo.mappingOffset = fileOffset - textSegFileOffset;
            }
            else {
                // cache file was loaded by dyld into shared region
                // vmaddrs of segments are correct except for ASLR slide
                segInfo.mappingOffset = vmAddr - textSegAddr;
           }
        }
        else {
            // individual mach-o file mapped in one region, so mappingOffset == fileOffset
            segInfo.mappingOffset    = fileOffset;
        }
        segInfo.fileOffset        = fileOffset;
        segInfo.fileSize          = fileSize;
        segInfo.segUnslidAddress  = vmAddr;
        segInfo.segSize           = vmSize;
        segInfo.writable          = ((protections & VM_PROT_WRITE)   == VM_PROT_WRITE);
        segInfo.executable        = ((protections & VM_PROT_EXECUTE) == VM_PROT_EXECUTE);
        segInfo.textRelocsAllowed = textRelocsAllowed;
        if ( strcmp(segName, "__LINKEDIT") == 0 ) {
            result.linkeditSegIndex = result.segmentCount;
        }
        ++result.segmentCount;
        if ( result.segmentCount > 127 )
            stop = true;
    });
#endif
}


void MachOParser::forEachSection(void (^callback)(const char* segName, const char* sectionName, uint32_t flags,
                                                  const void* content, size_t size, bool illegalSectionSize, bool& stop)) const
{
    forEachSection(^(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr,
                     const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop) {
        callback(segName, sectionName, flags, content, (size_t)size, illegalSectionSize, stop);
    });
}

void MachOParser::forEachSection(void (^callback)(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr,
                                                  const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2,
                                                  bool illegalSectionSize, bool& stop)) const
{
    Diagnostics diag;
    //fprintf(stderr, "forEachSection() mh=%p\n", header());
    LayoutInfo layout;
    getLayoutInfo(layout);
    forEachSection(^(const char* segName, uint32_t segIndex, uint64_t segVMAddr, const char* sectionName, uint32_t sectFlags,
                      uint64_t sectAddr, uint64_t sectSize, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop) {
    #if DYLD_IN_PROCESS
        const uint8_t* segContentStart = (uint8_t*)(segVMAddr + layout.slide);
    #else
        const uint8_t* segContentStart = (uint8_t*)header() + layout.segments[segIndex].mappingOffset;
    #endif
        const void* contentAddr = segContentStart + (sectAddr - segVMAddr);
        callback(segName, sectionName, sectFlags, sectAddr, contentAddr, sectSize, alignP2, reserved1, reserved2, illegalSectionSize, stop);
    });

}

// this iterator just walks the segment/section array.  It does interpret addresses
void MachOParser::forEachSection(void (^callback)(const char* segName, uint32_t segIndex, uint64_t segVMAddr, const char* sectionName, uint32_t sectFlags,
                                 uint64_t sectAddr, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop)) const
{
    Diagnostics diag;
    //fprintf(stderr, "forEachSection() mh=%p\n", header());
    __block uint32_t segIndex = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            const section_64* const sectionsStart = (section_64*)((char*)seg + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section_64* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                char sectNameCopy[20];
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool illegalSectionSize = (sect->addr < seg->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, seg->vmaddr + seg->filesize);
                callback(seg->segname, segIndex, seg->vmaddr, sectName, sect->flags, sect->addr, sect->size, sect->align, sect->reserved1, sect->reserved2, illegalSectionSize, stop);
            }
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            const section* const sectionsStart = (section*)((char*)seg + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                char sectNameCopy[20];
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool illegalSectionSize = (sect->addr < seg->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, seg->vmaddr + seg->filesize);
                callback(seg->segname, segIndex, seg->vmaddr, sectName, sect->flags, sect->addr, sect->size, sect->align, sect->reserved1, sect->reserved2, illegalSectionSize, stop);
            }
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOParser::forEachGlobalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t globalsStartIndex = 0;
        uint32_t globalsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            globalsStartIndex = leInfo.dynSymTab->iextdefsym;
            globalsCount      = leInfo.dynSymTab->nextdefsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)(getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        bool                   stop             = false;
        for (uint32_t i=0; (i < globalsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[globalsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( (sym.n_type & N_EXT) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}

void MachOParser::forEachLocalSymbol(Diagnostics& diag, void (^callback)(const char* symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const bool is64Bit = is64();
    if ( leInfo.symTab != nullptr ) {
        uint32_t localsStartIndex = 0;
        uint32_t localsCount      = leInfo.symTab->nsyms;
        if ( leInfo.dynSymTab != nullptr ) {
            localsStartIndex = leInfo.dynSymTab->ilocalsym;
            localsCount      = leInfo.dynSymTab->nlocalsym;
        }
        uint32_t               maxStringOffset  = leInfo.symTab->strsize;
        const char*            stringPool       =             (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        const struct nlist*    symbols          = (struct nlist*)   (getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        const struct nlist_64* symbols64        = (struct nlist_64*)(getLinkEditContent(leInfo.layout, leInfo.symTab->symoff));
        bool                   stop             = false;
        for (uint32_t i=0; (i < localsCount) && !stop; ++i) {
            if ( is64Bit ) {
                const struct nlist_64& sym = symbols64[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
            else {
                const struct nlist& sym = symbols[localsStartIndex+i];
                if ( sym.n_un.n_strx > maxStringOffset )
                    continue;
                if ( ((sym.n_type & N_EXT) == 0) && ((sym.n_type & N_TYPE) == N_SECT) && ((sym.n_type & N_STAB) == 0) )
                    callback(&stringPool[sym.n_un.n_strx], sym.n_value, sym.n_type, sym.n_sect, sym.n_desc, stop);
            }
        }
    }
}


bool MachOParser::findExportedSymbol(Diagnostics& diag, const char* symbolName, void* extra, FoundSymbol& foundInfo, DependentFinder findDependent) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;
    if ( leInfo.dyldInfo != nullptr ) {
        const uint8_t* trieStart    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->export_off);
        const uint8_t* trieEnd      = trieStart + leInfo.dyldInfo->export_size;
        const uint8_t* node         = trieWalk(diag, trieStart, trieEnd, symbolName);
        if ( node == nullptr ) {
            // symbol not exported from this image. Seach any re-exported dylibs
            __block unsigned        depIndex = 0;
            __block bool            foundInReExportedDylib = false;
            forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
                if ( isReExport && findDependent ) {
                    const mach_header*  depMH;
                    void*               depExtra;
                    if ( findDependent(depIndex, loadPath, extra, &depMH, &depExtra) ) {
                        bool depInRawCache = inRawCache() && (depMH->flags & 0x80000000);
                        MachOParser dep(depMH, depInRawCache);
                        if ( dep.findExportedSymbol(diag, symbolName, depExtra, foundInfo, findDependent) ) {
                            stop = true;
                            foundInReExportedDylib = true;
                        }
                    }
                    else {
                        fprintf(stderr, "could not find re-exported dylib %s\n", loadPath);
                    }
                }
                ++depIndex;
            });
            return foundInReExportedDylib;
        }
        const uint8_t* p = node;
        const uint64_t flags = read_uleb128(diag, p, trieEnd);
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            if ( !findDependent )
                return false;
            // re-export from another dylib, lookup there
            const uint64_t ordinal = read_uleb128(diag, p, trieEnd);
            const char* importedName = (char*)p;
            if ( importedName[0] == '\0' )
                importedName = symbolName;
            assert(ordinal >= 1);
            if (ordinal > dependentDylibCount()) {
                diag.error("ordinal %lld out of range for %s", ordinal, symbolName);
                return false;
            }
            uint32_t depIndex = (uint32_t)(ordinal-1);
            const mach_header*  depMH;
            void*               depExtra;
            if ( findDependent(depIndex, dependentDylibLoadPath(depIndex), extra, &depMH, &depExtra) ) {
                bool depInRawCache = inRawCache() && (depMH->flags & 0x80000000);
                MachOParser depParser(depMH, depInRawCache);
                return depParser.findExportedSymbol(diag, importedName, depExtra, foundInfo, findDependent);
            }
            else {
                diag.error("dependent dylib %lld not found for re-exported symbol %s", ordinal, symbolName);
                return false;
            }
        }
        foundInfo.kind               = FoundSymbol::Kind::headerOffset;
        foundInfo.isThreadLocal      = false;
        foundInfo.foundInDylib       = header();
        foundInfo.foundExtra         = extra;
        foundInfo.value              = read_uleb128(diag, p, trieEnd);
        foundInfo.resolverFuncOffset = 0;
        foundInfo.foundSymbolName    = symbolName;
        if ( diag.hasError() )
            return false;
        switch ( flags & EXPORT_SYMBOL_FLAGS_KIND_MASK ) {
            case EXPORT_SYMBOL_FLAGS_KIND_REGULAR:
                if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                    foundInfo.resolverFuncOffset = (uint32_t)read_uleb128(diag, p, trieEnd);
                }
                else {
                    foundInfo.kind = FoundSymbol::Kind::headerOffset;
                }
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL:
                foundInfo.isThreadLocal = true;
                break;
            case EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE:
                foundInfo.kind = FoundSymbol::Kind::absolute;
                break;
            default:
                diag.error("unsupported exported symbol kind. flags=%llu at node offset=0x%0lX", flags, (long)(node-trieStart));
                return false;
        }
        return true;
    }
    else {
        // this is an old binary (before macOS 10.6), scan the symbol table
        foundInfo.foundInDylib = nullptr;
        uint64_t baseAddress = preferredLoadAddress();
        forEachGlobalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
            if ( strcmp(aSymbolName, symbolName) == 0 ) {
                foundInfo.kind               = FoundSymbol::Kind::headerOffset;
                foundInfo.isThreadLocal      = false;
                foundInfo.foundInDylib       = header();
                foundInfo.foundExtra         = extra;
                foundInfo.value              = n_value - baseAddress;
                foundInfo.resolverFuncOffset = 0;
                foundInfo.foundSymbolName    = symbolName;
                stop = true;
            }
        });
        return (foundInfo.foundInDylib != nullptr);
    }
}


void MachOParser::getLinkEditLoadCommands(Diagnostics& diag, LinkEditInfo& result) const
{
    result.dyldInfo       = nullptr;
    result.symTab         = nullptr;
    result.dynSymTab      = nullptr;
    result.splitSegInfo   = nullptr;
    result.functionStarts = nullptr;
    result.dataInCode     = nullptr;
    result.codeSig        = nullptr;
    __block bool hasUUID    = false;
    __block bool hasVersion = false;
    __block bool hasEncrypt = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                if ( cmd->cmdsize != sizeof(dyld_info_command) )
                    diag.error("LC_DYLD_INFO load command size wrong");
                else if ( result.dyldInfo != nullptr )
                    diag.error("multiple LC_DYLD_INFO load commands");
                result.dyldInfo = (dyld_info_command*)cmd;
                break;
            case LC_SYMTAB:
                if ( cmd->cmdsize != sizeof(symtab_command) )
                    diag.error("LC_SYMTAB load command size wrong");
                else if ( result.symTab != nullptr )
                    diag.error("multiple LC_SYMTAB load commands");
                result.symTab = (symtab_command*)cmd;
                break;
            case LC_DYSYMTAB:
                if ( cmd->cmdsize != sizeof(dysymtab_command) )
                    diag.error("LC_DYSYMTAB load command size wrong");
                else if ( result.dynSymTab != nullptr )
                    diag.error("multiple LC_DYSYMTAB load commands");
                result.dynSymTab = (dysymtab_command*)cmd;
                break;
            case LC_SEGMENT_SPLIT_INFO:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_SEGMENT_SPLIT_INFO load command size wrong");
                else if ( result.splitSegInfo != nullptr )
                    diag.error("multiple LC_SEGMENT_SPLIT_INFO load commands");
                result.splitSegInfo = (linkedit_data_command*)cmd;
                break;
            case LC_FUNCTION_STARTS:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_FUNCTION_STARTS load command size wrong");
                else if ( result.functionStarts != nullptr )
                    diag.error("multiple LC_FUNCTION_STARTS load commands");
                result.functionStarts = (linkedit_data_command*)cmd;
                break;
            case LC_DATA_IN_CODE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_DATA_IN_CODE load command size wrong");
                else if ( result.dataInCode != nullptr )
                    diag.error("multiple LC_DATA_IN_CODE load commands");
                result.dataInCode = (linkedit_data_command*)cmd;
                break;
            case LC_CODE_SIGNATURE:
                if ( cmd->cmdsize != sizeof(linkedit_data_command) )
                    diag.error("LC_CODE_SIGNATURE load command size wrong");
                else if ( result.codeSig != nullptr )
                     diag.error("multiple LC_CODE_SIGNATURE load commands");
                result.codeSig = (linkedit_data_command*)cmd;
                break;
            case LC_UUID:
                if ( cmd->cmdsize != sizeof(uuid_command) )
                    diag.error("LC_UUID load command size wrong");
                else if ( hasUUID )
                     diag.error("multiple LC_UUID load commands");
                hasUUID = true;
                break;
            case LC_VERSION_MIN_IPHONEOS:
            case LC_VERSION_MIN_MACOSX:
            case LC_VERSION_MIN_TVOS:
            case LC_VERSION_MIN_WATCHOS:
                if ( cmd->cmdsize != sizeof(version_min_command) )
                    diag.error("LC_VERSION_* load command size wrong");
                 else if ( hasVersion )
                     diag.error("multiple LC_VERSION_MIN_* load commands");
                hasVersion = true;
                break;
            case LC_BUILD_VERSION:
                if ( cmd->cmdsize != (sizeof(build_version_command) + ((build_version_command*)cmd)->ntools * sizeof(build_tool_version)) )
                    diag.error("LC_BUILD_VERSION load command size wrong");
                else if ( hasVersion )
                     diag.error("multiple LC_BUILD_VERSION load commands");
                hasVersion = true;
                break;
            case LC_ENCRYPTION_INFO:
                if ( cmd->cmdsize != sizeof(encryption_info_command) )
                    diag.error("LC_ENCRYPTION_INFO load command size wrong");
                else if ( hasEncrypt )
                     diag.error("multiple LC_ENCRYPTION_INFO load commands");
                else if ( is64() )
                      diag.error("LC_ENCRYPTION_INFO found in 64-bit mach-o");
                hasEncrypt = true;
                break;
            case LC_ENCRYPTION_INFO_64:
                if ( cmd->cmdsize != sizeof(encryption_info_command_64) )
                    diag.error("LC_ENCRYPTION_INFO_64 load command size wrong");
                else if ( hasEncrypt )
                     diag.error("multiple LC_ENCRYPTION_INFO_64 load commands");
                else if ( !is64() )
                      diag.error("LC_ENCRYPTION_INFO_64 found in 32-bit mach-o");
                hasEncrypt = true;
                break;
        }
    });
    if ( diag.noError() && (result.dynSymTab != nullptr) && (result.symTab == nullptr) )
        diag.error("LC_DYSYMTAB but no LC_SYMTAB load command");

}

void MachOParser::getLinkEditPointers(Diagnostics& diag, LinkEditInfo& result) const
{
    getLinkEditLoadCommands(diag, result);
    if ( diag.noError() )
        getLayoutInfo(result.layout);
}

void MachOParser::forEachSegment(void (^callback)(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            callback(seg->segname, (uint32_t)seg->fileoff, (uint32_t)seg->filesize, seg->vmaddr, seg->vmsize, seg->initprot, stop);
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            callback(seg->segname, seg->fileoff, seg->filesize, seg->vmaddr, seg->vmsize, seg->initprot, stop);
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

const uint8_t* MachOParser::trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol)
{
    uint32_t visitedNodeOffsets[128];
    int visitedNodeOffsetCount = 0;
    visitedNodeOffsets[visitedNodeOffsetCount++] = 0;
    const uint8_t* p = start;
    while ( p < end ) {
        uint64_t terminalSize = *p++;
        if ( terminalSize > 127 ) {
            // except for re-export-with-rename, all terminal sizes fit in one byte
            --p;
            terminalSize = read_uleb128(diag, p, end);
            if ( diag.hasError() )
                return nullptr;
        }
        if ( (*symbol == '\0') && (terminalSize != 0) ) {
            return p;
        }
        const uint8_t* children = p + terminalSize;
        if ( children > end ) {
            diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
            return nullptr;
        }
        uint8_t childrenRemaining = *children++;
        p = children;
        uint64_t nodeOffset = 0;
        for (; childrenRemaining > 0; --childrenRemaining) {
            const char* ss = symbol;
            bool wrongEdge = false;
            // scan whole edge to get to next edge
            // if edge is longer than target symbol name, don't read past end of symbol name
            char c = *p;
            while ( c != '\0' ) {
                if ( !wrongEdge ) {
                    if ( c != *ss )
                        wrongEdge = true;
                    ++ss;
                }
                ++p;
                c = *p;
            }
            if ( wrongEdge ) {
                // advance to next child
                ++p; // skip over zero terminator
                // skip over uleb128 until last byte is found
                while ( (*p & 0x80) != 0 )
                    ++p;
                ++p; // skip over last byte of uleb128
                if ( p > end ) {
                    diag.error("malformed trie node, child node extends past end of trie\n");
                    return nullptr;
                }
            }
            else {
                 // the symbol so far matches this edge (child)
                // so advance to the child's node
                ++p;
                nodeOffset = read_uleb128(diag, p, end);
                if ( diag.hasError() )
                    return nullptr;
                if ( (nodeOffset == 0) || ( &start[nodeOffset] > end) ) {
                    diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
                    return nullptr;
                }
                symbol = ss;
                break;
            }
        }
        if ( nodeOffset != 0 ) {
            if ( nodeOffset > (end-start) ) {
                diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
               return nullptr;
            }
            for (int i=0; i < visitedNodeOffsetCount; ++i) {
                if ( visitedNodeOffsets[i] == nodeOffset ) {
                    diag.error("malformed trie child, cycle to nodeOffset=0x%llX\n", nodeOffset);
                    return nullptr;
                }
            }
            visitedNodeOffsets[visitedNodeOffsetCount++] = (uint32_t)nodeOffset;
            if ( visitedNodeOffsetCount >= 128 ) {
                diag.error("malformed trie too deep\n");
                return nullptr;
            }
            p = &start[nodeOffset];
        }
        else
            p = end;
    }
    return nullptr;
}


uint64_t MachOParser::read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    uint64_t result = 0;
    int         bit = 0;
    do {
        if ( p == end ) {
            diag.error("malformed uleb128");
            break;
        }
        uint64_t slice = *p & 0x7f;

        if ( bit > 63 ) {
            diag.error("uleb128 too big for uint64");
            break;
        }
        else {
            result |= (slice << bit);
            bit += 7;
        }
    }
    while (*p++ & 0x80);
    return result;
}


int64_t MachOParser::read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
{
    int64_t  result = 0;
    int      bit = 0;
    uint8_t  byte = 0;
    do {
        if ( p == end ) {
            diag.error("malformed sleb128");
            break;
        }
        byte = *p++;
        result |= (((int64_t)(byte & 0x7f)) << bit);
        bit += 7;
    } while (byte & 0x80);
    // sign extend negative numbers
    if ( (byte & 0x40) != 0 )
        result |= (-1LL) << bit;
    return result;
}

bool MachOParser::is64() const
{
#if DYLD_IN_PROCESS
    return (sizeof(void*) == 8);
#else
    return (header()->magic == MH_MAGIC_64);
#endif
}




bool MachOParser::findClosestSymbol(uint64_t targetUnslidAddress, const char** symbolName, uint64_t* symbolUnslidAddr) const
{
    Diagnostics diag;
    __block uint64_t    closestNValueSoFar = 0;
    __block const char* closestNameSoFar   = nullptr;
    forEachGlobalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
        if ( n_value <= targetUnslidAddress ) {
            if ( (closestNameSoFar == nullptr) || (closestNValueSoFar < n_value) ) {
                closestNValueSoFar = n_value;
                closestNameSoFar   = aSymbolName;
            }
        }
    });
    forEachLocalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool& stop) {
        if ( n_value <= targetUnslidAddress ) {
            if ( (closestNameSoFar == nullptr) || (closestNValueSoFar < n_value) ) {
                closestNValueSoFar = n_value;
                closestNameSoFar   = aSymbolName;
            }
        }
    });
    if ( closestNameSoFar == nullptr ) {
        return false;
    }

    *symbolName       = closestNameSoFar;
    *symbolUnslidAddr = closestNValueSoFar;
    return true;
}


#if DYLD_IN_PROCESS

bool MachOParser::findClosestSymbol(const void* addr, const char** symbolName, const void** symbolAddress) const
{
    uint64_t slide = getSlide();
    uint64_t symbolUnslidAddr;
    if ( findClosestSymbol((uint64_t)addr - slide, symbolName, &symbolUnslidAddr) ) {
        *symbolAddress = (const void*)(long)(symbolUnslidAddr + slide);
        return true;
    }
    return false;
}

intptr_t MachOParser::getSlide() const
{
    Diagnostics diag;
    __block intptr_t slide = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
#if __LP64__
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = ((uint64_t)header()) - seg->vmaddr;
                stop = true;
            }
        }
#else
        if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            if ( strcmp(seg->segname, "__TEXT") == 0 ) {
                slide = ((uint32_t)header()) - seg->vmaddr;
                stop = true;
            }
        }
#endif
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return slide;
}

// this is only used by dlsym() at runtime.  All other binding is done when the closure is built.
bool MachOParser::hasExportedSymbol(const char* symbolName, DependentFinder finder, void** result) const
{
    typedef void* (*ResolverFunc)(void);
    ResolverFunc resolver;
    Diagnostics diag;
    FoundSymbol foundInfo;
    if ( findExportedSymbol(diag, symbolName, (void*)header(), foundInfo, finder) ) {
        switch ( foundInfo.kind ) {
            case FoundSymbol::Kind::headerOffset:
                *result = (uint8_t*)foundInfo.foundInDylib + foundInfo.value;
                break;
            case FoundSymbol::Kind::absolute:
                *result = (void*)(long)foundInfo.value;
                break;
            case FoundSymbol::Kind::resolverOffset:
                // foundInfo.value contains "stub".
                // in dlsym() we want to call resolver function to get final function address
                resolver = (ResolverFunc)((uint8_t*)foundInfo.foundInDylib + foundInfo.resolverFuncOffset);
                *result = (*resolver)();
                break;
        }
        return true;
    }
    return false;
}

const char* MachOParser::segmentName(uint32_t targetSegIndex) const
{
    __block const char* result = nullptr;
    __block uint32_t segIndex  = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( segIndex == targetSegIndex ) {
            result = segName;
            stop = true;
        }
        ++segIndex;
    });
    return result;
}

#else 


bool MachOParser::uses16KPages() const
{
    return (header()->cputype == CPU_TYPE_ARM64);
}


bool MachOParser::isEncrypted() const
{
    __block bool result = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            if ( segCmd->flags & SG_PROTECTED_VERSION_1 ) {
                result = true;
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            if ( segCmd->flags & SG_PROTECTED_VERSION_1 ) {
                result = true;
                stop = true;
            }
        }
        else if ( (cmd->cmd == LC_ENCRYPTION_INFO) || (cmd->cmd == LC_ENCRYPTION_INFO_64) ) {
            const encryption_info_command* encCmd = (encryption_info_command*)cmd;
            if ( encCmd->cryptid != 0 ) {
                result = true;
                stop = true;
            }
        }
    });
    return result;
}

bool MachOParser::hasWeakDefs() const
{
    return (header()->flags & (MH_WEAK_DEFINES|MH_BINDS_TO_WEAK));
}

bool MachOParser::hasObjC() const
{
    __block bool result = false;
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, const void* content, size_t size, bool illegalSectionSize, bool& stop) {
        if ( (strncmp(sectionName, "__objc_imageinfo", 16) == 0) && (strncmp(segmentName, "__DATA", 6) == 0) ) {
            result = true;
            stop = true;
        }
        if ( (header()->cputype == CPU_TYPE_I386) && (strcmp(sectionName, "__image_info") == 0) && (strcmp(segmentName, "__OBJC") == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOParser::hasPlusLoadMethod(Diagnostics& diag) const
{
#if 1
    __block bool result = false;
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop) {
        if ( ( (flags & SECTION_TYPE) == S_CSTRING_LITERALS ) ) {
            if (illegalSectionSize) {
                diag.error("cstring section %s/%s extends beyond the end of the segment", segmentName, sectionName);
                return;
            }
            const char* s   = (char*)content;
            const char* end = s + size;
            while ( s < end ) {
                if ( strcmp(s, "load") == 0 ) {
                    result = true;
                    stop = true;
                    return;
                }
                while (*s != '\0' )
                    ++s;
                ++s;
            }
        }
    });
    return result;
#else
    LayoutInfo layout;
    getLayoutInfo(layout);

    __block bool        hasSwift            = false;
    __block const void* classList           = nullptr;
    __block size_t      classListSize       = 0;
    __block const void* objcData            = nullptr;
    __block size_t      objcDataSize        = 0;
    __block const void* objcConstData       = nullptr;
    __block size_t      objcConstDataSize   = 0;
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool& stop) {
        if ( (strcmp(sectionName, "__objc_classlist") == 0) && (strncmp(segmentName, "__DATA", 6) == 0) ) {
            classList     = content;
            classListSize = size;
        }
        if ( (strcmp(sectionName, "__objc_imageinfo") == 0) && (strncmp(segmentName, "__DATA", 6) == 0) ) {
            const uint32_t* info = (uint32_t*)content;
            uint8_t swiftVersion = (info[1] >> 8) & 0xFF;
            if ( swiftVersion != 0 )
                hasSwift = true;
        }
    });
    if ( classList == nullptr )
        return false;
    // FIXME: might be objc and swift intermixed
    if ( hasSwift )
        return true;
    const bool      p64            = is64();
    const uint32_t  pointerSize    = (p64 ? 8 : 4);
    const uint64_t* classArray64   = (uint64_t*)classList;
    const uint32_t* classArray32   = (uint32_t*)classList;
    const uint32_t  classListCount = (uint32_t)(classListSize/pointerSize);
    for (uint32_t i=0; i < classListCount; ++i) {
        if ( p64 ) {
            uint64_t classObjAddr = classArray64[i];
            const uint64_t* classObjContent = (uint64_t*)getContentForVMAddr(layout, classObjAddr);
            uint64_t classROAddr = classObjContent[4];
            uint64_t metaClassObjAddr = classObjContent[0];
            const uint64_t* metaClassObjContent = (uint64_t*)getContentForVMAddr(layout, metaClassObjAddr);
            uint64_t metaClassROObjAddr = metaClassObjContent[4];
            const uint64_t* metaClassROObjContent = (uint64_t*)getContentForVMAddr(layout, metaClassROObjAddr);
            uint64_t metaClassMethodListAddr = metaClassROObjContent[4];
            if ( metaClassMethodListAddr != 0 ) {
                const uint64_t* metaClassMethodListContent = (uint64_t*)getContentForVMAddr(layout, metaClassMethodListAddr);
                const uint32_t methodListCount = ((uint32_t*)metaClassMethodListContent)[1];
                for (uint32_t m=0; m < methodListCount; ++m) {
                    uint64_t methodNameAddr = metaClassMethodListContent[m*3+1];
                    const char* methodNameContent = (char*)getContentForVMAddr(layout, methodNameAddr);
                    if ( strcmp(methodNameContent, "load") == 0 ) {
                        return true;
                    }
                }
            }
        }
        else {

        }
    }

    return false;
#endif
}

bool MachOParser::getCDHash(uint8_t cdHash[20])
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return false;

    return cdHashOfCodeSignature(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff), leInfo.codeSig->datasize, cdHash);
 }

bool MachOParser::usesLibraryValidation() const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return false;

    const CS_CodeDirectory* cd = (const CS_CodeDirectory*)findCodeDirectoryBlob(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff), leInfo.codeSig->datasize);
    if ( cd == nullptr )
        return false;

    // check for CS_REQUIRE_LV in CS_CodeDirectory.flags
    return (htonl(cd->flags) & CS_REQUIRE_LV);
 }


bool MachOParser::isRestricted() const
{
    __block bool result = false;
    forEachSection(^(const char* segName, const char* sectionName, uint32_t flags, const void* content, size_t size, bool illegalSectionSize, bool& stop) {
        if ( (strcmp(segName, "__RESTRICT") == 0) && (strcmp(sectionName, "__restrict") == 0) ) {
            result = true;
            stop = true;
        }

    });
    return result;
}

bool MachOParser::hasCodeSignature(uint32_t& fileOffset, uint32_t& size)
{
    fileOffset = 0;
    size = 0;

	// <rdar://problem/13622786> ignore code signatures in macOS binaries built with pre-10.9 tools
    Platform platform;
    uint32_t minOS;
    uint32_t sdk;
    if ( getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        // if have LC_VERSION_MIN_MACOSX and it says SDK < 10.9, so ignore code signature
        if ( (platform == Platform::macOS) && (sdk < 0x000A0900) )
            return false;
    }
    else {
        switch ( header()->cputype ) {
            case CPU_TYPE_I386:
            case CPU_TYPE_X86_64:
                // old binary with no LC_VERSION_*, assume intel binaries are old macOS binaries (ignore code signature)
                return false;
        }
    }

    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_CODE_SIGNATURE ) {
            const linkedit_data_command* sigCmd = (linkedit_data_command*)cmd;
            fileOffset = sigCmd->dataoff;
            size       = sigCmd->datasize;
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return (fileOffset != 0);
}

bool MachOParser::getEntry(uint32_t& offset, bool& usesCRT)
{
    Diagnostics diag;
    offset = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_MAIN ) {
            entry_point_command* mainCmd = (entry_point_command*)cmd;
            usesCRT = false;
            offset = (uint32_t)mainCmd->entryoff;
            stop = true;
        }
        else if ( cmd->cmd == LC_UNIXTHREAD ) {
            stop = true;
            usesCRT = true;
            const uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
            const uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);
            uint64_t startAddress = 0;
            switch ( header()->cputype ) {
                case CPU_TYPE_I386:
                    startAddress = regs32[10]; // i386_thread_state_t.eip
                    break;
                case CPU_TYPE_X86_64:
                    startAddress = regs64[16]; // x86_thread_state64_t.rip
                    break;
                case CPU_TYPE_ARM:
                    startAddress = regs32[15]; // arm_thread_state_t.__pc
                    break;
                case CPU_TYPE_ARM64:
                    startAddress = regs64[32]; // arm_thread_state64_t.__pc
                    break;
            }
            offset = (uint32_t)(startAddress - preferredLoadAddress());
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    // FIXME: validate offset is into executable segment
    return (offset != 0);
}

bool MachOParser::canBePlacedInDyldCache(const std::string& path) const {
    std::set<std::string> reasons;
    return canBePlacedInDyldCache(path, reasons);
}

bool MachOParser::canBePlacedInDyldCache(const std::string& path, std::set<std::string>& reasons) const
{
    bool retval = true;
    // only dylibs can go in cache
    if ( fileType() != MH_DYLIB ) {
        reasons.insert("Not MH_DYLIB");
        return false; // cannot continue, installName() will assert() if not a dylib
    }

    // only dylibs built for /usr/lib or /System/Library can go in cache
    const char* dylibName = installName();
    if ( (strncmp(dylibName, "/usr/lib/", 9) != 0) && (strncmp(dylibName, "/System/Library/", 16) != 0) ) {
        retval = false;
        reasons.insert("Not in '/usr/lib/' or '/System/Library/'");
    }

    // flat namespace files cannot go in cache
    if ( (header()->flags & MH_TWOLEVEL) == 0 ) {
        retval = false;
        reasons.insert("Not built with two level namespaces");
    }

    // don't put debug variants into dyld cache
    if ( endsWith(path, "_profile.dylib") || endsWith(path, "_debug.dylib") || endsWith(path, "_profile") || endsWith(path, "_debug") || endsWith(path, "/CoreADI") ) {
        retval = false;
        reasons.insert("Variant image");
    }

    // dylib must have extra info for moving DATA and TEXT segments apart
    __block bool hasExtraInfo = false;
    __block bool hasDyldInfo = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO )
            hasExtraInfo = true;
        if ( cmd->cmd == LC_DYLD_INFO_ONLY )
            hasDyldInfo = true;
    });
    if ( !hasExtraInfo ) {
        retval = false;
        reasons.insert("Missing split seg info");
    }
    if ( !hasDyldInfo ) {
        retval = false;
        reasons.insert("Old binary, missing dyld info");
    }

    // dylib can only depend on other dylibs in the shared cache
    __block bool allDepPathsAreGood = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( (strncmp(loadPath, "/usr/lib/", 9) != 0) && (strncmp(loadPath, "/System/Library/", 16) != 0) ) {
            allDepPathsAreGood = false;
            stop = true;
        }
    });
    if ( !allDepPathsAreGood ) {
        retval = false;
        reasons.insert("Depends on cache inelegible dylibs");
    }

    // dylibs with interposing info cannot be in cache
    __block bool hasInterposing = false;
    forEachInterposingTuple(diag, ^(uint32_t segIndex, uint64_t replacementSegOffset, uint64_t replaceeSegOffset, uint64_t replacementContent, bool& stop) {
        hasInterposing = true;
    });
    if ( hasInterposing ) {
        retval = false;
        reasons.insert("Has interposing tuples");
    }

    return retval;
}

bool MachOParser::isDynamicExecutable() const
{
    if ( fileType() != MH_EXECUTE )
        return false;
    
    // static executables do not have dyld load command
    __block bool hasDyldLoad = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_LOAD_DYLINKER ) {
            hasDyldLoad = true;
            stop = true;
        }
    });
    return hasDyldLoad;
}


bool MachOParser::isSlideable() const
{
    if ( header()->filetype == MH_DYLIB )
        return true;
    if ( header()->filetype == MH_BUNDLE )
        return true;
    if ( (header()->filetype == MH_EXECUTE) && (header()->flags & MH_PIE) )
        return true;

    return false;
}



bool MachOParser::hasInitializer(Diagnostics& diag) const
{
    __block bool result = false;
    forEachInitializer(diag, ^(uint32_t offset) {
        result = true;
    });
    return result;
}

void MachOParser::forEachInitializer(Diagnostics& diag, void (^callback)(uint32_t offset)) const
{
    __block uint64_t textSegAddrStart = 0;
    __block uint64_t textSegAddrEnd   = 0;

    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            textSegAddrStart = vmAddr;
            textSegAddrEnd   = vmAddr + vmSize;
            stop = true;
        }
    });
    if ( textSegAddrStart == textSegAddrEnd ) {
        diag.error("no __TEXT segment");
        return;
    }

    // if dylib linked with -init linker option, that initializer is first
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ROUTINES ) {
            const routines_command* routines = (routines_command*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( (textSegAddrStart < dashInit) && (dashInit < textSegAddrEnd) )
                callback((uint32_t)(dashInit - textSegAddrStart));
            else
                diag.error("-init does not point within __TEXT segment");
        }
        else if ( cmd->cmd == LC_ROUTINES_64 ) {
            const routines_command_64* routines = (routines_command_64*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( (textSegAddrStart < dashInit) && (dashInit < textSegAddrEnd) )
                callback((uint32_t)(dashInit - textSegAddrStart));
            else
                diag.error("-init does not point within __TEXT segment");
        }
    });

    // next any function pointers in mod-init section
    bool p64 = is64();
    unsigned pointerSize = p64 ? 8 : 4;
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, const void* content, size_t size, bool illegalSectionSize, bool& stop) {
        if ( (flags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS ) {
            if ( (size % pointerSize) != 0 ) {
                diag.error("initializer section %s/%s has bad size", segmentName, sectionName);
                stop = true;
                return;
            }
            if ( illegalSectionSize ) {
                diag.error("initializer section %s/%s extends beyond the end of the segment", segmentName, sectionName);
                stop = true;
                return;
            }
            if ( ((long)content % pointerSize) != 0 ) {
                diag.error("initializer section %s/%s is not pointer aligned", segmentName, sectionName);
                stop = true;
                return;
            }
            if ( p64 ) {
                const uint64_t* initsStart = (uint64_t*)content;
                const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)content + size);
                for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                    uint64_t anInit = *p;
                    if ( (anInit <= textSegAddrStart) || (anInit > textSegAddrEnd) ) {
                         diag.error("initializer 0x%0llX does not point within __TEXT segment", anInit);
                         stop = true;
                         break;
                    }
                    callback((uint32_t)(anInit - textSegAddrStart));
                }
            }
            else {
                const uint32_t* initsStart = (uint32_t*)content;
                const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + size);
                for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                    uint32_t anInit = *p;
                    if ( (anInit <= textSegAddrStart) || (anInit > textSegAddrEnd) ) {
                         diag.error("initializer 0x%0X does not point within __TEXT segment", anInit);
                         stop = true;
                         break;
                    }
                    callback(anInit - (uint32_t)textSegAddrStart);
                }
            }
        }
    });
}

void MachOParser::forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const
{
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, const void* content, size_t size, bool illegalSectionSize, bool& stop) {
        if ( ( (flags & SECTION_TYPE) == S_DTRACE_DOF ) && !illegalSectionSize ) {
            callback((uint32_t)((uintptr_t)content - (uintptr_t)header()));
        }
    });
}


uint32_t MachOParser::segmentCount() const
{
    __block uint32_t count   = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
        ++count;
    });
    return count;
}

void MachOParser::forEachSegment(void (^callback)(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& stop)) const
{
    Diagnostics diag;
    __block uint32_t segIndex = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* segCmd = (segment_command_64*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            const section_64* const sectionsStart = (section_64*)((char*)segCmd + sizeof(struct segment_command_64));
            const section_64* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section_64* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
            }
            callback(segCmd->segname, (uint32_t)segCmd->fileoff, (uint32_t)segCmd->filesize, segCmd->vmaddr, segCmd->vmsize, segCmd->initprot, segIndex, sizeOfSections, p2align, stop);
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
            }
            callback(segCmd->segname, (uint32_t)segCmd->fileoff, (uint32_t)segCmd->filesize, segCmd->vmaddr, segCmd->vmsize, segCmd->initprot, segIndex, sizeOfSections, p2align, stop);
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOParser::forEachExportedSymbol(Diagnostics diag, void (^handler)(const char* symbolName, uint64_t imageOffset, bool isReExport, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    if ( leInfo.dyldInfo != nullptr ) {
        const uint8_t* trieStart    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->export_off);
        const uint8_t* trieEnd      = trieStart + leInfo.dyldInfo->export_size;
        std::vector<ExportInfoTrie::Entry> exports;
        if ( !ExportInfoTrie::parseTrie(trieStart, trieEnd, exports) ) {
            diag.error("malformed exports trie");
            return;
        }
        bool stop = false;
        for (const ExportInfoTrie::Entry& exp : exports) {
            bool isReExport = (exp.info.flags & EXPORT_SYMBOL_FLAGS_REEXPORT);
            handler(exp.name.c_str(), exp.info.address, isReExport, stop);
            if ( stop )
                break;
        }
    }
}

bool MachOParser::invalidRebaseState(Diagnostics& diag, const char* opcodeName, const MachOParser::LinkEditInfo& leInfo,
                                    bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type) const
{
    if ( !segIndexSet ) {
        diag.error("%s missing preceding REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.segmentCount )  {
        diag.error("%s segment index %d too large", opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (leInfo.layout.segments[segmentIndex].segSize-pointerSize) ) {
        diag.error("%s current segment offset 0x%08llX beyond segment size (0x%08llX)", opcodeName, segmentOffset, leInfo.layout.segments[segmentIndex].segSize);
        return true;
    }
    switch ( type )  {
        case REBASE_TYPE_POINTER:
            if ( !leInfo.layout.segments[segmentIndex].writable ) {
                diag.error("%s pointer rebase is in non-writable segment", opcodeName);
                return true;
            }
            if ( leInfo.layout.segments[segmentIndex].executable ) {
                diag.error("%s pointer rebase is in executable segment", opcodeName);
                return true;
            }
            break;
        case REBASE_TYPE_TEXT_ABSOLUTE32:
        case REBASE_TYPE_TEXT_PCREL32:
            if ( !leInfo.layout.segments[segmentIndex].textRelocsAllowed ) {
                diag.error("%s text rebase is in segment that does not support text relocations", opcodeName);
                return true;
            }
            if ( leInfo.layout.segments[segmentIndex].writable ) {
                diag.error("%s text rebase is in writable segment", opcodeName);
                return true;
            }
            if ( !leInfo.layout.segments[segmentIndex].executable ) {
                diag.error("%s pointer rebase is in non-executable segment", opcodeName);
                return true;
            }
            break;
        default:
            diag.error("%s unknown rebase type %d", opcodeName, type);
            return true;
    }
    return false;
}

void MachOParser::forEachRebase(Diagnostics& diag, void (^handler)(uint32_t segIndex, uint64_t segOffset, uint8_t type, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    if ( leInfo.dyldInfo != nullptr ) {
        // work around linker bug that laid down rebase opcodes for lazy pointer section when -bind_at_load used
        __block int      lpSegIndex       = 0;
        __block uint64_t lpSegOffsetStart = 0;
        __block uint64_t lpSegOffsetEnd   = 0;
        bool             hasWeakBinds     = (leInfo.dyldInfo->weak_bind_size != 0);
        if ( leInfo.dyldInfo->lazy_bind_size == 0 ) {
            __block uint64_t lpAddr = 0;
            __block uint64_t lpSize = 0;
            forEachSection(^(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& sectStop) {
                if ( (flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ) {
                    lpAddr = addr;
                    lpSize = size;
                    sectStop =  true;
                }
            });
            forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& segStop) {
                if ( (vmAddr <= lpAddr) && (vmAddr+vmSize >= lpAddr+lpSize) ) {
                    lpSegOffsetStart = lpAddr - vmAddr;
                    lpSegOffsetEnd   = lpSegOffsetStart + lpSize;
                    segStop = true;
                    return;
                }
                ++lpSegIndex;
            });
        }
        // don't remove rebase if there is a weak-bind at pointer location
        bool (^weakBindAt)(uint64_t segOffset) = ^(uint64_t segOffset) {
            if ( !hasWeakBinds )
                return false;
            __block bool result = false;
            Diagnostics weakDiag;
            forEachWeakDef(weakDiag, ^(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset, uint64_t addend, const char* symbolName, bool& weakStop) {
                if ( segOffset == dataSegOffset ) {
                    result = true;
                    weakStop = true;
                }
            });
            return result;
        };


        const uint8_t* p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
        const uint8_t* end  = p + leInfo.dyldInfo->rebase_size;
        const uint32_t pointerSize = (is64() ? 8 : 4);
        uint8_t  type = 0;
        int      segIndex = 0;
        uint64_t segOffset = 0;
        uint64_t count;
        uint64_t skip;
        bool     segIndexSet = false;
        bool     stop = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & REBASE_IMMEDIATE_MASK;
            uint8_t opcode = *p & REBASE_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case REBASE_OPCODE_DONE:
                    stop = true;
                    break;
                case REBASE_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segIndex = immediate;
                    segOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case REBASE_OPCODE_ADD_ADDR_ULEB:
                    segOffset += read_uleb128(diag, p, end);
                    break;
                case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                    segOffset += immediate*pointerSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                    for (int i=0; i < immediate; ++i) {
                        if ( invalidRebaseState(diag, "REBASE_OPCODE_DO_REBASE_IMM_TIMES", leInfo, segIndexSet, pointerSize, segIndex, segOffset, type) )
                            return;
                        if ( (segIndex != lpSegIndex) || (segOffset > lpSegOffsetEnd) || (segOffset < lpSegOffsetStart) || weakBindAt(segOffset) )
                            handler(segIndex, segOffset, type, stop);
                        segOffset += pointerSize;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                    count = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                         if ( invalidRebaseState(diag, "REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segIndexSet, pointerSize, segIndex, segOffset, type) )
                            return;
                        if ( (segIndex != lpSegIndex) || (segOffset > lpSegOffsetEnd) || (segOffset < lpSegOffsetStart) || weakBindAt(segOffset) )
                            handler(segIndex, segOffset, type, stop);
                        segOffset += pointerSize;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                    if ( invalidRebaseState(diag, "REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segIndexSet, pointerSize, segIndex, segOffset, type) )
                        return;
                    handler(segIndex, segOffset, type, stop);
                    segOffset += read_uleb128(diag, p, end) + pointerSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    if ( diag.hasError() )
                        break;
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        if ( invalidRebaseState(diag, "REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", leInfo, segIndexSet, pointerSize, segIndex, segOffset, type) )
                            return;
                        handler(segIndex, segOffset, type, stop);
                        segOffset += skip + pointerSize;
                    }
                    break;
                default:
                    diag.error("unknown rebase opcode 0x%02X", opcode);
            }
        }
    }
    else {
        // old binary
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->locreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nlocrel];
        bool                            stop = false;
        const uint8_t                   relocSize = (is64() ? 3 : 2);
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                diag.error("local relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
                diag.error("local relocation has wrong r_type");
                break;
            }
            doLocalReloc(diag, reloc->r_address, stop, handler);
         }
        // then process indirect symbols
        forEachIndirectPointer(diag, ^(uint32_t segIndex, uint64_t segOffset, bool bind, int bindLibOrdinal,
                                       const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( !bind && !bindLazy )
                handler(segIndex, segOffset, REBASE_TYPE_POINTER, indStop);
        });
    }
}

bool MachOParser::doLocalReloc(Diagnostics& diag, uint32_t r_address, bool& stop, void (^handler)(uint32_t segIndex, uint64_t segOffset, uint8_t type, bool& stop)) const
{
    bool                firstWritable = (header()->cputype == CPU_TYPE_X86_64);
    __block uint64_t    relocBaseAddress = 0;
    __block bool        baseFound = false;
    __block uint32_t    segIndex = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool &stopSeg) {
        if ( !baseFound ) {
            if ( !firstWritable || (protections & VM_PROT_WRITE) ) {
                baseFound = true;
                relocBaseAddress = vmAddr;
            }
        }
        if ( baseFound && (vmAddr < relocBaseAddress+r_address) && (relocBaseAddress+r_address < vmAddr+vmSize) ) {
            uint8_t  type = REBASE_TYPE_POINTER;
            uint64_t segOffset = relocBaseAddress + r_address - vmAddr;
            handler(segIndex, segOffset, type, stop);
            stopSeg = true;
        }
        ++segIndex;
    });

    return false;
}

int MachOParser::libOrdinalFromDesc(uint16_t n_desc) const
{
    // -flat_namespace is always flat lookup
    if ( (header()->flags & MH_TWOLEVEL) == 0 )
        return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

    // extract byte from undefined symbol entry
    int libIndex = GET_LIBRARY_ORDINAL(n_desc);
    switch ( libIndex ) {
        case SELF_LIBRARY_ORDINAL:
            return BIND_SPECIAL_DYLIB_SELF;

        case DYNAMIC_LOOKUP_ORDINAL:
            return BIND_SPECIAL_DYLIB_FLAT_LOOKUP;

        case EXECUTABLE_ORDINAL:
            return BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE;
    }

    return libIndex;
}

bool MachOParser::doExternalReloc(Diagnostics& diag, uint32_t r_address, uint32_t r_symbolnum, LinkEditInfo& leInfo, bool& stop,
                                    void (^handler)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type, int libOrdinal,
                                                    uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const
{
    const bool          firstWritable    = (header()->cputype == CPU_TYPE_X86_64);
    const bool          is64Bit          = is64();
    __block uint64_t    relocBaseAddress = 0;
    __block bool        baseFound        = false;
    __block uint32_t    segIndex         = 0;
    forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool &stopSeg) {
        if ( !baseFound ) {
            if ( !firstWritable || (protections & VM_PROT_WRITE) ) {
                baseFound = true;
                relocBaseAddress = vmAddr;
            }
        }
        if ( baseFound && (vmAddr < relocBaseAddress+r_address) && (relocBaseAddress+r_address < vmAddr+vmSize) ) {
            uint8_t                 type        = BIND_TYPE_POINTER;
            uint64_t                segOffset   = relocBaseAddress + r_address - vmAddr;
            const void*             symbolTable = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
            const struct nlist_64*  symbols64   = (nlist_64*)symbolTable;
            const struct nlist*     symbols32   = (struct nlist*)symbolTable;
            const char*             stringPool  = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
            uint32_t                symCount    = leInfo.symTab->nsyms;
            uint32_t                poolSize    = leInfo.symTab->strsize;
            if ( r_symbolnum < symCount ) {
                uint16_t n_desc = is64Bit ? symbols64[r_symbolnum].n_desc : symbols32[r_symbolnum].n_desc;
                uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
                uint32_t strOffset = is64Bit ? symbols64[r_symbolnum].n_un.n_strx : symbols32[r_symbolnum].n_un.n_strx;
                if ( strOffset < poolSize ) {
                    const char* symbolName  = stringPool + strOffset;
                    bool        weakImport  = (n_desc & N_WEAK_REF);
                    bool        lazy        = false;
                    uint64_t    addend      = is64Bit ? (*((uint64_t*)((char*)header()+fileOffset+segOffset))) : (*((uint32_t*)((char*)header()+fileOffset+segOffset)));
                    handler(segIndex, segOffset, type, libOrdinal, addend, symbolName, weakImport, lazy, stop);
                    stopSeg = true;
                }
            }
        }
        ++segIndex;
    });

    return false;
}

bool MachOParser::invalidBindState(Diagnostics& diag, const char* opcodeName, const LinkEditInfo& leInfo, bool segIndexSet, bool libraryOrdinalSet,
                                   uint32_t dylibCount, int libOrdinal, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const
{
    if ( !segIndexSet ) {
        diag.error("%s missing preceding BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.segmentCount )  {
        diag.error("%s segment index %d too large", opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (leInfo.layout.segments[segmentIndex].segSize-pointerSize) ) {
        diag.error("%s current segment offset 0x%08llX beyond segment size (0x%08llX)", opcodeName, segmentOffset, leInfo.layout.segments[segmentIndex].segSize);
        return true;
    }
    if ( symbolName == NULL ) {
        diag.error("%s missing preceding BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", opcodeName);
        return true;
    }
    if ( !libraryOrdinalSet ) {
        diag.error("%s missing preceding BIND_OPCODE_SET_DYLIB_ORDINAL", opcodeName);
        return true;
    }
    if ( libOrdinal > (int)dylibCount ) {
        diag.error("%s has library ordinal too large (%d) max (%d)", opcodeName, libOrdinal, dylibCount);
        return true;
    }
    if ( libOrdinal < -2 ) {
        diag.error("%s has unknown library special ordinal (%d)", opcodeName, libOrdinal);
        return true;
    }
    switch ( type )  {
        case BIND_TYPE_POINTER:
            if ( !leInfo.layout.segments[segmentIndex].writable ) {
                diag.error("%s pointer bind is in non-writable segment", opcodeName);
                return true;
            }
            if ( leInfo.layout.segments[segmentIndex].executable ) {
                diag.error("%s pointer bind is in executable segment", opcodeName);
                return true;
            }
            break;
        case BIND_TYPE_TEXT_ABSOLUTE32:
        case BIND_TYPE_TEXT_PCREL32:
            if ( !leInfo.layout.segments[segmentIndex].textRelocsAllowed ) {
                diag.error("%s text bind is in segment that does not support text relocations", opcodeName);
                return true;
            }
            if ( leInfo.layout.segments[segmentIndex].writable ) {
                diag.error("%s text bind is in writable segment", opcodeName);
                return true;
            }
            if ( !leInfo.layout.segments[segmentIndex].executable ) {
                diag.error("%s pointer bind is in non-executable segment", opcodeName);
                return true;
            }
            break;
        default:
            diag.error("%s unknown bind type %d", opcodeName, type);
            return true;
    }
    return false;
}

void MachOParser::forEachBind(Diagnostics& diag, void (^handler)(uint32_t dataSegIndex, uint64_t dataSegOffset, uint8_t type,
                              int libOrdinal, uint64_t addend, const char* symbolName, bool weakImport, bool lazy, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;
    const uint32_t dylibCount = dependentDylibCount();

    if ( leInfo.dyldInfo != nullptr ) {
        // process bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->bind_size;
        const uint32_t  pointerSize = (is64() ? 8 : 4);
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int             libraryOrdinal = 0;
        bool            segIndexSet = false;
        bool            libraryOrdinalSet = false;

        int64_t         addend = 0;
        uint64_t        count;
        uint64_t        skip;
        bool            weakImport = false;
        bool            done = false;
        bool            stop = false;
        while ( !done && !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    done = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    libraryOrdinal = immediate;
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    libraryOrdinal = (int)read_uleb128(diag, p, end);
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    // the special ordinals are negative numbers
                    if ( immediate == 0 )
                        libraryOrdinal = 0;
                    else {
                        int8_t signExtended = BIND_OPCODE_MASK | immediate;
                        libraryOrdinal = signExtended;
                    }
                    libraryOrdinalSet = true;
                    break;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                    symbolName = (char*)p;
                    while (*p != '\0')
                        ++p;
                    ++p;
                    break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_ADD_ADDR_ULEB:
                    segmentOffset += read_uleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, weakImport, false, stop);
                    segmentOffset += pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, weakImport, false, stop);
                    segmentOffset += read_uleb128(diag, p, end) + pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, weakImport, false, stop);
                    segmentOffset += immediate*pointerSize + pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                            return;
                        handler(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, weakImport, false, stop);
                        segmentOffset += skip + pointerSize;
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", *p);
            }
        }
        if ( diag.hasError() || stop )
            return;
        // process lazy bind opcodes
        if ( leInfo.dyldInfo->lazy_bind_size != 0 ) {
            p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->lazy_bind_off);
            end             = p + leInfo.dyldInfo->lazy_bind_size;
            type            = BIND_TYPE_POINTER;
            segmentOffset   = 0;
            segmentIndex    = 0;
            symbolName      = NULL;
            libraryOrdinal  = 0;
            segIndexSet     = false;
            libraryOrdinalSet= false;
            addend          = 0;
            weakImport      = false;
            stop            = false;
            while ( !stop && diag.noError() && (p < end) ) {
                uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
                uint8_t opcode = *p & BIND_OPCODE_MASK;
                ++p;
                switch (opcode) {
                    case BIND_OPCODE_DONE:
                        // this opcode marks the end of each lazy pointer binding
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                        libraryOrdinal = immediate;
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                        libraryOrdinal = (int)read_uleb128(diag, p, end);
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                        // the special ordinals are negative numbers
                        if ( immediate == 0 )
                            libraryOrdinal = 0;
                        else {
                            int8_t signExtended = BIND_OPCODE_MASK | immediate;
                            libraryOrdinal = signExtended;
                        }
                        libraryOrdinalSet = true;
                        break;
                    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                        weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                        symbolName = (char*)p;
                        while (*p != '\0')
                            ++p;
                        ++p;
                        break;
                    case BIND_OPCODE_SET_ADDEND_SLEB:
                        addend = read_sleb128(diag, p, end);
                        break;
                    case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                        segmentIndex = immediate;
                        segmentOffset = read_uleb128(diag, p, end);
                        segIndexSet = true;
                        break;
                    case BIND_OPCODE_DO_BIND:
                        if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                            return;
                        handler(segmentIndex, segmentOffset, type, libraryOrdinal, addend, symbolName, weakImport, true, stop);
                        segmentOffset += pointerSize;
                        break;
                    case BIND_OPCODE_SET_TYPE_IMM:
                    case BIND_OPCODE_ADD_ADDR_ULEB:
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    default:
                        diag.error("bad lazy bind opcode 0x%02X", opcode);
                        break;
                }
            }
        }
    }
    else {
        // old binary, first process relocation
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->extreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nextrel];
        bool                            stop = false;
        const uint8_t                   relocSize = (is64() ? 3 : 2);
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                diag.error("external relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA == ARM64_RELOC_UNSIGNED
                 diag.error("external relocation has wrong r_type");
                break;
            }
            doExternalReloc(diag, reloc->r_address, reloc->r_symbolnum, leInfo, stop, handler);
        }
        // then process indirect symbols
        forEachIndirectPointer(diag, ^(uint32_t segIndex, uint64_t segOffset, bool bind, int bindLibOrdinal,
                                       const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( bind )
                handler(segIndex, segOffset, (selfModifyingStub ? BIND_TYPE_IMPORT_JMP_REL32 : BIND_TYPE_POINTER), bindLibOrdinal, 0, bindSymbolName, bindWeakImport, bindLazy, indStop);
        });
    }
}


void MachOParser::forEachWeakDef(Diagnostics& diag, void (^handler)(bool strongDef, uint32_t dataSegIndex, uint64_t dataSegOffset,
                                                                    uint64_t addend, const char* symbolName, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    const uint32_t dylibCount = dependentDylibCount();
    if ( leInfo.dyldInfo != nullptr ) {
        // process weak bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->weak_bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->weak_bind_size;
        const uint32_t  pointerSize = (is64() ? 8 : 4);
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int64_t         addend = 0;
        uint64_t        count;
        uint64_t        skip;
        bool            segIndexSet = false;
        bool            done = false;
        bool            stop = false;
        while ( !done && !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    done = true;
                    break;
                case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                    diag.error("unexpected dylib ordinal in weak binding info");
                    return;
                case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                    symbolName = (char*)p;
                    while (*p != '\0')
                        ++p;
                    ++p;
                    if ( (immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION) != 0 )
                        handler(true, 0, 0, 0, symbolName, stop);
                   break;
                case BIND_OPCODE_SET_TYPE_IMM:
                    type = immediate;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_ADD_ADDR_ULEB:
                    segmentOffset += read_uleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, true, dylibCount, -2, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(false, segmentIndex, segmentOffset, addend, symbolName, stop);
                    segmentOffset += pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, true, dylibCount, -2, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(false, segmentIndex, segmentOffset, addend, symbolName, stop);
                    segmentOffset += read_uleb128(diag, p, end) + pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                     if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, true, dylibCount, -2, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                        return;
                    handler(false, segmentIndex, segmentOffset, addend, symbolName, stop);
                    segmentOffset += immediate*pointerSize + pointerSize;
                    break;
                case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        if ( invalidBindState(diag, "BIND_OPCODE_DO_BIND", leInfo, segIndexSet, true, dylibCount, -2, pointerSize, segmentIndex, segmentOffset, type, symbolName) )
                            return;
                        handler(false, segmentIndex, segmentOffset, addend, symbolName, stop);
                        segmentOffset += skip + pointerSize;
                    }
                    break;
                default:
                    diag.error("bad weak bind opcode 0x%02X", *p);
            }
        }
        if ( diag.hasError() || stop )
            return;
     }
    else {
        // old binary
        //assert(0 && "weak defs not supported for old binaries yet");
    }
}



void MachOParser::forEachIndirectPointer(Diagnostics& diag, void (^handler)(uint32_t dataSegIndex, uint64_t dataSegOffset, bool bind, int bindLibOrdinal,
                                                                            const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    // find lazy and non-lazy pointer sections
    const bool              is64Bit                  = is64();
    const uint32_t* const   indirectSymbolTable      = (uint32_t*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->indirectsymoff);
    const uint32_t          indirectSymbolTableCount = leInfo.dynSymTab->nindirectsyms;
    const uint32_t          pointerSize              = is64Bit ? 8 : 4;
    const void*             symbolTable              = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
    const struct nlist_64*  symbols64                = (nlist_64*)symbolTable;
    const struct nlist*     symbols32                = (struct nlist*)symbolTable;
    const char*             stringPool               = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    uint32_t                symCount                 = leInfo.symTab->nsyms;
    uint32_t                poolSize                 = leInfo.symTab->strsize;
    __block bool            stop                     = false;
    forEachSection(^(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content,
                     uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& sectionStop) {
        uint8_t  sectionType  = (flags & SECTION_TYPE);
        if ( (sectionType != S_LAZY_SYMBOL_POINTERS) && (sectionType != S_NON_LAZY_SYMBOL_POINTERS) && (sectionType != S_SYMBOL_STUBS) )
            return;
        bool selfModifyingStub = (sectionType == S_SYMBOL_STUBS) && (flags & S_ATTR_SELF_MODIFYING_CODE) && (reserved2 == 5) && (header()->cputype == CPU_TYPE_I386);
        if ( (flags & S_ATTR_SELF_MODIFYING_CODE) && !selfModifyingStub ) {
            diag.error("S_ATTR_SELF_MODIFYING_CODE section type only valid in old i386 binaries");
            sectionStop = true;
            return;
        }
        uint32_t elementSize = selfModifyingStub ? reserved2 : pointerSize;
        uint32_t elementCount = (uint32_t)(size/elementSize);
        if (greaterThanAddOrOverflow(reserved1, elementCount, indirectSymbolTableCount)) {
            diag.error("section %s overflows indirect symbol table", sectionName);
            sectionStop = true;
            return;
        }
        __block uint32_t index = 0;
        __block uint32_t segIndex = 0;
        __block uint64_t sectionSegOffset;
        forEachSegment(^(const char* segmentName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool &segStop) {
            if ( (vmAddr <= addr) && (addr < vmAddr+vmSize) ) {
                sectionSegOffset = addr - vmAddr;
                segIndex = index;
                segStop = true;
            }
            ++index;
        });

        for (int i=0; (i < elementCount) && !stop; ++i) {
            uint32_t symNum = indirectSymbolTable[reserved1 + i];
            if ( symNum == INDIRECT_SYMBOL_ABS )
                continue;
            uint64_t segOffset = sectionSegOffset+i*elementSize;
            if ( symNum == INDIRECT_SYMBOL_LOCAL ) {
                handler(segIndex, segOffset, false, 0, "", false, false, false, stop);
                continue;
            }
            if ( symNum > symCount ) {
                diag.error("indirect symbol[%d] = %d which is invalid symbol index", reserved1 + i, symNum);
                sectionStop = true;
                return;
            }
            uint16_t n_desc = is64Bit ? symbols64[symNum].n_desc : symbols32[symNum].n_desc;
            uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
            uint32_t strOffset = is64Bit ? symbols64[symNum].n_un.n_strx : symbols32[symNum].n_un.n_strx;
            if ( strOffset > poolSize ) {
               diag.error("symbol[%d] string offset out of range", reserved1 + i);
                sectionStop = true;
                return;
            }
            const char* symbolName  = stringPool + strOffset;
            bool        weakImport  = (n_desc & N_WEAK_REF);
            bool        lazy        = (sectionType == S_LAZY_SYMBOL_POINTERS);
            handler(segIndex, segOffset, true, libOrdinal, symbolName, weakImport, lazy, selfModifyingStub, stop);
        }
        sectionStop = stop;
    });
}

void MachOParser::forEachInterposingTuple(Diagnostics& diag, void (^handler)(uint32_t segIndex, uint64_t replacementSegOffset, uint64_t replaceeSegOffset, uint64_t replacementContent, bool& stop)) const
{
    const bool     is64Bit      = is64();
    const unsigned entrySize    = is64Bit ? 16 : 8;
    const unsigned pointerSize  = is64Bit ?  8 : 4;
    forEachSection(^(const char* segmentName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& secStop) {
        if ( ((flags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(sectionName, "__interpose") == 0) && (strcmp(segmentName, "__DATA") == 0)) ) {
            if ( (size % entrySize) != 0 ) {
                diag.error("interposing section %s/%s has bad size", segmentName, sectionName);
                secStop = true;
                return;
            }
            if ( illegalSectionSize ) {
                diag.error("interposing section %s/%s extends beyond the end of the segment", segmentName, sectionName);
                secStop = true;
                return;
            }
            if ( ((long)content % pointerSize) != 0 ) {
                diag.error("interposing section %s/%s is not pointer aligned", segmentName, sectionName);
                secStop = true;
                return;
            }
            __block uint32_t sectionSegIndex  = 0;
            __block uint64_t sectionSegOffset = 0;
            forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool& segStop) {
                if ( (vmAddr <= addr) && (addr < vmAddr+vmSize) ) {
                    sectionSegIndex  = segIndex;
                    sectionSegOffset = addr - vmAddr;
                    segStop          = true;
                }
            });
            if ( sectionSegIndex == 0 ) {
                diag.error("interposing section %s/%s is not in a segment", segmentName, sectionName);
                secStop = true;
                return;
            }
            uint32_t offset = 0;
            bool tupleStop = false;
            for (int i=0; i < (size/entrySize); ++i) {
                uint64_t replacementContent = is64Bit ? (*(uint64_t*)((char*)content + offset)) :  (*(uint32_t*)((char*)content + offset));
                handler(sectionSegIndex, sectionSegOffset+offset, sectionSegOffset+offset+pointerSize, replacementContent, tupleStop);
                offset += entrySize;
                if ( tupleStop )
                    break;
            }
        }
    });
}


const void* MachOParser::content(uint64_t vmOffset)
{
    __block const void* result = nullptr;
    __block uint32_t firstSegFileOffset = 0;
    __block uint64_t firstSegVmAddr = 0;
	if ( isRaw() ) {
        forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool &stop) {
            if ( firstSegFileOffset == 0) {
                if ( fileSize == 0 )
                    return; // skip __PAGEZERO
                firstSegFileOffset = fileOffset;
                firstSegVmAddr = vmAddr;
            }
            uint64_t segVmOffset = vmAddr - firstSegVmAddr;
            if ( (vmOffset >= segVmOffset) && (vmOffset < segVmOffset+vmSize) ) {
                result = (char*)(header()) + (fileOffset - firstSegFileOffset) + (vmOffset - segVmOffset);
                stop = true;
            }
        });
	}
    else if ( inRawCache() ) {
        forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, uint32_t segIndex, uint64_t sizeOfSections, uint8_t p2align, bool &stop) {
            if ( firstSegFileOffset == 0 ) {
                firstSegFileOffset = fileOffset;
                firstSegVmAddr = vmAddr;
            }
            uint64_t segVmOffset = vmAddr - firstSegVmAddr;
            if ( (vmOffset >= segVmOffset) && (vmOffset < segVmOffset+vmSize) ) {
                result = (char*)(header()) + (fileOffset - firstSegFileOffset) + (vmOffset - segVmOffset);
                stop = true;
            }
        });
    }
    else {
        // non-raw cache is easy
        result = (char*)(header()) + vmOffset;
    }
	return result;
}

#endif  //  !DYLD_IN_PROCESS

bool MachOParser::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size)
{
    textOffset = 0;
    size = 0;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( (cmd->cmd == LC_ENCRYPTION_INFO) || (cmd->cmd == LC_ENCRYPTION_INFO_64) ) {
            const encryption_info_command* encCmd = (encryption_info_command*)cmd;
            if ( encCmd->cryptid == 1 ) {
                // Note: cryptid is 0 in just-built apps.  The iTunes App Store sets cryptid to 1
                textOffset = encCmd->cryptoff;
                size       = encCmd->cryptsize;
            }
            stop = true;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
    return (textOffset != 0);
}

bool MachOParser::cdHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen, uint8_t cdHash[20])
{
    const CS_CodeDirectory* cd = (const CS_CodeDirectory*)findCodeDirectoryBlob(codeSigStart, codeSignLen);
    if ( cd == nullptr )
        return false;

    uint32_t cdLength = htonl(cd->length);
    if ( cd->hashType == CS_HASHTYPE_SHA256 ) {
        uint8_t digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(cd, cdLength, digest);
        // cd-hash of sigs that use SHA256 is the first 20 bytes of the SHA256 of the code digest
        memcpy(cdHash, digest, 20);
        return true;
    }
    else if ( cd->hashType == CS_HASHTYPE_SHA1 ) {
        // compute hash directly into return buffer
        CC_SHA1(cd, cdLength, cdHash);
        return true;
    }

    return false;
}

const void* MachOParser::findCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen)
{
    // verify min length of overall code signature
    if ( codeSignLen < sizeof(CS_SuperBlob) )
        return nullptr;

    // verify magic at start
    const CS_SuperBlob* codeSuperBlob = (CS_SuperBlob*)codeSigStart;
    if ( codeSuperBlob->magic != htonl(CSMAGIC_EMBEDDED_SIGNATURE) )
        return nullptr;

    // verify count of sub-blobs not too large
    uint32_t subBlobCount = htonl(codeSuperBlob->count);
    if ( (codeSignLen-sizeof(CS_SuperBlob))/sizeof(CS_BlobIndex) < subBlobCount )
        return nullptr;

    // walk each sub blob, looking at ones with type CSSLOT_CODEDIRECTORY
    for (uint32_t i=0; i < subBlobCount; ++i) {
        if ( codeSuperBlob->index[i].type != htonl(CSSLOT_CODEDIRECTORY) )
            continue;
        uint32_t cdOffset = htonl(codeSuperBlob->index[i].offset);
        // verify offset is not out of range
        if ( cdOffset > (codeSignLen - sizeof(CS_CodeDirectory)) )
            return nullptr;
        const CS_CodeDirectory* cd = (CS_CodeDirectory*)((uint8_t*)codeSuperBlob + cdOffset);
        uint32_t cdLength = htonl(cd->length);
        // verify code directory length not out of range
        if ( cdLength > (codeSignLen - cdOffset) )
            return nullptr;
        if ( cd->magic == htonl(CSMAGIC_CODEDIRECTORY) )
            return cd;
    }
    return nullptr;
}




} // namespace dyld3

