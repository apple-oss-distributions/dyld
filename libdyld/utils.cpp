/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mach-o/utils.h>
#include <mach-o/utils_priv.h>
#include <mach-o/dyld_priv.h>

#include "MachOFile.h"
#include "FileUtils.h"
#include "DyldDelegates.h"

using dyld3::MachOFile;
using dyld3::FatFile;
using dyld3::GradedArchs;
using dyld3::Platform;


// used by unit tests
__attribute__((visibility("hidden")))
int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchs& launchArchs, const GradedArchs& dylibArchs,  bool isOSBinary,
                                    void (^bestSlice)(const struct mach_header* slice, uint64_t sliceOffset, size_t sliceSize));


///
/// arch-name <--> cpu-type
///

bool macho_cpu_type_for_arch_name(const char* archName, cpu_type_t* type, cpu_subtype_t* subtype)
{
    return MachOFile::cpuTypeFromArchName(archName, type, subtype);
}

const char* macho_arch_name_for_cpu_type(cpu_type_t type, cpu_subtype_t subtype)
{
    const char* result = MachOFile::archName(type, subtype);
    if ( strcmp(result, "unknown") == 0 )
        return nullptr;
    return result;
}

const char* macho_arch_name_for_mach_header(const mach_header* mh)
{
    if ( mh == nullptr )
        mh = _dyld_get_prog_image_header();
    return macho_arch_name_for_cpu_type(mh->cputype, mh->cpusubtype);
}



///
/// fat file utilities
///

int macho_for_each_slice(const char* path, void (^callback)(const struct mach_header* slice, uint64_t fileOffset, size_t size, bool* stop)__MACHO_NOESCAPE)
{
    int fd = ::open(path, O_RDONLY, 0);
    if ( fd == -1 )
        return errno;

    int result = macho_for_each_slice_in_fd(fd, callback);
    ::close(fd);
    return result;
}

int macho_for_each_slice_in_fd(int fd, void (^callback)(const struct mach_header* slice, uint64_t fileOffset, size_t size, bool* stop)__MACHO_NOESCAPE)
{
    struct stat statbuf;
    if ( ::fstat(fd, &statbuf) == -1 )
        return errno;

    const void* mappedFile = ::mmap(nullptr, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( mappedFile == MAP_FAILED )
        return errno;

    int result = 0;
    Diagnostics diag;
    if ( const FatFile* ff = FatFile::isFatFile(mappedFile) ) {
        ff->forEachSlice(diag, statbuf.st_size, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
            size_t sliceOffset = (uint8_t*)sliceStart - (uint8_t*)mappedFile;
            if ( callback )
                callback((mach_header*)sliceStart, sliceOffset, (size_t)sliceSize, &stop);
        });
        if ( diag.hasError() )
            result = EBADMACHO;
    }
    else if ( ((MachOFile*)mappedFile)->isMachO(diag, statbuf.st_size) ) {
        bool stop;
        if ( callback )
            callback((mach_header*)mappedFile, 0, (size_t)statbuf.st_size, &stop);
    }
    else {
        // not a fat file nor a mach-o file
        result = EFTYPE;
    }

    ::munmap((void*)mappedFile, (size_t)statbuf.st_size);
    return result;
}


int macho_best_slice(const char* path, void (^bestSlice)(const struct mach_header* slice, uint64_t sliceFileOffset, size_t sliceSize)__MACHO_NOESCAPE)
{
    int fd = ::open(path, O_RDONLY, 0);
    if ( fd == -1 )
        return errno;

    int result = macho_best_slice_in_fd(fd, bestSlice);
    ::close(fd);

    return result;
}

static bool launchableOnCurrentPlatform(const MachOFile* mf)
{
#if TARGET_OS_OSX
    // macOS is special and can launch macOS, catalyst, and iOS apps
    return ( mf->builtForPlatform(Platform::macOS) || mf->builtForPlatform(Platform::iOSMac) || mf->builtForPlatform(Platform::iOS) );
#else
    return mf->builtForPlatform(MachOFile::currentPlatform());
#endif
}

