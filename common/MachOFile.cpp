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

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <TargetConditionals.h>
#include "Defines.h"
#if TARGET_OS_EXCLAVEKIT
  #define OSSwapBigToHostInt32 __builtin_bswap32
  #define OSSwapBigToHostInt64 __builtin_bswap64
  #define htonl                __builtin_bswap32
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <sys/errno.h>
  #include <sys/fcntl.h>
  #include <unistd.h>
  #include <mach/host_info.h>
  #include <mach/mach.h>
  #include <mach/mach_host.h>
#if SUPPORT_CLASSIC_RELOCS
  #include <mach-o/reloc.h>
  #include <mach-o/x86_64/reloc.h>
#endif
extern "C" {
  #include <corecrypto/ccdigest.h>
  #include <corecrypto/ccsha1.h>
  #include <corecrypto/ccsha2.h>
}
#endif

#include "Defines.h"

#include <mach-o/nlist.h>

#include "Array.h"
#include "MachOFile.h"
#include "SupportedArchs.h"
#include "CodeSigningTypes.h"

#if (BUILDING_DYLD || BUILDING_LIBDYLD) && !TARGET_OS_EXCLAVEKIT
    #include <subsystem.h>
#endif

namespace dyld3 {

#if !TARGET_OS_EXCLAVEKIT

////////////////////////////  posix wrappers ////////////////////////////////////////

// <rdar://problem/10111032> wrap calls to stat() with check for EAGAIN
int stat(const char* path, struct stat* buf)
{
    int result;
    do {
#if BUILDING_DYLD
        result = ::stat_with_subsystem(path, buf);
#else
        result = ::stat(path, buf);
#endif
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}

// <rdar://problem/10111032> wrap calls to stat() with check for EAGAIN
int fstatat(int fd, const char *path, struct stat *buf, int flag)
{
    int result;
    do {
        result = ::fstatat(fd, path, buf, flag);
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}

// <rdar://problem/13805025> dyld should retry open() if it gets an EGAIN
int open(const char* path, int flag, int other)
{
    int result;
    do {
#if BUILDING_DYLD
        if (flag & O_CREAT)
            result = ::open(path, flag, other);
        else
            result = ::open_with_subsystem(path, flag);
#else
        result = ::open(path, flag, other);
#endif
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT


////////////////////////////  FatFile ////////////////////////////////////////

const FatFile* FatFile::isFatFile(const void* fileStart)
{
    const FatFile* fileStartAsFat = (FatFile*)fileStart;
    if ( (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC)) || (fileStartAsFat->magic == OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return fileStartAsFat;
    else
        return nullptr;
}

bool FatFile::isValidSlice(Diagnostics& diag, uint64_t fileLen, uint32_t sliceIndex,
                           uint32_t sliceCpuType, uint32_t sliceCpuSubType, uint64_t sliceOffset, uint64_t sliceLen) const {
    if ( greaterThanAddOrOverflow(sliceOffset, sliceLen, fileLen) ) {
        diag.error("slice %d extends beyond end of file", sliceIndex);
        return false;
    }
    const dyld3::MachOFile* mf = (const dyld3::MachOFile*)((uint8_t*)this+sliceOffset);
    if (!mf->isMachO(diag, sliceLen))
        return false;
    if ( mf->cputype != (cpu_type_t)sliceCpuType ) {
        diag.error("cpu type in slice (0x%08X) does not match fat header (0x%08X)", mf->cputype, sliceCpuType);
        return false;
    }
    else if ( (mf->cpusubtype & ~CPU_SUBTYPE_MASK) != (sliceCpuSubType & ~CPU_SUBTYPE_MASK) ) {
        diag.error("cpu subtype in slice (0x%08X) does not match fat header (0x%08X)", mf->cpusubtype, sliceCpuSubType);
        return false;
    }
    uint32_t pageSizeMask = mf->uses16KPages() ? 0x3FFF : 0xFFF;
    if ( (sliceOffset & pageSizeMask) != 0 ) {
        // slice not page aligned
        if ( strncmp((char*)this+sliceOffset, "!<arch>", 7) == 0 )
            diag.error("file is static library");
        else
            diag.error("slice is not page aligned");
        return false;
    }
    return true;
}

void FatFile::forEachSlice(Diagnostics& diag, uint64_t fileLen, bool validate,
                           void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const
{
    if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC) ) {
        const uint64_t maxArchs = ((4096 - sizeof(fat_header)) / sizeof(fat_arch));
        const uint32_t numArchs = OSSwapBigToHostInt32(nfat_arch);
        if ( numArchs > maxArchs ) {
            diag.error("fat header too large: %u entries", numArchs);
            return;
        }
        // <rdar://90700132> make sure architectures list doesn't exceed the file size
        // We can’t overflow due to maxArch check
        // Check numArchs+1 to cover the extra read after the loop
        if ( (sizeof(fat_header) + ((numArchs + 1) * sizeof(fat_arch))) > fileLen ) {
            diag.error("fat header malformed, architecture slices extend beyond end of file");
            return;
        }
        bool stop = false;
        const fat_arch* const archs = (fat_arch*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < numArchs; ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint32_t offset     = OSSwapBigToHostInt32(archs[i].offset);
            uint32_t len        = OSSwapBigToHostInt32(archs[i].size);
            Diagnostics sliceDiag;
            if ( !validate || isValidSlice(sliceDiag, fileLen, i, cpuType, cpuSubType, offset, len) )
                callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
            if ( sliceDiag.hasError() )
                diag.appendError("%s, ", sliceDiag.errorMessageCStr());
        }

        // Look for one more slice
        if ( numArchs != maxArchs ) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[numArchs].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[numArchs].cpusubtype);
            uint32_t offset     = OSSwapBigToHostInt32(archs[numArchs].offset);
            uint32_t len        = OSSwapBigToHostInt32(archs[numArchs].size);
            if ((cpuType == CPU_TYPE_ARM64) && ((cpuSubType == CPU_SUBTYPE_ARM64_ALL || cpuSubType == CPU_SUBTYPE_ARM64_V8))) {
                if ( !validate || isValidSlice(diag, fileLen, numArchs, cpuType, cpuSubType, offset, len) )
                    callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            }
        }
    }
    else if ( this->magic == OSSwapBigToHostInt32(FAT_MAGIC_64) ) {
        const uint32_t numArchs = OSSwapBigToHostInt32(nfat_arch);
        if ( numArchs > ((4096 - sizeof(fat_header)) / sizeof(fat_arch_64)) ) {
            diag.error("fat header too large: %u entries", OSSwapBigToHostInt32(nfat_arch));
            return;
        }
        // <rdar://90700132> make sure architectures list doesn't exceed the file size
        // We can’t overflow due to maxArch check
        if ( (sizeof(fat_header) + (numArchs * sizeof(fat_arch_64))) > fileLen ) {
            diag.error("fat header malformed, architecture slices extend beyond end of file");
            return;
        }
        bool stop = false;
        const fat_arch_64* const archs = (fat_arch_64*)(((char*)this)+sizeof(fat_header));
        for (uint32_t i=0; i < numArchs; ++i) {
            uint32_t cpuType    = OSSwapBigToHostInt32(archs[i].cputype);
            uint32_t cpuSubType = OSSwapBigToHostInt32(archs[i].cpusubtype);
            uint64_t offset     = OSSwapBigToHostInt64(archs[i].offset);
            uint64_t len        = OSSwapBigToHostInt64(archs[i].size);
            if ( !validate || isValidSlice(diag, fileLen, i, cpuType, cpuSubType, offset, len) )
                callback(cpuType, cpuSubType, (uint8_t*)this+offset, len, stop);
            if ( stop )
                break;
        }
    }
    else {
        diag.error("not a fat file");
    }
}

void FatFile::forEachSlice(Diagnostics& diag, uint64_t fileLen, void (^callback)(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop)) const
{
    forEachSlice(diag, fileLen, true, callback);
}

const char* FatFile::archNames(char strBuf[256], uint64_t fileLen) const
{
    strBuf[0] = '\0';
    Diagnostics   diag;
    __block bool  needComma = false;
    this->forEachSlice(diag, fileLen, false, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
        if ( needComma )
            strlcat(strBuf, ",", 256);
        strlcat(strBuf, MachOFile::archName(sliceCpuType, sliceCpuSubType), 256);
        needComma = true;
    });
    return strBuf;
}

bool FatFile::isFatFileWithSlice(Diagnostics& diag, uint64_t fileLen, const GradedArchs& archs, bool isOSBinary,
                                 uint64_t& sliceOffset, uint64_t& sliceLen, bool& missingSlice) const
{
    missingSlice = false;
    if ( (this->magic != OSSwapBigToHostInt32(FAT_MAGIC)) && (this->magic != OSSwapBigToHostInt32(FAT_MAGIC_64)) )
        return false;

    __block int bestGrade = 0;
    forEachSlice(diag, fileLen, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
        if (int sliceGrade = archs.grade(sliceCpuType, sliceCpuSubType, isOSBinary)) {
            if ( sliceGrade > bestGrade ) {
                sliceOffset = (char*)sliceStart - (char*)this;
                sliceLen    = sliceSize;
                bestGrade   = sliceGrade;
            }
        }
    });
    if ( diag.hasError() )
        return false;

    if ( bestGrade == 0 )
        missingSlice = true;

    return (bestGrade != 0);
}


////////////////////////////  GradedArchs ////////////////////////////////////////


#define GRADE_i386        CPU_TYPE_I386,       CPU_SUBTYPE_I386_ALL,    false
#define GRADE_x86_64      CPU_TYPE_X86_64,     CPU_SUBTYPE_X86_64_ALL,  false
#define GRADE_x86_64h     CPU_TYPE_X86_64,     CPU_SUBTYPE_X86_64_H,    false
#define GRADE_armv7       CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7,      false
#define GRADE_armv7s      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7S,     false
#define GRADE_armv7k      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7K,     false
#define GRADE_armv6m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V6M,     false
#define GRADE_armv7m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7M,     false
#define GRADE_armv7em     CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V7EM,    false
#define GRADE_armv8m      CPU_TYPE_ARM,        CPU_SUBTYPE_ARM_V8M,     false
#define GRADE_arm64       CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64_ALL,   false
#define GRADE_arm64e      CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64E,      false
#define GRADE_arm64e_pb   CPU_TYPE_ARM64,      CPU_SUBTYPE_ARM64E,      true
#define GRADE_arm64_32    CPU_TYPE_ARM64_32,   CPU_SUBTYPE_ARM64_32_V8, false

const GradedArchs GradedArchs::i386              = GradedArchs({GRADE_i386,    1});
const GradedArchs GradedArchs::x86_64            = GradedArchs({GRADE_x86_64,  1});
const GradedArchs GradedArchs::x86_64h           = GradedArchs({GRADE_x86_64h, 2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::arm64             = GradedArchs({GRADE_arm64,   1});
#if SUPPORT_ARCH_arm64e
const GradedArchs GradedArchs::arm64e_keysoff    = GradedArchs({GRADE_arm64e,    2}, {GRADE_arm64, 1});
const GradedArchs GradedArchs::arm64e_keysoff_pb = GradedArchs({GRADE_arm64e_pb, 2}, {GRADE_arm64, 1});
const GradedArchs GradedArchs::arm64e            = GradedArchs({GRADE_arm64e,    1});
const GradedArchs GradedArchs::arm64e_pb         = GradedArchs({GRADE_arm64e_pb, 1});
#endif
const GradedArchs GradedArchs::armv7             = GradedArchs({GRADE_armv7,   1});
const GradedArchs GradedArchs::armv7s            = GradedArchs({GRADE_armv7s,  2}, {GRADE_armv7, 1});
const GradedArchs GradedArchs::armv7k            = GradedArchs({GRADE_armv7k,  1});
const GradedArchs GradedArchs::armv7m            = GradedArchs({GRADE_armv7m,  1});
const GradedArchs GradedArchs::armv7em           = GradedArchs({GRADE_armv7em,  1});


#if SUPPORT_ARCH_arm64_32
const GradedArchs GradedArchs::arm64_32          = GradedArchs({GRADE_arm64_32, 1});
#endif
#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
const GradedArchs GradedArchs::launch_AS         = GradedArchs({GRADE_arm64e,  3}, {GRADE_arm64,  2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::launch_AS_Sim     = GradedArchs({GRADE_arm64,   2}, {GRADE_x86_64, 1});
const GradedArchs GradedArchs::launch_Intel_h    = GradedArchs({GRADE_x86_64h, 3}, {GRADE_x86_64, 2}, {GRADE_i386, 1});
const GradedArchs GradedArchs::launch_Intel      = GradedArchs({GRADE_x86_64,  2}, {GRADE_i386,   1});
const GradedArchs GradedArchs::launch_Intel_Sim  = GradedArchs({GRADE_x86_64,  2}, {GRADE_i386,   1});
#endif

int GradedArchs::grade(uint32_t cputype, uint32_t cpusubtype, bool isOSBinary) const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0) { break; }
        if ( (p.type == cputype) && (p.subtype == (cpusubtype & ~CPU_SUBTYPE_MASK)) ) {
            if ( p.osBinary ) {
                if ( isOSBinary )
                    return p.grade;
            }
            else {
                return p.grade;
            }
        }
    }
    return 0;
}

const char* GradedArchs::name() const
{
    return MachOFile::archName(_orderedCpuTypes[0].type, _orderedCpuTypes[0].subtype);
}

void GradedArchs::forEachArch(bool platformBinariesOnly, void (^handler)(const char*)) const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0)
            break;
        if ( p.osBinary && !platformBinariesOnly )
            continue;
        handler(MachOFile::archName(p.type, p.subtype));
    }
}

bool GradedArchs::checksOSBinary() const
{
    for (const auto& p : _orderedCpuTypes) {
        if (p.type == 0) { return false; }
        if ( p.osBinary ) { return true; }
    }
    __builtin_unreachable();
}

bool GradedArchs::supports64() const
{
    return (_orderedCpuTypes.front().type & CPU_ARCH_ABI64) != 0;
}

#if __x86_64__
static bool isHaswell()
{
    // FIXME: figure out a commpage way to check this
    struct host_basic_info info;
    mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
    mach_port_t hostPort = mach_host_self();
    kern_return_t result = host_info(hostPort, HOST_BASIC_INFO, (host_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), hostPort);
    return (result == KERN_SUCCESS) && (info.cpu_subtype == CPU_SUBTYPE_X86_64_H);
}
#endif

const GradedArchs& GradedArchs::forCurrentOS(bool keysOff, bool osBinariesOnly)
{
#if __arm64e__
    if ( osBinariesOnly )
        return (keysOff ? arm64e_keysoff_pb : arm64e_pb);
    else
        return (keysOff ? arm64e_keysoff : arm64e);
#elif __ARM64_ARCH_8_32__
    return arm64_32;
#elif __arm64__
    return arm64;
#elif __ARM_ARCH_7K__
    return armv7k;
#elif __ARM_ARCH_7S__
    return armv7s;
#elif __ARM_ARCH_7A__
    return armv7;
#elif __x86_64__
 #if TARGET_OS_SIMULATOR
    return x86_64;
  #else
    return isHaswell() ? x86_64h : x86_64;
  #endif
#elif __i386__
    return i386;
#else
    #error unknown platform
#endif
}

#if BUILDING_LIBDYLD || BUILDING_UNIT_TESTS
const GradedArchs& GradedArchs::launchCurrentOS(const char* simArches)
{
#if TARGET_OS_SIMULATOR
    // on Apple Silicon, there is both an arm64 and an x86_64 (under rosetta) simulators
    // You cannot tell if you are running under rosetta, so CoreSimulator sets SIMULATOR_ARCHS
    if ( strcmp(simArches, "arm64 x86_64") == 0 )
        return launch_AS_Sim;
    else
        return x86_64;
#elif TARGET_OS_OSX
  #if __arm64__
    return launch_AS;
  #else
    return isHaswell() ? launch_Intel_h : launch_Intel;
  #endif
#else
    // all other platforms use same grading for executables as dylibs
    return forCurrentOS(true, false);
#endif
}
#endif // BUILDING_LIBDYLD

const GradedArchs& GradedArchs::forName(const char* archName, bool keysOff)
{
    if (strcmp(archName, "x86_64h") == 0 )
        return x86_64h;
    else if (strcmp(archName, "x86_64") == 0 )
        return x86_64;
#if SUPPORT_ARCH_arm64e
    else if (strcmp(archName, "arm64e") == 0 )
        return keysOff ? arm64e_keysoff : arm64e;
#endif
    else if (strcmp(archName, "arm64") == 0 )
        return arm64;
    else if (strcmp(archName, "armv7k") == 0 )
        return armv7k;
    else if (strcmp(archName, "armv7s") == 0 )
        return armv7s;
    else if (strcmp(archName, "armv7") == 0 )
        return armv7;
    else if (strcmp(archName, "armv7m") == 0 )
        return armv7m;
    else if (strcmp(archName, "armv7em") == 0 )
        return armv7em;
#if SUPPORT_ARCH_arm64_32
    else if (strcmp(archName, "arm64_32") == 0 )
        return arm64_32;
#endif
    else if (strcmp(archName, "i386") == 0 )
        return i386;
    assert(0 && "unknown arch name");
}



////////////////////////////  MachOFile ////////////////////////////////////////