// use directly by unit tests
int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchs& launchArchs, const GradedArchs& dylibArchs, bool isOSBinary,
                                    void (^ _Nullable bestSlice)(const struct mach_header* slice, uint64_t fileOffset, size_t sliceSize))
{
    struct stat statbuf;
    if ( ::fstat(fd, &statbuf) == -1 )
        return errno;

    // FIXME: possible that mmap() of the whole file will fail on memory constrainted devices (e.g. watch), so may need to read() instead
    const void* mappedFile = ::mmap(nullptr, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( mappedFile == MAP_FAILED )
        return errno;
 
    Diagnostics  diag;
    int result = 0;
    if ( const FatFile* ff = FatFile::isFatFile(mappedFile) ) {
        __block int      bestGrade   = 0;
        __block uint64_t sliceOffset = 0;
        __block uint64_t sliceLen    = 0;
        ff->forEachSlice(diag, statbuf.st_size, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
            if ( const MachOFile* mf = MachOFile::isMachO(sliceStart) ) {
                if ( mf->filetype == MH_EXECUTE ) {
                    int sliceGrade = launchArchs.grade(mf->cputype, mf->cpusubtype, isOSBinary);
                    if ( (sliceGrade > bestGrade) && launchableOnCurrentPlatform(mf) ) {
                        sliceOffset = (char*)sliceStart - (char*)mappedFile;
                        sliceLen    = sliceSize;
                        bestGrade   = sliceGrade;
                    }
                }
                else {
                    int sliceGrade = dylibArchs.grade(mf->cputype, mf->cpusubtype, isOSBinary);
                    if ( (sliceGrade > bestGrade) && mf->loadableIntoProcess(platform, "") ) {
                        sliceOffset = (char*)sliceStart - (char*)mappedFile;
                        sliceLen    = sliceSize;
                        bestGrade   = sliceGrade;
                    }
                }
            }
        });
        if ( diag.hasError() )
            return EBADMACHO;

        if ( bestGrade != 0 ) {
            if ( bestSlice )
                bestSlice((MachOFile*)((char*)mappedFile + sliceOffset), (size_t)sliceOffset, (size_t)sliceLen);
        }
        else
            result = EBADARCH;
    }
    else if ( const MachOFile* mf = MachOFile::isMachO(mappedFile) ) {
        if ( (mf->filetype == MH_EXECUTE) && (launchArchs.grade(mf->cputype, mf->cpusubtype, isOSBinary) != 0) && launchableOnCurrentPlatform(mf) )  {
            // the "best" of a main executable must pass grading and be a launchable
            if ( bestSlice )
                bestSlice(mf, 0, (size_t)statbuf.st_size);
        }
        else if ( (dylibArchs.grade(mf->cputype, mf->cpusubtype, isOSBinary) != 0) && mf->loadableIntoProcess(platform, "") ) {
            // the "best" of a dylib/bundle must pass grading and match the platform of the current process
            if ( bestSlice )
                bestSlice(mf, 0, (size_t)statbuf.st_size);
        }
        else {
            result = EBADARCH;
        }
    }
    else {
        // not a fat file nor a mach-o file
        result = EFTYPE;
    }

    ::munmap((void*)mappedFile, (size_t)statbuf.st_size);
    return result;
}


int macho_best_slice_in_fd(int fd, void (^bestSlice)(const struct mach_header* slice, uint64_t sliceFileOffset, size_t sliceSize)__MACHO_NOESCAPE)
{
    const Platform     platform    = MachOFile::currentPlatform();
    const GradedArchs* launchArchs = &GradedArchs::forCurrentOS(false, false);
    const GradedArchs* dylibArchs  = &GradedArchs::forCurrentOS(false, false);
#if TARGET_OS_SIMULATOR
    const char* simArchNames = getenv("SIMULATOR_ARCHS");
    if ( simArchNames == nullptr )
        simArchNames = "x86_64";
    launchArchs = &GradedArchs::launchCurrentOS(simArchNames);
#endif
    return macho_best_slice_fd_internal(fd, platform, *launchArchs, *dylibArchs, false, bestSlice);
}


///
/// utils_priv.h
///
const char* _Nullable macho_dylib_install_name(const struct mach_header* _Nonnull mh)
{
    if ( const MachOFile* mf = MachOFile::isMachO(mh) )
        return mf->installName();

    return nullptr;
}