const MachOFile::ArchInfo MachOFile::_s_archInfos[] = {
    { "x86_64",   CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_ALL  },
    { "x86_64h",  CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_H    },
    { "i386",     CPU_TYPE_I386,     CPU_SUBTYPE_I386_ALL    },
    { "arm64",    CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64_ALL   },
#if SUPPORT_ARCH_arm64e
    { "arm64e",   CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64E     },
#endif
#if SUPPORT_ARCH_arm64_32
    { "arm64_32", CPU_TYPE_ARM64_32, CPU_SUBTYPE_ARM64_32_V8 },
#endif
    { "armv7k",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7K     },
    { "armv7s",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7S     },
    { "armv7",    CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7      },
    { "armv6m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V6M     },
    { "armv7m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7M     },
    { "armv7em",  CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7EM    },
    { "armv8m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V8M     },
};

const MachOFile::PlatformInfo MachOFile::_s_platformInfos[] = {
    { "macOS",              Platform::macOS,                LC_VERSION_MIN_MACOSX   },
    { "iOS",                Platform::iOS,                  LC_VERSION_MIN_IPHONEOS },
    { "tvOS",               Platform::tvOS,                 LC_VERSION_MIN_TVOS     },
    { "watchOS",            Platform::watchOS,              LC_VERSION_MIN_WATCHOS  },
    { "bridgeOS",           Platform::bridgeOS,             LC_BUILD_VERSION        },
    { "MacCatalyst",        Platform::iOSMac,               LC_BUILD_VERSION        },
    { "iOS-sim",            Platform::iOS_simulator,        LC_BUILD_VERSION        },
    { "tvOS-sim",           Platform::tvOS_simulator,       LC_BUILD_VERSION        },
    { "watchOS-sim",        Platform::watchOS_simulator,    LC_BUILD_VERSION        },
    { "driverKit",          Platform::driverKit,            LC_BUILD_VERSION        },
    { "xrOS",               Platform::xrOS,                 LC_BUILD_VERSION        },
    { "xrOS-sim",           Platform::xrOS_simulator,       LC_BUILD_VERSION        },
};



bool MachOFile::is64() const
{
    return (this->magic == MH_MAGIC_64);
}

size_t MachOFile::machHeaderSize() const
{
    return is64() ? sizeof(mach_header_64) : sizeof(mach_header);
}

uint32_t MachOFile::maskedCpuSubtype() const
{
    return (this->cpusubtype & ~CPU_SUBTYPE_MASK);
}

uint32_t MachOFile::pointerSize() const
{
    if (this->magic == MH_MAGIC_64)
        return 8;
    else
        return 4;
}

bool MachOFile::uses16KPages() const
{
    switch (this->cputype) {
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM64_32:
            return true;
        case CPU_TYPE_ARM:
            // iOS is 16k aligned for armv7/armv7s and watchOS armv7k is 16k aligned
            // HACK: Pretend armv7k kexts are 4k aligned
            if ( this->isKextBundle() )
                return false;
            return this->cpusubtype == CPU_SUBTYPE_ARM_V7K;
        default:
            return false;
    }
}

bool MachOFile::isArch(const char* aName) const
{
    return (strcmp(aName, archName(this->cputype, this->cpusubtype)) == 0);
}

const char* MachOFile::archName(uint32_t cputype, uint32_t cpusubtype)
{
    for (const ArchInfo& info : _s_archInfos) {
        if ( (cputype == info.cputype) && ((cpusubtype & ~CPU_SUBTYPE_MASK) == info.cpusubtype) ) {
            return info.name;
        }
    }
    return "unknown";
}

bool MachOFile::cpuTypeFromArchName(const char* archName, cpu_type_t* cputype, cpu_subtype_t* cpusubtype)
{
   for (const ArchInfo& info : _s_archInfos) {
        if ( strcmp(archName, info.name) == 0 ) {
            *cputype    = info.cputype;
            *cpusubtype = info.cpusubtype;
            return true;
       }
    }
    return false;
}

const char* MachOFile::archName() const
{
    return archName(this->cputype, this->cpusubtype);
}

static void appendDigit(char*& s, unsigned& num, unsigned place, bool& startedPrinting)
{
    if ( num >= place ) {
        unsigned dig = (num/place);
        *s++ = '0' + dig;
        num -= (dig*place);
        startedPrinting = true;
    }
    else if ( startedPrinting ) {
        *s++ = '0';
    }
}

static void appendNumber(char*& s, unsigned num)
{
    assert(num < 99999);
    bool startedPrinting = false;
    appendDigit(s, num, 10000, startedPrinting);
    appendDigit(s, num,  1000, startedPrinting);
    appendDigit(s, num,   100, startedPrinting);
    appendDigit(s, num,    10, startedPrinting);
    appendDigit(s, num,     1, startedPrinting);
    if ( !startedPrinting )
        *s++ = '0';
}

void MachOFile::packedVersionToString(uint32_t packedVersion, char versionString[32])
{
    // sprintf(versionString, "%d.%d.%d", (packedVersion >> 16), ((packedVersion >> 8) & 0xFF), (packedVersion & 0xFF));
    char* s = versionString;
    appendNumber(s, (packedVersion >> 16));
    *s++ = '.';
    appendNumber(s, (packedVersion >> 8) & 0xFF);
    if ( (packedVersion & 0xFF) != 0 ) {
        *s++ = '.';
        appendNumber(s, (packedVersion & 0xFF));
    }
    *s++ = '\0';
}

bool MachOFile::builtForPlatform(Platform reqPlatform, bool onlyOnePlatform) const
{
    __block bool foundRequestedPlatform = false;
    __block bool foundOtherPlatform     = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platform == reqPlatform )
            foundRequestedPlatform = true;
        else
            foundOtherPlatform = true;
    });
    // if checking that this binary is built for exactly one platform, fail if more
    if ( foundOtherPlatform && onlyOnePlatform )
        return false;
    if ( foundRequestedPlatform )
        return true;

    // binary has no explict load command to mark platform
    // could be an old macOS binary, look at arch
    if  ( !foundOtherPlatform && (reqPlatform == Platform::macOS) ) {
        if ( this->cputype == CPU_TYPE_X86_64 )
            return true;
        if ( this->cputype == CPU_TYPE_I386 )
            return true;
    }

#if BUILDING_DYLDINFO
    // Allow offline tools to analyze binaries dyld doesn't load, ie, those with platforms
    if ( !foundOtherPlatform && (reqPlatform == Platform::unknown) )
        return true;
#endif

    return false;
}

bool MachOFile::loadableIntoProcess(Platform processPlatform, const char* path, bool internalInstall) const
{
    if ( this->builtForPlatform(processPlatform) )
        return true;

    // Some host macOS dylibs can be loaded into simulator processes
    if ( MachOFile::isSimulatorPlatform(processPlatform) && this->builtForPlatform(Platform::macOS)) {
        static const char* const macOSHost[] = {
            "/usr/lib/system/libsystem_kernel.dylib",
            "/usr/lib/system/libsystem_platform.dylib",
            "/usr/lib/system/libsystem_pthread.dylib",
            "/usr/lib/system/libsystem_platform_debug.dylib",
            "/usr/lib/system/libsystem_pthread_debug.dylib",
            "/usr/lib/system/host/liblaunch_sim.dylib",
        };
        for (const char* libPath : macOSHost) {
            if (strcmp(libPath, path) == 0)
                return true;
        }
    }

    // If this is being called on main executable where we expect a macOS program, Catalyst programs are also runnable
    if ( (this->filetype == MH_EXECUTE) && (processPlatform == Platform::macOS) && this->builtForPlatform(Platform::iOSMac, true) )
        return true;
#if (TARGET_OS_OSX && TARGET_CPU_ARM64)
    if ( (this->filetype == MH_EXECUTE) && (processPlatform == Platform::macOS) && this->builtForPlatform(Platform::iOS, true) )
        return true;
#endif


    bool iOSonMac = (processPlatform == Platform::iOSMac);
#if (TARGET_OS_OSX && TARGET_CPU_ARM64)
    // allow iOS binaries in iOSApp
    if ( processPlatform == Platform::iOS ) {
        // can load Catalyst binaries into iOS process
        if ( this->builtForPlatform(Platform::iOSMac) )
            return true;
        iOSonMac = true;
    }
#endif
    // macOS dylibs can be loaded into iOSMac processes
    if ( (iOSonMac) && this->builtForPlatform(Platform::macOS, true) )
        return true;

    return false;
}

bool MachOFile::isZippered() const
{
    __block bool macOS = false;
    __block bool iOSMac = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platform == Platform::macOS )
            macOS = true;
        else if ( platform == Platform::iOSMac )
            iOSMac = true;
    });
    return macOS && iOSMac;
}

bool MachOFile::inDyldCache() const {
    return (this->flags & MH_DYLIB_IN_CACHE);
}

Platform MachOFile::currentPlatform()
{
#if TARGET_OS_SIMULATOR
  #if TARGET_OS_WATCH
    return Platform::watchOS_simulator;
  #elif TARGET_OS_TV
    return Platform::tvOS_simulator;
  #else
    return Platform::iOS_simulator;
  #endif
#elif TARGET_OS_BRIDGE
    return Platform::bridgeOS;
#elif TARGET_OS_WATCH
    return Platform::watchOS;
#elif TARGET_OS_TV
    return Platform::tvOS;
#elif TARGET_OS_IOS
    return Platform::iOS;
#elif TARGET_OS_OSX
    return Platform::macOS;
#elif TARGET_OS_DRIVERKIT
    return Platform::driverKit;
#else
    #error unknown platform
#endif
}

Platform MachOFile::basePlatform(dyld3::Platform reqPlatform) {
    switch(reqPlatform) {
        case Platform::unknown:               return Platform::unknown;
        case Platform::macOS:                 return Platform::macOS;
        case Platform::iOS:                   return Platform::iOS;
        case Platform::tvOS:                  return Platform::tvOS;
        case Platform::watchOS:               return Platform::watchOS;
        case Platform::bridgeOS:              return Platform::bridgeOS;
        case Platform::iOSMac:                return Platform::iOS;
        case Platform::iOS_simulator:         return Platform::iOS;
        case Platform::tvOS_simulator:        return Platform::tvOS;
        case Platform::watchOS_simulator:     return Platform::watchOS;
        case Platform::driverKit:             return Platform::driverKit;
        default:                              return Platform::unknown;
    }
}


const char* MachOFile::currentArchName()
{
#if __ARM_ARCH_7K__
    return "armv7k";
#elif __ARM_ARCH_7A__
    return "armv7";
#elif __ARM_ARCH_7S__
    return "armv7s";
#elif __arm64e__
    return "arm64e";
#elif __arm64__
#if __LP64__
    return "arm64";
#else
    return "arm64_32";
#endif
#elif __x86_64__
    return isHaswell() ? "x86_64h" : "x86_64";
#elif __i386__
    return "i386";
#else
    #error unknown arch
#endif
}

bool MachOFile::isExclaveKitPlatform(Platform platform, Platform* basePlatform)
{
    switch ( platform ) {
        case Platform::macOSExclaveKit:
            if ( basePlatform )
                *basePlatform = Platform::macOS;
            return true;
        case Platform::iOSExclaveKit:
            if ( basePlatform )
                *basePlatform = Platform::iOS;
            return true;
        case Platform::tvOSExclaveKit:
            if ( basePlatform )
                *basePlatform = Platform::tvOS;
            return true;
       default:
            return false;
    }
}

bool MachOFile::isSimulatorPlatform(Platform platform, Platform* basePlatform)
{
    switch ( platform ) {
        case Platform::iOS_simulator:
            if ( basePlatform )
                *basePlatform = Platform::iOS;
            return true;
        case Platform::watchOS_simulator:
            if ( basePlatform )
                *basePlatform = Platform::watchOS;
            return true;
        case Platform::tvOS_simulator:
            if ( basePlatform )
                *basePlatform = Platform::tvOS;
            return true;
       default:
            return false;
    }
}

bool MachOFile::isBuiltForSimulator() const
{
    __block bool result = false;
    this->forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        switch ( platform ) {
            case Platform::iOS_simulator:
            case Platform::watchOS_simulator:
            case Platform::tvOS_simulator:
                result = true;
                break;
           default:
                break;
        }
    });
    return result;
}

bool MachOFile::isDyld() const
{
    return (this->filetype == MH_DYLINKER);
}

bool MachOFile::isDyldManaged() const {
    switch ( this->filetype ) {
        case MH_BUNDLE:
        case MH_EXECUTE:
        case MH_DYLIB:
            return true;
        default:
            break;
    }
    return false;
}

bool MachOFile::isDylib() const
{
    return (this->filetype == MH_DYLIB);
}

bool MachOFile::isBundle() const
{
    return (this->filetype == MH_BUNDLE);
}

bool MachOFile::isMainExecutable() const
{
    return (this->filetype == MH_EXECUTE);
}

bool MachOFile::isDynamicExecutable() const
{
    if ( this->filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return hasLoadCommand(LC_LOAD_DYLINKER);
}

bool MachOFile::isStaticExecutable() const
{
    if ( this->filetype != MH_EXECUTE )
        return false;

    // static executables do not have dyld load command
    return !hasLoadCommand(LC_LOAD_DYLINKER);
}

bool MachOFile::isKextBundle() const
{
    return (this->filetype == MH_KEXT_BUNDLE);
}

bool MachOFile::isFileSet() const
{
    return (this->filetype == MH_FILESET);
}

bool MachOFile::isPIE() const
{
    return (this->flags & MH_PIE);
}

bool MachOFile::isPreload() const
{
    return (this->filetype == MH_PRELOAD);
}

const char* MachOFile::platformName(Platform reqPlatform)
{
    for (const PlatformInfo& info : _s_platformInfos) {
        if ( info.platform == reqPlatform )
            return info.name;
    }
    return "unknown";
}

void MachOFile::forEachSupportedPlatform(void (^handler)(Platform platform, uint32_t minOS, uint32_t sdk)) const
{
    Diagnostics diag;
    __block bool foundPlatform = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const build_version_command* buildCmd = (build_version_command *)cmd;
        const version_min_command*   versCmd  = (version_min_command*)cmd;
        uint32_t                     sdk;
        switch ( cmd->cmd ) {
            case LC_BUILD_VERSION:
                handler((Platform)(buildCmd->platform), buildCmd->minos, buildCmd->sdk);
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_MACOSX:
                sdk = versCmd->sdk;
                // The original LC_VERSION_MIN_MACOSX did not have an sdk field, assume sdk is same as minOS for those old binaries
                if ( sdk == 0 )
                    sdk = versCmd->version;
                handler(Platform::macOS, versCmd->version, sdk);
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_IPHONEOS:
                if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) )
                    handler(Platform::iOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::iOS, versCmd->version, versCmd->sdk);
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_TVOS:
                if ( this->cputype == CPU_TYPE_X86_64 )
                    handler(Platform::tvOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::tvOS, versCmd->version, versCmd->sdk);
                foundPlatform = true;
                break;
            case LC_VERSION_MIN_WATCHOS:
                if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) )
                    handler(Platform::watchOS_simulator, versCmd->version, versCmd->sdk); // old sim binary
                else
                    handler(Platform::watchOS, versCmd->version, versCmd->sdk);
                foundPlatform = true;
                break;
        }
    });
    if ( !foundPlatform ) {
        // old binary with no explicit platform
#if (BUILDING_DYLD || BUILDING_CLOSURE_UTIL) && TARGET_OS_OSX
        if ( this->cputype == CPU_TYPE_X86_64 )
            handler(Platform::macOS, 0x000A0500, 0x000A0500); // guess it is a macOS 10.5 binary
        // <rdar://problem/75343399>
        // The Go linker emits non-standard binaries without a platform and we have to live with it.
        if ( this->cputype == CPU_TYPE_ARM64 )
            handler(Platform::macOS, 0x000B0000, 0x000B0000); // guess it is a macOS 11.0 binary
#endif
    }
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOFile::forEachSupportedBuildTool(void (^handler)(Platform platform, uint32_t tool, uint32_t version)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_BUILD_VERSION: {
                const build_version_command* buildCmd = (build_version_command *)cmd;
                for ( uint32_t i = 0; i != buildCmd->ntools; ++i ) {
                    uint32_t offset = sizeof(build_version_command) + (i * sizeof(build_tool_version));
                    if ( offset >= cmd->cmdsize )
                        break;

                    const build_tool_version* firstTool = (const build_tool_version*)(&buildCmd[1]);
                    handler((Platform)(buildCmd->platform), firstTool[i].tool, firstTool[i].version);
                }
            }
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}


bool MachOFile::isMachO(Diagnostics& diag, uint64_t fileSize) const
{
    if ( fileSize < sizeof(mach_header) ) {
        diag.error("MachO header exceeds file length");
        return false;
    }

    if ( !hasMachOMagic() ) {
        // old PPC slices are not currently valid "mach-o" but should not cause an error
        if ( !hasMachOBigEndianMagic() )
            diag.error("file does not start with MH_MAGIC[_64]");
        return false;
    }
    if ( this->sizeofcmds + machHeaderSize() > fileSize ) {
        diag.error("load commands exceed length of first segment");
        return false;
    }
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) { });
    return diag.noError();
}


const MachOFile* MachOFile::isMachO(const void* content)
{
    const MachOFile* mf = (MachOFile*)content;
    if ( mf->hasMachOMagic() )
        return mf;
    return nullptr;
}

bool MachOFile::hasMachOMagic() const
{
    return ( (this->magic == MH_MAGIC) || (this->magic == MH_MAGIC_64) );
}

bool MachOFile::hasMachOBigEndianMagic() const
{
    return ( (this->magic == MH_CIGAM) || (this->magic == MH_CIGAM_64) );
}


void MachOFile::forEachLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& stop)) const
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( this->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)this + sizeof(mach_header_64));
    else if ( this->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return;  // can't process big endian mach-o
    else {
        const uint32_t* h = (uint32_t*)this;
        diag.error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h [1]);
        return;  // not a mach-o file
    }
    if ( this->filetype > 12 ) {
        diag.error("unknown mach-o filetype (%u)", this->filetype);
        return;
    }
    const load_command* const cmdsEnd  = (load_command*)((char*)startCmds + this->sizeofcmds);
    const load_command* const cmdsLast = (load_command*)((char*)startCmds + this->sizeofcmds - sizeof(load_command));
    const load_command*       cmd      = startCmds;
    for (uint32_t i = 0; i < this->ncmds; ++i) {
        if ( cmd > cmdsLast ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, extends past sizeofcmds", i, this->ncmds, cmd, this);
            return;
        }
        uint32_t cmdsize = cmd->cmdsize;
        if ( cmdsize < 8 ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) too small", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        if ( (cmdsize % 4) != 0 ) {
            // FIXME: on 64-bit mach-o, should be 8-byte aligned, (might reveal bin-compat issues)
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) not multiple of 4", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        const load_command* nextCmd = (load_command*)((char *)cmd + cmdsize);
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%u of %u at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, this->ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
            return;
        }
        callback(cmd, stop);
        if ( stop )
            return;
        cmd = nextCmd;
    }
}

void MachOFile::removeLoadCommand(Diagnostics& diag, void (^callback)(const load_command* cmd, bool& remove, bool& stop))
{
    bool stop = false;
    const load_command* startCmds = nullptr;
    if ( this->magic == MH_MAGIC_64 )
        startCmds = (load_command*)((char *)this + sizeof(mach_header_64));
    else if ( this->magic == MH_MAGIC )
        startCmds = (load_command*)((char *)this + sizeof(mach_header));
    else if ( hasMachOBigEndianMagic() )
        return;  // can't process big endian mach-o
    else {
        const uint32_t* h = (uint32_t*)this;
        diag.error("file does not start with MH_MAGIC[_64]: 0x%08X 0x%08X", h[0], h [1]);
        return;  // not a mach-o file
    }
    const load_command* const cmdsEnd = (load_command*)((char*)startCmds + this->sizeofcmds);
    auto cmd = (load_command*)startCmds;
    const uint32_t origNcmds = this->ncmds;
    unsigned bytesRemaining = this->sizeofcmds;
    for (uint32_t i = 0; i < origNcmds; ++i) {
        bool remove = false;
        auto nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
        if ( cmd->cmdsize < 8 ) {
            diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) too small", i, this->ncmds, cmd, this, cmd->cmdsize);
            return;
        }
        if ( (nextCmd > cmdsEnd) || (nextCmd < startCmds) ) {
            diag.error("malformed load command #%d of %d at %p with mh=%p, size (0x%X) is too large, load commands end at %p", i, this->ncmds, cmd, this, cmd->cmdsize, cmdsEnd);
            return;
        }
        callback(cmd, remove, stop);
        if ( remove ) {
            this->sizeofcmds -= cmd->cmdsize;
            ::memmove((void*)cmd, (void*)nextCmd, bytesRemaining);
            this->ncmds--;
        } else {
            bytesRemaining -= cmd->cmdsize;
            cmd = nextCmd;
        }
        if ( stop )
            break;
    }
    if ( cmd )
     ::bzero(cmd, bytesRemaining);
}


bool MachOFile::hasObjC() const
{
    __block bool result = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(info.sectName, "__objc_imageinfo") == 0) && (strncmp(info.segInfo.segName, "__DATA", 6) == 0) ) {
            result = true;
            stop = true;
        }
        if ( (this->cputype == CPU_TYPE_I386) && (strcmp(info.sectName, "__image_info") == 0) && (strcmp(info.segInfo.segName, "__OBJC") == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOFile::hasConstObjCSection() const
{
    return hasSection("__DATA_CONST", "__objc_selrefs")
        || hasSection("__DATA_CONST", "__objc_classrefs")
        || hasSection("__DATA_CONST", "__objc_protorefs")
        || hasSection("__DATA_CONST", "__objc_superrefs");
}

bool MachOFile::hasSection(const char* segName, const char* sectName) const
{
    __block bool result = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(info.segInfo.segName, segName) == 0) && (strcmp(info.sectName, sectName) == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

const char* MachOFile::installName() const
{
    const char*  name;
    uint32_t     compatVersion;
    uint32_t     currentVersion;
    if ( getDylibInstallName(&name, &compatVersion, &currentVersion) )
        return name;
    return nullptr;
}

bool MachOFile::getDylibInstallName(const char** installName, uint32_t* compatVersion, uint32_t* currentVersion) const
{
    Diagnostics diag;
    __block bool found = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_ID_DYLIB) || (cmd->cmd == LC_ID_DYLINKER) ) {
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

bool MachOFile::getUuid(uuid_t uuid) const
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

UUID MachOFile::uuid() const {
    Diagnostics diag;
    __block UUID result;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UUID ) {
            const uuid_command* uc = (const uuid_command*)cmd;
            result = UUID(uc->uuid);
            stop = true;
        }
    });
    diag.assertNoError();
    return result;
}

void MachOFile::forEachDependentDylib(void (^callback)(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop)) const
{
    Diagnostics       diag;
    __block unsigned  count   = 0;
    __block bool      stopped = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         switch ( cmd->cmd ) {
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB: {
                const dylib_command* dylibCmd = (dylib_command*)cmd;
                const char* loadPath = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                callback(loadPath, (cmd->cmd == LC_LOAD_WEAK_DYLIB), (cmd->cmd == LC_REEXPORT_DYLIB), (cmd->cmd == LC_LOAD_UPWARD_DYLIB),
                                    dylibCmd->dylib.compatibility_version, dylibCmd->dylib.current_version, stop);
                ++count;
                if ( stop )
                    stopped = true;
            }
            break;
        }
    });
#if !BUILDING_SHARED_CACHE_UTIL && !BUILDING_DYLDINFO && !BUILDING_UNIT_TESTS
    // everything must link with something
    if ( (count == 0) && !stopped ) {
        // The dylibs that make up libSystem can link with nothing
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
#if TARGET_OS_EXCLAVEKIT
        if ( !this->isDylib() || (strncmp(this->installName(), "/System/ExclaveKit/usr/lib/system/", 34) != 0) )
            callback("/System/ExclaveKit/usr/lib/libSystem.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
#else
        if ( this->builtForPlatform(Platform::driverKit, true) ) {
            if ( !this->isDylib() || (strncmp(this->installName(), "/System/DriverKit/usr/lib/system/", 33) != 0) )
                callback("/System/DriverKit/usr/lib/libSystem.B.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
        }
        else {
            if ( !this->isDylib() || (strncmp(this->installName(), "/usr/lib/system/", 16) != 0) )
                callback("/usr/lib/libSystem.B.dylib", false, false, false, 0x00010000, 0x00010000, stopped);
        }
#endif // TARGET_OS_EXCLAVEKIT
    }
#endif // !BUILDING_SHARED_CACHE_UTIL && !BUILDING_DYLDINFO && !BUILDING_UNIT_TESTS
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOFile::forDyldEnv(void (^callback)(const char* envVar, bool& stop)) const
{
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( cmd->cmd == LC_DYLD_ENVIRONMENT ) {
            const dylinker_command* envCmd = (dylinker_command*)cmd;
            const char* keyEqualsValue = (char*)envCmd + envCmd->name.offset;
            // only process variables that start with DYLD_ and end in _PATH
            if ( (strncmp(keyEqualsValue, "DYLD_", 5) == 0) ) {
                const char* equals = strchr(keyEqualsValue, '=');
                if ( equals != NULL ) {
                    callback(keyEqualsValue, stop);
                }
            }
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

bool MachOFile::enforceCompatVersion() const
{
    __block bool result = true;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        switch ( platform ) {
            case Platform::macOS:
                if ( minOS >= 0x000A0E00 )  // macOS 10.14
                    result = false;
                break;
            case Platform::iOS:
            case Platform::tvOS:
            case Platform::iOS_simulator:
            case Platform::tvOS_simulator:
                if ( minOS >= 0x000C0000 )  // iOS 12.0
                    result = false;
                break;
            case Platform::watchOS:
            case Platform::watchOS_simulator:
                if ( minOS >= 0x00050000 )  // watchOS 5.0
                    result = false;
                break;
            case Platform::bridgeOS:
                if ( minOS >= 0x00030000 )  // bridgeOS 3.0
                    result = false;
                break;
            case Platform::driverKit:
            case Platform::iOSMac:
                result = false;
                break;
            case Platform::unknown:
                break;
        }
    });
    return result;
}

const thread_command* MachOFile::unixThreadLoadCommand() const {
    Diagnostics diag;
    __block const thread_command* command = nullptr;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_UNIXTHREAD ) {
            command = (const thread_command*)cmd;
            stop = true;
        }
    });
    return command;
}

const linkedit_data_command* MachOFile::chainedFixupsCmd() const {
    Diagnostics diag;
    __block const linkedit_data_command* command = nullptr;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_DYLD_CHAINED_FIXUPS ) {
            command = (const linkedit_data_command*)cmd;
            stop = true;
        }
    });
    return command;
}


uint32_t MachOFile::entryAddrRegisterIndexForThreadCmd() const
{
    switch ( this->cputype ) {
        case CPU_TYPE_I386:
            return 10; // i386_thread_state_t.eip
        case CPU_TYPE_X86_64:
            return 16; // x86_thread_state64_t.rip
        case CPU_TYPE_ARM:
            return 15; // arm_thread_state_t.pc
        case CPU_TYPE_ARM64:
        case CPU_TYPE_ARM64_32:
            return 32; // arm_thread_state64_t.__pc
    }
    return ~0U;
}

bool MachOFile::use64BitEntryRegs() const
{
    return is64() || isArch("arm64_32");
}

uint64_t MachOFile::entryAddrFromThreadCmd(const thread_command* cmd) const
{
    assert(cmd->cmd == LC_UNIXTHREAD);
    const uint32_t* regs32 = (uint32_t*)(((char*)cmd) + 16);
    const uint64_t* regs64 = (uint64_t*)(((char*)cmd) + 16);

    uint32_t index = entryAddrRegisterIndexForThreadCmd();
    if (index == ~0U)
        return 0;

    return use64BitEntryRegs() ? regs64[index] : regs32[index];
}


bool MachOFile::getEntry(uint64_t& offset, bool& usesCRT) const
{
    Diagnostics diag;
    offset = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_MAIN ) {
            entry_point_command* mainCmd = (entry_point_command*)cmd;
            usesCRT = false;
            offset = mainCmd->entryoff;
            stop = true;
        }
        else if ( cmd->cmd == LC_UNIXTHREAD ) {
            stop = true;
            usesCRT = true;
            uint64_t startAddress = entryAddrFromThreadCmd((thread_command*)cmd);
            offset = startAddress - preferredLoadAddress();
        }
    });
    return (offset != 0);
}


void MachOFile::forEachSegment(void (^callback)(const SegmentInfo& info, bool& stop)) const
{
    Diagnostics diag;
    const bool  intel32  = (this->cputype == CPU_TYPE_I386);
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
            SegmentInfo info;
            info.fileOffset        = segCmd->fileoff;
            info.fileSize          = segCmd->filesize;
            info.vmAddr            = segCmd->vmaddr;
            info.vmSize            = segCmd->vmsize;
            info.sizeOfSections    = sizeOfSections;
            info.segName           = segCmd->segname;
            info.loadCommandOffset = (uint32_t)((uint8_t*)segCmd - (uint8_t*)this);
            info.protections       = segCmd->initprot;
            info.textRelocs        = false;
            info.readOnlyData      = ((segCmd->flags & SG_READ_ONLY) != 0);
            info.isProtected       = (segCmd->flags & SG_PROTECTED_VERSION_1) ? 1 : 0;
            info.hasZeroFill       = (segCmd->initprot == 3) && (segCmd->filesize < segCmd->vmsize);
            info.p2align           = p2align;
            info.segIndex          = segIndex;
            callback(info, stop);
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            bool  hasTextRelocs = false;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
                if ( sect->flags & (S_ATTR_EXT_RELOC|S_ATTR_LOC_RELOC) )
                    hasTextRelocs = true;
           }
            SegmentInfo info;
            info.fileOffset        = segCmd->fileoff;
            info.fileSize          = segCmd->filesize;
            info.vmAddr            = segCmd->vmaddr;
            info.vmSize            = segCmd->vmsize;
            info.sizeOfSections    = sizeOfSections;
            info.segName           = segCmd->segname;
            info.loadCommandOffset = (uint32_t)((uint8_t*)segCmd - (uint8_t*)this);
            info.protections       = segCmd->initprot;
            info.textRelocs        = intel32 && !info.writable() && hasTextRelocs;
            info.readOnlyData      = ((segCmd->flags & SG_READ_ONLY) != 0);
            info.isProtected       = (segCmd->flags & SG_PROTECTED_VERSION_1) ? 1 : 0;
            info.hasZeroFill       = (segCmd->initprot == 3) && (segCmd->filesize < segCmd->vmsize);
            info.p2align           = p2align;
            info.segIndex          = segIndex;
            callback(info, stop);
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

uint64_t MachOFile::preferredLoadAddress() const
{
    __block uint64_t textVmAddr = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            textVmAddr = info.vmAddr;
            stop = true;
        }
    });
    return textVmAddr;
}

void MachOFile::forEachSection(void (^callback)(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop)) const
{
    Diagnostics diag;
    BLOCK_ACCCESSIBLE_ARRAY(char, sectNameCopy, 20);  // read as:  char sectNameCopy[20];
    const bool intel32 = (this->cputype == CPU_TYPE_I386);
    __block uint32_t segIndex = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        SectionInfo sectInfo;
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
            sectInfo.segInfo.fileOffset        = segCmd->fileoff;
            sectInfo.segInfo.fileSize          = segCmd->filesize;
            sectInfo.segInfo.vmAddr            = segCmd->vmaddr;
            sectInfo.segInfo.vmSize            = segCmd->vmsize;
            sectInfo.segInfo.sizeOfSections    = sizeOfSections;
            sectInfo.segInfo.segName           = segCmd->segname;
            sectInfo.segInfo.loadCommandOffset = (uint32_t)((uint8_t*)segCmd - (uint8_t*)this);
            sectInfo.segInfo.protections       = segCmd->initprot;
            sectInfo.segInfo.textRelocs        = false;
            sectInfo.segInfo.readOnlyData      = ((segCmd->flags & SG_READ_ONLY) != 0);
            sectInfo.segInfo.isProtected       = (segCmd->flags & SG_PROTECTED_VERSION_1) ? 1 : 0;
            sectInfo.segInfo.p2align           = p2align;
            sectInfo.segInfo.segIndex          = segIndex;
            for (const section_64* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool malformedSectionRange = (sect->addr < segCmd->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, segCmd->vmaddr + segCmd->filesize);
                sectInfo.sectName       = sectName;
                sectInfo.sectFileOffset = sect->offset;
                sectInfo.sectFlags      = sect->flags;
                sectInfo.sectAddr       = sect->addr;
                sectInfo.sectSize       = sect->size;
                sectInfo.sectAlignP2    = sect->align;
                sectInfo.reserved1      = sect->reserved1;
                sectInfo.reserved2      = sect->reserved2;
                callback(sectInfo, malformedSectionRange, stop);
            }
            ++segIndex;
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* segCmd = (segment_command*)cmd;
            uint64_t sizeOfSections = segCmd->vmsize;
            uint8_t p2align = 0;
            bool  hasTextRelocs = false;
            const section* const sectionsStart = (section*)((char*)segCmd + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[segCmd->nsects];
            for (const section* sect=sectionsStart; sect < sectionsEnd; ++sect) {
                sizeOfSections = sect->addr + sect->size - segCmd->vmaddr;
                if ( sect->align > p2align )
                    p2align = sect->align;
                if ( sect->flags & (S_ATTR_EXT_RELOC|S_ATTR_LOC_RELOC) )
                    hasTextRelocs = true;
            }
            sectInfo.segInfo.fileOffset        = segCmd->fileoff;
            sectInfo.segInfo.fileSize          = segCmd->filesize;
            sectInfo.segInfo.vmAddr            = segCmd->vmaddr;
            sectInfo.segInfo.vmSize            = segCmd->vmsize;
            sectInfo.segInfo.sizeOfSections    = sizeOfSections;
            sectInfo.segInfo.segName           = segCmd->segname;
            sectInfo.segInfo.loadCommandOffset = (uint32_t)((uint8_t*)segCmd - (uint8_t*)this);
            sectInfo.segInfo.protections       = segCmd->initprot;
            sectInfo.segInfo.textRelocs        = intel32 && !sectInfo.segInfo.writable() && hasTextRelocs;
            sectInfo.segInfo.readOnlyData      = ((segCmd->flags & SG_READ_ONLY) != 0);
            sectInfo.segInfo.isProtected       = (segCmd->flags & SG_PROTECTED_VERSION_1) ? 1 : 0;
            sectInfo.segInfo.p2align           = p2align;
            sectInfo.segInfo.segIndex          = segIndex;
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
                const char* sectName = sect->sectname;
                if ( sectName[15] != '\0' ) {
                    strlcpy(sectNameCopy, sectName, 17);
                    sectName = sectNameCopy;
                }
                bool malformedSectionRange = (sect->addr < segCmd->vmaddr) || greaterThanAddOrOverflow(sect->addr, sect->size, segCmd->vmaddr + segCmd->filesize);
                sectInfo.sectName       = sectName;
                sectInfo.sectFileOffset = sect->offset;
                sectInfo.sectFlags      = sect->flags;
                sectInfo.sectAddr       = sect->addr;
                sectInfo.sectSize       = sect->size;
                sectInfo.sectAlignP2    = sect->align;
                sectInfo.reserved1      = sect->reserved1;
                sectInfo.reserved2      = sect->reserved2;
                callback(sectInfo, malformedSectionRange, stop);
            }
            ++segIndex;
        }
    });
    diag.assertNoError();   // any malformations in the file should have been caught by earlier validate() call
}

void MachOFile::forEachInterposingSection(Diagnostics& diag, void (^handler)(uint64_t vmOffset, uint64_t vmSize, bool& stop)) const
{
    const unsigned ptrSize   = pointerSize();
    const unsigned entrySize = 2 * ptrSize;
    forEachSection(^(const MachOFile::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ((info.sectFlags & SECTION_TYPE) == S_INTERPOSING) || ((strcmp(info.sectName, "__interpose") == 0) && ((strncmp(info.segInfo.segName, "__DATA", 6) == 0) || strncmp(info.segInfo.segName, "__AUTH", 6) == 0)) ) {
            if ( info.sectSize % entrySize != 0 ) {
                diag.error("interposing section %s/%s has bad size", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( malformedSectionRange ) {
                diag.error("interposing section %s/%s extends beyond the end of the segment", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            if ( (info.sectAddr % ptrSize) != 0 ) {
                diag.error("interposing section %s/%s is not pointer aligned", info.segInfo.segName, info.sectName);
                stop = true;
                return;
            }
            handler(info.sectAddr - preferredLoadAddress(), info.sectSize, stop);
        }
    });
}

bool MachOFile::isRestricted() const
{
    __block bool result = false;
    forEachSection(^(const MachOFile::SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( (strcmp(info.segInfo.segName, "__RESTRICT") == 0) && (strcmp(info.sectName, "__restrict") == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOFile::hasWeakDefs() const
{
    return (this->flags & MH_WEAK_DEFINES);
}

bool MachOFile::usesWeakDefs() const
{
    return (this->flags & MH_BINDS_TO_WEAK);
}

bool MachOFile::hasThreadLocalVariables() const
{
    return (this->flags & MH_HAS_TLV_DESCRIPTORS);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
static bool endsWith(const char* str, const char* suffix)
{
    size_t strLen    = strlen(str);
    size_t suffixLen = strlen(suffix);
    if ( strLen < suffixLen )
        return false;
    return (strcmp(&str[strLen-suffixLen], suffix) == 0);
}

bool MachOFile::isSharedCacheEligiblePath(const char* dylibName) {
    return (   (strncmp(dylibName, "/usr/lib/", 9) == 0)
            || (strncmp(dylibName, "/System/Library/", 16) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/usr/lib/", 27) == 0)
            || (strncmp(dylibName, "/System/iOSSupport/System/Library/", 34) == 0)
            || (strncmp(dylibName, "/Library/Apple/usr/lib/", 23) == 0)
            || (strncmp(dylibName, "/Library/Apple/System/Library/", 30) == 0)
            || (strncmp(dylibName, "/System/DriverKit/", 18) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/usr/lib/", 29) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/Library/", 36) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/iOSSupport/usr/lib/", 47) == 0)
            || (strncmp(dylibName, "/System/Cryptexes/OS/System/iOSSupport/System/Library/", 54) == 0)
            || (strncmp(dylibName, "/System/ExclaveKit/usr/lib/", 27) == 0)
            || (strncmp(dylibName, "/System/ExclaveKit/System/Library/", 34) == 0));
}

static bool startsWith(const char* buffer, const char* valueToFind) {
    return strncmp(buffer, valueToFind, strlen(valueToFind)) == 0;
}

static bool platformExcludesSharedCache_macOS(const char* installName) {
    // Note: This function basically matches dontCache() from update dyld shared cache

    if ( startsWith(installName, "/usr/lib/system/introspection/") )
        return true;
    if ( startsWith(installName, "/System/Library/QuickTime/") )
        return true;
    if ( startsWith(installName, "/System/Library/Tcl/") )
        return true;
    if ( startsWith(installName, "/System/Library/Perl/") )
        return true;
    if ( startsWith(installName, "/System/Library/MonitorPanels/") )
        return true;
    if ( startsWith(installName, "/System/Library/Accessibility/") )
        return true;
    if ( startsWith(installName, "/usr/local/") )
        return true;
    if ( startsWith(installName, "/usr/lib/pam/") )
        return true;
    // We no longer support ROSP, so skip all paths which start with the special prefix
    if ( startsWith(installName, "/System/Library/Templates/Data/") )
        return true;

    // anything inside a .app bundle is specific to app, so should not be in shared cache
    if ( strstr(installName, ".app/") != NULL )
        return true;

    // Depends on UHASHelloExtensionPoint-macOS which is not always cache eligible
    if ( !strcmp(installName, "/System/Library/PrivateFrameworks/HelloWorldMacHelper.framework/Versions/A/HelloWorldMacHelper") )
        return true;

    return false;
}

static bool platformExcludesSharedCache_iOS(const char* installName) {
    if ( strcmp(installName, "/System/Library/Caches/com.apple.xpc/sdk.dylib") == 0 )
        return true;
    if ( strcmp(installName, "/System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib") == 0 )
        return true;
    return false;
}

// Returns true if the current platform requires that this install name be excluded from the shared cache
// Note that this overrides any exclusion from anywhere else.
static bool platformExcludesSharedCache(Platform platform, const char* installName) {
    if ( (platform == dyld3::Platform::macOS) || (platform == dyld3::Platform::iOSMac) )
        return platformExcludesSharedCache_macOS(installName);
    // Everything else is based on iOS so just use that value
    return platformExcludesSharedCache_iOS(installName);
}

bool MachOFile::canBePlacedInDyldCache(const char* path, void (^failureReason)(const char* format, ...)) const
{
    if ( !isSharedCacheEligiblePath(path) ) {
        // Dont spam the user with an error about paths when we know these are never eligible.
        return false;
    }

    // only dylibs can go in cache
    if ( !this->isDylib() && !this->isDyld() ) {
        failureReason("Not MH_DYLIB");
        return false; // cannot continue, installName() will assert() if not a dylib
    }


    const char* dylibName = installName();
    if ( dylibName[0] != '/' ) {
        failureReason("install name not an absolute path");
        // Don't continue as we don't want to spam the log with errors we don't need.
        return false;
    }
    else if ( strcmp(dylibName, path) != 0 ) {
        failureReason("install path does not match install name");
        return false;
    }
    else if ( strstr(dylibName, "//") != 0 ) {
        failureReason("install name should not include //");
        return false;
    }
    else if ( strstr(dylibName, "./") != 0 ) {
        failureReason("install name should not include ./");
        return false;
    }

    __block bool platformExcludedFile = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platformExcludedFile )
            return;
        if ( platformExcludesSharedCache(platform, dylibName) ) {
            platformExcludedFile = true;
            return;
        }
    });
    if ( platformExcludedFile ) {
        failureReason("install name is not shared cache eligible on platform");
        return false;
    }

    // flat namespace files cannot go in cache
    if ( (this->flags & MH_TWOLEVEL) == 0 ) {
        failureReason("Not built with two level namespaces");
        return false;
    }

    // don't put debug variants into dyld cache
    if ( endsWith(path, "_profile.dylib") || endsWith(path, "_debug.dylib") || endsWith(path, "_asan.dylib")
        || endsWith(path, "_profile") || endsWith(path, "_debug") || endsWith(path, "/CoreADI") ) {
        failureReason("Variant image");
        return false;
    }

    // dylib must have extra info for moving DATA and TEXT segments apart
    __block bool hasExtraInfo = false;
    __block bool hasDyldInfo = false;
    __block bool hasExportTrie = false;
    __block Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_SPLIT_INFO )
            hasExtraInfo = true;
        if ( cmd->cmd == LC_DYLD_INFO_ONLY )
            hasDyldInfo = true;
        if ( cmd->cmd == LC_DYLD_EXPORTS_TRIE )
            hasExportTrie = true;
    });
    if ( !hasExtraInfo ) {
        std::string_view ignorePaths[] = {
            "/usr/lib/libobjc-trampolines.dylib",
            "/usr/lib/libffi-trampolines.dylib"
        };
        for ( std::string_view ignorePath : ignorePaths ) {
            if ( ignorePath == path )
                return false;
        }
        failureReason("Missing split seg info");
        return false;
    }
    if ( !hasDyldInfo && !hasExportTrie ) {
        failureReason("Old binary, missing dyld info or export trie");
        return false;
    }

    // dylib can only depend on other dylibs in the shared cache
    __block const char* badDep = nullptr;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        // Skip weak links.  They are allowed to be missing
        if ( isWeak )
            return;
        if ( !isSharedCacheEligiblePath(loadPath) ) {
            badDep = loadPath;
            stop = true;
        }
    });
    if ( badDep != nullptr ) {
        failureReason("Depends on dylibs ineligible for dyld cache '%s'.  (cache dylibs must start /usr/lib or /System/Library or similar)",
                      badDep);
        return false;
    }

    // dylibs with interposing info cannot be in cache
    if ( hasInterposingTuples() ) {
        failureReason("Has interposing tuples");
        return false;
    }

    // Temporarily kick out swift binaries out of dyld cache on watchOS simulators as they have missing split seg
    if ( (this->cputype == CPU_TYPE_I386) && builtForPlatform(Platform::watchOS_simulator) ) {
        if ( strncmp(dylibName, "/usr/lib/swift/", 15) == 0 ) {
            failureReason("i386 swift binary");
            return false;
        }
    }

    // These used to be in MachOAnalyzer
    __block bool passedLinkeditChecks = false;
    this->withFileLayout(diag, ^(const mach_o::Layout &layout) {

        mach_o::SplitSeg splitSeg(layout);
        mach_o::Fixups fixups(layout);

        // arm64e requires split seg v2 as the split seg code can't handle chained fixups for split seg v1
        if ( isArch("arm64e") ) {
            if ( !splitSeg.isV2() ) {
                failureReason("chained fixups requires split seg v2");
                return;
            }
        }

        // evict swift dylibs with split seg v1 info
        if ( layout.isSwiftLibrary() && splitSeg.isV1() )
            return;

        if ( splitSeg.isV1() ) {
            // Split seg v1 can only support 1 __DATA, and no other writable segments
            __block bool foundBadSegment = false;
            forEachSegment(^(const SegmentInfo& info, bool& stop) {
                if ( info.protections == (VM_PROT_READ | VM_PROT_WRITE) ) {
                    if ( strcmp(info.segName, "__DATA") == 0 )
                        return;

                    failureReason("RW segments other than __DATA requires split seg v2");
                    foundBadSegment = true;
                    stop = true;
                }
            });

            if ( foundBadSegment )
                return;
        }

        // <rdar://problem/57769033> dyld_cache_patchable_location only supports addend in range 0..31
        // rdar://96164956 (dyld needs to support arbitrary addends in cache patch table)
        const bool is64bit = is64();
        __block bool addendTooLarge = false;
        const uint64_t tooLargeRegularAddend = 1 << 23;
        const uint64_t tooLargeAuthAddend = 1 << 5;
        if ( this->hasChainedFixups() ) {

            // with chained fixups, addends can be in the import table or embedded in a bind pointer
            __block std::vector<uint64_t> targetAddends;
            fixups.forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
                if ( is64bit )
                    addend &= 0x00FFFFFFFFFFFFFF; // ignore TBI
                targetAddends.push_back(addend);
            });
            // check each pointer for embedded addend
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                fixups.forEachFixupInAllChains(diag, starts, false, ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                    switch (segInfo->pointer_format) {
                        case DYLD_CHAINED_PTR_ARM64E:
                        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
                            if ( fixupLoc->arm64e.bind.bind ) {
                                uint64_t ordinal = fixupLoc->arm64e.bind.ordinal;
                                uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                                if ( fixupLoc->arm64e.bind.auth ) {
                                    if ( addend >= tooLargeAuthAddend ) {
                                        addendTooLarge = true;
                                        stop = true;
                                    }
                                } else {
                                    addend += fixupLoc->arm64e.signExtendedAddend();
                                    if ( addend >= tooLargeRegularAddend ) {
                                        addendTooLarge = true;
                                        stop = true;
                                    }
                                }
                            }
                            break;
                        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                            if ( fixupLoc->arm64e.bind24.bind ) {
                                uint64_t ordinal = fixupLoc->arm64e.bind24.ordinal;
                                uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                                if ( fixupLoc->arm64e.bind24.auth ) {
                                    if ( addend >= tooLargeAuthAddend ) {
                                        addendTooLarge = true;
                                        stop = true;
                                    }
                                } else {
                                    addend += fixupLoc->arm64e.signExtendedAddend();
                                    if ( addend >= tooLargeRegularAddend ) {
                                        addendTooLarge = true;
                                        stop = true;
                                    }
                                }
                            }
                            break;
                        case DYLD_CHAINED_PTR_64:
                        case DYLD_CHAINED_PTR_64_OFFSET: {
                            if ( fixupLoc->generic64.rebase.bind ) {
                                uint64_t ordinal = fixupLoc->generic64.bind.ordinal;
                                uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                                addend += fixupLoc->generic64.bind.addend;
                                if ( addend >= tooLargeRegularAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            }
                            break;
                        }
                        case DYLD_CHAINED_PTR_32:
                            if ( fixupLoc->generic32.bind.bind ) {
                                uint64_t ordinal = fixupLoc->generic32.bind.ordinal;
                                uint64_t addend = (ordinal < targetAddends.size()) ? targetAddends[ordinal] : 0;
                                addend += fixupLoc->generic32.bind.addend;
                                if ( addend >= tooLargeRegularAddend ) {
                                    addendTooLarge = true;
                                    stop = true;
                                }
                            }
                            break;
                    }
                });
            });
        }
        else {
            // scan bind opcodes for large addend
            auto handler = ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                uint64_t addend = info.addend;
                if ( is64bit )
                    addend &= 0x00FFFFFFFFFFFFFF; // ignore TBI
                if ( addend >= tooLargeRegularAddend ) {
                    addendTooLarge = true;
                    stop = true;
                }
            };
            fixups.forEachBindTarget_Opcodes(diag, true, handler, handler);
        }
        if ( addendTooLarge ) {
            failureReason("bind addend too large");
            return;
        }

        if ( (isArch("x86_64") || isArch("x86_64h")) ) {
            __block bool rebasesOk = true;
            uint64_t startVMAddr = preferredLoadAddress();
            uint64_t endVMAddr = startVMAddr + mappedSize();
            fixups.forEachRebase(diag, ^(uint64_t runtimeOffset, uint64_t rebasedValue, bool &stop) {
                // We allow TBI for x86_64 dylibs, but then require that the remainder of the offset
                // is a 32-bit offset from the mach-header.
                rebasedValue &= 0x00FFFFFFFFFFFFFFULL;
                if ( (rebasedValue < startVMAddr) || (rebasedValue >= endVMAddr) ) {
                    failureReason("rebase value out of range of dylib");
                    rebasesOk = false;
                    stop = true;
                    return;
                }

                // Also error if the rebase location is anything other than 4/8 byte aligned
                if ( (runtimeOffset & 0x3) != 0 ) {
                    failureReason("rebase value is not 4-byte aligned");
                    rebasesOk = false;
                    stop = true;
                    return;
                }

                // Error if the fixup will cross a page
                if ( (runtimeOffset & 0xFFF) == 0xFFC ) {
                    failureReason("rebase value crosses page boundary");
                    rebasesOk = false;
                    stop = true;
                    return;
                }
            });

            if ( !rebasesOk )
                return;

            if ( this->hasChainedFixups() ) {
                fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                    fixups.forEachFixupInAllChains(diag, starts, false, ^(mach_o::ChainedFixupPointerOnDisk* fixupLoc, uint64_t fixupSegmentOffset, const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                        if ( (fixupSegmentOffset & 0xFFF) == 0xFFC ) {
                            failureReason("chained fixup crosses page boundary");
                            rebasesOk = false;
                            stop = true;
                            return;
                        }
                    });
                });
            }

            if ( !rebasesOk )
                return;
        }

        // Check that shared cache dylibs don't use undefined lookup
        {
            __block bool bindsOk = true;

            auto checkBind = ^(int libOrdinal, bool& stop) {
                if ( libOrdinal == BIND_SPECIAL_DYLIB_FLAT_LOOKUP ) {
                    failureReason("has dynamic_lookup binds");
                    bindsOk = false;
                    stop = true;
                }
            };

            if (hasChainedFixups()) {
                fixups.forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
                    checkBind(libOrdinal, stop);
                });
            } else {
                auto handler = ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                    checkBind(info.libOrdinal, stop);
                };
                fixups.forEachBindTarget_Opcodes(diag, true, handler, handler);
            }

            if ( !bindsOk )
                return;
        }

        passedLinkeditChecks = true;
    });

    return passedLinkeditChecks;
}

// Returns true if the executable path is eligible for a PrebuiltLoader on the given platform.
bool MachOFile::canHavePrebuiltExecutableLoader(dyld3::Platform platform, const std::string_view& path,
                                                void (^failureReason)(const char*)) const
{
    // For now we can't build prebuilt loaders for the simulator
    if ( isSimulatorPlatform(platform) ) {
        // Don't spam with tons of messages about executables
        return false;
    }

    if ( (platform == dyld3::Platform::macOS) || (platform == dyld3::Platform::iOSMac) ) {
        // We no longer support ROSP, so skip all paths which start with the special prefix
        if ( path.starts_with("/System/Library/Templates/Data/") ) {
            // Dont spam the user with an error about paths when we know these are never eligible.
            return false;
        }

        static const char* sAllowedPrefixes[] = {
            "/bin/",
            "/sbin/",
            "/usr/",
            "/System/",
            "/Library/Apple/System/",
            "/Library/Apple/usr/",
            "/System/Applications/Safari.app/",
            "/Library/CoreMediaIO/Plug-Ins/DAL/" // temp until plugins moved or closured working
        };

        bool inSearchDir = false;
        for ( const char* searchDir : sAllowedPrefixes ) {
            if ( path.starts_with(searchDir) ) {
                inSearchDir = true;
                break;
            }
        }

        if ( !inSearchDir ) {
            failureReason("path not eligible");
            return false;
        }
    } else {
        // On embedded, only staged apps are excluded.  They will run from a different location at runtime
        if ( path.find("/staged_system_apps/") != std::string::npos ) {
            // Dont spam the user with an error about paths when we know these are never eligible.
            return false;
        }
    }

    if ( !hasCodeSignature() ) {
        failureReason("missing code signature");
        return false;
    }

    return true;
}
#endif

#if BUILDING_APP_CACHE_UTIL
bool MachOFile::canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const
{
    // only dylibs and the kernel itself can go in cache
    if ( this->filetype == MH_EXECUTE ) {
        // xnu
    } else if ( this->isKextBundle() ) {
        // kext's
    } else {
        failureReason("Not MH_KEXT_BUNDLE");
        return false;
    }

    if ( this->filetype == MH_EXECUTE ) {
        // xnu

        // two-level namespace binaries cannot go in cache
        if ( (this->flags & MH_TWOLEVEL) != 0 ) {
            failureReason("Built with two level namespaces");
            return false;
        }

        // xnu kernel cannot have a page zero
        __block bool foundPageZero = false;
        forEachSegment(^(const SegmentInfo &segmentInfo, bool &stop) {
            if ( strcmp(segmentInfo.segName, "__PAGEZERO") == 0 ) {
                foundPageZero = true;
                stop = true;
            }
        });
        if (foundPageZero) {
            failureReason("Has __PAGEZERO");
            return false;
        }

        // xnu must have an LC_UNIXTHREAD to point to the entry point
        __block bool foundMainLC = false;
        __block bool foundUnixThreadLC = false;
        Diagnostics diag;
        forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
            if ( cmd->cmd == LC_MAIN ) {
                foundMainLC = true;
                stop = true;
            }
            else if ( cmd->cmd == LC_UNIXTHREAD ) {
                foundUnixThreadLC = true;
            }
        });
        if (foundMainLC) {
            failureReason("Found LC_MAIN");
            return false;
        }
        if (!foundUnixThreadLC) {
            failureReason("Expected LC_UNIXTHREAD");
            return false;
        }

        if (diag.hasError()) {
            failureReason("Error parsing load commands");
            return false;
        }

        // The kernel should be a static executable, not a dynamic one
        if ( !isStaticExecutable() ) {
            failureReason("Expected static executable");
            return false;
        }

        // The kernel must be built with -pie
        if ( !isPIE() ) {
            failureReason("Expected pie");
            return false;
        }
    }

    if ( isArch("arm64e") && isKextBundle() && !hasChainedFixups() ) {
        failureReason("Missing fixup information");
        return false;
    }

    // dylibs with interposing info cannot be in cache
    if ( hasInterposingTuples() ) {
        failureReason("Has interposing tuples");
        return false;
    }

    // Only x86_64 is allowed to have RWX segments
    if ( !isArch("x86_64") && !isArch("x86_64h") ) {
        __block bool foundBadSegment = false;
        forEachSegment(^(const SegmentInfo &info, bool &stop) {
            if ( (info.protections & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_WRITE | VM_PROT_EXECUTE) ) {
                failureReason("Segments are not allowed to be both writable and executable");
                foundBadSegment = true;
                stop = true;
            }
        });
        if ( foundBadSegment )
            return false;
    }

    return true;
}
#endif // BUILDING_APP_CACHE_UTIL

#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
bool MachOFile::usesClassicRelocationsInKernelCollection() const {
    // The xnu x86_64 static executable needs to do the i386->x86_64 transition
    // so will be emitted with classic relocations
    if ( isArch("x86_64") || isArch("x86_64h") ) {
        return isStaticExecutable() || isFileSet();
    }
    return false;
}
#endif // BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
static bool platformExcludesPrebuiltClosure_macOS(const char* path) {
    // We no longer support ROSP, so skip all paths which start with the special prefix
    if ( startsWith(path, "/System/Library/Templates/Data/") )
        return true;

    // anything inside a .app bundle is specific to app, so should not get a prebuilt closure
    if ( strstr(path, ".app/") != NULL )
        return true;

    return false;
}

static bool platformExcludesPrebuiltClosure_iOS(const char* path) {
    if ( strcmp(path, "/System/Library/Caches/com.apple.xpc/sdk.dylib") == 0 )
        return true;
    if ( strcmp(path, "/System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib") == 0 )
        return true;
    return false;
}

// Returns true if the current platform requires that this install name be excluded from the shared cache
// Note that this overrides any exclusion from anywhere else.
static bool platformExcludesPrebuiltClosure(Platform platform, const char* path) {
    if ( MachOFile::isSimulatorPlatform(platform) )
        return false;
    if ( (platform == dyld3::Platform::macOS) || (platform == dyld3::Platform::iOSMac) )
        return platformExcludesPrebuiltClosure_macOS(path);
    // Everything else is based on iOS so just use that value
    return platformExcludesPrebuiltClosure_iOS(path);
}

bool MachOFile::canHavePrecomputedDlopenClosure(const char* path, void (^failureReason)(const char*)) const
{
    __block bool retval = true;

    // only dylibs can go in cache
    if ( (this->filetype != MH_DYLIB) && (this->filetype != MH_BUNDLE) ) {
        retval = false;
        failureReason("not MH_DYLIB or MH_BUNDLE");
    }

    // flat namespace files cannot go in cache
    if ( (this->flags & MH_TWOLEVEL) == 0 ) {
        retval = false;
        failureReason("not built with two level namespaces");
    }

    // can only depend on other dylibs with absolute paths
    __block bool allDepPathsAreGood = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        if ( loadPath[0] != '/' ) {
            allDepPathsAreGood = false;
            stop = true;
        }
    });
    if ( !allDepPathsAreGood ) {
        retval = false;
        failureReason("depends on dylibs that are not absolute paths");
    }

    __block bool platformExcludedFile = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        if ( platformExcludedFile )
            return;
        if ( platformExcludesPrebuiltClosure(platform, path) ) {
            platformExcludedFile = true;
            return;
        }
    });
    if ( platformExcludedFile ) {
        failureReason("file cannot get a prebuilt closure on this platform");
        return false;
    }

    // dylibs with interposing info cannot have dlopen closure pre-computed
    if ( hasInterposingTuples() ) {
        retval = false;
        failureReason("has interposing tuples");
    }

    // special system dylib overrides cannot have closure pre-computed
    if ( strncmp(path, "/usr/lib/system/introspection/", 30) == 0 ) {
        retval = false;
        failureReason("override of OS dylib");
    }

    return retval;
}
#endif

bool MachOFile::hasInterposingTuples() const
{
    __block bool hasInterposing = false;
    Diagnostics diag;
    forEachInterposingSection(diag, ^(uint64_t vmOffset, uint64_t vmSize, bool &stop) {
        hasInterposing = true;
        stop = true;
    });
    return hasInterposing;
}

bool MachOFile::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const
{
    if ( const encryption_info_command* encCmd = findFairPlayEncryptionLoadCommand() ) {
       if ( encCmd->cryptid == 1 ) {
            // Note: cryptid is 0 in just-built apps.  The AppStore sets cryptid to 1
            textOffset = encCmd->cryptoff;
            size       = encCmd->cryptsize;
            return true;
        }
    }
    textOffset = 0;
    size = 0;
    return false;
}

bool MachOFile::canBeFairPlayEncrypted() const
{
    return (findFairPlayEncryptionLoadCommand() != nullptr);
}

const encryption_info_command* MachOFile::findFairPlayEncryptionLoadCommand() const
{
    __block const encryption_info_command* result = nullptr;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
         if ( (cmd->cmd == LC_ENCRYPTION_INFO) || (cmd->cmd == LC_ENCRYPTION_INFO_64) ) {
            result = (encryption_info_command*)cmd;
            stop = true;
        }
    });
    if ( diag.noError() )
        return result;
    else
        return nullptr;
}


bool MachOFile::hasLoadCommand(uint32_t cmdNum) const
{
    __block bool hasLC = false;
    Diagnostics diag;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == cmdNum ) {
            hasLC = true;
            stop = true;
        }
    });
    return hasLC;
}

bool MachOFile::allowsAlternatePlatform() const
{
    __block bool result = false;
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(info.sectName, "__allow_alt_plat") == 0) && (strncmp(info.segInfo.segName, "__DATA", 6) == 0) ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOFile::hasChainedFixups() const
{
#if SUPPORT_ARCH_arm64e
    // arm64e always uses chained fixups
    if ( (this->cputype == CPU_TYPE_ARM64) && (this->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) ) {
        // Not all binaries have fixups at all so check for the load commands
        return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
    }
#endif
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool MachOFile::hasChainedFixupsLoadCommand() const
{
    return hasLoadCommand(LC_DYLD_CHAINED_FIXUPS);
}

bool MachOFile::hasOpcodeFixups() const
{
    return hasLoadCommand(LC_DYLD_INFO_ONLY) || hasLoadCommand(LC_DYLD_INFO) ;
}

uint16_t MachOFile::chainedPointerFormat(const dyld_chained_fixups_header* header)
{
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)header + header->starts_offset);
    for (uint32_t i=0; i < startsInfo->seg_count; ++i) {
        uint32_t segInfoOffset = startsInfo->seg_info_offset[i];
        // 0 offset means this segment has no fixups
        if ( segInfoOffset == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + segInfoOffset);
        if ( segInfo->page_count != 0 )
            return segInfo->pointer_format;
    }
    return 0;  // no chains (perhaps no __DATA segment)
}

// find dyld_chained_starts_in_image* in image
// if old arm64e binary, synthesize dyld_chained_starts_in_image*
void MachOFile::withChainStarts(Diagnostics& diag, const dyld_chained_fixups_header* chainHeader, void (^callback)(const dyld_chained_starts_in_image*))
{
    if ( chainHeader == nullptr ) {
        diag.error("Must pass in a chain header");
        return;
    }
    // we have a pre-computed offset into LINKEDIT for dyld_chained_starts_in_image
    callback((dyld_chained_starts_in_image*)((uint8_t*)chainHeader + chainHeader->starts_offset));
}

void MachOFile::forEachFixupChainSegment(Diagnostics& diag, const dyld_chained_starts_in_image* starts,
                                           void (^handler)(const dyld_chained_starts_in_segment* segInfo, uint32_t segIndex, bool& stop))
{
    bool stopped = false;
    for (uint32_t segIndex=0; segIndex < starts->seg_count && !stopped; ++segIndex) {
        if ( starts->seg_info_offset[segIndex] == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)starts + starts->seg_info_offset[segIndex]);
        handler(segInfo, segIndex, stopped);
    }
}


bool MachOFile::walkChain(Diagnostics& diag, ChainedFixupPointerOnDisk* chain, uint16_t pointer_format, bool notifyNonPointers, uint32_t max_valid_pointer,
                          void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop))
{
    const unsigned stride = ChainedFixupPointerOnDisk::strideSize(pointer_format);
    bool  stop = false;
    bool  chainEnd = false;
    while (!stop && !chainEnd) {
        // copy chain content, in case handler modifies location to final value
        ChainedFixupPointerOnDisk chainContent = *chain;
        handler(chain, stop);

        if ( !stop ) {
            switch (pointer_format) {
                case DYLD_CHAINED_PTR_ARM64E:
                case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                case DYLD_CHAINED_PTR_ARM64E_USERLAND:
                case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
                    if ( chainContent.arm64e.rebase.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.arm64e.rebase.next*stride);
                    break;
                case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
                    if ( chainContent.cache64e.regular.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.cache64e.regular.next*stride);
                    break;
                case DYLD_CHAINED_PTR_64:
                case DYLD_CHAINED_PTR_64_OFFSET:
                    if ( chainContent.generic64.rebase.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.generic64.rebase.next*4);
                    break;
                case DYLD_CHAINED_PTR_32:
                    if ( chainContent.generic32.rebase.next == 0 )
                        chainEnd = true;
                    else {
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.generic32.rebase.next*4);
                        if ( !notifyNonPointers ) {
                            while ( (chain->generic32.rebase.bind == 0) && (chain->generic32.rebase.target > max_valid_pointer) ) {
                                // not a real pointer, but a non-pointer co-opted into chain
                                chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chain->generic32.rebase.next*4);
                            }
                        }
                    }
                    break;
                case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
                case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
                    if ( chainContent.kernel64.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.kernel64.next*stride);
                    break;
                case DYLD_CHAINED_PTR_32_FIRMWARE:
                    if ( chainContent.firmware32.next == 0 )
                        chainEnd = true;
                    else
                        chain = (ChainedFixupPointerOnDisk*)((uint8_t*)chain + chainContent.firmware32.next*4);
                    break;
                default:
                    diag.error("unknown pointer format 0x%04X", pointer_format);
                    stop = true;
            }
        }
    }
    return stop;
}

void MachOFile::forEachFixupInSegmentChains(Diagnostics& diag, const dyld_chained_starts_in_segment* segInfo,
                                            bool notifyNonPointers, uint8_t* segmentContent,
                                            void (^handler)(ChainedFixupPointerOnDisk* fixupLocation, bool& stop))
{
    bool stopped = false;
    for (uint32_t pageIndex=0; pageIndex < segInfo->page_count && !stopped; ++pageIndex) {
        uint16_t offsetInPage = segInfo->page_start[pageIndex];
        if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
            continue;
        if ( offsetInPage & DYLD_CHAINED_PTR_START_MULTI ) {
            // 32-bit chains which may need multiple starts per page
            uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
            bool chainEnd = false;
            while (!stopped && !chainEnd) {
                chainEnd = (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST);
                offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                uint8_t* pageContentStart = segmentContent + (pageIndex * segInfo->page_size);
                ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);
                stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, handler);
                ++overflowIndex;
            }
        }
        else {
            // one chain per page
            uint8_t* pageContentStart = segmentContent + (pageIndex * segInfo->page_size);
            ChainedFixupPointerOnDisk* chain = (ChainedFixupPointerOnDisk*)(pageContentStart+offsetInPage);
            stopped = walkChain(diag, chain, segInfo->pointer_format, notifyNonPointers, segInfo->max_valid_pointer, handler);
        }
    }
}

void MachOFile::forEachChainedFixupTarget(Diagnostics& diag, const dyld_chained_fixups_header* header,
                                          const linkedit_data_command* chainedFixups,
                                          void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop))
{
    if ( (header->imports_offset > chainedFixups->datasize) || (header->symbols_offset > chainedFixups->datasize) ) {
        diag.error("malformed import table");
        return;
    }

    bool stop    = false;

    const dyld_chained_import*          imports;
    const dyld_chained_import_addend*   importsA32;
    const dyld_chained_import_addend64* importsA64;
    const char*                         symbolsPool     = (char*)header + header->symbols_offset;
    uint32_t                            maxSymbolOffset = chainedFixups->datasize - header->symbols_offset;
    int                                 libOrdinal;
    switch (header->imports_format) {
        case DYLD_CHAINED_IMPORT:
            imports = (dyld_chained_import*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[imports[i].name_offset];
                if ( imports[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint8_t libVal = imports[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, 0, imports[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND:
            importsA32 = (dyld_chained_import_addend*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA32[i].name_offset];
                if ( importsA32[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint8_t libVal = importsA32[i].lib_ordinal;
                if ( libVal > 0xF0 )
                    libOrdinal = (int8_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA32[i].addend, importsA32[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
        case DYLD_CHAINED_IMPORT_ADDEND64:
            importsA64 = (dyld_chained_import_addend64*)((uint8_t*)header + header->imports_offset);
            for (uint32_t i=0; i < header->imports_count && !stop; ++i) {
                const char* symbolName = &symbolsPool[importsA64[i].name_offset];
                if ( importsA64[i].name_offset > maxSymbolOffset ) {
                    diag.error("malformed import table, string overflow");
                    return;
                }
                uint16_t libVal = importsA64[i].lib_ordinal;
                if ( libVal > 0xFFF0 )
                    libOrdinal = (int16_t)libVal;
                else
                    libOrdinal = libVal;
                callback(libOrdinal, symbolName, importsA64[i].addend, importsA64[i].weak_import, stop);
                if ( stop )
                    return;
            }
            break;
       default:
            diag.error("unknown imports format");
            return;
    }
}

uint64_t MachOFile::read_uleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
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


int64_t MachOFile::read_sleb128(Diagnostics& diag, const uint8_t*& p, const uint8_t* end)
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
    if ( ((byte & 0x40) != 0) && (bit < 64) )
        result |= (~0ULL) << bit;
    return result;
}

static void getArchNames(const GradedArchs& archs, bool isOSBinary, char buffer[256])
{
    buffer[0] = '\0';
    archs.forEachArch(isOSBinary, ^(const char* archName) {
        if ( buffer[0] != '\0' )
            strlcat(buffer, "' or '", 256);
        strlcat(buffer, archName, 256);
    });
}

const MachOFile* MachOFile::compatibleSlice(Diagnostics& diag, const void* fileContent, size_t contentSize, const char* path, Platform platform, bool isOSBinary, const GradedArchs& archs, bool internalInstall)
{
    const MachOFile* mf = nullptr;
    if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(fileContent) ) {
        uint64_t  sliceOffset;
        uint64_t  sliceLen;
        bool      missingSlice;
        if ( ff->isFatFileWithSlice(diag, contentSize, archs, isOSBinary, sliceOffset, sliceLen, missingSlice) ) {
            mf = (MachOFile*)((long)fileContent + sliceOffset);
        }
        else {
            BLOCK_ACCCESSIBLE_ARRAY(char, gradedArchsBuf, 256);
            getArchNames(archs, isOSBinary, gradedArchsBuf);

            char strBuf[256];
            diag.error("fat file, but missing compatible architecture (have '%s', need '%s')", ff->archNames(strBuf, contentSize), gradedArchsBuf);
            return nullptr;
        }
    }
    else {
        mf = (MachOFile*)fileContent;
    }

    if ( !mf->hasMachOMagic() || !mf->isMachO(diag, contentSize) ) {
        if ( diag.noError() )
            diag.error("not a mach-o file");
        return nullptr;
    }

    if ( archs.grade(mf->cputype, mf->cpusubtype, isOSBinary) == 0 ) {
        BLOCK_ACCCESSIBLE_ARRAY(char, gradedArchsBuf, 256);
        getArchNames(archs, isOSBinary, gradedArchsBuf);
        diag.error("mach-o file, but is an incompatible architecture (have '%s', need '%s')", mf->archName(), gradedArchsBuf);
        return nullptr;
    }

    if ( !mf->loadableIntoProcess(platform, path, internalInstall) ) {
        __block Platform havePlatform = Platform::unknown;
        mf->forEachSupportedPlatform(^(Platform aPlat, uint32_t minOS, uint32_t sdk) {
            havePlatform = aPlat;
        });
        diag.error("mach-o file (%s), but incompatible platform (have '%s', need '%s')", path, MachOFile::platformName(havePlatform), MachOFile::platformName(platform));
        return nullptr;
    }

    return mf;
}

const uint8_t* MachOFile::trieWalk(Diagnostics& diag, const uint8_t* start, const uint8_t* end, const char* symbol)
{
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint32_t, visitedNodeOffsets, 128);
    visitedNodeOffsets.push_back(0);
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
            //diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
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
            if ( nodeOffset > (uint64_t)(end-start) ) {
                diag.error("malformed trie child, nodeOffset=0x%llX out of range\n", nodeOffset);
               return nullptr;
            }
            // check for cycles
            for (uint32_t aVisitedNodeOffset : visitedNodeOffsets) {
                if ( aVisitedNodeOffset == nodeOffset ) {
                    diag.error("malformed trie child, cycle to nodeOffset=0x%llX\n", nodeOffset);
                    return nullptr;
                }
            }
            visitedNodeOffsets.push_back((uint32_t)nodeOffset);
            p = &start[nodeOffset];
        }
        else
            p = end;
    }
    return nullptr;
}

void MachOFile::forEachRPath(void (^callback)(const char* rPath, bool& stop)) const
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


bool MachOFile::inCodeSection(uint32_t runtimeOffset) const
{
    // only needed for arm64e code to know to sign pointers
    if ( (this->cputype != CPU_TYPE_ARM64) || (this->maskedCpuSubtype() != CPU_SUBTYPE_ARM64E) )
        return false;

    __block bool result = false;
    uint64_t baseAddress = this->preferredLoadAddress();
    this->forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( ((sectInfo.sectAddr-baseAddress) <= runtimeOffset) && (runtimeOffset < (sectInfo.sectAddr+sectInfo.sectSize-baseAddress)) ) {
            result = ( (sectInfo.sectFlags & S_ATTR_PURE_INSTRUCTIONS) || (sectInfo.sectFlags & S_ATTR_SOME_INSTRUCTIONS) );
            stop = true;
        }
    });
    return result;
}

uint32_t MachOFile::dependentDylibCount(bool* allDepsAreNormalPtr) const
{
    __block uint32_t count = 0;
    __block bool allDepsAreNormal = true;
    forEachDependentDylib(^(const char* loadPath, bool isWeak, bool isReExport, bool isUpward, uint32_t compatVersion, uint32_t curVersion, bool& stop) {
        ++count;
        if ( isWeak || isReExport || isUpward )
            allDepsAreNormal = false;
    });

    if ( allDepsAreNormalPtr != nullptr )
        *allDepsAreNormalPtr = allDepsAreNormal;
    return count;
}

bool MachOFile::hasPlusLoadMethod(Diagnostics& diag) const
{
    __block bool result = false;

    // in new objc runtime compiler puts classes/categories with +load method in specical section
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( strncmp(info.segInfo.segName, "__DATA", 6) != 0 )
            return;
        if ( (strcmp(info.sectName, "__objc_nlclslist") == 0) || (strcmp(info.sectName, "__objc_nlcatlist") == 0)) {
            result = true;
            stop = true;
        }
    });
    return result;
}

uint32_t MachOFile::getFixupsLoadCommandFileOffset() const
{
    Diagnostics diag;
    __block uint32_t fileOffset = 0;
    this->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:
                fileOffset = (uint32_t)( (uint8_t*)cmd - (uint8_t*)this );
                break;
            case LC_DYLD_CHAINED_FIXUPS:
                fileOffset = (uint32_t)( (uint8_t*)cmd - (uint8_t*)this );
                break;
        }
    });
    if ( diag.hasError() )
        return 0;

    return fileOffset;
}

bool MachOFile::hasInitializer(Diagnostics& diag) const
{
    __block bool result = false;

    // if dylib linked with -init linker option, that initializer is first
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( (cmd->cmd == LC_ROUTINES) || (cmd->cmd == LC_ROUTINES_64) ) {
            result = true;
            stop = true;
        }
    });

    if ( result )
        return true;

    // next any function pointers in mod-init section
    forEachInitializerPointerSection(diag, ^(uint32_t sectionOffset, uint32_t sectionSize, bool& stop) {
        result = true;
        stop = true;
    });

    if ( result )
        return true;

    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& stop) {
        if ( (info.sectFlags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS )
            return;
        result = true;
        stop = true;
    });

    return result;
}

void MachOFile::forEachInitializerPointerSection(Diagnostics& diag, void (^callback)(uint32_t sectionOffset, uint32_t sectionSize, bool& stop)) const
{
    const unsigned ptrSize     = pointerSize();
    const uint64_t baseAddress = preferredLoadAddress();
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool& sectStop) {
        if ( (info.sectFlags & SECTION_TYPE) == S_MOD_INIT_FUNC_POINTERS ) {
            if ( (info.sectSize % ptrSize) != 0 ) {
                diag.error("initializer section %s/%s has bad size", info.segInfo.segName, info.sectName);
                sectStop = true;
                return;
            }
            if ( malformedSectionRange ) {
                diag.error("initializer section %s/%s extends beyond its segment", info.segInfo.segName, info.sectName);
                sectStop = true;
                return;
            }
            if ( (info.sectAddr % ptrSize) != 0 ) {
                diag.error("initializer section %s/%s is not pointer aligned", info.segInfo.segName, info.sectName);
                sectStop = true;
                return;
            }
            callback((uint32_t)(info.sectAddr - baseAddress), (uint32_t)info.sectSize, sectStop);
        }
    });
}

bool MachOFile::hasCodeSignature() const
{
    return this->hasLoadCommand(LC_CODE_SIGNATURE);
}

bool MachOFile::hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const
{
    fileOffset = 0;
    size = 0;

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

    // early exist if no LC_CODE_SIGNATURE
    if ( fileOffset == 0 )
        return false;

    // <rdar://problem/13622786> ignore code signatures in macOS binaries built with pre-10.9 tools
    if ( (this->cputype == CPU_TYPE_X86_64) || (this->cputype == CPU_TYPE_I386) ) {
        __block bool foundPlatform = false;
        __block bool badSignature  = false;
        forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
            foundPlatform = true;
            if ( (platform == Platform::macOS) && (sdk < 0x000A0900) )
                badSignature = true;
        });
        return foundPlatform && !badSignature;
    }

    return true;
}

uint64_t MachOFile::mappedSize() const
{
    uint64_t vmSpace;
    bool     hasZeroFill;
    analyzeSegmentsLayout(vmSpace, hasZeroFill);
    return vmSpace;
}

void MachOFile::analyzeSegmentsLayout(uint64_t& vmSpace, bool& hasZeroFill) const
{
    __block bool     writeExpansion = false;
    __block uint64_t lowestVmAddr   = 0xFFFFFFFFFFFFFFFFULL;
    __block uint64_t highestVmAddr  = 0;
    __block uint64_t sumVmSizes     = 0;
    forEachSegment(^(const SegmentInfo& segmentInfo, bool& stop) {
        if ( strcmp(segmentInfo.segName, "__PAGEZERO") == 0 )
            return;
        if ( segmentInfo.writable() && (segmentInfo.fileSize !=  segmentInfo.vmSize) )
            writeExpansion = true; // zerofill at end of __DATA
        if ( segmentInfo.vmSize == 0 ) {
            // Always zero fill if we have zero-sized segments
            writeExpansion = true;
        }
        if ( segmentInfo.vmAddr < lowestVmAddr )
            lowestVmAddr = segmentInfo.vmAddr;
        if ( segmentInfo.vmAddr+segmentInfo.vmSize > highestVmAddr )
            highestVmAddr = segmentInfo.vmAddr+segmentInfo.vmSize;
        sumVmSizes += segmentInfo.vmSize;
    });
    uint64_t totalVmSpace = (highestVmAddr - lowestVmAddr);
    // LINKEDIT vmSize is not required to be a multiple of page size.  Round up if that is the case
    const uint64_t pageSize = uses16KPages() ? 0x4000 : 0x1000;
    totalVmSpace = (totalVmSpace + (pageSize - 1)) & ~(pageSize - 1);
    bool hasHole = (totalVmSpace != sumVmSizes); // segments not contiguous

    // The aux KC may have __DATA first, in which case we always want to vm_copy to the right place
    bool hasOutOfOrderSegments = false;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
    uint64_t textSegVMAddr = preferredLoadAddress();
    hasOutOfOrderSegments = textSegVMAddr != lowestVmAddr;
#endif

    vmSpace     = totalVmSpace;
    hasZeroFill = writeExpansion || hasHole || hasOutOfOrderSegments;
}

uint32_t MachOFile::segmentCount() const
{
    __block uint32_t count   = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        ++count;
    });
    return count;
}


void MachOFile::forEachDOFSection(Diagnostics& diag, void (^callback)(uint32_t offset)) const
{
    forEachSection(^(const SectionInfo& info, bool malformedSectionRange, bool &stop) {
        if ( ( (info.sectFlags & SECTION_TYPE) == S_DTRACE_DOF ) && !malformedSectionRange ) {
            callback((uint32_t)(info.sectAddr - info.segInfo.vmAddr));
        }
    });
}

bool MachOFile::hasExportTrie(uint32_t& runtimeOffset, uint32_t& size) const
{
    __block uint64_t textUnslidVMAddr   = 0;
    __block uint64_t linkeditUnslidVMAddr   = 0;
    __block uint64_t linkeditFileOffset     = 0;
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            textUnslidVMAddr = info.vmAddr;
        } else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            linkeditUnslidVMAddr = info.vmAddr;
            linkeditFileOffset   = info.fileOffset;
            stop = true;
        }
    });

    Diagnostics diag;
    __block uint32_t fileOffset = ~0U;
    this->forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch ( cmd->cmd ) {
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                const auto* dyldInfo = (const dyld_info_command*)cmd;
                fileOffset = dyldInfo->export_off;
                size = dyldInfo->export_size;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const auto* linkeditCmd = (const linkedit_data_command*)cmd;
                fileOffset = linkeditCmd->dataoff;
                size = linkeditCmd->datasize;
                break;
            }
        }
    });
    if ( diag.hasError() )
        return false;

    if ( fileOffset == ~0U )
        return false;

    runtimeOffset = (uint32_t)((fileOffset - linkeditFileOffset) + (linkeditUnslidVMAddr - textUnslidVMAddr));
    return true;
}

#if !TARGET_OS_EXCLAVEKIT
// Note, this has to match the kernel
static const uint32_t hashPriorities[] = {
    CS_HASHTYPE_SHA1,
    CS_HASHTYPE_SHA256_TRUNCATED,
    CS_HASHTYPE_SHA256,
    CS_HASHTYPE_SHA384,
};

static unsigned int hash_rank(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities) / sizeof(hashPriorities[0]); ++n) {
        if (hashPriorities[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}

// Note, this does NOT match the kernel.
// On watchOS, in main executables, we will record all cd hashes then make sure
// one of the ones we record matches the kernel.
// This list is only for dylibs where we embed the cd hash in the closure instead of the
// mod time and inode
// This is sorted so that we choose sha1 first when checking dylibs
static const uint32_t hashPriorities_watchOS_dylibs[] = {
    CS_HASHTYPE_SHA256_TRUNCATED,
    CS_HASHTYPE_SHA256,
    CS_HASHTYPE_SHA384,
    CS_HASHTYPE_SHA1
};

static unsigned int hash_rank_watchOS_dylibs(const CS_CodeDirectory *cd)
{
    uint32_t type = cd->hashType;
    for (uint32_t n = 0; n < sizeof(hashPriorities_watchOS_dylibs) / sizeof(hashPriorities_watchOS_dylibs[0]); ++n) {
        if (hashPriorities_watchOS_dylibs[n] == type)
            return n + 1;
    }

    /* not supported */
    return 0;
}

// This calls the callback for all code directories required for a given platform/binary combination.
// On watchOS main executables this is all cd hashes.
// On watchOS dylibs this is only the single cd hash we need (by rank defined by dyld, not the kernel).
// On all other platforms this always returns a single best cd hash (ranked to match the kernel).
// Note the callback parameter is really a CS_CodeDirectory.
void MachOFile::forEachCodeDirectoryBlob(const void* codeSigStart, size_t codeSignLen,
                                         void (^callback)(const void* cd)) const
{
    // verify min length of overall code signature
    if ( codeSignLen < sizeof(CS_SuperBlob) )
        return;

    // verify magic at start
    const CS_SuperBlob* codeSuperBlob = (CS_SuperBlob*)codeSigStart;
    if ( codeSuperBlob->magic != htonl(CSMAGIC_EMBEDDED_SIGNATURE) )
        return;

    // verify count of sub-blobs not too large
    uint32_t subBlobCount = htonl(codeSuperBlob->count);
    if ( (codeSignLen-sizeof(CS_SuperBlob))/sizeof(CS_BlobIndex) < subBlobCount )
        return;

    // Note: The kernel sometimes chooses sha1 on watchOS, and sometimes sha256.
    // Embed all of them so that we just need to match any of them
    const bool isWatchOS = this->builtForPlatform(Platform::watchOS);
    const bool isMainExecutable = this->isMainExecutable();
    auto hashRankFn = isWatchOS ? &hash_rank_watchOS_dylibs : &hash_rank;

    // walk each sub blob, looking at ones with type CSSLOT_CODEDIRECTORY
    const CS_CodeDirectory* bestCd = nullptr;
    for (uint32_t i=0; i < subBlobCount; ++i) {
        if ( codeSuperBlob->index[i].type == htonl(CSSLOT_CODEDIRECTORY) ) {
            // Ok, this is the regular code directory
        } else if ( codeSuperBlob->index[i].type >= htonl(CSSLOT_ALTERNATE_CODEDIRECTORIES) && codeSuperBlob->index[i].type <= htonl(CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT)) {
            // Ok, this is the alternative code directory
        } else {
            continue;
        }
        uint32_t cdOffset = htonl(codeSuperBlob->index[i].offset);
        // verify offset is not out of range
        if ( cdOffset > (codeSignLen - sizeof(CS_CodeDirectory)) )
            continue;
        const CS_CodeDirectory* cd = (CS_CodeDirectory*)((uint8_t*)codeSuperBlob + cdOffset);
        uint32_t cdLength = htonl(cd->length);
        // verify code directory length not out of range
        if ( cdLength > (codeSignLen - cdOffset) )
            continue;

        // The watch main executable wants to know about all cd hashes
        if ( isWatchOS && isMainExecutable ) {
            callback(cd);
            continue;
        }

        if ( cd->magic == htonl(CSMAGIC_CODEDIRECTORY) ) {
            if ( !bestCd || (hashRankFn(cd) > hashRankFn(bestCd)) )
                bestCd = cd;
        }
    }

    // Note this callback won't happen on watchOS as that one was done in the loop
    if ( bestCd != nullptr )
        callback(bestCd);
}

void MachOFile::forEachCDHashOfCodeSignature(const void* codeSigStart, size_t codeSignLen,
                                             void (^callback)(const uint8_t cdHash[20])) const
{
    forEachCodeDirectoryBlob(codeSigStart, codeSignLen, ^(const void *cdBuffer) {
        const CS_CodeDirectory* cd = (const CS_CodeDirectory*)cdBuffer;
        uint32_t cdLength = htonl(cd->length);
        uint8_t cdHash[20];
        if ( cd->hashType == CS_HASHTYPE_SHA384 ) {
            uint8_t digest[CCSHA384_OUTPUT_SIZE];
            const struct ccdigest_info* di = ccsha384_di();
            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
            ccdigest_init(di, tempBuf);
            ccdigest_update(di, tempBuf, cdLength, cd);
            ccdigest_final(di, tempBuf, digest);
            ccdigest_di_clear(di, tempBuf);
            // cd-hash of sigs that use SHA384 is the first 20 bytes of the SHA384 of the code digest
            memcpy(cdHash, digest, 20);
            callback(cdHash);
            return;
        }
        else if ( (cd->hashType == CS_HASHTYPE_SHA256) || (cd->hashType == CS_HASHTYPE_SHA256_TRUNCATED) ) {
            uint8_t digest[CCSHA256_OUTPUT_SIZE];
            const struct ccdigest_info* di = ccsha256_di();
            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
            ccdigest_init(di, tempBuf);
            ccdigest_update(di, tempBuf, cdLength, cd);
            ccdigest_final(di, tempBuf, digest);
            ccdigest_di_clear(di, tempBuf);
            // cd-hash of sigs that use SHA256 is the first 20 bytes of the SHA256 of the code digest
            memcpy(cdHash, digest, 20);
            callback(cdHash);
            return;
        }
        else if ( cd->hashType == CS_HASHTYPE_SHA1 ) {
            // compute hash directly into return buffer
            const struct ccdigest_info* di = ccsha1_di();
            ccdigest_di_decl(di, tempBuf); // declares tempBuf array in stack
            ccdigest_init(di, tempBuf);
            ccdigest_update(di, tempBuf, cdLength, cd);
            ccdigest_final(di, tempBuf, cdHash);
            ccdigest_di_clear(di, tempBuf);
            callback(cdHash);
            return;
        }
    });
}
#endif // !TARGET_OS_EXCLAVEKIT

// These are mangled symbols for all the variants of operator new and delete
// which a main executable can define (non-weak) and override the
// weak-def implementation in the OS.
static const char* const sTreatAsWeak[] = {
    "__Znwm", "__ZnwmRKSt9nothrow_t",
    "__Znam", "__ZnamRKSt9nothrow_t",
    "__ZdlPv", "__ZdlPvRKSt9nothrow_t", "__ZdlPvm",
    "__ZdaPv", "__ZdaPvRKSt9nothrow_t", "__ZdaPvm",
    "__ZnwmSt11align_val_t", "__ZnwmSt11align_val_tRKSt9nothrow_t",
    "__ZnamSt11align_val_t", "__ZnamSt11align_val_tRKSt9nothrow_t",
    "__ZdlPvSt11align_val_t", "__ZdlPvSt11align_val_tRKSt9nothrow_t", "__ZdlPvmSt11align_val_t",
    "__ZdaPvSt11align_val_t", "__ZdaPvSt11align_val_tRKSt9nothrow_t", "__ZdaPvmSt11align_val_t",
    "__ZnwmSt19__type_descriptor_t", "__ZnamSt19__type_descriptor_t"
};

void MachOFile::forEachTreatAsWeakDef(void (^handler)(const char* symbolName))
{
    for (const char*  sym : sTreatAsWeak)
        handler(sym);
}

MachOFile::PointerMetaData::PointerMetaData()
{
    this->diversity           = 0;
    this->high8               = 0;
    this->authenticated       = 0;
    this->key                 = 0;
    this->usesAddrDiversity   = 0;
}

MachOFile::PointerMetaData::PointerMetaData(const ChainedFixupPointerOnDisk* fixupLoc, uint16_t pointer_format)
{
    this->diversity           = 0;
    this->high8               = 0;
    this->authenticated       = 0;
    this->key                 = 0;
    this->usesAddrDiversity   = 0;
    switch ( pointer_format ) {
        case DYLD_CHAINED_PTR_ARM64E:
        case DYLD_CHAINED_PTR_ARM64E_KERNEL:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND:
        case DYLD_CHAINED_PTR_ARM64E_FIRMWARE:
        case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
            this->authenticated = fixupLoc->arm64e.authRebase.auth;
            if ( this->authenticated ) {
                this->key               = fixupLoc->arm64e.authRebase.key;
                this->usesAddrDiversity = fixupLoc->arm64e.authRebase.addrDiv;
                this->diversity         = fixupLoc->arm64e.authRebase.diversity;
            }
            else if ( fixupLoc->arm64e.bind.bind == 0 ) {
                this->high8             = fixupLoc->arm64e.rebase.high8;
            }
            break;
        case DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE:
            this->authenticated = fixupLoc->cache64e.auth.auth;
            if ( this->authenticated ) {
                this->key               = fixupLoc->cache64e.auth.keyIsData ? 2 : 0; // true -> DA (2), false -> IA (0)
                this->usesAddrDiversity = fixupLoc->cache64e.auth.addrDiv;
                this->diversity         = fixupLoc->cache64e.auth.diversity;
            }
            else {
                this->high8 = fixupLoc->cache64e.regular.high8;
            }
            break;
        case DYLD_CHAINED_PTR_64:
        case DYLD_CHAINED_PTR_64_OFFSET:
            if ( fixupLoc->generic64.bind.bind == 0 )
                this->high8             = fixupLoc->generic64.rebase.high8;
            break;
    }
}

bool MachOFile::PointerMetaData::operator==(const PointerMetaData& other) const
{
    return (this->diversity == other.diversity)
        && (this->high8 == other.high8)
        && (this->authenticated == other.authenticated)
        && (this->key == other.key)
        && (this->usesAddrDiversity == other.usesAddrDiversity);
}

#if !SUPPORT_VM_LAYOUT
bool MachOFile::getLinkeditLayout(Diagnostics& diag, mach_o::LinkeditLayout& layout) const
{
    // Note, in file layout all linkedit offsets are just file offsets.
    // It is essential no-one calls this on a MachOLoaded or MachOAnalyzer

    // FIXME: Other load commands
    this->forEachLoadCommand(diag, ^(const load_command *cmd, bool &stop) {
        switch ( cmd->cmd ) {
            case LC_SYMTAB: {
                const symtab_command* symTabCmd = (const symtab_command*)cmd;

                // Record that we found a LC_SYMTAB
                layout.hasSymTab = true;

                // NList
                uint64_t nlistEntrySize  = this->is64() ? sizeof(struct nlist_64) : sizeof(struct nlist);
                layout.symbolTable.fileOffset       = symTabCmd->symoff;
                layout.symbolTable.buffer           = (uint8_t*)this + symTabCmd->symoff;
                layout.symbolTable.bufferSize       = (uint32_t)(symTabCmd->nsyms * nlistEntrySize);
                layout.symbolTable.entryCount       = symTabCmd->nsyms;
                layout.symbolTable.hasLinkedit      = true;

                // Symbol strings
                layout.symbolStrings.fileOffset     = symTabCmd->stroff;
                layout.symbolStrings.buffer         = (uint8_t*)this + symTabCmd->stroff;
                layout.symbolStrings.bufferSize     = symTabCmd->strsize;
                layout.symbolStrings.hasLinkedit    = true;
                break;
            }
            case LC_DYSYMTAB: {
                const dysymtab_command* dynSymTabCmd = (const dysymtab_command*)cmd;

                // Record that we found a LC_DYSYMTAB
                layout.hasDynSymTab = true;

                // Local relocs
                layout.localRelocs.fileOffset          = dynSymTabCmd->locreloff;
                layout.localRelocs.buffer              = (uint8_t*)this + dynSymTabCmd->locreloff;
                layout.localRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.localRelocs.entryIndex          = 0;         // Use buffer instead
                layout.localRelocs.entryCount          = dynSymTabCmd->nlocrel;
                layout.localRelocs.hasLinkedit         = true;

                // Extern relocs
                layout.externRelocs.fileOffset          = dynSymTabCmd->extreloff;
                layout.externRelocs.buffer              = (uint8_t*)this + dynSymTabCmd->extreloff;
                layout.externRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.externRelocs.entryIndex          = 0;         // Use buffer instead
                layout.externRelocs.entryCount          = dynSymTabCmd->nextrel;
                layout.externRelocs.hasLinkedit         = true;

                // Indirect symbol table
                layout.indirectSymbolTable.fileOffset   = dynSymTabCmd->indirectsymoff;
                layout.indirectSymbolTable.buffer       = (uint8_t*)this + dynSymTabCmd->indirectsymoff;
                layout.indirectSymbolTable.bufferSize   = 0;         // Use entryCount instead
                layout.indirectSymbolTable.entryIndex   = 0;         // Use buffer instead
                layout.indirectSymbolTable.entryCount   = dynSymTabCmd->nindirectsyms;
                layout.indirectSymbolTable.hasLinkedit  = true;

                // Locals
                layout.localSymbolTable.fileOffset     = 0;         // unused
                layout.localSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.localSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.localSymbolTable.entryIndex     = dynSymTabCmd->ilocalsym;
                layout.localSymbolTable.entryCount     = dynSymTabCmd->nlocalsym;
                layout.localSymbolTable.hasLinkedit    = true;

                // Globals
                layout.globalSymbolTable.fileOffset     = 0;         // unused
                layout.globalSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.globalSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.globalSymbolTable.entryIndex     = dynSymTabCmd->iextdefsym;
                layout.globalSymbolTable.entryCount     = dynSymTabCmd->nextdefsym;
                layout.globalSymbolTable.hasLinkedit    = true;

                // Imports
                layout.undefSymbolTable.fileOffset     = 0;         // unused
                layout.undefSymbolTable.buffer         = nullptr;   // Use entryIndex instead
                layout.undefSymbolTable.bufferSize     = 0;         // Use entryCount instead
                layout.undefSymbolTable.entryIndex     = dynSymTabCmd->iundefsym;
                layout.undefSymbolTable.entryCount     = dynSymTabCmd->nundefsym;
                layout.undefSymbolTable.hasLinkedit    = true;
                break;
            }
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY: {
                const dyld_info_command* linkeditCmd = (const dyld_info_command*)cmd;

                // Record what kind of DYLD_INFO we found
                layout.dyldInfoCmd = cmd->cmd;

                // Rebase
                layout.rebaseOpcodes.fileOffset         = linkeditCmd->rebase_off;
                layout.rebaseOpcodes.buffer             = (uint8_t*)this + linkeditCmd->rebase_off;
                layout.rebaseOpcodes.bufferSize         = linkeditCmd->rebase_size;
                layout.rebaseOpcodes.hasLinkedit        = true;

                // Bind
                layout.regularBindOpcodes.fileOffset    = linkeditCmd->bind_off;
                layout.regularBindOpcodes.buffer        = (uint8_t*)this + linkeditCmd->bind_off;
                layout.regularBindOpcodes.bufferSize    = linkeditCmd->bind_size;
                layout.regularBindOpcodes.hasLinkedit   = true;

                // Lazy bind
                layout.lazyBindOpcodes.fileOffset       = linkeditCmd->lazy_bind_off;
                layout.lazyBindOpcodes.buffer           = (uint8_t*)this + linkeditCmd->lazy_bind_off;
                layout.lazyBindOpcodes.bufferSize       = linkeditCmd->lazy_bind_size;
                layout.lazyBindOpcodes.hasLinkedit      = true;

                // Weak bind
                layout.weakBindOpcodes.fileOffset       = linkeditCmd->weak_bind_off;
                layout.weakBindOpcodes.buffer           = (uint8_t*)this + linkeditCmd->weak_bind_off;
                layout.weakBindOpcodes.bufferSize       = linkeditCmd->weak_bind_size;
                layout.weakBindOpcodes.hasLinkedit      = true;

                // Export trie
                layout.exportsTrie.fileOffset           = linkeditCmd->export_off;
                layout.exportsTrie.buffer               = (uint8_t*)this + linkeditCmd->export_off;
                layout.exportsTrie.bufferSize           = linkeditCmd->export_size;
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.chainedFixups.fileOffset         = linkeditCmd->dataoff;
                layout.chainedFixups.buffer             = (uint8_t*)this + linkeditCmd->dataoff;
                layout.chainedFixups.bufferSize         = linkeditCmd->datasize;
                layout.chainedFixups.entryCount         = 0; // Not needed here
                layout.chainedFixups.hasLinkedit        = true;
                layout.chainedFixups.cmd                = linkeditCmd;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.exportsTrie.fileOffset           = linkeditCmd->dataoff;
                layout.exportsTrie.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.exportsTrie.bufferSize           = linkeditCmd->datasize;
                layout.exportsTrie.entryCount           = 0; // Not needed here
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_SEGMENT_SPLIT_INFO: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.splitSegInfo.fileOffset           = linkeditCmd->dataoff;
                layout.splitSegInfo.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.splitSegInfo.bufferSize           = linkeditCmd->datasize;
                layout.splitSegInfo.entryCount           = 0; // Not needed here
                layout.splitSegInfo.hasLinkedit          = true;
                break;
            }
            case LC_FUNCTION_STARTS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.functionStarts.fileOffset           = linkeditCmd->dataoff;
                layout.functionStarts.buffer               = (uint8_t*)this + linkeditCmd->dataoff;
                layout.functionStarts.bufferSize           = linkeditCmd->datasize;
                layout.functionStarts.entryCount           = 0; // Not needed here
                layout.functionStarts.hasLinkedit          = true;
                break;
            }
            case LC_DATA_IN_CODE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.dataInCode.fileOffset    = linkeditCmd->dataoff;
                layout.dataInCode.buffer        = (uint8_t*)this + linkeditCmd->dataoff;
                layout.dataInCode.bufferSize    = linkeditCmd->datasize;
                layout.dataInCode.entryCount    = 0; // Not needed here
                layout.dataInCode.hasLinkedit   = true;
                break;
            }
            case LC_CODE_SIGNATURE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.codeSignature.fileOffset    = linkeditCmd->dataoff;
                layout.codeSignature.buffer        = (uint8_t*)this + linkeditCmd->dataoff;
                layout.codeSignature.bufferSize    = linkeditCmd->datasize;
                layout.codeSignature.entryCount    = 0; // Not needed here
                layout.codeSignature.hasLinkedit   = true;
                break;
            }
        }
    });

    return true;
}

void MachOFile::withFileLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout &layout)) const
{
    // Use the fixups from the source dylib
    mach_o::LinkeditLayout linkedit;
    if ( !this->getLinkeditLayout(diag, linkedit) ) {
        diag.error("Couldn't get dylib layout");
        return;
    }

    uint32_t numSegments = this->segmentCount();
    BLOCK_ACCCESSIBLE_ARRAY(mach_o::SegmentLayout, segmentLayout, numSegments);
    this->forEachSegment(^(const SegmentInfo &info, bool &stop) {
        mach_o::SegmentLayout segment;
        segment.vmAddr      = info.vmAddr;
        segment.vmSize      = info.vmSize;
        segment.fileOffset  = info.fileOffset;
        segment.fileSize    = info.fileSize;
        segment.buffer      = (uint8_t*)this + info.fileOffset;
        segment.protections = info.protections;

        segment.kind        = mach_o::SegmentLayout::Kind::unknown;
        if ( !strcmp(info.segName, "__TEXT") ) {
            segment.kind    = mach_o::SegmentLayout::Kind::text;
        } else if ( !strcmp(info.segName, "__LINKEDIT") ) {
            segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
        }

        segmentLayout[info.segIndex] = segment;
    });

    mach_o::Layout layout(this, { &segmentLayout[0], &segmentLayout[numSegments] }, linkedit);
    callback(layout);
}
#endif // !SUPPORT_VM_LAYOUT

bool MachOFile::hasObjCMessageReferences() const {

    __block bool foundSection = false;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( strncmp(sectInfo.segInfo.segName, "__DATA", 6) != 0 )
            return;
        if ( strcmp(sectInfo.sectName, "__objc_msgrefs") != 0 )
            return;
        foundSection = true;
        stop = true;
    });
    return foundSection;
}

uint32_t MachOFile::loadCommandsFreeSpace() const
{
    __block uint32_t firstSectionFileOffset = 0;
    __block uint32_t firstSegmentFileOffset = 0;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        firstSectionFileOffset = sectInfo.sectFileOffset;
        firstSegmentFileOffset = (uint32_t)sectInfo.segInfo.fileOffset;
        stop = true;
    });

    uint32_t headerSize = (this->magic == MH_MAGIC_64) ? sizeof(mach_header_64) : sizeof(mach_header);
    uint32_t existSpaceUsed = this->sizeofcmds + headerSize;
    return firstSectionFileOffset - firstSegmentFileOffset - existSpaceUsed;
}

bool MachOFile::findObjCDataSection(const char *sectionName, uint64_t& sectionRuntimeOffset, uint64_t& sectionSize) const
{
    uint64_t baseAddress = preferredLoadAddress();

    __block bool foundSection = false;
    forEachSection(^(const SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(sectInfo.segInfo.segName, "__DATA") != 0) &&
             (strcmp(sectInfo.segInfo.segName, "__DATA_CONST") != 0) &&
             (strcmp(sectInfo.segInfo.segName, "__DATA_DIRTY") != 0) )
            return;
        if ( strcmp(sectInfo.sectName, sectionName) != 0 )
            return;
        foundSection         = true;
        sectionRuntimeOffset = sectInfo.sectAddr - baseAddress;
        sectionSize          = sectInfo.sectSize;
        stop                 = true;
    });
    return foundSection;
}

bool MachOFile::enforceFormat(Malformed kind) const
{
    // TODO: Add a mapping from generic releases to platform versions
#if BUILDING_DYLDINFO || BUILDING_APP_CACHE_UTIL || BUILDING_RUN_STATIC
    // HACK: If we are the kernel, we have a different format to enforce
    if ( isFileSet() ) {
        bool result = false;
        switch (kind) {
        case Malformed::linkeditOrder:
        case Malformed::linkeditAlignment:
        case Malformed::dyldInfoAndlocalRelocs:
            result = true;
            break;
        case Malformed::segmentOrder:
        // The aux KC has __DATA first
            result = false;
            break;
        case Malformed::linkeditPermissions:
        case Malformed::executableData:
        case Malformed::writableData:
        case Malformed::codeSigAlignment:
        case Malformed::sectionsAddrRangeWithinSegment:
        case Malformed::loaderPathsAreReal:
        case Malformed::mainExecInDyldCache:
            result = true;
            break;
        case Malformed::noLinkedDylibs:
        case Malformed::textPermissions:
            // The kernel has its own __TEXT_EXEC for executable memory
            result = false;
            break;
        case Malformed::noUUID:
        case Malformed::zerofillSwiftMetadata:
        case Malformed::sdkOnOrAfter2021:
        case Malformed::sdkOnOrAfter2022:
            result = true;
            break;
        }
        return result;
    }

    if ( isStaticExecutable() ) {
        bool result = false;
        switch (kind) {
        case Malformed::linkeditOrder:
        case Malformed::linkeditAlignment:
        case Malformed::dyldInfoAndlocalRelocs:
            result = true;
            break;
        case Malformed::segmentOrder:
        case Malformed::textPermissions:
            result = false;
            break;
        case Malformed::linkeditPermissions:
        case Malformed::executableData:
        case Malformed::codeSigAlignment:
        case Malformed::sectionsAddrRangeWithinSegment:
        case Malformed::loaderPathsAreReal:
        case Malformed::mainExecInDyldCache:
            result = true;
            break;
        case Malformed::noLinkedDylibs:
        case Malformed::writableData:
        case Malformed::noUUID:
        case Malformed::zerofillSwiftMetadata:
        case Malformed::sdkOnOrAfter2021:
        case Malformed::sdkOnOrAfter2022:
            // The kernel has __DATA_CONST marked as r/o
            result = false;
            break;
        }
        return result;
    }

#endif

    __block bool result = false;
    forEachSupportedPlatform(^(Platform platform, uint32_t minOS, uint32_t sdk) {
        switch (platform) {
        case Platform::macOS:
            switch (kind) {
            case Malformed::linkeditOrder:
            case Malformed::linkeditAlignment:
            case Malformed::dyldInfoAndlocalRelocs:
                // enforce these checks on new binaries only
                if (sdk >= 0x000A0E00) // macOS 10.14
                    result = true;
                break;
            case Malformed::segmentOrder:
            case Malformed::linkeditPermissions:
            case Malformed::textPermissions:
            case Malformed::executableData:
            case Malformed::writableData:
            case Malformed::codeSigAlignment:
                // enforce these checks on new binaries only
                if (sdk >= 0x000A0F00) // macOS 10.15
                    result = true;
                break;
            case Malformed::sectionsAddrRangeWithinSegment:
                // enforce these checks on new binaries only
                if (sdk >= 0x000A1000) // macOS 10.16
                    result = true;
                break;
            case Malformed::noLinkedDylibs:
            case Malformed::loaderPathsAreReal:
            case Malformed::mainExecInDyldCache:
            case Malformed::zerofillSwiftMetadata:
            case Malformed::sdkOnOrAfter2021:
                // enforce these checks on new binaries only
                if (sdk >= 0x000D0000) // macOS 13.0
                    result = true;
                break;
            case Malformed::noUUID:
            case Malformed::sdkOnOrAfter2022:
                if (sdk >= 0x000E0000) // macOS 14.0  FIXME
                    result = true;
                break;
            }
            break;
        case Platform::iOS:
        case Platform::tvOS:
        case Platform::iOSMac:
            switch (kind) {
            case Malformed::linkeditOrder:
            case Malformed::dyldInfoAndlocalRelocs:
            case Malformed::textPermissions:
            case Malformed::executableData:
            case Malformed::writableData:
                result = true;
                break;
            case Malformed::linkeditAlignment:
            case Malformed::segmentOrder:
            case Malformed::linkeditPermissions:
            case Malformed::codeSigAlignment:
                // enforce these checks on new binaries only
                if (sdk >= 0x000D0000) // iOS 13
                    result = true;
                break;
            case Malformed::sectionsAddrRangeWithinSegment:
                // enforce these checks on new binaries only
                if (sdk >= 0x000E0000) // iOS 14
                    result = true;
                break;
            case Malformed::noLinkedDylibs:
            case Malformed::loaderPathsAreReal:
            case Malformed::mainExecInDyldCache:
            case Malformed::zerofillSwiftMetadata:
            case Malformed::sdkOnOrAfter2021:
                // enforce these checks on new binaries only
                if (sdk >= 0x00100000) // iOS 16
                    result = true;
                break;
            case Malformed::noUUID:
            case Malformed::sdkOnOrAfter2022:
                if (sdk >= 0x00110000) // iOS 17.0 FIXME
                    result = true;
                break;
            }
            break;
        case Platform::watchOS:
            switch (kind) {
            case Malformed::linkeditOrder:
            case Malformed::dyldInfoAndlocalRelocs:
            case Malformed::textPermissions:
            case Malformed::executableData:
            case Malformed::writableData:
                result = true;
                break;
            case Malformed::linkeditAlignment:
            case Malformed::segmentOrder:
            case Malformed::linkeditPermissions:
            case Malformed::codeSigAlignment:
            case Malformed::sectionsAddrRangeWithinSegment:
            case Malformed::noLinkedDylibs:
            case Malformed::loaderPathsAreReal:
            case Malformed::mainExecInDyldCache:
            case Malformed::zerofillSwiftMetadata:
            case Malformed::sdkOnOrAfter2021:
                // enforce these checks on new binaries only
                if (sdk >= 0x00090000) // watchOS 9
                    result = true;
                break;
            case Malformed::noUUID:
            case Malformed::sdkOnOrAfter2022:
                if (sdk >= 0x000A0000) // watchOS 10 FIXME
                    result = true;
                break;
            }
            break;
        case Platform::driverKit:
            result = true;
            break;
        default:
            result = true;
            break;
        }
    });
    // if binary is so old, there is no platform info, don't enforce malformed errors
    return result;
}

bool MachOFile::validSegments(Diagnostics& diag, const char* path, size_t fileLen) const
{
    // check segment load command size
    __block bool badSegmentLoadCommand = false;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_SEGMENT_64 ) {
            const segment_command_64* seg = (segment_command_64*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command_64);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT_64", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section_64)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (int32_t)(seg->nsects * sizeof(section_64)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( greaterThanAddOrOverflow(seg->fileoff, seg->filesize, fileLen) ) {
                diag.error("in '%s' segment load command content extends beyond end of file", path);
                badSegmentLoadCommand = true;
                stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment '%s' filesize exceeds vmsize", path, seg->segname);
                badSegmentLoadCommand = true;
                stop = true;
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            int32_t sectionsSpace = cmd->cmdsize - sizeof(segment_command);
            if ( sectionsSpace < 0 ) {
               diag.error("in '%s' load command size too small for LC_SEGMENT", path);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (sectionsSpace % sizeof(section)) != 0 ) {
               diag.error("in '%s' segment load command size 0x%X will not fit whole number of sections", path, cmd->cmdsize);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( sectionsSpace != (int32_t)(seg->nsects * sizeof(section)) ) {
               diag.error("in '%s' load command size 0x%X does not match nsects %d", path, cmd->cmdsize, seg->nsects);
               badSegmentLoadCommand = true;
               stop = true;
            }
            else if ( (seg->filesize > seg->vmsize) && ((seg->vmsize != 0) || ((seg->flags & SG_NORELOC) == 0)) ) {
                // <rdar://problem/19986776> dyld should support non-allocatable __LLVM segment
                diag.error("in '%s' segment  '%s' filesize exceeds vmsize", path, seg->segname);
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
    forEachSegment(^(const SegmentInfo& info, bool& stop) {
        if ( strcmp(info.segName, "__TEXT") == 0 ) {
            if ( (info.protections != (VM_PROT_READ|VM_PROT_EXECUTE)) && enforceFormat(Malformed::textPermissions) ) {
                diag.error("in '%s' __TEXT segment permissions is not 'r-x'", path);
                badPermissions = true;
                stop = true;
            }
            hasTEXT = true;
        }
        else if ( strcmp(info.segName, "__LINKEDIT") == 0 ) {
            if ( (info.protections != VM_PROT_READ) && enforceFormat(Malformed::linkeditPermissions) ) {
                diag.error("in '%s' __LINKEDIT segment permissions is not 'r--'", path);
                badPermissions = true;
                stop = true;
            }
            hasLINKEDIT = true;
        }
        else if ( (info.protections & 0xFFFFFFF8) != 0 ) {
            diag.error("in '%s' %s segment permissions has invalid bits set", path, info.segName);
            badPermissions = true;
            stop = true;
        }
        if ( greaterThanAddOrOverflow(info.fileOffset, info.fileSize, fileLen) ) {
            diag.error("in '%s' %s segment content extends beyond end of file", path, info.segName);
            badSize = true;
            stop = true;
        }
        if ( is64() ) {
            if ( info.vmAddr+info.vmSize < info.vmAddr ) {
                diag.error("in '%s' %s segment vm range wraps", path, info.segName);
                badSize = true;
                stop = true;
            }
       }
       else {
            if ( (uint32_t)(info.vmAddr+info.vmSize) < (uint32_t)(info.vmAddr) ) {
                diag.error("in '%s' %s segment vm range wraps", path, info.segName);
                badSize = true;
                stop = true;
            }
       }
    });
    if ( badPermissions || badSize )
        return false;
    if ( !hasTEXT ) {
        diag.error("in '%s' missing __TEXT segment", path);
        return false;
    }
    if ( !hasLINKEDIT && !this->isPreload() ) {
       diag.error("in '%s' missing __LINKEDIT segment", path);
       return false;
    }

    // check for overlapping segments
    __block bool badSegments = false;
    forEachSegment(^(const SegmentInfo& info1, bool& stop1) {
        uint64_t seg1vmEnd   = info1.vmAddr + info1.vmSize;
        uint64_t seg1FileEnd = info1.fileOffset + info1.fileSize;
        forEachSegment(^(const SegmentInfo& info2, bool& stop2) {
            if ( info1.segIndex == info2.segIndex )
                return;
            uint64_t seg2vmEnd   = info2.vmAddr + info2.vmSize;
            uint64_t seg2FileEnd = info2.fileOffset + info2.fileSize;
            if ( ((info2.vmAddr <= info1.vmAddr) && (seg2vmEnd > info1.vmAddr) && (seg1vmEnd > info1.vmAddr )) || ((info2.vmAddr >= info1.vmAddr ) && (info2.vmAddr < seg1vmEnd) && (seg2vmEnd > info2.vmAddr)) ) {
                diag.error("in '%s' segment %s vm range overlaps segment %s", path, info1.segName, info2.segName);
                badSegments = true;
                stop1 = true;
                stop2 = true;
            }
             if ( ((info2.fileOffset  <= info1.fileOffset) && (seg2FileEnd > info1.fileOffset) && (seg1FileEnd > info1.fileOffset)) || ((info2.fileOffset  >= info1.fileOffset) && (info2.fileOffset  < seg1FileEnd) && (seg2FileEnd > info2.fileOffset )) ) {
                 if ( !inDyldCache() ) {
                     // HACK: Split shared caches might put the __TEXT in a SubCache, then the __DATA in a later SubCache.
                     // The file offsets are in to each SubCache file, which means that they might overlap
                     // For now we have no choice but to disable this error
                     diag.error("in '%s' segment %s file content overlaps segment %s", path, info1.segName, info2.segName);
                     badSegments = true;
                     stop1 = true;
                     stop2 = true;
                 }
            }
            if ( (info1.segIndex < info2.segIndex) && !stop1 ) {
                if ( (info1.vmAddr > info2.vmAddr) || ((info1.fileOffset > info2.fileOffset ) && (info1.fileOffset != 0) && (info2.fileOffset  != 0)) ){
                    if ( !inDyldCache() && enforceFormat(Malformed::segmentOrder) && !isStaticExecutable() ) {
                        // <rdar://80084852> whitelist go libraries __DWARF segments
                        if ( (strcmp(info1.segName, "__DWARF") != 0 && strcmp(info2.segName, "__DWARF") != 0) ) {
                            // dyld cache __DATA_* segments are moved around
                            // The static kernel also has segments with vmAddr's before __TEXT
                            diag.error("in '%s' segment load commands out of order with respect to layout for %s and %s", path, info1.segName, info2.segName);
                            badSegments = true;
                            stop1 = true;
                            stop2 = true;
                        }
                    }
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
                    diag.error("in '%s' section '%s' size too large 0x%llX", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section '%s' start address 0x%llX is before containing segment's address 0x%0llX", path, sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    bool ignoreError = !enforceFormat(Malformed::sectionsAddrRangeWithinSegment);
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
                    if ( (seg->vmsize == 0) && !strcmp(seg->segname, "__CTF") )
                        ignoreError = true;
#endif
                    if ( !ignoreError ) {
                        diag.error("in '%s' section '%s' end address 0x%llX is beyond containing segment's end address 0x%0llX", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                        badSections = true;
                    }
                }
            }
        }
        else if ( cmd->cmd == LC_SEGMENT ) {
            const segment_command* seg = (segment_command*)cmd;
            const section* const sectionsStart = (section*)((char*)seg + sizeof(struct segment_command));
            const section* const sectionsEnd   = &sectionsStart[seg->nsects];
            for (const section* sect=sectionsStart; !stop && (sect < sectionsEnd); ++sect) {
               if ( (int64_t)(sect->size) < 0 ) {
                    diag.error("in '%s' section %s size too large 0x%X", path, sect->sectname, sect->size);
                    badSections = true;
                }
                else if ( sect->addr < seg->vmaddr ) {
                    diag.error("in '%s' section %s start address 0x%X is before containing segment's address 0x%0X", path,  sect->sectname, sect->addr, seg->vmaddr);
                    badSections = true;
                }
                else if ( sect->addr+sect->size > seg->vmaddr+seg->vmsize ) {
                    diag.error("in '%s' section %s end address 0x%X is beyond containing segment's end address 0x%0X", path, sect->sectname, sect->addr+sect->size, seg->vmaddr+seg->vmsize);
                    badSections = true;
                }
            }
        }
    });

    return !badSections;
}

void MachOFile::forEachSingletonPatch(Diagnostics& diag, void (^handler)(SingletonPatchKind kind,
                                                                         uint64_t runtimeOffset)) const
{
    uint32_t ptrSize = this->pointerSize();
    uint32_t elementSize = (2 * ptrSize);
    uint64_t loadAddress = this->preferredLoadAddress();
    this->forEachSection(^(const SectionInfo &sectInfo, bool malformedSectionRange, bool &stop) {
        if ( strcmp(sectInfo.sectName, "__const_cfobj2") != 0 )
            return;
        stop = true;

        if ( (sectInfo.sectSize % elementSize) != 0 ) {
            diag.error("Incorrect patching size (%lld).  Should be a multiple of (2 * ptrSize)", sectInfo.sectSize);
            return;
        }

        if ( sectInfo.reserved2 != elementSize ) {
            // ld64 must have rejected one or more of the elements in the section, so
            // didn't set the reserved2 to let us patch
            diag.error("reserved2 is unsupported value %d.  Expected %d",
                       sectInfo.reserved2, elementSize);
            return;
        }

        for ( uint64_t offset = 0; offset != sectInfo.sectSize; offset += elementSize ) {
            uint64_t targetRuntimeOffset = (sectInfo.sectAddr + offset) - loadAddress;
            handler(SingletonPatchKind::cfObj2, targetRuntimeOffset);
        }
    });
}


} // namespace dyld3

