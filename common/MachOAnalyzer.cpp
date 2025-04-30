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

#include <TargetConditionals.h>
#include "Defines.h"
#if !TARGET_OS_EXCLAVEKIT
  #include <sys/types.h>
  #include <sys/fcntl.h>
  #include <sys/mman.h>
  #include <mach/mach.h>
  #include <unistd.h>
#endif

#if SUPPORT_CLASSIC_RELOCS
  #include <mach-o/reloc.h>
  #include <mach-o/x86_64/reloc.h>
#endif

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/nlist.h>
#include <ptrauth.h>

#include "MachOAnalyzer.h"
#include "CodeSigningTypes.h"
#include "Array.h"
#include "Header.h"

// FIXME: We should get this from cctools
#define DYLD_CACHE_ADJ_V2_FORMAT 0x7F

using mach_o::Header;
using mach_o::Platform;

namespace dyld3 {

bool MachOAnalyzer::isValidMainExecutable(Diagnostics& diag, const char* path, uint64_t sliceLength,
                                          const GradedArchs& archs, Platform platform) const
{
    if ( !this->validMachOForArchAndPlatform(diag, (size_t)sliceLength, path, archs, platform, true) )
        return false;

    if ( !this->isDynamicExecutable() ) {
        diag.error("could not use '%s' because it is not an executable, filetype=0x%08X", path, this->filetype);
        return false;
    }

    if ( !this->validLinkedit(diag, path) )
        return false;

    return true;
}

#if !TARGET_OS_EXCLAVEKIT
bool MachOAnalyzer::loadFromBuffer(Diagnostics& diag, const closure::FileSystem& fileSystem,
                                   const char* path, const GradedArchs& archs, Platform platform,
                                   closure::LoadedFileInfo& info)
{
    // if fat, remap just slice needed
    bool fatButMissingSlice;
    const FatFile*       fh = (FatFile*)info.fileContent;
    uint64_t sliceOffset = info.sliceOffset;
    uint64_t sliceLen = info.sliceLen;
    if ( fh->isFatFileWithSlice(diag, info.fileContentLen, archs, info.isOSBinary, sliceOffset, sliceLen, fatButMissingSlice) ) {
        // unmap anything before slice
        fileSystem.unloadPartialFile(info, sliceOffset, sliceLen);
        // Update the info to keep track of the new slice offset.
        info.sliceOffset = sliceOffset;
        info.sliceLen = sliceLen;
    }
    else if ( diag.hasError() ) {
        // We must have generated an error in the fat file parsing so use that error
        fileSystem.unloadFile(info);
        return false;
    }
    else if ( fatButMissingSlice ) {
        diag.error("missing compatible arch in %s", path);
        fileSystem.unloadFile(info);
        return false;
    }

    const MachOAnalyzer* mh = (MachOAnalyzer*)info.fileContent;

    // validate is mach-o of requested arch and platform
    if ( !mh->validMachOForArchAndPlatform(diag, (size_t)info.sliceLen, path, archs, platform, info.isOSBinary) ) {
        fileSystem.unloadFile(info);
        return false;
    }

    // if has zero-fill expansion, re-map
    if ( !mh->isPreload() )
        mh = mh->remapIfZeroFill(diag, fileSystem, info);

    // on error, remove mappings and return nullptr
    if ( diag.hasError() ) {
        fileSystem.unloadFile(info);
        return false;
    }

    // now that LINKEDIT is at expected offset, finish validation
    if ( !mh->isPreload() )
        mh->validLinkedit(diag, path);

    // on error, remove mappings and return nullptr
    if ( diag.hasError() ) {
        fileSystem.unloadFile(info);
        return false;
    }

    return true;
}


closure::LoadedFileInfo MachOAnalyzer::load(Diagnostics& diag, const closure::FileSystem& fileSystem,
                                            const char* path, const GradedArchs& archs, Platform platform, char realerPath[PATH_MAX])
{
    // FIXME: This should probably be an assert, but if we happen to have a diagnostic here then something is wrong
    // above us and we should quickly return instead of doing unnecessary work.
    if (diag.hasError())
        return closure::LoadedFileInfo();

    closure::LoadedFileInfo info;
    void (^fileErrorLog)(const char *format, ...) __printflike(1, 2)
        = ^(const char *format, ...) __printflike(1, 2) {
            va_list list;
            va_start(list, format);
            diag.error(format, va_list_wrap(list));
            va_end(list);
        };
    if ( !fileSystem.loadFile(path, info, realerPath, fileErrorLog) ) {
        return closure::LoadedFileInfo();
    }

    // If we now have an error, but succeeded, then we must have tried multiple paths, one of which errored, but
    // then succeeded on a later path.  So clear the error.
    if (diag.hasError())
        diag.clearError();

    bool loaded = loadFromBuffer(diag, fileSystem, path, archs, platform, info);
    if (!loaded)
        return {};
    return info;
}


// for use with already mmap()ed file
bool MachOAnalyzer::isOSBinary(int fd, uint64_t sliceOffset, uint64_t sliceSize) const
{
#ifdef F_GETSIGSINFO
    if ( fd == -1 )
        return false;

    uint32_t sigOffset;
    uint32_t sigSize;
    if ( !((const Header*)this)->hasCodeSignature(sigOffset, sigSize) )
        return false;

    // register code signature
    fsignatures_t sigreg;
    sigreg.fs_file_start = sliceOffset;                // start of mach-o slice in fat file
    sigreg.fs_blob_start = (void*)(long)sigOffset;     // start of CD in mach-o file
    sigreg.fs_blob_size  = sigSize;                    // size of CD
    if ( ::fcntl(fd, F_ADDFILESIGS_RETURN, &sigreg) == -1 )
        return false;

    // ask if code signature is for something in the OS
    fgetsigsinfo siginfo = { (off_t)sliceOffset, GETSIGSINFO_PLATFORM_BINARY, 0 };
    if ( ::fcntl(fd, F_GETSIGSINFO, &siginfo) == -1 )
        return false;

    return (siginfo.fg_sig_is_platform);
#else
    return false;
#endif
}

// for use when just the fat_header has been read
bool MachOAnalyzer::sliceIsOSBinary(int fd, uint64_t sliceOffset, uint64_t sliceSize)
{
    if ( fd == -1 )
        return false;

    // need to mmap() slice so we can find the code signature
	void* mappedSlice = ::mmap(nullptr, (size_t)sliceSize, PROT_READ, MAP_PRIVATE, fd, sliceOffset);
	if ( mappedSlice == MAP_FAILED )
		return false;

    const MachOAnalyzer* ma = (MachOAnalyzer*)mappedSlice;
    bool result = ma->isOSBinary(fd, sliceOffset, sliceSize);
    ::munmap(mappedSlice, (size_t)sliceSize);

    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT



#if DEBUG
// only used in debug builds of cache builder to verify segment moves are valid
void MachOAnalyzer::validateDyldCacheDylib(Diagnostics& diag, const char* path) const
{
    validLinkedit(diag, path);
    validSegments(diag, path, 0xffffffff);
}
#endif

bool MachOAnalyzer::validMachOForArchAndPlatform(Diagnostics& diag, size_t sliceLength, const char* path, const GradedArchs& archs, Platform reqPlatform, bool isOSBinary, bool internalInstall) const
{
    // must start with mach-o magic value
    if ( (this->magic != MH_MAGIC) && (this->magic != MH_MAGIC_64) ) {
        diag.error("could not use '%s' because it is not a mach-o file: 0x%08X 0x%08X", path, this->magic, this->cputype);
        return false;
    }

    if ( !archs.grade(this->cputype, this->cpusubtype, isOSBinary) ) {
        diag.error("could not use '%s' because it is not a compatible arch", path);
        return false;
    }

    // must be a filetype dyld can load
    switch ( this->filetype ) {
        case MH_EXECUTE:
        case MH_DYLIB:
        case MH_BUNDLE:
        case MH_DYLINKER:
           break;
#if BUILDING_DYLDINFO || BUILDING_APP_CACHE_UTIL
        // Allow offline tools to analyze binaries dyld doesn't load
        case MH_KEXT_BUNDLE:
        case MH_FILESET:
        case MH_PRELOAD:
            break;
#endif
        default:
            diag.error("could not use '%s' because it is not a dylib, bundle, or executable, filetype=0x%08X", path, this->filetype);
           return false;
    }

    // validate load commands structure
    if ( !this->validLoadCommands(diag, path, sliceLength) ) {
        return false;
    }

    // filter out static executables
    if ( (this->filetype == MH_EXECUTE) && !isDynamicExecutable() ) {
#if !BUILDING_DYLDINFO && !BUILDING_APP_CACHE_UTIL
        // dyldinfo should be able to inspect static executables such as the kernel
        diag.error("could not use '%s' because it is a static executable", path);
        return false;
#endif
    }

    // HACK: If we are asking for no platform, then make sure the binary doesn't have one
#if BUILDING_DYLDINFO || BUILDING_APP_CACHE_UTIL
    if ( isFileSet() ) {
        // A statically linked kernel collection should contain a 0 platform
        mach_o::PlatformAndVersions pvs = ((const Header*)this)->platformAndVersions();
        if ( !pvs.platform.empty() ) {
            diag.error("could not use '%s' because is has the wrong platform", path);
            return false;
        }
    } else if ( reqPlatform.empty() ) {
        // This is handled elsewhere in the kernel collection builder, where we have access
        // to the kernel binary and can infer its platform
    } else
#endif
    if ( !((const Header*)this)->loadableIntoProcess(reqPlatform, path, internalInstall) ) {
        diag.error("could not use '%s' because it was not built for platform %s", path, reqPlatform.name().c_str());
        return false;
    }

    // validate dylib loads
    if ( !validEmbeddedPaths(diag, reqPlatform, path, internalInstall) )
        return false;

    // validate segments
    if ( !validSegments(diag, path, sliceLength) )
        return false;

    // validate entry
    if ( this->filetype == MH_EXECUTE ) {
        if ( !validMain(diag, path) )
            return false;
    }

    // further validations done in validLinkedit()

    return true;
}

bool MachOAnalyzer::validLinkedit(Diagnostics& diag, const char* path) const
{
    // validate LINKEDIT layout
    if ( !validLinkeditLayout(diag, path) )
        return false;

    // rdar://75492733 (enforce that binaries built against Fall2021 SDK have a LC_UUID)
    if ( enforceFormat(Malformed::noUUID) ) {
        if ( !hasLoadCommand(LC_UUID) ) {
            diag.error("missing LC_UUID");
            return false;
        }
    }

    if ( hasLoadCommand(LC_DYLD_CHAINED_FIXUPS) ) {
        if ( !validChainedFixupsInfo(diag, path) )
            return false;
    }
#if SUPPORT_ARCH_arm64e
    else if ( (this->cputype == CPU_TYPE_ARM64) && (this->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) ) {
        if ( !validChainedFixupsInfoOldArm64e(diag, path) )
            return false;
    }
#endif
    else {
        // validate rebasing info
        if ( !validRebaseInfo(diag, path) )
            return false;

       // validate binding info
        if ( !validBindInfo(diag, path) )
            return false;
    }

    return true;
}

bool MachOAnalyzer::validLoadCommands(Diagnostics& diag, const char* path, size_t fileLen) const
{
    // check load command don't exceed file length
    if ( this->sizeofcmds + machHeaderSize() > fileLen ) {
        diag.error("in '%s' load commands exceed length of file", path);
        return false;
    }

    // walk all load commands and sanity check them
    Diagnostics walkDiag;
    forEachLoadCommand(walkDiag, ^(const load_command* cmd, bool& stop) {});
    if ( walkDiag.hasError() ) {
#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
        diag.error("in '%s' %s", path, walkDiag.errorMessage().c_str());
#else
        diag.error("in '%s' %s", path, walkDiag.errorMessage());
#endif
        return false;
    }

    // check load commands fit in TEXT segment
    __block bool foundTEXT    = false;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.segmentName == "__TEXT" ) {
            foundTEXT = true;
            if ( this->sizeofcmds + machHeaderSize() > info.fileSize ) {
                diag.error("in '%s' load commands exceed length of __TEXT segment", path);
            }
            if ( (info.fileOffset != 0) && !this->isPreload() ) {
                diag.error("in '%s' __TEXT segment not start of mach-o", path);
            }
            stop = true;
        }
    });
    if ( !diag.noError() && !foundTEXT ) {
        diag.error("in '%s' __TEXT segment not found", path);
        return false;
    }

    return true;
}

#if !TARGET_OS_EXCLAVEKIT
const MachOAnalyzer* MachOAnalyzer::remapIfZeroFill(Diagnostics& diag, const closure::FileSystem& fileSystem, closure::LoadedFileInfo& info) const
{
    uint64_t vmSpaceRequired;
    bool     hasZeroFill;
    analyzeSegmentsLayout(vmSpaceRequired, hasZeroFill);

    if ( hasZeroFill ) {
        vm_address_t newMappedAddr;
        if ( ::vm_allocate(mach_task_self(), &newMappedAddr, (size_t)vmSpaceRequired, VM_FLAGS_ANYWHERE) != 0 ) {
            diag.error("vm_allocate failure");
            return nullptr;
        }

        // re-map each segment read-only, with runtime layout
#if BUILDING_APP_CACHE_UTIL
        // The auxKC is mapped with __DATA first, so we need to get either the __DATA or __TEXT depending on what is earliest
        __block uint64_t baseAddress = ~0ULL;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& segmentInfo, bool& stop) {
            baseAddress = std::min(baseAddress, segmentInfo.vmaddr);
        });
        uint64_t textSegVMAddr = ((const Header*)this)->preferredLoadAddress();
#else
        uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
#endif

        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& segmentInfo, bool& stop) {
            if ( (segmentInfo.fileSize != 0) && (segmentInfo.vmsize != 0) ) {
                kern_return_t r = vm_copy(mach_task_self(), (vm_address_t)((long)info.fileContent+segmentInfo.fileOffset), (vm_size_t)segmentInfo.fileSize, (vm_address_t)(newMappedAddr+segmentInfo.vmaddr-baseAddress));
                if ( r != KERN_SUCCESS ) {
                    diag.error("vm_copy() failure");
                    stop = true;
                }
            }
        });
        if ( diag.noError() ) {
            // remove original mapping and return new mapping
            fileSystem.unloadFile(info);

            // make the new mapping read-only
            ::vm_protect(mach_task_self(), newMappedAddr, (vm_size_t)vmSpaceRequired, false, VM_PROT_READ);

#if BUILDING_APP_CACHE_UTIL
            if ( textSegVMAddr != baseAddress ) {
                info.unload = [](const closure::LoadedFileInfo& info) {
                    // Unloading binaries where __DATA is first requires working out the real range of the binary
                    // The fileContent points at the mach_header, not the actaul start of the file content, unfortunately.
                    const Header* hdr = (const Header*)info.fileContent;
                    __block uint64_t baseAddress = ~0ULL;
                    hdr->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
                        baseAddress = std::min(baseAddress, segInfo.vmaddr);
                    });
                    uint64_t textSegVMAddr = hdr->preferredLoadAddress();

                    uint64_t basePointerOffset = textSegVMAddr - baseAddress;
                    uint8_t* bufferStart = (uint8_t*)info.fileContent - basePointerOffset;
                    ::vm_deallocate(mach_task_self(), (vm_address_t)bufferStart, (size_t)info.fileContentLen);
                };

                // And update the file content to the new location
                info.fileContent = (const void*)(newMappedAddr + textSegVMAddr - baseAddress);
                info.fileContentLen = vmSpaceRequired;
                return (const MachOAnalyzer*)info.fileContent;
            }
#endif

            // Set vm_deallocate as the unload method.
            info.unload = [](const closure::LoadedFileInfo& info) {
                ::vm_deallocate(mach_task_self(), (vm_address_t)info.fileContent, (size_t)info.fileContentLen);
            };

            // And update the file content to the new location
            info.fileContent = (const void*)newMappedAddr;
            info.fileContentLen = vmSpaceRequired;
            return (const MachOAnalyzer*)info.fileContent;
        }
        else {
            // new mapping failed, return old mapping with an error in diag
            ::vm_deallocate(mach_task_self(), newMappedAddr, (size_t)vmSpaceRequired);
            return nullptr;
        }
    }

    return this;
}
#endif // !TARGET_OS_EXCLAVEKIT

bool MachOAnalyzer::validEmbeddedPaths(Diagnostics& diag, Platform platform, const char* path, bool internalInstall) const
{
    __block int         index = 1;
    __block bool        allGood = true;
    __block int         dependentsCount = 0;
    __block const char* installName = nullptr;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        const dylib_command* dylibCmd;
        const rpath_command* rpathCmd;
        switch ( cmd->cmd ) {
            case LC_ID_DYLIB:
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
                dylibCmd = (dylib_command*)cmd;
                if ( dylibCmd->dylib.name.offset > cmd->cmdsize ) {
                    diag.error("in '%s' load command #%d name offset (%u) outside its size (%u)", path, index, dylibCmd->dylib.name.offset, cmd->cmdsize);
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
                        diag.error("in '%s' load command #%d string extends beyond end of load command", path, index);
                        stop = true;
                        allGood = false;
                    }
                }
                if ( cmd->cmd  == LC_ID_DYLIB )
                    installName = (char*)dylibCmd + dylibCmd->dylib.name.offset;
                else
                    ++dependentsCount;
                break;
            case LC_RPATH:
                rpathCmd = (rpath_command*)cmd;
                if ( rpathCmd->path.offset > cmd->cmdsize ) {
                    diag.error("in '%s' load command #%d path offset (%u) outside its size (%u)", path, index, rpathCmd->path.offset, cmd->cmdsize);
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
                        diag.error("in '%s' load command #%d string extends beyond end of load command", path, index);
                        stop = true;
                        allGood = false;
                    }
                }
                break;
        }
        ++index;
    });
    if ( !allGood )
        return false;

    if ( this->filetype == MH_DYLIB ) {
        if ( installName == nullptr ) {
            diag.error("in '%s' MH_DYLIB is missing LC_ID_DYLIB", path);
            return false;
        }

        if ( this->enforceFormat(MachOAnalyzer::Malformed::loaderPathsAreReal) ) {
            // new binary, so check that part after @xpath/ is real (not symlinks)
            if ( (strncmp(installName, "@loader_path/", 13) == 0) || (strncmp(installName, "@executable_path/", 17) == 0) ) {
                if ( const char* s = strchr(installName, '/') ) {
                    while (strncmp(s, "/..", 3) == 0)
                        s += 3;
                    const char* trailingInstallPath = s;
                    const char* trailingRealPath = &path[strlen(path)-strlen(trailingInstallPath)];
                    if (strcmp(trailingRealPath, trailingInstallPath) != 0 ) {
                        diag.error("install name '%s' contains symlinks", installName);
                        return false;
                    }
                }
            }
        }
    }
    else {
        if ( installName != nullptr ) {
            diag.error("in '%s' LC_ID_DYLIB found in non-MH_DYLIB", path);
            return false;
        }
    }

    // all new binaries must link with something else
    if ( (dependentsCount == 0) && enforceFormat(Malformed::noLinkedDylibs) ) {
        const Header *hdr = (const Header*)this;
        const char* libSystemDir = hdr->builtForPlatform(Platform::driverKit, true) ? "/System/DriverKit/usr/lib/system/" : "/usr/lib/system/";
        // except for dylibs in libSystem.dylib which are ok to link with nothing (they are on bottom)
        bool isNotLibSystem = (installName == nullptr) || (strncmp(installName, libSystemDir, strlen(libSystemDir)) != 0);

        if ( internalInstall && (   hdr->builtForPlatform(Platform::macOS_exclaveKit, true)
                                 || hdr->builtForPlatform(Platform::iOS_exclaveKit, true)
                                 || hdr->builtForPlatform(Platform::tvOS_exclaveKit, true)
                                 || hdr->builtForPlatform(Platform::watchOS_exclaveKit, true)
                                 || hdr->builtForPlatform(Platform::visionOS_exclaveKit, true)) ) {
            // The path of ExclaveKit libSystem libraries starts with /System/ExclaveKit
            const size_t prefixLength = 18;
            isNotLibSystem = true;
            if ( installName != nullptr && strlen(installName) > prefixLength )
                if ( strncmp(installName + prefixLength, "/usr/lib/system/", 16) == 0 )
                    isNotLibSystem = false;
        }
        if ( this->isDyldManaged() && isNotLibSystem ) {
            diag.error("in '%s' missing LC_LOAD_DYLIB (must link with at least libSystem.dylib)", path);
            return false;
        }
    }

    return true;
}


bool MachOAnalyzer::validMain(Diagnostics& diag, const char* path) const
{
    if ( this->inDyldCache() && MachOAnalyzer::enforceFormat(Malformed::mainExecInDyldCache) ) {
        diag.error("MH_EXECUTE is in dyld shared cache");
        return false;
    }

    __block int mainCount   = 0;
    __block int threadCount = 0;
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        switch (cmd->cmd) {
            case LC_MAIN: {
                ++mainCount;
                entry_point_command* mainCmd = (entry_point_command*)cmd;
                uint64_t startAddress = ((const Header*)this)->preferredLoadAddress() + mainCmd->entryoff;

                __block bool foundSegment = false;
                ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stopSegment) {
                    // Skip segments which don't contain this address
                    if ( (startAddress < info.vmaddr) || (startAddress >= info.vmaddr+info.vmsize) )
                        return;
                    foundSegment = true;
                    if ( !info.executable() )
                        diag.error("LC_MAIN points to non-executable segment");
                    stopSegment = true;
                });
                if (!foundSegment)
                    diag.error("LC_MAIN entryoff is out of range");

                stop = true;
                break;
            }
            case LC_UNIXTHREAD: {
                ++threadCount;
                uint64_t startAddress = entryAddrFromThreadCmd((thread_command*)cmd);
                if ( startAddress == 0 ) {
                    diag.error("LC_UNIXTHREAD not valid for arch %s", archName());
                    stop = true;
                } else {
                    __block bool foundSegment = false;
                    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stopSegment) {
                        // Skip segments which don't contain this address
                        if ( (startAddress < info.vmaddr) || (startAddress >= info.vmaddr+info.vmsize) )
                            return;
                        foundSegment = true;
                        if ( !info.executable() ) {
                            // Suppress this error for the x86_64 kernel
                            if ( !this->isStaticExecutable() )
                                diag.error("LC_UNIXTHREAD points to non-executable segment");
                        }
                        stopSegment = true;
                    });
                    if (!foundSegment)
                        diag.error("LC_UNIXTHREAD entry is out of range");
                    stop = true;
                }
                break;
            }
        }
    });
    if ( diag.hasError() )
        return false;

    if ( ((const Header*)this)->builtForPlatform(Platform::driverKit) ) {
        if ( mainCount + threadCount == 0 )
            return true;
        diag.error("LC_MAIN not allowed for driverkit");
        return false;
    }

    if ( mainCount+threadCount == 1 )
        return true;
    if ( mainCount + threadCount == 0 )
        diag.error("missing LC_MAIN or LC_UNIXTHREAD");
    else
        diag.error("only one LC_MAIN or LC_UNIXTHREAD is allowed");
    return false;
}


namespace {
    struct LinkEditContentChunk
    {
        const char* name;
        uint32_t    alignment;
        uint32_t    fileOffsetStart;
        uint32_t    size;

        // only have a few chunks, so bubble sort is ok.  Don't use libc's qsort because it may call malloc
        static void sort(LinkEditContentChunk array[], unsigned long count)
        {
            for (unsigned i=0; i < count-1; ++i) {
                bool done = true;
                for (unsigned j=0; j < count-i-1; ++j) {
                    if ( array[j].fileOffsetStart > array[j+1].fileOffsetStart ) {
                        LinkEditContentChunk temp = array[j];
                        array[j]   = array[j+1];
                        array[j+1] = temp;
                        done = false;
                    }
                }
                if ( done )
                    break;
            }
        }
    };
} // anonymous namespace



bool MachOAnalyzer::validLinkeditLayout(Diagnostics& diag, const char* path) const
{
    __block bool result = false;
    this->withVMLayout(diag, ^(const mach_o::Layout &layout) {
        result = layout.isValidLinkeditLayout(diag, path);
    });
    return result;
}



bool MachOAnalyzer::invalidRebaseState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                                      bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind) const
{
    if ( !segIndexSet ) {
        diag.error("in '%s' %s missing preceding REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path, opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
        diag.error("in '%s' %s segment index %d too large", path, opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (segments[segmentIndex].vmsize - ptrSize) ) {
        diag.error("in '%s' %s current segment offset 0x%08llX beyond segment size (0x%08llX)", path, opcodeName, segmentOffset, segments[segmentIndex].vmsize);
        return true;
    }
    switch ( kind )  {
        case Rebase::pointer32:
        case Rebase::pointer64:
            if ( !segments[segmentIndex].writable() && enforceFormat(Malformed::writableData) ) {
                diag.error("in '%s' %s pointer rebase is in non-writable segment", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].executable() && enforceFormat(Malformed::executableData) ) {
                diag.error("in '%s' %s pointer rebase is in executable segment", path, opcodeName);
                return true;
            }
            break;
        case Rebase::textAbsolute32:
        case Rebase::textPCrel32:
            if ( segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s text rebase is in writable segment", path, opcodeName);
                return true;
            }
            if ( !segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer rebase is in non-executable segment", path, opcodeName);
                return true;
            }
            break;
        case Rebase::unknown:
            diag.error("in '%s' %s unknown rebase type", path, opcodeName);
            return true;
    }
    return false;
}


void MachOAnalyzer::getAllSegmentsInfos(Diagnostics& diag, Header::SegmentInfo segments[]) const
{
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        segments[info.segmentIndex] = info;
    });
}


bool MachOAnalyzer::validRebaseInfo(Diagnostics& diag, const char* path) const
{
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop) {
        if ( invalidRebaseState(diag, opcodeName, path, leInfo, segments, segIndexSet, ptrSize, segmentIndex, segmentOffset, kind) )
            stop = true;
    });
    return diag.noError();
}


void MachOAnalyzer::forEachTextRebase(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop) {
        if ( kind != Rebase::textAbsolute32 )
            return;
        if ( !startVmAddrSet ) {
            for (int i=0; i <= segmentIndex; ++i) {
                if ( segments[i].segmentName == "__TEXT" ) {
                    startVmAddr = segments[i].vmaddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t rebaseVmAddr  = segments[segmentIndex].vmaddr + segmentOffset;
        uint64_t runtimeOffset = rebaseVmAddr - startVmAddr;
        handler(runtimeOffset, stop);
    });
}

void MachOAnalyzer::forEachRebase(Diagnostics& diag, void (^callback)(uint64_t runtimeOffset, bool isLazyPointerRebase, bool& stop)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    __block uint64_t lpVmAddr       = 0;
    __block uint64_t lpEndVmAddr    = 0;
    __block uint64_t shVmAddr       = 0;
    __block uint64_t shEndVmAddr    = 0;
    forEachSection(^(const Header::SectionInfo& info, bool &stop) {
        if ( (info.flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ) {
            lpVmAddr    = info.address;
            lpEndVmAddr = info.address + info.size;
        }
        else if ( (info.flags & S_ATTR_PURE_INSTRUCTIONS) && (info.sectionName == "__stub_helper") ) {
            shVmAddr    = info.address;
            shEndVmAddr = info.address + info.size;
        }
    });
    forEachRebase(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                          bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop) {
        switch ( kind ) {
            case Rebase::unknown:
                return;
            case Rebase::pointer32:
            case Rebase::pointer64:
                // We only handle these kinds for now.
                break;
            case Rebase::textPCrel32:
            case Rebase::textAbsolute32:
                return;
        }
        if ( !startVmAddrSet ) {
            for (int i=0; i < segmentIndex; ++i) {
                if ( segments[i].segmentName == "__TEXT" ) {
                    startVmAddr = segments[i].vmaddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t rebaseVmAddr  = segments[segmentIndex].vmaddr + segmentOffset;
        bool isLazyPointerRebase = false;
        if ( (rebaseVmAddr >= lpVmAddr) && (rebaseVmAddr < lpEndVmAddr) ) {
            // rebase is in lazy pointer section
            uint64_t lpValue = 0;
            if ( ptrSize == 8 )
                lpValue = *((uint64_t*)(rebaseVmAddr-startVmAddr+(uint8_t*)this));
            else
                lpValue = *((uint32_t*)(rebaseVmAddr-startVmAddr+(uint8_t*)this));
            if ( (lpValue >= shVmAddr) && (lpValue < shEndVmAddr) ) {
                // content is into stub_helper section
                uint64_t lpTargetImageOffset = lpValue - startVmAddr;
                const uint8_t* helperContent = (uint8_t*)this + lpTargetImageOffset;
                bool isLazyStub = contentIsRegularStub(helperContent);
                // ignore rebases for normal lazy pointers, but leave rebase for resolver helper stub
                if ( isLazyStub )
                    isLazyPointerRebase = true;
            }
            else {
                // if lazy pointer does not point into stub_helper, then it points to weak-def symbol and we need rebase
            }
        }
        uint64_t runtimeOffset = rebaseVmAddr - startVmAddr;
        callback(runtimeOffset, isLazyPointerRebase, stop);
    });
}



void MachOAnalyzer::forEachRebase(Diagnostics& diag, bool ignoreLazyPointers, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    forEachRebase(diag, ^(uint64_t runtimeOffset, bool isLazyPointerRebase, bool& stop) {
        if ( isLazyPointerRebase && ignoreLazyPointers )
            return;
        handler(runtimeOffset, stop);
    });
}

bool MachOAnalyzer::contentIsRegularStub(const uint8_t* helperContent) const
{
    switch (this->cputype) {
        case CPU_TYPE_X86_64:
            return ( (helperContent[0] == 0x68) && (helperContent[5] == 0xE9) ); // push $xxx / JMP pcRel
        case CPU_TYPE_I386:
            return ( (helperContent[0] == 0x68) && (helperContent[5] == 0xFF) && (helperContent[2] == 0x26) ); // push $xxx / JMP *pcRel
        case CPU_TYPE_ARM:
            return ( (helperContent[0] == 0x00) && (helperContent[1] == 0xC0) && (helperContent[2] == 0x9F) && (helperContent[3] == 0xE5) ); // ldr  ip, [pc, #0]
        case CPU_TYPE_ARM64:
            return ( (helperContent[0] == 0x50) && (helperContent[1] == 0x00) && (helperContent[2] == 0x00) && (helperContent[3] == 0x18) ); // ldr w16, L0

    }
    return false;
}


void MachOAnalyzer::forEachRebase(Diagnostics& diag,
                                 void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                                                 bool segIndexSet, uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                                 Rebase kind, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    const Rebase pointerRebaseKind = is64() ? Rebase::pointer64 : Rebase::pointer32;

    if ( leInfo.dyldInfo != nullptr ) {
        const uint8_t* const start = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
        const uint8_t* const end   = start + leInfo.dyldInfo->rebase_size;
        const uint8_t* p           = start;
        const uint32_t ptrSize     = pointerSize();
        Rebase   kind = Rebase::unknown;
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
                    // Allow some padding, in case rebases were somehow aligned to 16-bytes in size
                    if ( (end - p) > 15 )
                        diag.error("rebase opcodes terminated early at offset %d of %d", (int)(p-start), (int)(end-start));
                    stop = true;
                    break;
                case REBASE_OPCODE_SET_TYPE_IMM:
                    switch ( immediate ) {
                        case REBASE_TYPE_POINTER:
                            kind = pointerRebaseKind;
                            break;
                        case REBASE_TYPE_TEXT_ABSOLUTE32:
                            kind = Rebase::textAbsolute32;
                            break;
                        case REBASE_TYPE_TEXT_PCREL32:
                            kind = Rebase::textPCrel32;
                            break;
                        default:
                            kind = Rebase::unknown;
                            break;
                    }
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
                    segOffset += immediate*ptrSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                    for (int i=0; i < immediate; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_IMM_TIMES", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                        segOffset += ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                    count = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                        segOffset += ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                    handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += read_uleb128(diag, p, end) + ptrSize;
                    break;
                case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    if ( diag.hasError() )
                        break;
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                        segOffset += skip + ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                default:
                    diag.error("unknown rebase opcode 0x%02X", opcode);
            }
        }
        return;
    }

    if ( leInfo.chainedFixups != nullptr ) {
        // binary uses chained fixups, so do nothing
        // The kernel collections need to support both chained and classic relocations
        // If we are anything other than a kernel collection, then return here as we won't have
        // anything else to do.
        if ( !isFileSet() )
            return;
    }

#if SUPPORT_CLASSIC_RELOCS
    if ( leInfo.dynSymTab != nullptr ) {
        // old binary, walk relocations
        const uint64_t                  relocsStartAddress = localRelocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->locreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nlocrel];
        bool                            stop = false;
        const uint8_t                   relocSize = (is64() ? 3 : 2);
        const uint8_t                   ptrSize   = pointerSize();
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(relocation_info, relocs, 2048);
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            if ( reloc->r_length != relocSize ) {
                bool shouldEmitError = true;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
                if ( usesClassicRelocationsInKernelCollection() && (reloc->r_length == 2) && (relocSize == 3) )
                    shouldEmitError = false;
#endif
                if ( shouldEmitError ) {
                    diag.error("local relocation has wrong r_length");
                    break;
                }
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
                diag.error("local relocation has wrong r_type");
                break;
            }
            relocs.push_back(*reloc);
        }
        if ( !relocs.empty() ) {
            sortRelocations(relocs);
            for (relocation_info reloc : relocs) {
                uint32_t addrOff = reloc.r_address;
                uint32_t segIndex  = 0;
                uint64_t segOffset = 0;
                uint64_t addr = 0;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
                // xnu for x86_64 has __HIB mapped before __DATA, so offsets appear to be
                // negative
                if ( isStaticExecutable() || isFileSet() ) {
                    addr = relocsStartAddress + (int32_t)addrOff;
                } else {
                    addr = relocsStartAddress + addrOff;
                }
#else
                addr = relocsStartAddress + addrOff;
#endif
                if ( segIndexAndOffsetForAddress(addr, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                    Rebase kind = (reloc.r_length == 2) ? Rebase::pointer32 : Rebase::pointer64;
                    if ( this->cputype == CPU_TYPE_I386 ) {
                        if ( segmentsInfo[segIndex].executable() )
                            kind = Rebase::textAbsolute32;
                    }
                    handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, kind, stop);
                }
                else {
                    diag.error("local relocation has out of range r_address");
                    break;
                }
            }
        }
        // then process indirect symbols
        forEachIndirectPointer(diag, false,
                               ^(uint64_t address, bool bind, int bindLibOrdinal,
                                 const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( bind )
               return;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, pointerRebaseKind, indStop);
            }
            else {
                diag.error("local relocation has out of range r_address");
                indStop = true;
            }
        });
    }
#endif // SUPPORT_CLASSIC_RELOCS
}

bool MachOAnalyzer::segIndexAndOffsetForAddress(uint64_t addr, const Header::SegmentInfo segmentsInfos[], uint32_t segCount, uint32_t& segIndex, uint64_t& segOffset) const
{
    for (uint32_t i=0; i < segCount; ++i) {
        if ( (segmentsInfos[i].vmaddr <= addr) && (addr < segmentsInfos[i].vmaddr+segmentsInfos[i].vmsize) ) {
            segIndex  = i;
            segOffset = addr - segmentsInfos[i].vmaddr;
            return true;
        }
    }
    return false;
}

uint64_t MachOAnalyzer::localRelocBaseAddress(const Header::SegmentInfo segmentsInfos[], uint32_t segCount) const
{
    if ( isArch("x86_64") || isArch("x86_64h") ) {
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
        if ( isKextBundle() ) {
            // for kext bundles the reloc base address starts at __TEXT segment
            return segmentsInfos[0].vmaddr;
        }
#endif
        // for all other kinds, the x86_64 reloc base address starts at first writable segment (usually __DATA)
        for (uint32_t i=0; i < segCount; ++i) {
            if ( segmentsInfos[i].writable() )
                return segmentsInfos[i].vmaddr;
        }
    }
    // reloc base address is start of TEXT segment
    if ( this->isMainExecutable() && (segmentsInfos[0].initProt == 0) )
        return segmentsInfos[1].vmaddr;
    else
        return segmentsInfos[0].vmaddr;
}

uint64_t MachOAnalyzer::externalRelocBaseAddress(const Header::SegmentInfo segmentsInfos[], uint32_t segCount) const
{
    // Dyld caches are too large for a raw r_address, so everything is an offset from the base address
    if ( inDyldCache() ) {
        return ((const Header*)this)->preferredLoadAddress();
    }

#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
    if ( isKextBundle() ) {
        // for kext bundles the reloc base address starts at __TEXT segment
        return ((const Header*)this)->preferredLoadAddress();
    }
#endif

    if ( isArch("x86_64") || isArch("x86_64h") ) {
        // for x86_64 reloc base address starts at first writable segment (usually __DATA)
        for (uint32_t i=0; i < segCount; ++i) {
            if ( segmentsInfos[i].writable() )
                return segmentsInfos[i].vmaddr;
        }
    }
    // For everyone else we start at 0
    return 0;
}



void MachOAnalyzer::forEachIndirectPointer(Diagnostics& diag, bool supportPrivateExternsWorkaround,
                                           void (^handler)(uint64_t pointerAddress, bool bind, int bindLibOrdinal, const char* bindSymbolName,
                                                           bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    // find lazy and non-lazy pointer sections
    const bool              is64Bit                  = is64();
    const uint32_t* const   indirectSymbolTable      = (uint32_t*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->indirectsymoff);
    const uint32_t          indirectSymbolTableCount = leInfo.dynSymTab->nindirectsyms;
    const uint32_t          ptrSize                  = pointerSize();
    const void*             symbolTable              = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
    const struct nlist_64*  symbols64                = (nlist_64*)symbolTable;
    const struct nlist*     symbols32                = (struct nlist*)symbolTable;
    const char*             stringPool               = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    uint32_t                symCount                 = leInfo.symTab->nsyms;
    uint32_t                poolSize                 = leInfo.symTab->strsize;
    __block bool            stop                     = false;

    // Old kexts put S_LAZY_SYMBOL_POINTERS on the __got section, even if they didn't have indirect symbols to prcess.
    // In that case, skip the loop as there shouldn't be anything to process
    if ( (indirectSymbolTableCount == 0) && isKextBundle() )
        return;

    forEachSection(^(const Header::SectionInfo& sectInfo, bool& sectionStop) {
        uint8_t  sectionType  = (sectInfo.flags & SECTION_TYPE);
        bool selfModifyingStub = (sectionType == S_SYMBOL_STUBS) && (sectInfo.flags & S_ATTR_SELF_MODIFYING_CODE) && (sectInfo.reserved2 == 5) && (this->cputype == CPU_TYPE_I386);
        if ( (sectionType != S_LAZY_SYMBOL_POINTERS) && (sectionType != S_NON_LAZY_SYMBOL_POINTERS) && !selfModifyingStub )
            return;
        if ( (flags & S_ATTR_SELF_MODIFYING_CODE) && !selfModifyingStub ) {
            diag.error("S_ATTR_SELF_MODIFYING_CODE section type only valid in old i386 binaries");
            sectionStop = true;
            return;
        }
        uint32_t elementSize = selfModifyingStub ? sectInfo.reserved2 : ptrSize;
        uint32_t elementCount = (uint32_t)(sectInfo.size/elementSize);
        if ( greaterThanAddOrOverflow(sectInfo.reserved1, elementCount, indirectSymbolTableCount) ) {
            diag.error("section %.*s overflows indirect symbol table", (int)sectInfo.sectionName.size(), sectInfo.sectionName.data());
            sectionStop = true;
            return;
        }

        for (uint32_t i=0; (i < elementCount) && !stop; ++i) {
            uint32_t symNum = indirectSymbolTable[sectInfo.reserved1 + i];
            if ( symNum == INDIRECT_SYMBOL_ABS )
                continue;
            if ( symNum == INDIRECT_SYMBOL_LOCAL ) {
                handler(sectInfo.address+i*elementSize, false, 0, "", false, false, false, stop);
                continue;
            }
            if ( symNum > symCount ) {
                diag.error("indirect symbol[%d] = %d which is invalid symbol index", sectInfo.reserved1 + i, symNum);
                sectionStop = true;
                return;
            }
            uint16_t n_desc = is64Bit ? symbols64[symNum].n_desc : symbols32[symNum].n_desc;
            uint8_t  n_type     = is64Bit ? symbols64[symNum].n_type : symbols32[symNum].n_type;
            uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
            uint32_t strOffset = is64Bit ? symbols64[symNum].n_un.n_strx : symbols32[symNum].n_un.n_strx;
            if ( strOffset > poolSize ) {
               diag.error("symbol[%d] string offset out of range", sectInfo.reserved1 + i);
                sectionStop = true;
                return;
            }
            const char* symbolName  = stringPool + strOffset;
            bool        weakImport  = (n_desc & N_WEAK_REF);
            bool        lazy        = (sectionType == S_LAZY_SYMBOL_POINTERS);
#if BUILDING_DYLD && TARGET_OS_OSX && __x86_64__
            if ( lazy && ((n_type & N_PEXT) != 0) ) {
                // don't know why the static linker did not eliminate the internal reference to a private extern definition
                // As this is private extern, we know the symbol lookup will fail.  We also know that this is a lazy-bind, and so
                // there is a corresponding rebase.  The rebase will be run later, and will slide whatever value is in here.
                // So lets change the value in this slot, and let the existing rebase slide it for us
                // Note we only want to change the value in memory once, before rebases are applied.  We don't want to accidentally
                // change it again later.
                if ( supportPrivateExternsWorkaround ) {
                    uintptr_t* ptr = (uintptr_t*)((uint8_t*)(sectInfo.address+i*elementSize) + this->getSlide());
                    uint64_t n_value = is64Bit ? symbols64[symNum].n_value : symbols32[symNum].n_value;
                    *ptr = (uintptr_t)n_value;
                }
                continue;
            }
#endif
            // Handle defined weak def symbols which need to get a special ordinal
            if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) && ((n_desc & N_WEAK_DEF) != 0) )
                libOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
            handler(sectInfo.address+i*elementSize, true, libOrdinal, symbolName, weakImport, lazy, selfModifyingStub, stop);
        }
        sectionStop = stop;
    });
}

int MachOAnalyzer::libOrdinalFromDesc(uint16_t n_desc) const
{
    // -flat_namespace is always flat lookup
    if ( (this->flags & MH_TWOLEVEL) == 0 )
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

bool MachOAnalyzer::validBindInfo(Diagnostics& diag, const char* path) const
{
    forEachBind(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                         bool segIndexSet, bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                         uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                         uint8_t type, const char* symbolName, bool weakImport, bool lazyBind, uint64_t addend, bool& stop) {
        if ( invalidBindState(diag, opcodeName, path, leInfo, segments, segIndexSet, libraryOrdinalSet, dylibCount,
                              libOrdinal, ptrSize, segmentIndex, segmentOffset, type, symbolName) ) {
            stop = true;
        }
    }, ^(const char* symbolName) {
    });
    return diag.noError();
}

bool MachOAnalyzer::invalidBindState(Diagnostics& diag, const char* opcodeName, const char* path, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint32_t ptrSize,
                                    uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type, const char* symbolName) const
{
    if ( !segIndexSet ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path, opcodeName);
        return true;
    }
    if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
        diag.error("in '%s' %s segment index %d too large", path, opcodeName, segmentIndex);
        return true;
    }
    if ( segmentOffset > (segments[segmentIndex].vmsize - ptrSize) ) {
        diag.error("in '%s' %s current segment offset 0x%08llX beyond segment size (0x%08llX)", path, opcodeName, segmentOffset, segments[segmentIndex].vmsize);
        return true;
    }
    if ( symbolName == NULL ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", path, opcodeName);
        return true;
    }
    if ( !libraryOrdinalSet ) {
        diag.error("in '%s' %s missing preceding BIND_OPCODE_SET_DYLIB_ORDINAL", path, opcodeName);
        return true;
    }
    if ( libOrdinal > (int)dylibCount ) {
        diag.error("in '%s' %s has library ordinal too large (%d) max (%d)", path, opcodeName, libOrdinal, dylibCount);
        return true;
    }
    if ( libOrdinal < BIND_SPECIAL_DYLIB_WEAK_LOOKUP ) {
        diag.error("in '%s' %s has unknown library special ordinal (%d)", path, opcodeName, libOrdinal);
        return true;
    }
    switch ( type )  {
        case BIND_TYPE_POINTER:
            if ( !segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s pointer bind is in non-writable segment", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].executable() && enforceFormat(Malformed::executableData) ) {
                diag.error("in '%s' %s pointer bind is in executable segment", path, opcodeName);
                return true;
            }
            break;
        case BIND_TYPE_TEXT_ABSOLUTE32:
        case BIND_TYPE_TEXT_PCREL32: {
            // Text relocations are permitted in x86_64 kexts
            bool forceAllowTextRelocs = false;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
            if ( isKextBundle() && (isArch("x86_64") || isArch("x86_64h")) )
                forceAllowTextRelocs = true;
#endif
            if ( !forceAllowTextRelocs ) {
                diag.error("in '%s' %s text bind is in segment that does not support text relocations", path, opcodeName);
                return true;
            }
            if ( segments[segmentIndex].writable() ) {
                diag.error("in '%s' %s text bind is in writable segment", path, opcodeName);
                return true;
            }
            if ( !segments[segmentIndex].executable() ) {
                diag.error("in '%s' %s pointer bind is in non-executable segment", path, opcodeName);
                return true;
            }
            break;
        }
        default:
            diag.error("in '%s' %s unknown bind type %d", path, opcodeName, type);
            return true;
    }
    return false;
}

void MachOAnalyzer::forEachBind(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, int libOrdinal, uint8_t type, const char* symbolName,
                                                                  bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                                   void (^strongHandler)(const char* symbolName)) const
{
    __block bool     startVmAddrSet = false;
    __block uint64_t startVmAddr    = 0;
    forEachBind(diag, ^(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                        bool segIndexSet, bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                        uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset,
                        uint8_t type, const char* symbolName, bool weakImport, bool lazyBind, uint64_t addend, bool& stop) {
       if ( !startVmAddrSet ) {
            for (int i=0; i <= segmentIndex; ++i) {
                if ( segments[i].segmentName == "__TEXT" ) {
                    startVmAddr = segments[i].vmaddr;
                    startVmAddrSet = true;
                    break;
               }
            }
        }
        uint64_t bindVmOffset  = segments[segmentIndex].vmaddr + segmentOffset;
        uint64_t runtimeOffset = bindVmOffset - startVmAddr;
        handler(runtimeOffset, libOrdinal, type, symbolName, weakImport, lazyBind, addend, stop);
    }, ^(const char* symbolName) {
        strongHandler(symbolName);
    });
}

void MachOAnalyzer::forEachBind(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, int libOrdinal, const char* symbolName,
                                                                  bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                                   void (^strongHandler)(const char* symbolName)) const
{
    forEachBind(diag, ^(uint64_t runtimeOffset, int libOrdinal, uint8_t type, const char* symbolName,
                        bool weakImport, bool lazyBind, uint64_t addend, bool &stop) {
        handler(runtimeOffset, libOrdinal, symbolName, weakImport, lazyBind, addend, stop);
    }, strongHandler);
}

void MachOAnalyzer::forEachBind(Diagnostics& diag,
                                 void (^handler)(const char* opcodeName, const LinkEditInfo& leInfo, const Header::SegmentInfo segments[],
                                                 bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                                 uint32_t ptrSize, uint8_t segmentIndex, uint64_t segmentOffset, uint8_t type,
                                                 const char* symbolName, bool weakImport, bool lazyBind, uint64_t addend, bool& stop),
                                 void (^strongHandler)(const char* symbolName)) const
{
    const uint32_t  ptrSize = this->pointerSize();
    bool            stop    = false;

    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;



    const uint32_t dylibCount = dependentDylibCount();

    if ( leInfo.dyldInfo != nullptr ) {
        // process bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->bind_size;
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
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    stop = true;
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
                    handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                    segmentOffset += ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                    handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                    segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                    handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                    segmentOffset += immediate*ptrSize + ptrSize;
                    break;
                case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                    count = read_uleb128(diag, p, end);
                    skip = read_uleb128(diag, p, end);
                    for (uint32_t i=0; i < count; ++i) {
                        handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                        segmentOffset += skip + ptrSize;
                        if ( stop )
                            break;
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", *p);
            }
        }
        if ( diag.hasError() )
            return;

        // process lazy bind opcodes
        uint32_t lazyDoneCount = 0;
        uint32_t lazyBindCount = 0;
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
            while (  !stop && diag.noError() && (p < end) ) {
                uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
                uint8_t opcode = *p & BIND_OPCODE_MASK;
                ++p;
                switch (opcode) {
                    case BIND_OPCODE_DONE:
                        // this opcode marks the end of each lazy pointer binding
                        ++lazyDoneCount;
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
                        handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, true, addend, stop);
                        segmentOffset += ptrSize;
                        ++lazyBindCount;
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
            if ( lazyDoneCount > lazyBindCount+7 ) {
                // diag.error("lazy bind opcodes missing binds");
            }
        }
        if ( diag.hasError() )
            return;

        // process weak bind info
        if ( leInfo.dyldInfo->weak_bind_size != 0 ) {
            p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->weak_bind_off);
            end             = p + leInfo.dyldInfo->weak_bind_size;
            type            = BIND_TYPE_POINTER;
            segmentOffset   = 0;
            segmentIndex    = 0;
            symbolName      = NULL;
            libraryOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
            segIndexSet     = false;
            libraryOrdinalSet= true;
            addend          = 0;
            weakImport      = false;
            stop            = false;
            while ( !stop && diag.noError() && (p < end) ) {
                uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
                uint8_t opcode = *p & BIND_OPCODE_MASK;
                ++p;
                switch (opcode) {
                    case BIND_OPCODE_DONE:
                        stop = true;
                        break;
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
                    case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                    case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                        diag.error("unexpected dylib ordinal in weak_bind");
                        break;
                    case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                        weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                        symbolName = (char*)p;
                        while (*p != '\0')
                            ++p;
                        ++p;
                        if ( immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION ) {
                            strongHandler(symbolName);
                        }
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
                        handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                        segmentOffset += ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                        handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                        segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                        handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                        segmentOffset += immediate*ptrSize + ptrSize;
                        break;
                    case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                        count = read_uleb128(diag, p, end);
                        skip = read_uleb128(diag, p, end);
                        for (uint32_t i=0; i < count; ++i) {
                            handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                                    ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, stop);
                            segmentOffset += skip + ptrSize;
                            if ( stop )
                                break;
                        }
                        break;
                    default:
                        diag.error("bad bind opcode 0x%02X", *p);
                }
            }
        }
    }
    else if ( leInfo.chainedFixups != nullptr ) {
        // binary uses chained fixups, so do nothing
    }
#if SUPPORT_CLASSIC_RELOCS
    else if ( leInfo.dynSymTab != nullptr ) {
        // old binary, process external relocations
        const uint64_t                  relocsStartAddress = externalRelocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
        const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->extreloff);
        const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nextrel];
        bool                            is64Bit     = is64() ;
        const uint8_t                   relocSize   = (is64Bit ? 3 : 2);
        const void*                     symbolTable = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
        const struct nlist_64*          symbols64   = (nlist_64*)symbolTable;
        const struct nlist*             symbols32   = (struct nlist*)symbolTable;
        const char*                     stringPool  = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
        uint32_t                        symCount    = leInfo.symTab->nsyms;
        uint32_t                        poolSize    = leInfo.symTab->strsize;
        for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
            bool isBranch = false;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
            if ( isKextBundle() ) {
                // kext's may have other kinds of relocations, eg, branch relocs.  Skip them
                if ( isArch("x86_64") || isArch("x86_64h") ) {
                    if ( reloc->r_type == X86_64_RELOC_BRANCH ) {
                        if ( reloc->r_length != 2 ) {
                            diag.error("external relocation has wrong r_length");
                            break;
                        }
                        if ( reloc->r_pcrel != true ) {
                            diag.error("external relocation should be pcrel");
                            break;
                        }
                        isBranch = true;
                    }
                }
            }
#endif

            if ( !isBranch ) {
                if ( reloc->r_length != relocSize ) {
                    diag.error("external relocation has wrong r_length");
                    break;
                }
                if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA == ARM64_RELOC_UNSIGNED
                    diag.error("external relocation has wrong r_type");
                    break;
                }
            }
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(relocsStartAddress+reloc->r_address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                uint32_t symbolIndex = reloc->r_symbolnum;
                if ( symbolIndex > symCount ) {
                    diag.error("external relocation has out of range r_symbolnum");
                    break;
                }
                else {
                    uint32_t strOffset  = is64Bit ? symbols64[symbolIndex].n_un.n_strx : symbols32[symbolIndex].n_un.n_strx;
                    uint16_t n_desc     = is64Bit ? symbols64[symbolIndex].n_desc : symbols32[symbolIndex].n_desc;
                    uint8_t  n_type     = is64Bit ? symbols64[symbolIndex].n_type : symbols32[symbolIndex].n_type;
                    uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
                    if ( strOffset >= poolSize ) {
                        diag.error("external relocation has r_symbolnum=%d which has out of range n_strx", symbolIndex);
                        break;
                    }
                    else {
                        const char*     symbolName = stringPool + strOffset;
                        bool            weakImport = (n_desc & N_WEAK_REF);
                        const uint8_t*  content    = (uint8_t*)this + segmentsInfo[segIndex].vmaddr - leInfo.layout.textUnslidVMAddr + segOffset;
                        uint64_t        addend     = (reloc->r_length == 3) ? *((uint64_t*)content) : *((uint32_t*)content);
                        // Handle defined weak def symbols which need to get a special ordinal
                        if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) && ((n_desc & N_WEAK_DEF) != 0) )
                            libOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
                        uint8_t type = isBranch ? BIND_TYPE_TEXT_PCREL32 : BIND_TYPE_POINTER;
                        handler("external relocation", leInfo, segmentsInfo, true, true, dylibCount, libOrdinal,
                                ptrSize, segIndex, segOffset, type, symbolName, weakImport, false, addend, stop);
                    }
                }
            }
            else {
                diag.error("local relocation has out of range r_address");
                break;
            }
        }
        // then process indirect symbols
        forEachIndirectPointer(diag, false,
                               ^(uint64_t address, bool bind, int bindLibOrdinal,
                                 const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
            if ( !bind )
               return;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                handler("indirect symbol", leInfo, segmentsInfo, true, true, dylibCount, bindLibOrdinal,
                         ptrSize, segIndex, segOffset, BIND_TYPE_POINTER, bindSymbolName, bindWeakImport, bindLazy, 0, indStop);
            }
            else {
                diag.error("indirect symbol has out of range address");
                indStop = true;
            }
        });
    }
#endif // SUPPORT_CLASSIC_RELOCS
}

bool MachOAnalyzer::validChainedFixupsInfo(Diagnostics& diag, const char* path) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return false;

    // validate dyld_chained_fixups_header
    const dyld_chained_fixups_header* chainsHeader = (dyld_chained_fixups_header*)getLinkEditContent(leInfo.layout, leInfo.chainedFixups->dataoff);
    if ( chainsHeader->fixups_version != 0 ) {
        diag.error("chained fixups, unknown header version");
        return false;
    }
    if ( chainsHeader->starts_offset >= leInfo.chainedFixups->datasize )  {
        diag.error("chained fixups, starts_offset exceeds LC_DYLD_CHAINED_FIXUPS size");
        return false;
    }
    if ( chainsHeader->imports_offset > leInfo.chainedFixups->datasize )  {
        diag.error("chained fixups, imports_offset exceeds LC_DYLD_CHAINED_FIXUPS size");
        return false;
    }
   
    uint32_t formatEntrySize;
    switch ( chainsHeader->imports_format ) {
        case DYLD_CHAINED_IMPORT:
            formatEntrySize = sizeof(dyld_chained_import);
            break;
        case DYLD_CHAINED_IMPORT_ADDEND:
            formatEntrySize = sizeof(dyld_chained_import_addend);
            break;
        case DYLD_CHAINED_IMPORT_ADDEND64:
            formatEntrySize = sizeof(dyld_chained_import_addend64);
            break;
        default:
            diag.error("chained fixups, unknown imports_format");
            return false;
    }
    if ( greaterThanAddOrOverflow(chainsHeader->imports_offset, (formatEntrySize * chainsHeader->imports_count), chainsHeader->symbols_offset) ) {
         diag.error("chained fixups, imports array overlaps symbols");
         return false;
    }
    if ( chainsHeader->symbols_format != 0 )  {
         diag.error("chained fixups, symbols_format unknown");
         return false;
    }

    // validate dyld_chained_starts_in_image
    const dyld_chained_starts_in_image* startsInfo = (dyld_chained_starts_in_image*)((uint8_t*)chainsHeader + chainsHeader->starts_offset);
    if ( startsInfo->seg_count != leInfo.layout.linkeditSegIndex+1 ) {
        // We can have fewer segments than the count, so long as those we are missing have no relocs
        // This can happen because __CTF is inserted by ctf_insert after linking, and between __DATA and __LINKEDIT, but has no relocs
        // ctf_insert updates the load commands to put __CTF between __DATA and __LINKEDIT, but doesn't update the chained fixups data structures
        if ( startsInfo->seg_count > (leInfo.layout.linkeditSegIndex + 1) ) {
            diag.error("chained fixups, seg_count exceeds number of segments");
            return false;
        }

        // We can have fewer segments than the count, so long as those we are missing have no relocs
        uint32_t numNoRelocSegments = 0;
        uint32_t numExtraSegments = (leInfo.layout.lastSegIndex + 1) - startsInfo->seg_count;
        for (unsigned i = 0; i != numExtraSegments; ++i) {
            // Check each extra segment before linkedit
            const Header::SegmentInfo& segInfo = segmentsInfo[leInfo.layout.linkeditSegIndex - (i + 1)];
            if ( segInfo.vmsize == 0 )
                ++numNoRelocSegments;
        }

        if ( numNoRelocSegments != numExtraSegments ) {
            diag.error("chained fixups, seg_count does not match number of segments");
            return false;
        }
    }
    const uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
    uint32_t maxValidPointerSeen = 0;
    uint16_t pointer_format_for_all = 0;
    bool pointer_format_found = false;
    const uint8_t* endOfStarts = (uint8_t*)chainsHeader + chainsHeader->imports_offset;
    for (uint32_t i=0; i < startsInfo->seg_count; ++i) {
        uint32_t segInfoOffset = startsInfo->seg_info_offset[i];
        // 0 offset means this segment has no fixups
        if ( segInfoOffset == 0 )
            continue;
        const dyld_chained_starts_in_segment* segInfo = (dyld_chained_starts_in_segment*)((uint8_t*)startsInfo + segInfoOffset);
        if ( segInfo->size > (endOfStarts - (uint8_t*)segInfo) ) {
             diag.error("chained fixups, dyld_chained_starts_in_segment for segment #%d overruns imports table", i);
             return false;
        }

        // validate dyld_chained_starts_in_segment
        if ( (segInfo->page_size != 0x1000) && (segInfo->page_size != 0x4000) ) {
            diag.error("chained fixups, page_size not 4KB or 16KB in segment #%d", i);
            return false;
        }
        if ( segInfo->pointer_format > 13 ) {
            diag.error("chained fixups, unknown pointer_format in segment #%d", i);
            return false;
        }
        if ( !pointer_format_found ) {
            pointer_format_for_all = segInfo->pointer_format;
            pointer_format_found = true;
        }
        if ( segInfo->pointer_format != pointer_format_for_all) {
            diag.error("chained fixups, pointer_format not same for all segments %d and %d", segInfo->pointer_format, pointer_format_for_all);
            return false;
        }
        if ( segInfo->segment_offset != (segmentsInfo[i].vmaddr - baseAddress) ) {
            diag.error("chained fixups, segment_offset does not match vmaddr from LC_SEGMENT in segment #%d", i);
            return false;
        }
        if ( segInfo->max_valid_pointer != 0 ) {
            if ( maxValidPointerSeen == 0 ) {
                // record max_valid_pointer values seen
                maxValidPointerSeen = segInfo->max_valid_pointer;
            }
            else if ( maxValidPointerSeen != segInfo->max_valid_pointer ) {
                diag.error("chained fixups, different max_valid_pointer values seen in different segments");
                return false;
            }
        }
        // validate starts table in segment
        if ( offsetof(dyld_chained_starts_in_segment, page_start[segInfo->page_count]) > segInfo->size ) {
            diag.error("chained fixups, page_start array overflows size");
            return false;
        }
        uint32_t maxOverflowIndex = (uint32_t)(segInfo->size - offsetof(dyld_chained_starts_in_segment, page_start[0]))/sizeof(uint16_t);
        for (int pageIndex=0; pageIndex < segInfo->page_count; ++pageIndex) {
            uint16_t offsetInPage = segInfo->page_start[pageIndex];
            if ( offsetInPage == DYLD_CHAINED_PTR_START_NONE )
                continue;
            if ( (offsetInPage & DYLD_CHAINED_PTR_START_MULTI) == 0 ) {
                // this is the offset into the page where the first fixup is
                if ( offsetInPage > segInfo->page_size ) {
                    diag.error("chained fixups, in segment #%d page_start[%d]=0x%04X exceeds page size", i, pageIndex, offsetInPage);
                }
            }
            else {
                // this is actually an index into chain_starts[]
                uint32_t overflowIndex = offsetInPage & ~DYLD_CHAINED_PTR_START_MULTI;
                // now verify all starts are within the page and in ascending order
                uint16_t lastOffsetInPage = 0;
                do {
                    if ( overflowIndex > maxOverflowIndex )  {
                        diag.error("chain overflow index out of range %d (max=%d) in segment %.*s", overflowIndex, maxOverflowIndex,
                                   (int)segmentName(i).size(), segmentName(i).data());
                        return false;
                    }
                    offsetInPage = (segInfo->page_start[overflowIndex] & ~DYLD_CHAINED_PTR_START_LAST);
                    if ( offsetInPage > segInfo->page_size ) {
                        diag.error("chained fixups, in segment #%d overflow page_start[%d]=0x%04X exceeds page size", i, overflowIndex, offsetInPage);
                        return false;
                    }
                    if ( (offsetInPage <= lastOffsetInPage) && (lastOffsetInPage != 0) )  {
                        diag.error("chained fixups, in segment #%d overflow page_start[%d]=0x%04X is before previous at 0x%04X\n", i, overflowIndex, offsetInPage, lastOffsetInPage);
                        return false;
                    }
                    lastOffsetInPage = offsetInPage;
                    ++overflowIndex;
                } while ( (segInfo->page_start[overflowIndex] & DYLD_CHAINED_PTR_START_LAST) == 0 );
           }
        }

    }
    // validate import table size can fit
    if ( chainsHeader->imports_count != 0 ) {
        uint32_t maxBindOrdinal = 0;
        switch (pointer_format_for_all) {
            case DYLD_CHAINED_PTR_32:
                maxBindOrdinal = 0x0FFFFF; // 20-bits
                break;
            case DYLD_CHAINED_PTR_ARM64E:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND:
            case DYLD_CHAINED_PTR_ARM64E_OFFSET:
                maxBindOrdinal = 0x00FFFF; // 16-bits
                break;
            case DYLD_CHAINED_PTR_64:
            case DYLD_CHAINED_PTR_64_OFFSET:
            case DYLD_CHAINED_PTR_ARM64E_USERLAND24:
                maxBindOrdinal = 0xFFFFFF; // 24 bits
                break;
        }
        if ( chainsHeader->imports_count >= maxBindOrdinal )  {
            diag.error("chained fixups, imports_count (%d) exceeds max of %d", chainsHeader->imports_count, maxBindOrdinal);
            return false;
        }
    }

    // validate max_valid_pointer is larger than last segment
    if ( (maxValidPointerSeen != 0) && !inDyldCache() ) {
        uint64_t lastSegmentLastVMAddr = segmentsInfo[leInfo.layout.linkeditSegIndex-1].vmaddr + segmentsInfo[leInfo.layout.linkeditSegIndex-1].vmsize;
        if ( maxValidPointerSeen < lastSegmentLastVMAddr ) {
            diag.error("chained fixups, max_valid_pointer too small for image");
            return false;
        }
    }

    return diag.noError();
}

bool MachOAnalyzer::validChainedFixupsInfoOldArm64e(Diagnostics& diag, const char* path) const
{
    __block uint32_t maxTargetCount = 0;
    __block uint32_t currentTargetCount = 0;
    parseOrgArm64eChainedFixups(diag,
        ^(uint32_t totalTargets, bool& stop) {
            maxTargetCount = totalTargets;
        },
        ^(const LinkEditInfo& leInfo, const Header::SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop) {
           if ( symbolName == NULL ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM", path);
            }
            else if ( !libraryOrdinalSet ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_DYLIB_ORDINAL", path);
            }
            else if ( libOrdinal > (int)dylibCount ) {
                diag.error("in '%s' has library ordinal too large (%d) max (%d)", path, libOrdinal, dylibCount);
            }
            else if ( libOrdinal < BIND_SPECIAL_DYLIB_WEAK_LOOKUP ) {
                diag.error("in '%s' has unknown library special ordinal (%d)", path, libOrdinal);
            }
            else if ( type != BIND_TYPE_POINTER ) {
                diag.error("in '%s' unknown bind type %d", path, type);
            }
            else if ( currentTargetCount > maxTargetCount ) {
                diag.error("in '%s' chained target counts exceeds BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB", path);
            }
            ++currentTargetCount;
            if ( diag.hasError() )
                stop = true;
        },
        ^(const LinkEditInfo& leInfo, const Header::SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop) {
           if ( !segIndexSet ) {
                diag.error("in '%s' missing BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB", path);
            }
            else if ( segmentIndex >= leInfo.layout.linkeditSegIndex )  {
                diag.error("in '%s' segment index %d too large", path, segmentIndex);
            }
            else if ( segmentOffset > (segments[segmentIndex].vmsize-8) ) {
                diag.error("in '%s' current segment offset 0x%08llX beyond segment size (0x%08llX)", path, segmentOffset, segments[segmentIndex].vmsize);
            }
            else if ( !segments[segmentIndex].writable() ) {
                diag.error("in '%s' pointer bind is in non-writable segment", path);
            }
            else if ( segments[segmentIndex].executable() ) {
                diag.error("in '%s' pointer bind is in executable segment", path);
            }
            if ( diag.hasError() )
                stop = true;
        }
    );

    return diag.noError();
}



void MachOAnalyzer::parseOrgArm64eChainedFixups(Diagnostics& diag, void (^targetCount)(uint32_t totalTargets, bool& stop),
                                                                   void (^addTarget)(const LinkEditInfo& leInfo, const Header::SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal, uint8_t type, const char* symbolName, uint64_t addend, bool weakImport, bool& stop),
                                                                   void (^addChainStart)(const LinkEditInfo& leInfo, const Header::SegmentInfo segments[], uint8_t segmentIndex, bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop)) const
{
    bool            stop    = false;

    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    const uint32_t dylibCount = dependentDylibCount();

    if ( leInfo.dyldInfo != nullptr ) {
        // process bind opcodes
        const uint8_t*  p    = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
        const uint8_t*  end  = p + leInfo.dyldInfo->bind_size;
        uint8_t         type = 0;
        uint64_t        segmentOffset = 0;
        uint8_t         segmentIndex = 0;
        const char*     symbolName = NULL;
        int             libraryOrdinal = 0;
        bool            segIndexSet = false;
        bool            libraryOrdinalSet = false;
        uint64_t        targetTableCount;
        uint64_t        addend = 0;
        bool            weakImport = false;
        while ( !stop && diag.noError() && (p < end) ) {
            uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
            uint8_t opcode = *p & BIND_OPCODE_MASK;
            ++p;
            switch (opcode) {
                case BIND_OPCODE_DONE:
                    stop = true;
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
                case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                    segmentIndex = immediate;
                    segmentOffset = read_uleb128(diag, p, end);
                    segIndexSet = true;
                    break;
                case BIND_OPCODE_SET_ADDEND_SLEB:
                    addend = read_sleb128(diag, p, end);
                    break;
                case BIND_OPCODE_DO_BIND:
                    if ( addTarget )
                        addTarget(leInfo, segmentsInfo, libraryOrdinalSet, dylibCount, libraryOrdinal, type, symbolName, addend, weakImport, stop);
                    break;
                case BIND_OPCODE_THREADED:
                    switch (immediate) {
                        case BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB:
                            targetTableCount = read_uleb128(diag, p, end);
                            if ( targetTableCount > 65535 ) {
                                diag.error("BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB size too large");
                                stop = true;
                            }
                            else {
                                if ( targetCount )
                                    targetCount((uint32_t)targetTableCount, stop);
                            }
                            break;
                        case BIND_SUBOPCODE_THREADED_APPLY:
                            if ( addChainStart )
                                addChainStart(leInfo, segmentsInfo, segmentIndex, segIndexSet, segmentOffset, DYLD_CHAINED_PTR_ARM64E, stop);
                            break;
                        default:
                            diag.error("bad BIND_OPCODE_THREADED sub-opcode 0x%02X", immediate);
                    }
                    break;
                default:
                    diag.error("bad bind opcode 0x%02X", immediate);
            }
        }
        if ( diag.hasError() )
            return;
    }
}

void MachOAnalyzer::forEachChainedFixupTarget(Diagnostics& diag, void (^callback)(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    if ( leInfo.dyldInfo != nullptr ) {
        parseOrgArm64eChainedFixups(diag, nullptr, ^(const LinkEditInfo& leInfo2, const Header::SegmentInfo segments[], bool libraryOrdinalSet, uint32_t dylibCount,
                                                    int libOrdinal, uint8_t type, const char* symbolName, uint64_t fixAddend, bool weakImport, bool& stopChain) {
            callback(libOrdinal, symbolName, fixAddend, weakImport, stopChain);
        }, nullptr);
    }
    else if ( leInfo.chainedFixups != nullptr ) {
        const dyld_chained_fixups_header*  header = (dyld_chained_fixups_header*)getLinkEditContent(leInfo.layout, leInfo.chainedFixups->dataoff);
        MachOFile::forEachChainedFixupTarget(diag, header, leInfo.chainedFixups, callback);
    }
}


// Convert from a (possibly) live pointer to a vmAddr
uint64_t MachOAnalyzer::VMAddrConverter::convertToVMAddr(uint64_t value, const Array<uint64_t>& bindTargets) const {
    if ( contentRebased ) {
        if ( value == 0 )
            return 0;
        // The value may have been signed.  Strip the signature if that is the case
#if __has_feature(ptrauth_calls)
        value = (uint64_t)__builtin_ptrauth_strip((void*)value, ptrauth_key_asia);
#endif
        value -= slide;
        return value;
    }
    if ( chainedPointerFormat != 0 ) {
        // We try to only use the VMAddrConverter on locations which are pointers, but we don't know
        // for sure if the location contains a rebase or not.  Eg, it can be called on a NULL Protocol ISA field.
        // If we see a 0, then its extremely likely that this is not a rebase, as we only use VMAddrConverter
        // for initializers, terminators, and objc.  None of those have any reason to point to offset 0 in the binary,
        // ie, no reason to point to the mach_header
        if ( value == 0 )
            return 0;
        auto* chainedValue = (MachOAnalyzer::ChainedFixupPointerOnDisk*)&value;
        uint64_t targetRuntimeOffset;
        if ( chainedValue->isRebase(chainedPointerFormat, preferredLoadAddress, targetRuntimeOffset) ) {
            value = preferredLoadAddress + targetRuntimeOffset;
        }

#if !BUILDING_DYLD
        // Patchable objc classes use binds to self.  Support them in offline tools
        uint32_t    bindOrdinal = 0;
        int64_t     addend      = 0;
        if ( !bindTargets.empty() && chainedValue->isBind(chainedPointerFormat, bindOrdinal, addend))
            value = bindTargets[bindOrdinal] + addend;
#endif
        return value;
    }

#if !(BUILDING_LIBDYLD || BUILDING_DYLD)
    typedef MachOAnalyzer::VMAddrConverter VMAddrConverter;
    if ( sharedCacheChainedPointerFormat != VMAddrConverter::SharedCacheFormat::none ) {
        switch ( sharedCacheChainedPointerFormat ) {
            case VMAddrConverter::SharedCacheFormat::none:
                assert(false);
            case VMAddrConverter::SharedCacheFormat::v1: {
                // Nothing to do here.  We don't have chained fixup bits to remove, or a value_add to apply
                break;
            }
            case VMAddrConverter::SharedCacheFormat::v2_x86_64_tbi: {
                const uint64_t   deltaMask    = 0x00FFFF0000000000;
                const uint64_t   valueMask    = ~deltaMask;
                const uint64_t   valueAdd     = preferredLoadAddress;
                value = (value & valueMask);
                if ( value != 0 ) {
                    value += valueAdd;
                }
                break;
            }
            case VMAddrConverter::SharedCacheFormat::v3: {
                // Just use the chained pointer format for arm64e
                auto* chainedValue = (MachOAnalyzer::ChainedFixupPointerOnDisk*)&value;
                uint64_t targetRuntimeOffset;
                if ( chainedValue->isRebase(DYLD_CHAINED_PTR_ARM64E, preferredLoadAddress,
                                            targetRuntimeOffset) ) {
                    value = preferredLoadAddress + targetRuntimeOffset;
                }
                break;
            }
            case VMAddrConverter::SharedCacheFormat::v4: {
                const uint64_t   deltaMask    = 0x00000000C0000000;
                const uint64_t   valueMask    = ~deltaMask;
                const uint64_t   valueAdd     = preferredLoadAddress;
                value = (value & valueMask);
                if ( value != 0 ) {
                    value += valueAdd;
                }
                break;
            }
            case VMAddrConverter::SharedCacheFormat::v5: {
                // Just use the chained pointer format for arm64e
                if ( value == 0 )
                    return 0;
                auto* chainedValue = (MachOAnalyzer::ChainedFixupPointerOnDisk*)&value;
                uint64_t targetRuntimeOffset;
                if ( chainedValue->isRebase(DYLD_CHAINED_PTR_ARM64E_SHARED_CACHE, preferredLoadAddress,
                                            targetRuntimeOffset) ) {
                    value = preferredLoadAddress + targetRuntimeOffset;
                }
                break;
            }
        }
        return value;
    }
#endif

    return value;
}

uint64_t MachOAnalyzer::VMAddrConverter::convertToVMAddr(uint64_t v) const
{
    return this->convertToVMAddr(v, {});
}

MachOAnalyzer::VMAddrConverter MachOAnalyzer::makeVMAddrConverter(bool contentRebased) const {
    MachOAnalyzer::VMAddrConverter vmAddrConverter;
    vmAddrConverter.preferredLoadAddress   = ((const Header*)this)->preferredLoadAddress();
    vmAddrConverter.slide                  = getSlide();
    vmAddrConverter.chainedPointerFormat   = hasChainedFixups() ? chainedPointerFormat() : 0;
    vmAddrConverter.contentRebased         = contentRebased;
    return vmAddrConverter;
}


struct VIS_HIDDEN SegmentRanges
{
    struct SegmentRange {
        uint64_t vmAddrStart;
        uint64_t vmAddrEnd;
        uint32_t fileSize;
    };

    bool contains(uint64_t vmAddr) const {
        for (const SegmentRange& range : segments) {
            if ( (range.vmAddrStart <= vmAddr) && (vmAddr < range.vmAddrEnd) )
                return true;
        }
        return false;
    }

private:
    SegmentRange localAlloc[1];

public:
    dyld3::OverflowSafeArray<SegmentRange> segments { localAlloc, sizeof(localAlloc) / sizeof(localAlloc[0]) };
};

void MachOAnalyzer::forEachInitializer(Diagnostics& diag, const VMAddrConverter& vmAddrConverter, void (^callback)(uint32_t offset), const void* dyldCache) const
{
    __block SegmentRanges executableSegments;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( (info.initProt & VM_PROT_EXECUTE) != 0 ) {
            executableSegments.segments.push_back({ info.vmaddr, info.vmaddr + info.vmsize, (uint32_t)info.fileSize });
        }
    });

    if (executableSegments.segments.empty()) {
        diag.error("no exeutable segments");
        return;
    }

    uint64_t loadAddress = ((const Header*)this)->preferredLoadAddress();
    intptr_t slide = getSlide();

    // if dylib linked with -init linker option, that initializer is first
    forEachLoadCommand(diag, ^(const load_command* cmd, bool& stop) {
        if ( cmd->cmd == LC_ROUTINES ) {
            const routines_command* routines = (routines_command*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( executableSegments.contains(dashInit) )
                callback((uint32_t)(dashInit - loadAddress));
            else
                diag.error("-init does not point within __TEXT segment");
        }
        else if ( cmd->cmd == LC_ROUTINES_64 ) {
            const routines_command_64* routines = (routines_command_64*)cmd;
            uint64_t dashInit = routines->init_address;
            if ( executableSegments.contains(dashInit) )
                callback((uint32_t)(dashInit - loadAddress));
            else
                diag.error("-init does not point within __TEXT segment");
        }
    });

    // next any function pointers in mod-init section
    const unsigned ptrSize          = pointerSize();
    forEachInitializerPointerSection(diag, ^(uint32_t sectionOffset, uint32_t sectionSize, bool& stop) {
        const uint8_t* content = (uint8_t*)this + sectionOffset;
        if ( ptrSize == 8 ) {
            const uint64_t* initsStart = (uint64_t*)content;
            const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)content + sectionSize);
            for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                uint64_t anInit = vmAddrConverter.convertToVMAddr(*p);
                if ( !executableSegments.contains(anInit) ) {
                     diag.error("initializer 0x%0llX does not point within executable segment", anInit);
                     stop = true;
                     break;
                }
                callback((uint32_t)(anInit - loadAddress));
            }
        }
        else {
            const uint32_t* initsStart = (uint32_t*)content;
            const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + sectionSize);
            for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                uint32_t anInit = (uint32_t)vmAddrConverter.convertToVMAddr(*p);
                if ( !executableSegments.contains(anInit) ) {
                     diag.error("initializer 0x%0X does not point within executable segment", anInit);
                     stop = true;
                     break;
                }
                callback(anInit - (uint32_t)loadAddress);
            }
        }
    });

    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.flags & SECTION_TYPE) != S_INIT_FUNC_OFFSETS )
            return;
        const uint8_t* content = (uint8_t*)(info.address + slide);
        if ( info.segInitProt & VM_PROT_WRITE ) {
            diag.error("initializer offsets section %.*s/%.*s must be in read-only segment",
                       (int)info.segmentName.size(), info.segmentName.data(),
                       (int)info.sectionName.size(), info.sectionName.data());
            stop = true;
            return;
        }
        if ( (info.size % 4) != 0 ) {
            diag.error("initializer offsets section %.*s/%.*s has bad size",
                       (int)info.segmentName.size(), info.segmentName.data(),
                       (int)info.sectionName.size(), info.sectionName.data());
            stop = true;
            return;
        }
        if ( (info.address % 4) != 0 ) {
            diag.error("initializer offsets section %.*s/%.*s is not 4-byte aligned",
                       (int)info.segmentName.size(), info.segmentName.data(),
                       (int)info.sectionName.size(), info.sectionName.data());
            stop = true;
            return;
        }
        const uint32_t* initsStart = (uint32_t*)content;
        const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + info.size);
        for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
            uint32_t anInitOffset = *p;
            if ( !executableSegments.contains(loadAddress + anInitOffset) ) {
                 diag.error("initializer 0x%08X is not an offset to an executable segment", anInitOffset);
                 stop = true;
                 break;
            }
            callback(anInitOffset);
        }
    });
}

bool MachOAnalyzer::hasTerminators(Diagnostics& diag, const VMAddrConverter& vmAddrConverter) const
{
    __block bool result = false;
    forEachTerminator(diag, vmAddrConverter, ^(uint32_t offset) {
        result = true;
    });
    return result;
}

void MachOAnalyzer::forEachTerminator(Diagnostics& diag, const VMAddrConverter& vmAddrConverter, void (^callback)(uint32_t offset)) const
{
    __block SegmentRanges executableSegments;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( (info.initProt & VM_PROT_EXECUTE) != 0 ) {
            executableSegments.segments.push_back({ info.vmaddr, info.vmaddr + info.vmsize, (uint32_t)info.fileSize });
        }
    });

    if (executableSegments.segments.empty()) {
        diag.error("no exeutable segments");
        return;
    }

    uint64_t loadAddress = ((const Header*)this)->preferredLoadAddress();
    intptr_t slide = getSlide();

    // next any function pointers in mod-term section
    const unsigned ptrSize          = pointerSize();
    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.flags & SECTION_TYPE) == S_MOD_TERM_FUNC_POINTERS ) {
            const uint8_t* content;
            content = (uint8_t*)(info.address + slide);
            if ( (info.size % ptrSize) != 0 ) {
                diag.error("terminator section %.*s/%.*s has bad size",
                           (int)info.segmentName.size(), info.segmentName.data(),
                           (int)info.sectionName.size(), info.sectionName.data());
                stop = true;
                return;
            }
            if ( ((long)content % ptrSize) != 0 ) {
                diag.error("terminator section %.*s/%.*s is not pointer aligned",
                           (int)info.segmentName.size(), info.segmentName.data(),
                           (int)info.sectionName.size(), info.sectionName.data());
                stop = true;
                return;
            }
            if ( ptrSize == 8 ) {
                const uint64_t* initsStart = (uint64_t*)content;
                const uint64_t* initsEnd   = (uint64_t*)((uint8_t*)content + info.size);
                for (const uint64_t* p=initsStart; p < initsEnd; ++p) {
                    uint64_t rawContent = *p;
    #if __has_feature(ptrauth_calls)
                    rawContent = (uint64_t)__builtin_ptrauth_strip((void*)rawContent, ptrauth_key_asia);
    #endif
                    uint64_t anInit = vmAddrConverter.convertToVMAddr(rawContent);
                    if ( !executableSegments.contains(anInit) ) {
                         diag.error("terminator 0x%0llX does not point within executable segment", anInit);
                         stop = true;
                         break;
                    }
                    callback((uint32_t)(anInit - loadAddress));
                }
            }
            else {
                const uint32_t* initsStart = (uint32_t*)content;
                const uint32_t* initsEnd   = (uint32_t*)((uint8_t*)content + info.size);
                for (const uint32_t* p=initsStart; p < initsEnd; ++p) {
                    uint32_t anInit = (uint32_t)vmAddrConverter.convertToVMAddr(*p);
                    if ( !executableSegments.contains(anInit) ) {
                         diag.error("terminator 0x%0X does not point within executable segment", anInit);
                         stop = true;
                         break;
                    }
                    callback(anInit - (uint32_t)loadAddress);
                }
            }
        }
    });
}

bool MachOAnalyzer::hasSwiftOrObjC(bool* hasSwift) const
{
    struct objc_image_info {
        int32_t version;
        uint32_t flags;
    };

    if ( hasSwift != nullptr )
        *hasSwift = false;

    uintptr_t slide = getSlide();
    __block bool result = false;
    forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( (sectInfo.sectionName == "__objc_imageinfo") && sectInfo.segmentName.starts_with("__DATA") ) {
            if ( hasSwift != nullptr ) {
                objc_image_info* info =  (objc_image_info*)((uintptr_t)sectInfo.address + slide);
                uint32_t swiftVersion = ((info->flags >> 8) & 0xFF);
                if ( swiftVersion )
                     *hasSwift = true;
            }

            result = true;
            stop = true;
        }
        if ( (this->cputype == CPU_TYPE_I386) && (sectInfo.sectionName == "__image_info") && (sectInfo.sectionName == "__OBJC") ) {
            result = true;
            stop = true;
        }
    });
    return result;
}

bool MachOAnalyzer::hasSwift() const {
    bool hasSwift = false;
    this->hasSwiftOrObjC(&hasSwift);

    return hasSwift;
}

bool MachOAnalyzer::usesObjCGarbageCollection() const
{
    __block bool result = false;
    forEachSection(^(const Header::SectionInfo& info, bool& stop) {
        if ( (info.sectionName == "__objc_imageinfo") && info.segmentName.starts_with("__DATA") ) {
            const uint64_t  slide = (uint64_t)this - ((const Header*)this)->preferredLoadAddress();
            const uint32_t* flags = (uint32_t*)(info.address + slide);
            if ( flags[1] & 4 )
                result = true;
            stop = true;
        }
     });
    return result;
}

const void* MachOAnalyzer::getRebaseOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->rebase_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
}

const void* MachOAnalyzer::getBindOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->bind_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
}

const void* MachOAnalyzer::getLazyBindOpcodes(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.dyldInfo == nullptr) )
        return nullptr;

    size = leInfo.dyldInfo->lazy_bind_size;
    return getLinkEditContent(leInfo.layout, leInfo.dyldInfo->lazy_bind_off);
}

const void* MachOAnalyzer::getSplitSeg(uint32_t& size) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.splitSegInfo == nullptr) )
        return nullptr;

    size = leInfo.splitSegInfo->datasize;
    return getLinkEditContent(leInfo.layout, leInfo.splitSegInfo->dataoff);
}

bool MachOAnalyzer::hasSplitSeg() const {
    uint32_t splitSegSize = 0;
    const void* splitSegStart = getSplitSeg(splitSegSize);
    return splitSegStart != nullptr;
}

bool MachOAnalyzer::isSplitSegV1() const {
    uint32_t splitSegSize = 0;
    const void* splitSegStart = getSplitSeg(splitSegSize);
    if (!splitSegStart)
        return false;

    return (*(const uint8_t*)splitSegStart) != DYLD_CACHE_ADJ_V2_FORMAT;
}

bool MachOAnalyzer::isSplitSegV2() const {
    uint32_t splitSegSize = 0;
    const void* splitSegStart = getSplitSeg(splitSegSize);
    if (!splitSegStart)
        return false;

    return (*(const uint8_t*)splitSegStart) == DYLD_CACHE_ADJ_V2_FORMAT;
}


uint64_t MachOAnalyzer::segAndOffsetToRuntimeOffset(uint8_t targetSegIndex, uint64_t targetSegOffset) const
{
    __block uint64_t textVmAddr = 0;
    __block uint64_t result     = 0;
    ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        if ( info.segmentName == "__TEXT" )
            textVmAddr = info.vmaddr;
        if ( info.segmentIndex == targetSegIndex ) {
            result = (info.vmaddr - textVmAddr) + targetSegOffset;
        }
    });
    return result;
}

bool MachOAnalyzer::hasLazyPointers(uint32_t& runtimeOffset, uint32_t& size) const
{
    size = 0;
    forEachSection(^(const Header::SectionInfo& info, bool &stop) {
        if ( (info.flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS ) {
            runtimeOffset = (uint32_t)(info.address - ((const Header*)this)->preferredLoadAddress());
            size          = (uint32_t)info.size;
            stop = true;
        }
    });
    return (size != 0);
}

#if !TARGET_OS_EXCLAVEKIT
void MachOAnalyzer::forEachCDHash(void (^handler)(const uint8_t cdHash[20])) const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return;

    forEachCDHashOfCodeSignature(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff),
                                 leInfo.codeSig->datasize,
                                 handler);
}

bool MachOAnalyzer::usesLibraryValidation() const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.codeSig == nullptr) )
        return false;

    // check for CS_REQUIRE_LV in CS_CodeDirectory.flags
    __block bool requiresLV = false;
    forEachCodeDirectoryBlob(getLinkEditContent(leInfo.layout, leInfo.codeSig->dataoff),
                             leInfo.codeSig->datasize,
                             ^(const void *cdBuffer) {
         const CS_CodeDirectory* cd = (const CS_CodeDirectory*)cdBuffer;
         requiresLV |= (htonl(cd->flags) & CS_REQUIRE_LV);
    });

    return requiresLV;
}
#endif // !TARGET_OS_EXCLAVEKIT

bool MachOAnalyzer::hasUnalignedPointerFixups() const
{
    // only look at 64-bit architectures
    if ( pointerSize() == 4 )
        return false;

    __block Diagnostics diag;
    __block bool result = false;
    if ( hasChainedFixups() ) {
        withChainStarts(diag, chainStartsOffset(), ^(const dyld_chained_starts_in_image* startsInfo) {
            forEachFixupInAllChains(diag, startsInfo, false, ^(MachOLoaded::ChainedFixupPointerOnDisk* fixupLoc, const dyld_chained_starts_in_segment* segInfo, bool& fixupsStop) {
                if ( ((long)(fixupLoc) & 7) != 0 ) {
                    result = true;
                    fixupsStop = true;
                }
           });
        });
    }
    else {
        forEachBind(diag, ^(uint64_t runtimeOffset, int libOrdinal, const char* symbolName, bool weakImport, bool lazyBind, uint64_t addend, bool& stop) {
            if ( (runtimeOffset & 7) != 0 ) {
                result = true;
                stop = true;
            }
        },
        ^(const char* symbolName) {
        });
        forEachRebase(diag, true, ^(uint64_t runtimeOffset, bool& stop) {
            if ( (runtimeOffset & 7) != 0 ) {
                result = true;
                stop = true;
            }
        });
    }

    return result;
}

void MachOAnalyzer::recurseTrie(Diagnostics& diag, const uint8_t* const start, const uint8_t* p, const uint8_t* const end,
                                OverflowSafeArray<char>& cummulativeString, int curStrOffset, bool& stop, ExportsCallback callback) const
{
    if ( p >= end ) {
        diag.error("malformed trie, node past end");
        return;
    }
    const uint64_t terminalSize = read_uleb128(diag, p, end);
    const uint8_t* children = p + terminalSize;
    if ( terminalSize != 0 ) {
        uint64_t    imageOffset = 0;
        uint64_t    flags       = read_uleb128(diag, p, end);
        uint64_t    other       = 0;
        const char* importName  = nullptr;
        if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
            other = read_uleb128(diag, p, end); // dylib ordinal
            importName = (char*)p;
        }
        else {
            imageOffset = read_uleb128(diag, p, end);
            if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER )
                other = read_uleb128(diag, p, end);
            else
                other = 0;
        }
        if ( diag.hasError() )
            return;
        callback(cummulativeString.begin(), imageOffset, flags, other, importName, stop);
        if ( stop )
            return;
    }
    if ( children > end ) {
        diag.error("malformed trie, terminalSize extends beyond trie data");
        return;
    }
    const uint8_t childrenCount = *children++;
    const uint8_t* s = children;
    for (uint8_t i=0; i < childrenCount; ++i) {
        int edgeStrLen = 0;
        while (*s != '\0') {
            cummulativeString.resize(curStrOffset+edgeStrLen + 1);
            cummulativeString[curStrOffset+edgeStrLen] = *s++;
            ++edgeStrLen;
            if ( s > end ) {
                diag.error("malformed trie node, child node extends past end of trie\n");
                return;
            }
       }
        cummulativeString.resize(curStrOffset+edgeStrLen + 1);
        cummulativeString[curStrOffset+edgeStrLen] = *s++;
        uint64_t childNodeOffset = read_uleb128(diag, s, end);
        if (childNodeOffset == 0) {
            diag.error("malformed trie, childNodeOffset==0");
            return;
        }
        recurseTrie(diag, start, start+childNodeOffset, end, cummulativeString, curStrOffset+edgeStrLen, stop, callback);
        if ( diag.hasError() || stop )
            return;
    }
}

void MachOAnalyzer::forEachExportedSymbol(Diagnostics& diag, ExportsCallback callback) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;
    uint64_t trieSize;
    if ( const uint8_t* trieStart = getExportsTrie(leInfo, trieSize) ) {
        const uint8_t* trieEnd   = trieStart + trieSize;
        // We still emit empty export trie load commands just as a placeholder to show we have
        // no exports.  In that case, don't start recursing as we'll immediately think we ran
        // of the end of the buffer
        if ( trieSize == 0 )
            return;
        bool stop = false;
        STACK_ALLOC_OVERFLOW_SAFE_ARRAY(char, cummulativeString, 4096);
        recurseTrie(diag, trieStart, trieStart, trieEnd, cummulativeString, 0, stop, callback);
   }
}

bool MachOAnalyzer::neverUnload() const
{
    bool hasSwift = false;
    if ( this->hasSwiftOrObjC(&hasSwift) ) {
        // Policy: images with ObjC or Swift are never unloaded
        // except MH_BUNDLE *without* Swift can be unloaded
        if ( hasSwift || !this->isBundle() ) {
            return true;
        }
    }

    if ( (this->flags & MH_HAS_TLV_DESCRIPTORS) ) {
        return true;
    }
    else {
        // record if image has DOF sections
        __block bool  hasDOFs = false;
        Diagnostics   diag;
        this->forEachDOFSection(diag, ^(uint32_t offset) {
            hasDOFs = true;
        });
        if ( diag.noError() && hasDOFs )
            return true;
    }
    return false;
}

#if BUILDING_APP_CACHE_UTIL
bool MachOAnalyzer::canBePlacedInKernelCollection(const char* path, void (^failureReason)(const char*)) const
{
    if (!MachOFile::canBePlacedInKernelCollection(path, failureReason))
        return false;

    // App caches reguire that everything be built with split seg v2
    // This is because v1 can't move anything other than __TEXT and __DATA
    // but kernels have __TEXT_EXEC and other segments
    if ( isKextBundle() ) {
        // x86_64 kext's might not have split seg
        if ( !isArch("x86_64") && !isArch("x86_64h") ) {
            if ( !isSplitSegV2() ) {
                failureReason("Missing split seg v2");
                return false;
            }
        }
    } else if ( isStaticExecutable() ) {
        // The kernel must always have split seg V2
        if ( !isSplitSegV2() ) {
            failureReason("Missing split seg v2");
            return false;
        }

        // The kernel should have __TEXT and __TEXT_EXEC
        __block bool foundText = false;
        __block bool foundTextExec = false;
        __block bool foundHIB = false;
        __block uint64_t hibernateVMAddr = 0;
        __block uint64_t hibernateVMSize = 0;
        ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo &segmentInfo, bool &stop) {
            if ( segmentInfo.segmentName == "__TEXT" ) {
                foundText = true;
            }
            if ( segmentInfo.segmentName == "__TEXT_EXEC" ) {
                foundTextExec = true;
            }
            if ( segmentInfo.segmentName == "__HIB" ) {
                foundHIB = true;
                hibernateVMAddr = segmentInfo.vmaddr;
                hibernateVMSize = segmentInfo.vmsize;
            }
        });
        if (!foundText) {
            failureReason("Expected __TEXT segment");
            return false;
        }
        if ( foundTextExec && foundHIB ) {
            failureReason("Expected __TEXT_EXEC or __HIB segment, but found both");
            return false;
        }
        if ( !foundTextExec && !foundHIB ) {
            failureReason("Expected __TEXT_EXEC or __HIB segment, but found neither");
            return false;
        }

        // The hibernate segment should be mapped before the base address
        if ( foundHIB ) {
            uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
            if ( greaterThanAddOrOverflow(hibernateVMAddr, hibernateVMSize, baseAddress) ) {
                failureReason("__HIB segment should be mapped before base address");
                return false;
            }
        }
    }

    // Don't allow kext's to have load addresses
    if ( isKextBundle() && (((const Header*)this)->preferredLoadAddress() != 0) ) {
        failureReason("Has load address");
        return false;
    }

    // All kexts with an executable must have a kmod_info
    if ( isKextBundle() ) {
        __block bool found = false;
        __block Diagnostics diag;

        // Check for a global first
        FoundSymbol foundInfo;
        found = findExportedSymbol(diag, "_kmod_info", true, foundInfo, nullptr);
        if ( !found ) {
            // And fall back to a local if we need to
            forEachLocalSymbol(diag, ^(const char* aSymbolName, uint64_t n_value, uint8_t n_type,
                                       uint8_t n_sect, uint16_t n_desc, bool& stop) {
                if ( strcmp(aSymbolName, "_kmod_info") == 0 ) {
                    found = true;
                    stop = true;
                }
            });
        }

        if ( !found ) {
            failureReason("kexts must have a _kmod_info symbol");
            return false;
        }
    }

    if (hasChainedFixups()) {
        if ( usesClassicRelocationsInKernelCollection() ) {
            failureReason("Cannot use fixup chains with binary expecting classic relocations");
            return false;
        }

        __block bool fixupsOk = true;
        __block Diagnostics diag;
        withChainStarts(diag, 0, ^(const dyld_chained_starts_in_image* starts) {
            forEachFixupInAllChains(diag, starts, false, ^(dyld3::MachOLoaded::ChainedFixupPointerOnDisk* fixupLoc,
                                                           const dyld_chained_starts_in_segment* segInfo, bool& stop) {
                // We only support inputs from a few pointer format types, so that we don't need to handle them all later
                switch (segInfo->pointer_format) {
                    case DYLD_CHAINED_PTR_ARM64E:
                    case DYLD_CHAINED_PTR_64:
                    case DYLD_CHAINED_PTR_32:
                    case DYLD_CHAINED_PTR_32_CACHE:
                    case DYLD_CHAINED_PTR_32_FIRMWARE:
                        failureReason("unsupported chained fixups pointer format");
                        fixupsOk = false;
                        stop = true;
                        return;
                    case DYLD_CHAINED_PTR_64_OFFSET:
                        // arm64 kernel and kexts use this format
                        break;
                    case DYLD_CHAINED_PTR_ARM64E_KERNEL:
                        // arm64e kexts use this format
                        break;
                    case DYLD_CHAINED_PTR_64_KERNEL_CACHE:
                    case DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE:
                        failureReason("unsupported chained fixups pointer format");
                        fixupsOk = false;
                        stop = true;
                        return;
                    default:
                        failureReason("unknown chained fixups pointer format");
                        fixupsOk = false;
                        stop = true;
                        return;
                }

                uint64_t vmOffset = (uint8_t*)fixupLoc - (uint8_t*)this;
                // Error if the fixup location is anything other than 4/8 byte aligned
                if ( (vmOffset & 0x3) != 0 ) {
                    failureReason("fixup value is not 4-byte aligned");
                    fixupsOk = false;
                    stop = true;
                    return;
                }

                // We also must only need 30-bits for the chain format of the resulting cache
                if ( vmOffset >= (1 << 30) ) {
                    failureReason("fixup value does not fit in 30-bits");
                    fixupsOk = false;
                    stop = true;
                    return;
                }
            });
        });
        if (!fixupsOk)
            return false;
    } else {
        // x86_64 xnu will have unaligned text/data fixups and fixups inside __HIB __text.
        // We allow these as xnu is emitted with classic relocations
        bool canHaveUnalignedFixups = usesClassicRelocationsInKernelCollection();
        canHaveUnalignedFixups |= ( isArch("x86_64") || isArch("x86_64h") );
        __block bool rebasesOk = true;
        Diagnostics diag;
        forEachRebase(diag, false, ^(uint64_t runtimeOffset, bool &stop) {
            // Error if the rebase location is anything other than 4/8 byte aligned
            if ( !canHaveUnalignedFixups && ((runtimeOffset & 0x3) != 0) ) {
                failureReason("rebase value is not 4-byte aligned");
                rebasesOk = false;
                stop = true;
                return;
            }

#if BUILDING_APP_CACHE_UTIL
            // xnu for x86_64 has __HIB mapped before __DATA, so offsets appear to be
            // negative.  Adjust the fixups so that we don't think they are out of
            // range of the number of bits we have
            if ( isStaticExecutable() ) {
                __block uint64_t baseAddress = ~0ULL;
                ((const Header*)this)->forEachSegment(^(const Header::SegmentInfo& sinfo, bool& segStop) {
                    baseAddress = std::min(baseAddress, sinfo.vmaddr);
                });
                uint64_t textSegVMAddr = ((const Header*)this)->preferredLoadAddress();
                runtimeOffset = (textSegVMAddr + runtimeOffset) - baseAddress;
            }
#endif

            // We also must only need 30-bits for the chain format of the resulting cache
            if ( runtimeOffset >= (1 << 30) ) {
                failureReason("rebase value does not fit in 30-bits");
                rebasesOk = false;
                stop = true;
                return;
            }
        });
        if (!rebasesOk)
            return false;

        __block bool bindsOk = true;
        forEachBind(diag, ^(uint64_t runtimeOffset, int libOrdinal, uint8_t type, const char *symbolName,
                            bool weakImport, bool lazyBind, uint64_t addend, bool &stop) {

            // Don't validate branch fixups as we'll turn then in to direct jumps instead
            if ( type == BIND_TYPE_TEXT_PCREL32 )
                return;

            // Error if the bind location is anything other than 4/8 byte aligned
            if ( !canHaveUnalignedFixups && ((runtimeOffset & 0x3) != 0) ) {
                failureReason("bind value is not 4-byte aligned");
                bindsOk = false;
                stop = true;
                return;
            }

            // We also must only need 30-bits for the chain format of the resulting cache
            if ( runtimeOffset >= (1 << 30) ) {
                failureReason("bind value does not fit in 30-bits");
                rebasesOk = false;
                stop = true;
                return;
            }
        }, ^(const char *symbolName) {
        });
        if (!bindsOk)
            return false;
    }

    return true;
}

#endif

uint64_t MachOAnalyzer::chainStartsOffset() const
{
    const dyld_chained_fixups_header* header = chainedFixupsHeader();
    // old arm64e binary has no dyld_chained_fixups_header
    if ( header == nullptr )
        return 0;
    return header->starts_offset + ((uint8_t*)header - (uint8_t*)this);
}

const dyld_chained_fixups_header* MachOAnalyzer::chainedFixupsHeader() const
{
    Diagnostics diag;
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() || (leInfo.chainedFixups == nullptr) )
        return nullptr;

    return (dyld_chained_fixups_header*)getLinkEditContent(leInfo.layout, leInfo.chainedFixups->dataoff);
}

uint16_t MachOAnalyzer::chainedPointerFormat() const
{
    const dyld_chained_fixups_header* header = chainedFixupsHeader();
    if ( header != nullptr ) {
        // get pointer format from chain info struct in LINKEDIT
        return MachOFile::chainedPointerFormat(header);
    }
    assert(this->cputype == CPU_TYPE_ARM64 && (this->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) && "chainedPointerFormat() called on non-chained binary");
    return DYLD_CHAINED_PTR_ARM64E;
}

#if (BUILDING_DYLD || BUILDING_LIBDYLD) && !__arm64e__
  #define SUPPORT_OLD_ARM64E_FORMAT 0
#else
  #define SUPPORT_OLD_ARM64E_FORMAT 1
#endif

// find dyld_chained_starts_in_image* in image
// if old arm64e binary, synthesize dyld_chained_starts_in_image*
void MachOAnalyzer::withChainStarts(Diagnostics& diag, uint64_t startsStructOffsetHint, void (^callback)(const dyld_chained_starts_in_image*)) const
{
    if ( startsStructOffsetHint != 0 ) {
        // we have a pre-computed offset into LINKEDIT for dyld_chained_starts_in_image
        callback((dyld_chained_starts_in_image*)((uint8_t*)this + startsStructOffsetHint));
        return;
    }

    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    if ( leInfo.chainedFixups != nullptr ) {
        // find dyld_chained_starts_in_image from dyld_chained_fixups_header
        const dyld_chained_fixups_header* header = (dyld_chained_fixups_header*)getLinkEditContent(leInfo.layout, leInfo.chainedFixups->dataoff);
        callback((dyld_chained_starts_in_image*)((uint8_t*)header + header->starts_offset));
    }
#if SUPPORT_OLD_ARM64E_FORMAT
    // don't want this code in non-arm64e dyld because it causes a stack protector which dereferences a GOT pointer before GOT is set up
    else if ( (leInfo.dyldInfo != nullptr) && (this->cputype == CPU_TYPE_ARM64) && (this->maskedCpuSubtype() == CPU_SUBTYPE_ARM64E) ) {
        // old arm64e binary, create a dyld_chained_starts_in_image for caller
        uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
        uint64_t imagePageCount = this->mappedSize()/0x1000;
        size_t bufferSize = leInfo.dyldInfo->bind_size + (size_t)imagePageCount*sizeof(uint16_t) + 512;
        BLOCK_ACCCESSIBLE_ARRAY(uint8_t, buffer, bufferSize);
        uint8_t* bufferEnd = &buffer[bufferSize];
        dyld_chained_starts_in_image* header = (dyld_chained_starts_in_image*)buffer;
        header->seg_count = leInfo.layout.linkeditSegIndex;
        for (uint32_t i=0; i < header->seg_count; ++i)
            header->seg_info_offset[i] = 0;
        __block uint8_t curSegIndex = 0;
        __block dyld_chained_starts_in_segment* curSeg = (dyld_chained_starts_in_segment*)(&(header->seg_info_offset[header->seg_count]));
        parseOrgArm64eChainedFixups(diag, nullptr, nullptr, ^(const LinkEditInfo& leInfo2, const Header::SegmentInfo segments[], uint8_t segmentIndex,
                                                              bool segIndexSet, uint64_t segmentOffset, uint16_t format, bool& stop) {
            uint32_t pageIndex = (uint32_t)(segmentOffset/0x1000);
            if ( segmentIndex != curSegIndex ) {
                if ( curSegIndex == 0 ) {
                    header->seg_info_offset[segmentIndex] = (uint32_t)((uint8_t*)curSeg - buffer);
                }
                else {
                    header->seg_info_offset[segmentIndex] = (uint32_t)((uint8_t*)(&curSeg->page_start[curSeg->page_count]) - buffer);
                    curSeg = (dyld_chained_starts_in_segment*)((uint8_t*)header+header->seg_info_offset[segmentIndex]);
                    assert((uint8_t*)curSeg < bufferEnd);
               }
               curSeg->page_count = 0;
               curSegIndex = segmentIndex;
            }
            while ( curSeg->page_count != pageIndex ) {
                assert((uint8_t*)(&curSeg->page_start[curSeg->page_count]) < bufferEnd);
                curSeg->page_start[curSeg->page_count] = 0xFFFF;
                curSeg->page_count++;
            }
            curSeg->size                  = (uint32_t)((uint8_t*)(&curSeg->page_start[pageIndex]) - (uint8_t*)curSeg);
            curSeg->page_size             = 0x1000; // old arm64e encoding used 4KB pages
            curSeg->pointer_format        = DYLD_CHAINED_PTR_ARM64E;
            curSeg->segment_offset        = segments[segmentIndex].vmaddr - baseAddress;
            curSeg->max_valid_pointer     = 0;
            curSeg->page_count            = pageIndex+1;
            assert((uint8_t*)(&curSeg->page_start[pageIndex]) < bufferEnd);
            curSeg->page_start[pageIndex] = segmentOffset & 0xFFF;
            //fprintf(stderr, "segment_offset=0x%llX, vmAddr=0x%llX\n", curSeg->segment_offset, segments[segmentIndex].vmAddr );
            //printf("segIndex=%d, segOffset=0x%08llX, page_start[%d]=0x%04X, page_start[%d]=0x%04X\n",
            //        segmentIndex, segmentOffset, pageIndex, curSeg->page_start[pageIndex], pageIndex-1, pageIndex ? curSeg->page_start[pageIndex-1] : 0);
        });
        callback(header);
    }
#endif
    else {
        diag.error("image does not use chained fixups");
    }
}

struct OldThreadsStartSection
{
    uint32_t        padding : 31,
                    stride8 : 1;
    uint32_t        chain_starts[1];
};

// ld64 can't sometimes determine the size of __thread_starts accurately,
// because these sections have to be given a size before everything is laid out,
// and you don't know the actual size of the chains until everything is
// laid out. In order to account for this, the linker puts trailing 0xFFFFFFFF at
// the end of the section, that must be ignored when walking the chains. This
// patch adjust the section size accordingly.
static uint32_t adjustStartsCount(uint32_t startsCount, const uint32_t* starts) {
    for ( int i = startsCount; i > 0; --i )
    {
        if ( starts[i - 1] == 0xFFFFFFFF )
            startsCount--;
        else
            break;
    }
    return startsCount;
}

bool MachOAnalyzer::hasFirmwareChainStarts(uint16_t* pointerFormat, uint32_t* startsCount, const uint32_t** starts) const
{
    if ( !this->isPreload() && !this->isStaticExecutable() )
        return false;

    uint64_t sectionSize;
    if (const dyld_chained_starts_offsets* sect = (dyld_chained_starts_offsets*)this->findSectionContent("__TEXT", "__chain_starts", sectionSize) ) {
        *pointerFormat = sect->pointer_format;
        *startsCount   = sect->starts_count;
        *starts        = &sect->chain_starts[0];
        return true;
    }
    if (const OldThreadsStartSection* sect = (OldThreadsStartSection*)this->findSectionContent("__TEXT", "__thread_starts", sectionSize) ) {
        *pointerFormat = sect->stride8 ? DYLD_CHAINED_PTR_ARM64E : DYLD_CHAINED_PTR_ARM64E_FIRMWARE;
        *startsCount   = adjustStartsCount((uint32_t)(sectionSize/4) - 1, sect->chain_starts);
        *starts        = sect->chain_starts;
        return true;
    }
    return false;
}

bool MachOAnalyzer::hasRebaseRuns(const void** runs, size_t* runsSize) const
{
    if ( !this->isPreload() )
        return false;

    uint64_t sectionSize;
    if (const void* sect = this->findSectionContent("__TEXT", "__rebase_info", sectionSize) ) {
        *runs     = sect;
        *runsSize = (size_t)sectionSize;
        return true;
    }
    return false;
}

struct RebaseRuns
{
	uint32_t  startAddress;
	uint8_t   runs[];   // value of even indexes is how many pointers in a row are rebases, value of odd indexes times 4 is memory to skip over
						// two zero values in a row signals the end of the run
};

void MachOAnalyzer::forEachRebaseRunAddress(const void* runs, size_t runsSize, void (^handler)(uint32_t address)) const
{
    const RebaseRuns* rr  = (RebaseRuns*)runs;
    const RebaseRuns* end = (RebaseRuns*)((uint8_t*)runs + runsSize);
    while ( rr < end ) {
        uint32_t address = rr->startAddress;
        int index = 0;
        bool done = false;
        while ( !done ) {
            uint8_t count = rr->runs[index];
            if ( count == 0 ) {
                // two 0x00 in a row mean the run is complete
                if ( rr->runs[index+1] == 0 ) {
                    ++index;
                    done = true;
                }
            }
            else {
                if ( index & 1 ) {
                    // odd runs index => how much to jump forward
                    address += ((count-1) * 4);
                }
                else {
                    // even runs index => how many pointers in a row that need rebasing
                    for (int i=0; i < count; ++i) {
                        handler(address);
                        address += 4;
                    }
                }
            }
            ++index;
        }
        // 4-byte align for next run
        index = (index+3) & (-4);
        rr  = (RebaseRuns*)(&rr->runs[index]);
    }
}

MachOAnalyzer::ObjCInfo MachOAnalyzer::getObjCInfo() const
{
    __block ObjCInfo result;
    result.selRefCount      = 0;
    result.classDefCount    = 0;
    result.protocolDefCount = 0;

    const uint32_t ptrSize  = pointerSize();
    forEachSection(^(const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.segmentName.starts_with("__DATA") ) {
            if ( sectInfo.sectionName == "__objc_selrefs" )
                result.selRefCount += (sectInfo.size/ptrSize);
            else if ( sectInfo.sectionName == "__objc_classlist" )
                result.classDefCount += (sectInfo.size/ptrSize);
            else if ( sectInfo.sectionName == "__objc_protolist" )
                result.protocolDefCount += (sectInfo.size/ptrSize);
        }
        else if ( (this->cputype == CPU_TYPE_I386) && (sectInfo.sectionName == "__OBJC") ) {
            if ( sectInfo.sectionName == "__message_refs" )
                result.selRefCount += (sectInfo.size/4);
            else if ( sectInfo.sectionName == "__class" )
                result.classDefCount += (sectInfo.size/48);
            else if ( sectInfo.sectionName == "__protocol" )
                result.protocolDefCount += (sectInfo.size/20);
        }
   });

    return result;
}

uint64_t MachOAnalyzer::ObjCClassInfo::getReadOnlyDataField(ObjCClassInfo::ReadOnlyDataField field, uint32_t pointerSize) const {
    if (pointerSize == 8) {
        typedef uint64_t PtrTy;
        struct class_ro_t {
            uint32_t flags;
            uint32_t instanceStart;
            // Note there is 4-bytes of alignment padding between instanceSize and ivarLayout
            // on 64-bit archs, but no padding on 32-bit archs.
            // This union is a way to model that.
            union {
                uint32_t   instanceSize;
                PtrTy   pad;
            } instanceSize;
            PtrTy ivarLayoutVMAddr;
            PtrTy nameVMAddr;
            PtrTy baseMethodsVMAddr;
            PtrTy baseProtocolsVMAddr;
            PtrTy ivarsVMAddr;
            PtrTy weakIvarLayoutVMAddr;
            PtrTy basePropertiesVMAddr;
        };
        const class_ro_t* classData = (const class_ro_t*)(dataVMAddr + vmAddrConverter.slide);
        switch (field) {
        case ObjCClassInfo::ReadOnlyDataField::name:
            return vmAddrConverter.convertToVMAddr(classData->nameVMAddr);
        case ObjCClassInfo::ReadOnlyDataField::baseProtocols:
            return vmAddrConverter.convertToVMAddr(classData->baseProtocolsVMAddr);
        case ObjCClassInfo::ReadOnlyDataField::baseMethods:
            return vmAddrConverter.convertToVMAddr(classData->baseMethodsVMAddr);
        case ObjCClassInfo::ReadOnlyDataField::baseProperties:
            return vmAddrConverter.convertToVMAddr(classData->basePropertiesVMAddr);
        case ObjCClassInfo::ReadOnlyDataField::flags:
            return classData->flags;
        }
    } else {
        typedef uint32_t PtrTy;
        struct class_ro_t {
            uint32_t flags;
            uint32_t instanceStart;
            // Note there is 4-bytes of alignment padding between instanceSize and ivarLayout
            // on 64-bit archs, but no padding on 32-bit archs.
            // This union is a way to model that.
            union {
                uint32_t   instanceSize;
                PtrTy   pad;
            } instanceSize;
            PtrTy ivarLayoutVMAddr;
            PtrTy nameVMAddr;
            PtrTy baseMethodsVMAddr;
            PtrTy baseProtocolsVMAddr;
            PtrTy ivarsVMAddr;
            PtrTy weakIvarLayoutVMAddr;
            PtrTy basePropertiesVMAddr;
        };
        const class_ro_t* classData = (const class_ro_t*)(dataVMAddr + vmAddrConverter.slide);
        switch (field) {
            case ObjCClassInfo::ReadOnlyDataField::name:
                return vmAddrConverter.convertToVMAddr(classData->nameVMAddr);
            case ObjCClassInfo::ReadOnlyDataField::baseProtocols:
                return vmAddrConverter.convertToVMAddr(classData->baseProtocolsVMAddr);
            case ObjCClassInfo::ReadOnlyDataField::baseMethods:
                return vmAddrConverter.convertToVMAddr(classData->baseMethodsVMAddr);
            case ObjCClassInfo::ReadOnlyDataField::baseProperties:
                return vmAddrConverter.convertToVMAddr(classData->basePropertiesVMAddr);
            case ObjCClassInfo::ReadOnlyDataField::flags:
                return classData->flags;
        }
    }
}

const char* MachOAnalyzer::getPrintableString(uint64_t stringVMAddr, MachOAnalyzer::PrintableStringResult& result) const {
    uint32_t fairplayTextOffsetStart;
    uint32_t fairplayTextOffsetEnd;
    uint32_t fairplaySize;
    if ( ((const Header*)this)->isFairPlayEncrypted(fairplayTextOffsetStart, fairplaySize) ) {
        fairplayTextOffsetEnd = fairplayTextOffsetStart + fairplaySize;
    } else {
        fairplayTextOffsetEnd = 0;
    }

    result = PrintableStringResult::UnknownSection;
    forEachSection(^(const Header::SegmentInfo& segInfo, const Header::SectionInfo &sectInfo, bool &stop) {
        if ( stringVMAddr < sectInfo.address ) {
            return;
        }
        if ( stringVMAddr >= ( sectInfo.address + sectInfo.size) ) {
            return;
        }

        // We can't scan this section if its protected
        if ( segInfo.isProtected() ) {
            result = PrintableStringResult::ProtectedSection;
            stop = true;
            return;
        }

        // We can't scan this section if it overlaps with the fairplay range
        if ( fairplayTextOffsetEnd < sectInfo.fileOffset ) {
            // Fairplay range ends before section
        } else if ( fairplayTextOffsetStart > (sectInfo.fileOffset + sectInfo.size) ) {
            // Fairplay range starts after section
        } else {
            // Must overlap
            result = PrintableStringResult::FairPlayEncrypted;
            stop = true;
            return;
        }
    
        result = PrintableStringResult::CanPrint;
        stop = true;
    });

#if BUILDING_SHARED_CACHE_UTIL || BUILDING_DYLDINFO
    // The shared cache coalesces strings in to their own section.
    // Assume its a valid pointer
    if (result == PrintableStringResult::UnknownSection && this->inDyldCache()) {
        result = PrintableStringResult::CanPrint;
        return (const char*)(stringVMAddr + getSlide());
    }
#endif

    if (result == PrintableStringResult::CanPrint)
        return (const char*)(stringVMAddr + getSlide());
    return nullptr;
}

void MachOAnalyzer::forEachObjCClass(uint64_t classListRuntimeOffset, uint64_t classListCount,
                                     const VMAddrConverter& vmAddrConverter,
                                     ClassCallback& callback) const {
#if !BUILDING_DYLD
    // ObjC patching needs the bind targets for interposable references to the classes
    // build targets table
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(uint64_t, bindTargets, 32);
    if ( this->hasChainedFixups() ) {
        intptr_t slide = this->getSlide();
        Diagnostics diag;
        this->forEachBindTarget(diag, false, ^(const BindTargetInfo& info, bool& stop) {
            if ( diag.hasError() ) {
                stop = true;
                return;
            }

            if ( info.libOrdinal == BIND_SPECIAL_DYLIB_SELF ) {
                void* result = nullptr;
                bool  resultPointsToInstructions = false;
                if ( this->hasExportedSymbol(info.symbolName, nullptr, &result, &resultPointsToInstructions) ) {
                    uint64_t resultVMAddr = (uint64_t)result - (uint64_t)slide;
                    bindTargets.push_back(resultVMAddr);
                } else {
                    bindTargets.push_back(0);
                }
            } else {
                bindTargets.push_back(0);
            }
        }, ^(const MachOAnalyzer::BindTargetInfo& info, bool& stop) {
        });
    }
#else
    // dyld always analyzes objc after fixups, so we don't need the bind targets
    Array<uint64_t> bindTargets;
#endif

    const uint64_t ptrSize = pointerSize();
    const uint8_t* arrayBase = (uint8_t*)this + classListRuntimeOffset;
    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        __block bool stop = false;
        for ( uint64_t i = 0; i != classListCount; ++i ) {
            uint64_t classVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))), bindTargets);
            parseObjCClass(vmAddrConverter, classVMAddr, bindTargets,
                           ^(uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr, const ObjCClassInfo& objcClass) {
                callback(classVMAddr, classSuperclassVMAddr, classDataVMAddr, objcClass, false, stop);
                if ( stop )
                    return;

                // Then parse and call for the metaclass
                uint64_t isaVMAddr = objcClass.isaVMAddr;
                parseObjCClass(vmAddrConverter, isaVMAddr, bindTargets,
                               ^(uint64_t metaclassSuperclassVMAddr, uint64_t metaclassDataVMAddr, const ObjCClassInfo& objcMetaclass) {
                    callback(isaVMAddr, metaclassSuperclassVMAddr, metaclassDataVMAddr, objcMetaclass, true, stop);
                });
            });
            if ( stop )
                break;
        }
    } else {
        typedef uint32_t PtrTy;
        __block bool stop = false;
        for ( uint64_t i = 0; i != classListCount; ++i ) {
            uint64_t classVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))), bindTargets);
            parseObjCClass(vmAddrConverter, classVMAddr, bindTargets,
                           ^(uint64_t classSuperclassVMAddr, uint64_t classDataVMAddr, const ObjCClassInfo& objcClass) {
                callback(classVMAddr, classSuperclassVMAddr, classDataVMAddr, objcClass, false, stop);
                if ( stop )
                    return;

                // Then parse and call for the metaclass
                uint64_t isaVMAddr = objcClass.isaVMAddr;
                parseObjCClass(vmAddrConverter, isaVMAddr, bindTargets,
                               ^(uint64_t metaclassSuperclassVMAddr, uint64_t metaclassDataVMAddr, const ObjCClassInfo& objcMetaclass) {
                    callback(isaVMAddr, metaclassSuperclassVMAddr, metaclassDataVMAddr, objcMetaclass, true, stop);
                });
            });
            if ( stop )
                break;
        }
    }
}

void MachOAnalyzer::forEachObjCClass(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                                     ClassCallback& callback) const {
    uint64_t classListRuntimeOffset;
    uint64_t classListSize;
    bool foundSection = ((const Header*)this)->findObjCDataSection("__objc_classlist", classListRuntimeOffset, classListSize);
    if ( !foundSection )
        return;

    const uint64_t ptrSize = pointerSize();
    if ( (classListSize % ptrSize) != 0 ) {
        diag.error("Invalid objc class section size");
        return;
    }

    forEachObjCClass(classListRuntimeOffset, classListSize / ptrSize, vmAddrConverter, callback);
}

void MachOAnalyzer::parseObjCClass(const VMAddrConverter& vmAddrConverter,
                                   uint64_t classVMAddr, const Array<uint64_t>& bindTargets,
                                   void (^handler)(uint64_t classSuperclassVMAddr,
                                                   uint64_t classDataVMAddr,
                                                   const ObjCClassInfo& objcClass)) const {
    const uint64_t ptrSize = pointerSize();
    intptr_t slide = getSlide();

    uint64_t classSuperclassVMAddr = 0;
    uint64_t classDataVMAddr       = 0;
    ObjCClassInfo objcClass;

    if ( ptrSize == 8 ) {
       struct objc_class_t {
           uint64_t isaVMAddr;
           uint64_t superclassVMAddr;
           uint64_t methodCacheBuckets;
           uint64_t methodCacheProperties;
           uint64_t dataVMAddrAndFastFlags;
       };
        // This matches "struct TargetClassMetadata" from Metadata.h in Swift
        struct swift_class_metadata_t : objc_class_t {
            uint32_t swiftClassFlags;
        };
        enum : uint64_t {
            FAST_DATA_MASK = 0x00007ffffffffff8ULL
        };
        classSuperclassVMAddr = classVMAddr + offsetof(objc_class_t, superclassVMAddr);
        classDataVMAddr       = classVMAddr + offsetof(objc_class_t, dataVMAddrAndFastFlags);

        // First call the handler on the class
        const objc_class_t*           classPtr      = (const objc_class_t*)(classVMAddr + slide);
        const swift_class_metadata_t* swiftClassPtr = (const swift_class_metadata_t*)classPtr;
        objcClass.isaVMAddr         = vmAddrConverter.convertToVMAddr(classPtr->isaVMAddr, bindTargets);
        objcClass.superclassVMAddr  = vmAddrConverter.convertToVMAddr(classPtr->superclassVMAddr);
        objcClass.methodCacheVMAddr  = classPtr->methodCacheProperties == 0 ? 0 : vmAddrConverter.convertToVMAddr(classPtr->methodCacheProperties);
        objcClass.dataVMAddr        = vmAddrConverter.convertToVMAddr(classPtr->dataVMAddrAndFastFlags) & FAST_DATA_MASK;
        objcClass.vmAddrConverter   = vmAddrConverter;
        objcClass.isSwiftLegacy     = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_LEGACY;
        objcClass.isSwiftStable     = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_STABLE;
        // The Swift class flags are only present if the class is swift
        objcClass.swiftClassFlags   = (objcClass.isSwiftLegacy || objcClass.isSwiftStable) ? swiftClassPtr->swiftClassFlags : 0;
    } else {
        struct objc_class_t {
            uint32_t isaVMAddr;
            uint32_t superclassVMAddr;
            uint32_t methodCacheBuckets;
            uint32_t methodCacheProperties;
            uint32_t dataVMAddrAndFastFlags;
        };
        // This matches "struct TargetClassMetadata" from Metadata.h in Swift
        struct swift_class_metadata_t : objc_class_t {
            uint32_t swiftClassFlags;
        };
        enum : uint32_t {
            FAST_DATA_MASK = 0xfffffffcUL
        };
        classSuperclassVMAddr = classVMAddr + offsetof(objc_class_t, superclassVMAddr);
        classDataVMAddr       = classVMAddr + offsetof(objc_class_t, dataVMAddrAndFastFlags);

        // First call the handler on the class
        const objc_class_t*           classPtr      = (const objc_class_t*)(classVMAddr + slide);
        const swift_class_metadata_t* swiftClassPtr = (const swift_class_metadata_t*)classPtr;
        objcClass.isaVMAddr         = vmAddrConverter.convertToVMAddr(classPtr->isaVMAddr, bindTargets);
        objcClass.superclassVMAddr  = vmAddrConverter.convertToVMAddr(classPtr->superclassVMAddr);
        objcClass.methodCacheVMAddr  = classPtr->methodCacheProperties == 0 ? 0 : vmAddrConverter.convertToVMAddr(classPtr->methodCacheProperties);
        objcClass.dataVMAddr        = vmAddrConverter.convertToVMAddr(classPtr->dataVMAddrAndFastFlags) & FAST_DATA_MASK;
        objcClass.vmAddrConverter   = vmAddrConverter;
        objcClass.isSwiftLegacy     = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_LEGACY;
        objcClass.isSwiftStable     = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_STABLE;
        // The Swift class flags are only present if the class is swift
        objcClass.swiftClassFlags   = (objcClass.isSwiftLegacy || objcClass.isSwiftStable) ? swiftClassPtr->swiftClassFlags : 0;
    }
                                       
    handler(classSuperclassVMAddr, classDataVMAddr, objcClass);
}

bool MachOAnalyzer::isSwiftClass(const void* classLocation) const
{
    const uint64_t ptrSize = pointerSize();
    if ( ptrSize == 8 ) {
        struct objc_class_t {
            uint64_t isaVMAddr;
            uint64_t superclassVMAddr;
            uint64_t methodCacheBuckets;
            uint64_t methodCacheProperties;
            uint64_t dataVMAddrAndFastFlags;
        };
        enum : uint64_t {
            FAST_DATA_MASK = 0x00007ffffffffff8ULL
        };

        const objc_class_t* classPtr = (const objc_class_t*)classLocation;
        bool isSwiftLegacy = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_LEGACY;
        bool isSwiftStable = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_STABLE;

        // The Swift class flags are only present if the class is swift
        return (isSwiftLegacy || isSwiftStable);
    } else {
        struct objc_class_t {
            uint32_t isaVMAddr;
            uint32_t superclassVMAddr;
            uint32_t methodCacheBuckets;
            uint32_t methodCacheProperties;
            uint32_t dataVMAddrAndFastFlags;
        };
        enum : uint32_t {
            FAST_DATA_MASK = 0xfffffffcUL
        };

        const objc_class_t* classPtr = (const objc_class_t*)classLocation;
        bool isSwiftLegacy = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_LEGACY;
        bool isSwiftStable = classPtr->dataVMAddrAndFastFlags & ObjCClassInfo::FAST_IS_SWIFT_STABLE;

        // The Swift class flags are only present if the class is swift
        return (isSwiftLegacy || isSwiftStable);
    }
}

void MachOAnalyzer::forEachObjCCategory(uint64_t categoryListRuntimeOffset, uint64_t categoryListCount,
                                        const VMAddrConverter& vmAddrConverter, CategoryCallback& callback) const
{
    const uint64_t ptrSize = pointerSize();
    intptr_t slide = getSlide();
    const uint8_t* arrayBase = (uint8_t*)this + categoryListRuntimeOffset;
    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        struct objc_category_t {
            PtrTy nameVMAddr;
            PtrTy clsVMAddr;
            PtrTy instanceMethodsVMAddr;
            PtrTy classMethodsVMAddr;
            PtrTy protocolsVMAddr;
            PtrTy instancePropertiesVMAddr;
        };
        __block bool stop = false;
        for ( uint64_t i = 0; i != categoryListCount; ++i ) {
            uint64_t categoryVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))));
            const objc_category_t* categoryPtr = (const objc_category_t*)(categoryVMAddr + slide);
            ObjCCategory objCCategory;
            objCCategory.nameVMAddr                 = vmAddrConverter.convertToVMAddr(categoryPtr->nameVMAddr);
            objCCategory.clsVMAddr                  = vmAddrConverter.convertToVMAddr(categoryPtr->clsVMAddr);
            objCCategory.instanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(categoryPtr->instanceMethodsVMAddr);
            objCCategory.classMethodsVMAddr         = vmAddrConverter.convertToVMAddr(categoryPtr->classMethodsVMAddr);
            objCCategory.protocolsVMAddr            = vmAddrConverter.convertToVMAddr(categoryPtr->protocolsVMAddr);
            objCCategory.instancePropertiesVMAddr   = vmAddrConverter.convertToVMAddr(categoryPtr->instancePropertiesVMAddr);
            callback(categoryVMAddr, objCCategory, stop);
            if ( stop )
                break;
        }
    } else {
        typedef uint32_t PtrTy;
        struct objc_category_t {
            PtrTy nameVMAddr;
            PtrTy clsVMAddr;
            PtrTy instanceMethodsVMAddr;
            PtrTy classMethodsVMAddr;
            PtrTy protocolsVMAddr;
            PtrTy instancePropertiesVMAddr;
        };
        __block bool stop = false;
        for ( uint64_t i = 0; i != categoryListCount; ++i ) {
            uint64_t categoryVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))));
            const objc_category_t* categoryPtr = (const objc_category_t*)(categoryVMAddr + slide);
            ObjCCategory objCCategory;
            objCCategory.nameVMAddr                 = vmAddrConverter.convertToVMAddr(categoryPtr->nameVMAddr);
            objCCategory.clsVMAddr                  = vmAddrConverter.convertToVMAddr(categoryPtr->clsVMAddr);
            objCCategory.instanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(categoryPtr->instanceMethodsVMAddr);
            objCCategory.classMethodsVMAddr         = vmAddrConverter.convertToVMAddr(categoryPtr->classMethodsVMAddr);
            objCCategory.protocolsVMAddr            = vmAddrConverter.convertToVMAddr(categoryPtr->protocolsVMAddr);
            objCCategory.instancePropertiesVMAddr   = vmAddrConverter.convertToVMAddr(categoryPtr->instancePropertiesVMAddr);
            callback(categoryVMAddr, objCCategory, stop);
            if ( stop )
                break;
        }
    }
}

void MachOAnalyzer::forEachObjCCategory(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                                        CategoryCallback& callback) const {
    uint64_t categoryListRuntimeOffset;
    uint64_t categoryListSize;
    bool foundSection = ((const Header*)this)->findObjCDataSection("__objc_catlist", categoryListRuntimeOffset, categoryListSize);
    if ( !foundSection )
        return;

    const uint64_t ptrSize = pointerSize();
    if ( (categoryListSize % ptrSize) != 0 ) {
        diag.error("Invalid objc category section size");
        return;
    }

    forEachObjCCategory(categoryListRuntimeOffset, categoryListSize / ptrSize, vmAddrConverter, callback);
}

void MachOAnalyzer::forEachObjCProtocol(uint64_t protocolListRuntimeOffset, uint64_t protocolListCount,
                                        const VMAddrConverter& vmAddrConverter,
                                        ProtocolCallback& callback) const
{
    const uint64_t ptrSize = pointerSize();
    intptr_t slide = getSlide();
    const uint8_t* arrayBase = (uint8_t*)this + protocolListRuntimeOffset;
    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        struct protocol_t {
            PtrTy    isaVMAddr;
            PtrTy    nameVMAddr;
            PtrTy    protocolsVMAddr;
            PtrTy    instanceMethodsVMAddr;
            PtrTy    classMethodsVMAddr;
            PtrTy    optionalInstanceMethodsVMAddr;
            PtrTy    optionalClassMethodsVMAddr;
            PtrTy    instancePropertiesVMAddr;
            uint32_t size;
            uint32_t flags;
            // Fields below this point are not always present on disk.
            PtrTy    extendedMethodTypesVMAddr;
            PtrTy    demangledNameVMAddr;
            PtrTy    classPropertiesVMAddr;
        };
        __block bool stop = false;
        for ( uint64_t i = 0; i != protocolListCount; ++i ) {
            uint64_t protocolVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))));
            const protocol_t* protocolPtr = (const protocol_t*)(protocolVMAddr + slide);
            ObjCProtocol objCProtocol;
            objCProtocol.isaVMAddr                          = vmAddrConverter.convertToVMAddr(protocolPtr->isaVMAddr);
            objCProtocol.nameVMAddr                         = vmAddrConverter.convertToVMAddr(protocolPtr->nameVMAddr);
            objCProtocol.protocolsVMAddr                    = vmAddrConverter.convertToVMAddr(protocolPtr->protocolsVMAddr);
            objCProtocol.instanceMethodsVMAddr              = vmAddrConverter.convertToVMAddr(protocolPtr->instanceMethodsVMAddr);
            objCProtocol.classMethodsVMAddr                 = vmAddrConverter.convertToVMAddr(protocolPtr->classMethodsVMAddr);
            objCProtocol.optionalInstanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(protocolPtr->optionalInstanceMethodsVMAddr);
            objCProtocol.optionalClassMethodsVMAddr         = vmAddrConverter.convertToVMAddr(protocolPtr->optionalClassMethodsVMAddr);
            callback(protocolVMAddr, objCProtocol, stop);
            if ( stop )
                break;
        }
    } else {
        typedef uint32_t PtrTy;
        struct protocol_t {
            PtrTy    isaVMAddr;
            PtrTy    nameVMAddr;
            PtrTy    protocolsVMAddr;
            PtrTy    instanceMethodsVMAddr;
            PtrTy    classMethodsVMAddr;
            PtrTy    optionalInstanceMethodsVMAddr;
            PtrTy    optionalClassMethodsVMAddr;
            PtrTy    instancePropertiesVMAddr;
            uint32_t size;
            uint32_t flags;
            // Fields below this point are not always present on disk.
            PtrTy    extendedMethodTypesVMAddr;
            PtrTy    demangledNameVMAddr;
            PtrTy    classPropertiesVMAddr;
        };
        __block bool stop = false;
        for ( uint64_t i = 0; i != protocolListCount; ++i ) {
            uint64_t protocolVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(arrayBase + (i * sizeof(PtrTy))));
            const protocol_t* protocolPtr = (const protocol_t*)(protocolVMAddr + slide);
            ObjCProtocol objCProtocol;
            objCProtocol.isaVMAddr                          = vmAddrConverter.convertToVMAddr(protocolPtr->isaVMAddr);
            objCProtocol.nameVMAddr                         = vmAddrConverter.convertToVMAddr(protocolPtr->nameVMAddr);
            objCProtocol.protocolsVMAddr                    = vmAddrConverter.convertToVMAddr(protocolPtr->protocolsVMAddr);
            objCProtocol.instanceMethodsVMAddr              = vmAddrConverter.convertToVMAddr(protocolPtr->instanceMethodsVMAddr);
            objCProtocol.classMethodsVMAddr                 = vmAddrConverter.convertToVMAddr(protocolPtr->classMethodsVMAddr);
            objCProtocol.optionalInstanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(protocolPtr->optionalInstanceMethodsVMAddr);
            objCProtocol.optionalClassMethodsVMAddr         = vmAddrConverter.convertToVMAddr(protocolPtr->optionalClassMethodsVMAddr);
            callback(protocolVMAddr, objCProtocol, stop);
            if ( stop )
                break;
        }
    }
}

void MachOAnalyzer::forEachObjCProtocol(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                                        ProtocolCallback& callback) const {
    uint64_t protocolListRuntimeOffset;
    uint64_t protocolListSize;
    bool foundSection = ((const Header*)this)->findObjCDataSection("__objc_protolist", protocolListRuntimeOffset, protocolListSize);
    if ( !foundSection )
        return;

    const uint64_t ptrSize = pointerSize();
    if ( (protocolListSize % ptrSize) != 0 ) {
        diag.error("Invalid objc protocol section size");
        return;
    }

    forEachObjCProtocol(protocolListRuntimeOffset, protocolListSize / ptrSize, vmAddrConverter, callback);
}

static void ignorePreoptimizedListsOfLists(uint64_t& listVMAddr, intptr_t slide)
{
    // If this is a list of lists, then we likely just want the class list.  So go to the end which is where
    // we emitted it
    if ( listVMAddr & 1 ) {
        struct ListOfListsEntry {
            union {
                struct {
                    uint64_t imageIndex: 16;
                    int64_t  offset: 48;
                };
                struct {
                    uint32_t entsize;
                    uint32_t count;
                };
            };
        };

        listVMAddr = listVMAddr & ~1;
        const ListOfListsEntry* listHeader = (const ListOfListsEntry*)(listVMAddr + slide);
        if ( listHeader->count != 0 ) {
            const ListOfListsEntry& listEntry = (listHeader + 1)[listHeader->count - 1];

            // The list entry is a relative offset to the target
            // Work out the VMAddress of that target
            uint64_t listEntryVMOffset{(uint64_t)&listEntry - (uint64_t)listHeader};
            uint64_t listEntryVMAddr = listVMAddr + listEntryVMOffset;
            listVMAddr = listEntryVMAddr + (uint64_t)listEntry.offset;
        }
    }
}

void MachOAnalyzer::forEachObjCMethod(uint64_t methodListVMAddr, const VMAddrConverter& vmAddrConverter,
                                      uint64_t sharedCacheRelativeSelectorBaseVMAddress,
                                      void (^handler)(uint64_t methodVMAddr, const ObjCMethod& method, bool& stop)) const
{
    if ( methodListVMAddr == 0 )
        return;

    const uint64_t ptrSize = pointerSize();
    intptr_t slide = getSlide();

    ignorePreoptimizedListsOfLists(methodListVMAddr, slide);

    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        struct method_list_t {
            uint32_t    entsize;
            uint32_t    count;
            PtrTy       methodArrayBase; // Note this is the start the array method_t[0]

            uint32_t getEntsize() const {
                return entsize & ObjCMethodList::methodListSizeMask;
            }

            bool usesDirectOffsetsToSelectors() const {
                return (entsize & 0x40000000) != 0;
            }

            bool usesRelativeOffsets() const {
                return (entsize & 0x80000000) != 0;
            }
        };

        struct method_t {
            PtrTy nameVMAddr;   // SEL
            PtrTy typesVMAddr;  // const char *
            PtrTy impVMAddr;    // IMP
        };

        struct relative_method_t {
            int32_t nameOffset;   // SEL*
            int32_t typesOffset;  // const char *
            int32_t impOffset;    // IMP
        };

        const method_list_t* methodList = (const method_list_t*)(methodListVMAddr + slide);
        if ( methodList == nullptr )
            return;
        bool relativeMethodListsAreOffsetsToSelectors = methodList->usesDirectOffsetsToSelectors();
        uint64_t methodListArrayBaseVMAddr = methodListVMAddr + offsetof(method_list_t, methodArrayBase);
        for (unsigned i = 0; i != methodList->count; ++i) {
            uint64_t methodEntryOffset = i * methodList->getEntsize();
            uint64_t methodVMAddr = methodListArrayBaseVMAddr + methodEntryOffset;
            ObjCMethod method;
            if ( methodList->usesRelativeOffsets() ) {
                const relative_method_t* methodPtr = (const relative_method_t*)(methodVMAddr + slide);
                if ( relativeMethodListsAreOffsetsToSelectors ) {
                    if ( sharedCacheRelativeSelectorBaseVMAddress != 0 ) {
                        // // New shared caches use an offset from a magic selector for relative method lists
                        method.nameVMAddr = sharedCacheRelativeSelectorBaseVMAddress + methodPtr->nameOffset;
                    } else {
                        method.nameVMAddr  = methodVMAddr + offsetof(relative_method_t, nameOffset) + methodPtr->nameOffset;
                    }
                } else {
                    PtrTy* nameLocation = (PtrTy*)((uint8_t*)&methodPtr->nameOffset + methodPtr->nameOffset);
                    method.nameVMAddr   = vmAddrConverter.convertToVMAddr(*nameLocation);
                }
                method.typesVMAddr  = methodVMAddr + offsetof(relative_method_t, typesOffset) + methodPtr->typesOffset;
                method.impVMAddr    = methodVMAddr + offsetof(relative_method_t, impOffset) + methodPtr->impOffset;
                method.nameLocationVMAddr = methodVMAddr + offsetof(relative_method_t, nameOffset) + methodPtr->nameOffset;
            } else {
                const method_t* methodPtr = (const method_t*)(methodVMAddr + slide);
                method.nameVMAddr   = vmAddrConverter.convertToVMAddr(methodPtr->nameVMAddr);
                method.typesVMAddr  = vmAddrConverter.convertToVMAddr(methodPtr->typesVMAddr);
                method.impVMAddr    = vmAddrConverter.convertToVMAddr(methodPtr->impVMAddr);
                method.nameLocationVMAddr = methodVMAddr + offsetof(method_t, nameVMAddr);
            }
            bool stop = false;
            handler(methodVMAddr, method, stop);
            if ( stop )
                break;
        }
    } else {
        typedef uint32_t PtrTy;
        struct method_list_t {
            uint32_t    entsize;
            uint32_t    count;
            PtrTy       methodArrayBase; // Note this is the start the array method_t[0]

            uint32_t getEntsize() const {
                return entsize & ObjCMethodList::methodListSizeMask;
            }

            bool usesDirectOffsetsToSelectors() const {
                return (entsize & 0x40000000) != 0;
            }

            bool usesRelativeOffsets() const {
                return (entsize & 0x80000000) != 0;
            }
        };

        struct method_t {
            PtrTy nameVMAddr;   // SEL
            PtrTy typesVMAddr;  // const char *
            PtrTy impVMAddr;    // IMP
        };

        struct relative_method_t {
            int32_t nameOffset;   // SEL*
            int32_t typesOffset;  // const char *
            int32_t impOffset;    // IMP
        };

        const method_list_t* methodList = (const method_list_t*)(methodListVMAddr + slide);
        if ( methodList == nullptr )
            return;
        bool relativeMethodListsAreOffsetsToSelectors = methodList->usesDirectOffsetsToSelectors();
        uint64_t methodListArrayBaseVMAddr = methodListVMAddr + offsetof(method_list_t, methodArrayBase);
        for (unsigned i = 0; i != methodList->count; ++i) {
            uint64_t methodEntryOffset = i * methodList->getEntsize();
            uint64_t methodVMAddr = methodListArrayBaseVMAddr + methodEntryOffset;
            ObjCMethod method;
            if ( methodList->usesRelativeOffsets() ) {
                const relative_method_t* methodPtr = (const relative_method_t*)(methodVMAddr + slide);
                if ( relativeMethodListsAreOffsetsToSelectors ) {
                    if ( sharedCacheRelativeSelectorBaseVMAddress != 0 ) {
                        // // New shared caches use an offset from a magic selector for relative method lists
                        method.nameVMAddr = sharedCacheRelativeSelectorBaseVMAddress + methodPtr->nameOffset;
                    } else {
                        method.nameVMAddr  = methodVMAddr + offsetof(relative_method_t, nameOffset) + methodPtr->nameOffset;
                    }
                } else {
                    PtrTy* nameLocation = (PtrTy*)((uint8_t*)&methodPtr->nameOffset + methodPtr->nameOffset);
                    method.nameVMAddr   = vmAddrConverter.convertToVMAddr(*nameLocation);
                }
                method.typesVMAddr  = methodVMAddr + offsetof(relative_method_t, typesOffset) + methodPtr->typesOffset;
                method.impVMAddr    = methodVMAddr + offsetof(relative_method_t, impOffset) + methodPtr->impOffset;
                method.nameLocationVMAddr = methodVMAddr + offsetof(relative_method_t, nameOffset) + methodPtr->nameOffset;
            } else {
                const method_t* methodPtr = (const method_t*)(methodVMAddr + slide);
                method.nameVMAddr   = vmAddrConverter.convertToVMAddr(methodPtr->nameVMAddr);
                method.typesVMAddr  = vmAddrConverter.convertToVMAddr(methodPtr->typesVMAddr);
                method.impVMAddr    = vmAddrConverter.convertToVMAddr(methodPtr->impVMAddr);
                method.nameLocationVMAddr = methodVMAddr + offsetof(method_t, nameVMAddr);
            }
            bool stop = false;
            handler(methodVMAddr, method, stop);
            if ( stop )
                break;
        }
    }
}

bool MachOAnalyzer::objcMethodListIsRelative(uint64_t methodListRuntimeOffset) const
{
    if ( methodListRuntimeOffset == 0 )
        return false;

    struct method_list_t {
        uint32_t    entsize;
        uint32_t    count;

        bool usesRelativeOffsets() const {
            return (entsize & 0x80000000) != 0;
        }
    };

    const method_list_t* methodList = (const method_list_t*)((const uint8_t*)this + methodListRuntimeOffset);
    return methodList->usesRelativeOffsets();
}

void MachOAnalyzer::forEachObjCProperty(uint64_t propertyListVMAddr, const VMAddrConverter& vmAddrConverter,
                                        void (^handler)(uint64_t propertyVMAddr, const ObjCProperty& property)) const {
    if ( propertyListVMAddr == 0 )
        return;

    const uint64_t ptrSize = pointerSize();
    intptr_t slide = getSlide();

    ignorePreoptimizedListsOfLists(propertyListVMAddr, slide);

    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        struct property_list_t {
            uint32_t    entsize;
            uint32_t    count;
            PtrTy       propertyArrayBase; // Note this is the start the array property_t[0]

            uint32_t getEntsize() const {
                return (entsize) & ~(uint32_t)3;
            }
        };

        struct property_t {
            PtrTy nameVMAddr;   // SEL
            PtrTy attributesVMAddr;  // const char *
        };

        const property_list_t* propertyList = (const property_list_t*)(propertyListVMAddr + slide);
        uint64_t propertyListArrayBaseVMAddr = propertyListVMAddr + offsetof(property_list_t, propertyArrayBase);
        for (unsigned i = 0; i != propertyList->count; ++i) {
            uint64_t propertyEntryOffset = i * propertyList->getEntsize();
            uint64_t propertyVMAddr = propertyListArrayBaseVMAddr + propertyEntryOffset;
            const property_t* propertyPtr = (const property_t*)(propertyVMAddr + slide);
            ObjCProperty property;
            property.nameVMAddr         = vmAddrConverter.convertToVMAddr(propertyPtr->nameVMAddr);
            property.attributesVMAddr   = vmAddrConverter.convertToVMAddr(propertyPtr->attributesVMAddr);
            handler(propertyVMAddr, property);
        }
    } else {
        typedef uint32_t PtrTy;
        struct property_list_t {
            uint32_t    entsize;
            uint32_t    count;
            PtrTy       propertyArrayBase; // Note this is the start the array property_t[0]

            uint32_t getEntsize() const {
                return (entsize) & ~(uint32_t)3;
            }
        };

        struct property_t {
            PtrTy nameVMAddr;   // SEL
            PtrTy attributesVMAddr;  // const char *
        };

        const property_list_t* propertyList = (const property_list_t*)(propertyListVMAddr + slide);
        uint64_t propertyListArrayBaseVMAddr = propertyListVMAddr + offsetof(property_list_t, propertyArrayBase);
        for (unsigned i = 0; i != propertyList->count; ++i) {
            uint64_t propertyEntryOffset = i * propertyList->getEntsize();
            uint64_t propertyVMAddr = propertyListArrayBaseVMAddr + propertyEntryOffset;
            const property_t* propertyPtr = (const property_t*)(propertyVMAddr + slide);
            ObjCProperty property;
            property.nameVMAddr         = vmAddrConverter.convertToVMAddr(propertyPtr->nameVMAddr);
            property.attributesVMAddr   = vmAddrConverter.convertToVMAddr(propertyPtr->attributesVMAddr);
            handler(propertyVMAddr, property);
        }
    }
}

void MachOAnalyzer::forEachObjCProtocol(uint64_t protocolListVMAddr, const VMAddrConverter& vmAddrConverter,
                                        void (^handler)(uint64_t protocolRefVMAddr, const ObjCProtocol&)) const
{
    if ( protocolListVMAddr == 0 )
        return;

    auto ptrSize = pointerSize();
    intptr_t slide = getSlide();

    ignorePreoptimizedListsOfLists(protocolListVMAddr, slide);

    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        struct protocol_ref_t {
            PtrTy       refVMAddr;
        };
        struct protocol_list_t {
            PtrTy           count;
            protocol_ref_t  array[];
        };
        struct protocol_t {
            PtrTy    isaVMAddr;
            PtrTy    nameVMAddr;
            PtrTy    protocolsVMAddr;
            PtrTy    instanceMethodsVMAddr;
            PtrTy    classMethodsVMAddr;
            PtrTy    optionalInstanceMethodsVMAddr;
            PtrTy    optionalClassMethodsVMAddr;
            PtrTy    instancePropertiesVMAddr;
            uint32_t size;
            uint32_t flags;
            // Fields below this point are not always present on disk.
            PtrTy    extendedMethodTypesVMAddr;
            PtrTy    demangledNameVMAddr;
            PtrTy    classPropertiesVMAddr;
        };

        const protocol_list_t* protoList = (const protocol_list_t*)(protocolListVMAddr + slide);
        for (PtrTy i = 0; i != protoList->count; ++i) {
            uint64_t protocolVMAddr = vmAddrConverter.convertToVMAddr(protoList->array[i].refVMAddr);

            const protocol_t* protocolPtr = (const protocol_t*)(protocolVMAddr + slide);
            ObjCProtocol objCProtocol;
            objCProtocol.isaVMAddr                          = vmAddrConverter.convertToVMAddr(protocolPtr->isaVMAddr);
            objCProtocol.nameVMAddr                         = vmAddrConverter.convertToVMAddr(protocolPtr->nameVMAddr);
            objCProtocol.protocolsVMAddr                    = vmAddrConverter.convertToVMAddr(protocolPtr->protocolsVMAddr);
            objCProtocol.instanceMethodsVMAddr              = vmAddrConverter.convertToVMAddr(protocolPtr->instanceMethodsVMAddr);
            objCProtocol.classMethodsVMAddr                 = vmAddrConverter.convertToVMAddr(protocolPtr->classMethodsVMAddr);
            objCProtocol.optionalInstanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(protocolPtr->optionalInstanceMethodsVMAddr);
            objCProtocol.optionalClassMethodsVMAddr         = vmAddrConverter.convertToVMAddr(protocolPtr->optionalClassMethodsVMAddr);

            handler(protocolVMAddr, objCProtocol);
        }
    } else {
        typedef uint32_t PtrTy;
        struct protocol_ref_t {
            PtrTy       refVMAddr;
        };
        struct protocol_list_t {
            PtrTy           count;
            protocol_ref_t  array[];
        };
        struct protocol_t {
            PtrTy    isaVMAddr;
            PtrTy    nameVMAddr;
            PtrTy    protocolsVMAddr;
            PtrTy    instanceMethodsVMAddr;
            PtrTy    classMethodsVMAddr;
            PtrTy    optionalInstanceMethodsVMAddr;
            PtrTy    optionalClassMethodsVMAddr;
            PtrTy    instancePropertiesVMAddr;
            uint32_t size;
            uint32_t flags;
            // Fields below this point are not always present on disk.
            PtrTy    extendedMethodTypesVMAddr;
            PtrTy    demangledNameVMAddr;
            PtrTy    classPropertiesVMAddr;
        };

        const protocol_list_t* protoList = (const protocol_list_t*)(protocolListVMAddr + slide);
        for (PtrTy i = 0; i != protoList->count; ++i) {
            uint64_t protocolVMAddr = vmAddrConverter.convertToVMAddr(protoList->array[i].refVMAddr);

            const protocol_t* protocolPtr = (const protocol_t*)(protocolVMAddr + slide);
            ObjCProtocol objCProtocol;
            objCProtocol.isaVMAddr                          = vmAddrConverter.convertToVMAddr(protocolPtr->isaVMAddr);
            objCProtocol.nameVMAddr                         = vmAddrConverter.convertToVMAddr(protocolPtr->nameVMAddr);
            objCProtocol.protocolsVMAddr                    = vmAddrConverter.convertToVMAddr(protocolPtr->protocolsVMAddr);
            objCProtocol.instanceMethodsVMAddr              = vmAddrConverter.convertToVMAddr(protocolPtr->instanceMethodsVMAddr);
            objCProtocol.classMethodsVMAddr                 = vmAddrConverter.convertToVMAddr(protocolPtr->classMethodsVMAddr);
            objCProtocol.optionalInstanceMethodsVMAddr      = vmAddrConverter.convertToVMAddr(protocolPtr->optionalInstanceMethodsVMAddr);
            objCProtocol.optionalClassMethodsVMAddr         = vmAddrConverter.convertToVMAddr(protocolPtr->optionalClassMethodsVMAddr);

            handler(protocolVMAddr, objCProtocol);
        }
    }
}


void MachOAnalyzer::forEachObjCSelectorReference(uint64_t selRefsRuntimeOffset, uint64_t selRefsCount, const VMAddrConverter& vmAddrConverter,
                                                 void (^handler)(uint64_t selRefVMAddr, uint64_t selRefTargetVMAddr, bool& stop)) const
{
    uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
    const uint64_t ptrSize = pointerSize();
    const uint8_t* selRefs = (uint8_t*)this + selRefsRuntimeOffset;
    if ( ptrSize == 8 ) {
        typedef uint64_t PtrTy;
        bool stop = false;
        for ( uint64_t i = 0; i != selRefsCount; ++i ) {
            uint64_t selRefVMAddr = baseAddress + selRefsRuntimeOffset + (i * sizeof(PtrTy));
            uint64_t selRefTargetVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(selRefs + (i * sizeof(PtrTy))));
            handler(selRefVMAddr, selRefTargetVMAddr, stop);
            if ( stop )
                break;
        }
    } else {
        typedef uint32_t PtrTy;
        bool stop = false;
        for ( uint64_t i = 0; i != selRefsCount; ++i ) {
            uint64_t selRefVMAddr = baseAddress + selRefsRuntimeOffset + (i * sizeof(PtrTy));
            uint64_t selRefTargetVMAddr = vmAddrConverter.convertToVMAddr(*(PtrTy*)(selRefs + (i * sizeof(PtrTy))));
            handler(selRefVMAddr, selRefTargetVMAddr, stop);
            if ( stop )
                break;
        }
    }
}


void MachOAnalyzer::forEachObjCSelectorReference(Diagnostics& diag, const VMAddrConverter& vmAddrConverter,
                                                 void (^handler)(uint64_t selRefVMAddr, uint64_t selRefTargetVMAddr, bool& stop)) const
{
    uint64_t selRefsRuntimeOffset;
    uint64_t selRefsSize;
    bool foundSection = ((const Header*)this)->findObjCDataSection("__objc_selrefs", selRefsRuntimeOffset, selRefsSize);
    if ( !foundSection )
        return;

    const uint64_t ptrSize = pointerSize();
    if ( (selRefsSize % ptrSize) != 0 ) {
        diag.error("Invalid sel ref section size");
        return;
    }

    forEachObjCSelectorReference(selRefsRuntimeOffset, selRefsSize / ptrSize, vmAddrConverter, handler);
}

void MachOAnalyzer::forEachObjCMethodName(void (^handler)(const char* methodName)) const {
    intptr_t slide = getSlide();
    forEachSection(^(const Header::SegmentInfo& segInfo, const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.sectionName != "__TEXT" )
            return;
        if ( sectInfo.sectionName != "__objc_methname" )
            return;
        if ( segInfo.isProtected() || ( (sectInfo.flags & SECTION_TYPE) != S_CSTRING_LITERALS ) ) {
            stop = true;
            return;
        }

        const char* content       = (const char*)(sectInfo.address + slide);
        uint64_t    sectionSize   = sectInfo.size;

        const char* s   = (const char*)content;
        const char* end = s + sectionSize;
        while ( s < end ) {
            handler(s);
            s += strlen(s) + 1;
        }
    });
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void MachOAnalyzer::forEachObjCDuplicateClassToIgnore(void (^handler)(const char* className)) const {
    const uint32_t pointerSize = this->pointerSize();

    uint64_t sectionSize = 0;
    const void* section = findSectionContent("__DATA", "__objc_dupclass", sectionSize);

    if ( !section )
        return;

    // Ignore sections which are the wrong size
    if ( (sectionSize % pointerSize) != 0 )
        return;

    // Copied from objc-abi.h
    typedef struct _objc_duplicate_class {
        uint32_t version;
        uint32_t flags;
        const char name[64];
    } objc_duplicate_class;

    for (uint64_t offset = 0; offset != sectionSize; offset += pointerSize) {
        uintptr_t pointerValue = *(uintptr_t*)((uint64_t)section + offset);
        const objc_duplicate_class* duplicateClass = (const objc_duplicate_class*)pointerValue;
        handler(duplicateClass->name);
    }
}
#endif

const MachOAnalyzer::ObjCImageInfo* MachOAnalyzer::objcImageInfo() const {
    uintptr_t slide = getSlide();

    __block bool foundInvalidObjCImageInfo = false;
    __block const ObjCImageInfo* imageInfo = nullptr;
    forEachSection(^(const Header::SectionInfo& sectionInfo, bool& stop) {
        if ( !sectionInfo.segmentName.starts_with("__DATA") )
            return;
        if ( sectionInfo.sectionName != "__objc_imageinfo" )
            return;
        if ( sectionInfo.size != 8 ) {
            stop = true;
            return;
        }
        imageInfo = (const ObjCImageInfo*)((uintptr_t)sectionInfo.address + slide);
        if ( (imageInfo->flags & ObjCImageInfo::dyldPreoptimized) != 0 ) {
            foundInvalidObjCImageInfo = true;
            stop = true;
            return;
        }
        stop = true;
    });
    if ( foundInvalidObjCImageInfo )
        return nullptr;
    return imageInfo;
}

void MachOAnalyzer::forEachWeakDef(Diagnostics& diag,
                                   void (^handler)(const char* symbolName, uint64_t imageOffset, bool isFromExportTrie)) const {
    uint64_t baseAddress = ((const Header*)this)->preferredLoadAddress();
    forEachGlobalSymbol(diag, ^(const char *symbolName, uint64_t n_value, uint8_t n_type, uint8_t n_sect, uint16_t n_desc, bool &stop) {
        if ( (n_desc & N_WEAK_DEF) != 0 ) {
            handler(symbolName, n_value - baseAddress, false);
        }
    });
    forEachExportedSymbol(diag, ^(const char *symbolName, uint64_t imageOffset, uint64_t flags, uint64_t other, const char *importName, bool &stop) {
        if ( (flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION ) == 0 )
            return;
        // Skip resolvers and re-exports
        if ( (flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) != 0 )
            return;
        if ( (flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) != 0 )
            return;
        handler(symbolName, imageOffset, true);
    });
}


void MachOAnalyzer::forEachBindTarget(Diagnostics& diag, bool allowLazyBinds,
                                      void (^handler)(const BindTargetInfo& info, bool& stop),
                                      void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const
{
    if ( this->isPreload() )
        return;
    if ( this->hasChainedFixups() )
        this->forEachBindTarget_ChainedFixups(diag, handler);
    else if ( this->hasOpcodeFixups() )
        this->forEachBindTarget_Opcodes(diag, allowLazyBinds, handler, overrideHandler);
#if SUPPORT_CLASSIC_RELOCS
    else
        this->forEachBindTarget_Relocations(diag, handler);
#endif
}

struct WeakBindInfo
{
    uint64_t    segIndex  :  8,
                segOffset : 56;
};

// walk through all binds, unifying weak, lazy, and regular binds
void MachOAnalyzer::forEachBindUnified_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                               void (^handler)(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop),
                                               void (^overrideHandler)(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    {
        __block unsigned         targetIndex = 0;
        __block BindTargetInfo   targetInfo;
        BindDetailedHandler binder =  ^(const char* opcodeName, const LinkEditInfo&, const Header::SegmentInfo segments[],
                                        bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                        uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                        uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                        uint64_t addend, bool targetOrAddendChanged, bool& stop) {
            uint64_t bindVmOffset  = segments[segmentIndex].vmaddr + segmentOffset;
            uint64_t runtimeOffset = bindVmOffset - leInfo.layout.textUnslidVMAddr;
            if ( targetOrAddendChanged ) {
                targetInfo.targetIndex = targetIndex++;
                targetInfo.libOrdinal  = libOrdinal;
                targetInfo.symbolName  = symbolName;
                targetInfo.addend      = addend;
                targetInfo.weakImport  = weakImport;
                targetInfo.lazyBind    = lazyBind && allowLazyBinds;
            }
            handler(runtimeOffset, targetInfo, stop);
        };
        bool stopped = this->forEachBind_OpcodesRegular(diag, leInfo, segmentsInfo, binder);
        if ( stopped )
            return;
        stopped = this->forEachBind_OpcodesLazy(diag, leInfo, segmentsInfo, binder);
        if ( stopped )
            return;
    }

    // Opcode based weak-binds effectively override other binds/rebases.  Process them last
    // To match dyld2, they are allowed to fail to find a target, in which case the normal rebase/bind will
    // not be overridden.
    {
        __block unsigned         weakTargetIndex = 0;
        __block BindTargetInfo   weakTargetInfo;
        BindDetailedHandler weakBinder =  ^(const char* opcodeName, const LinkEditInfo&, const Header::SegmentInfo segments[],
                                            bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                            uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                            uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                            uint64_t addend, bool targetOrAddendChanged, bool& stop) {
            
            uint64_t bindVmOffset  = segmentsInfo[segmentIndex].vmaddr + segmentOffset;
            uint64_t runtimeOffset = bindVmOffset - leInfo.layout.textUnslidVMAddr;
            if ( (weakTargetIndex == 0) || (symbolName != weakTargetInfo.symbolName) || (strcmp(symbolName, weakTargetInfo.symbolName) != 0) || (weakTargetInfo.addend != addend) ) {
                weakTargetInfo.targetIndex = weakTargetIndex++;
                weakTargetInfo.libOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
                weakTargetInfo.symbolName  = symbolName;
                weakTargetInfo.addend      = addend;
                weakTargetInfo.weakImport  = false;
                weakTargetInfo.lazyBind    = false;
            }
            overrideHandler(runtimeOffset, weakTargetInfo, stop);
        };
        auto strongHandler = ^(const char* strongName) { };
        this->forEachBind_OpcodesWeak(diag, leInfo, segmentsInfo, weakBinder, strongHandler);
    }
}

void MachOAnalyzer::forEachBindTarget_Opcodes(Diagnostics& diag, bool allowLazyBinds,
                                              void (^handler)(const BindTargetInfo& info, bool& stop),
                                              void (^overrideHandler)(const BindTargetInfo& info, bool& stop)) const
{
    __block unsigned lastTargetIndex = -1;
    __block unsigned lastWeakBindTargetIndex = -1;
    this->forEachBindUnified_Opcodes(diag, allowLazyBinds, ^(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop) {
        // Regular/lazy binds
        if ( lastTargetIndex != targetInfo.targetIndex) {
            handler(targetInfo, stop);
            lastTargetIndex = targetInfo.targetIndex;
        }
    }, ^(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop) {
        // Weak binds
        if ( lastWeakBindTargetIndex != targetInfo.targetIndex) {
            overrideHandler(targetInfo, stop);
            lastWeakBindTargetIndex = targetInfo.targetIndex;
        }
    });
}

void MachOAnalyzer::forEachBindTarget_ChainedFixups(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const
{
    __block unsigned targetIndex = 0;
    this->forEachChainedFixupTarget(diag, ^(int libOrdinal, const char* symbolName, uint64_t addend, bool weakImport, bool& stop)  {
        BindTargetInfo info;
        info.targetIndex = targetIndex;
        info.libOrdinal  = libOrdinal;
        info.symbolName  = symbolName;
        info.addend      = addend;
        info.weakImport  = weakImport;
        info.lazyBind    = false;
        handler(info, stop);
       ++targetIndex;
    });

    // The C++ spec says main executables can define non-weak functions which override weak-defs in dylibs
    // This happens automatically for anything bound at launch, but the dyld cache is pre-bound so we need
    // to patch any binds that are overridden by this non-weak in the main executable.
    if ( diag.noError() && this->isMainExecutable() && this->hasWeakDefs() ) {
        MachOAnalyzer::forEachTreatAsWeakDef(^(const char* symbolName) {
            BindTargetInfo info;
            info.targetIndex = targetIndex;
            info.libOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
            info.symbolName  = symbolName;
            info.addend      = 0;
            info.weakImport  = false;
            info.lazyBind    = false;
            bool stop = false;
            handler(info, stop);
           ++targetIndex;
        });
    }
}


#if SUPPORT_CLASSIC_RELOCS
// old binary, walk external relocations and indirect symbol table
void MachOAnalyzer::forEachBindTarget_Relocations(Diagnostics& diag, void (^handler)(const BindTargetInfo& info, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    __block unsigned targetIndex = 0;
    this->forEachBind_Relocations(diag, leInfo, segmentsInfo, true,
                                  ^(const char* opcodeName, const LinkEditInfo&, const Header::SegmentInfo segments[],
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                    uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                    uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                    uint64_t addend, bool targetOrAddendChanged, bool& stop) {
        if ( targetOrAddendChanged ) {
            BindTargetInfo info;
            info.targetIndex = targetIndex;
            info.libOrdinal  = libOrdinal;
            info.symbolName  = symbolName;
            info.addend      = addend;
            info.weakImport  = weakImport;
            info.lazyBind    = lazyBind;
            handler(info, stop);
           ++targetIndex;
        }
    });
}
#endif // SUPPORT_CLASSIC_RELOCS


void MachOAnalyzer::forEachBindLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return;

    __block int targetIndex = -1;
    this->forEachBind_Relocations(diag, leInfo, segmentsInfo, false,
                                  ^(const char* opcodeName, const LinkEditInfo&, const Header::SegmentInfo segments[],
                                    bool segIndexSet,  bool libraryOrdinalSet, uint32_t dylibCount, int libOrdinal,
                                    uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset,
                                    uint8_t type, const char* symbolName, bool weakImport, bool lazyBind,
                                    uint64_t addend, bool targetOrAddendChanged, bool& stop) {
        if ( targetOrAddendChanged )
            ++targetIndex;
        uint64_t bindVmOffset  = segments[segmentIndex].vmaddr + segmentOffset;
        uint64_t runtimeOffset = bindVmOffset - leInfo.layout.textUnslidVMAddr;
        handler(runtimeOffset, targetIndex, stop);
    });
}

#if SUPPORT_CLASSIC_RELOCS
bool MachOAnalyzer::forEachBind_Relocations(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[],
                                            bool supportPrivateExternsWorkaround, BindDetailedHandler handler) const
{
    // Firmare binaries won't have a dynSymTab
    if ( leInfo.dynSymTab == nullptr )
        return false;

    const uint64_t                  relocsStartAddress = externalRelocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
    const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->extreloff);
    const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nextrel];
    bool                            is64Bit     = is64() ;
    const uint32_t                  ptrSize     = pointerSize();
    const uint32_t                  dylibCount  = dependentDylibCount();
    const uint8_t                   relocSize   = (is64Bit ? 3 : 2);
    const void*                     symbolTable = getLinkEditContent(leInfo.layout, leInfo.symTab->symoff);
    const struct nlist_64*          symbols64   = (nlist_64*)symbolTable;
    const struct nlist*             symbols32   = (struct nlist*)symbolTable;
    const char*                     stringPool  = (char*)getLinkEditContent(leInfo.layout, leInfo.symTab->stroff);
    uint32_t                        symCount    = leInfo.symTab->nsyms;
    uint32_t                        poolSize    = leInfo.symTab->strsize;
    uint32_t                        lastSymIndx = -1;
    uint64_t                        lastAddend  = 0;
    bool                            stop        = false;
    for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
        bool isBranch = false;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
        if ( isKextBundle() ) {
            // kext's may have other kinds of relocations, eg, branch relocs.  Skip them
            if ( isArch("x86_64") || isArch("x86_64h") ) {
                if ( reloc->r_type == X86_64_RELOC_BRANCH ) {
                    if ( reloc->r_length != 2 ) {
                        diag.error("external relocation has wrong r_length");
                        break;
                    }
                    if ( reloc->r_pcrel != true ) {
                        diag.error("external relocation should be pcrel");
                        break;
                    }
                    isBranch = true;
                }
            }
        }
#endif
        if ( !isBranch ) {
            if ( reloc->r_length != relocSize ) {
                diag.error("external relocation has wrong r_length");
                break;
            }
            if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA == ARM64_RELOC_UNSIGNED
                diag.error("external relocation has wrong r_type");
                break;
            }
        }
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(relocsStartAddress+reloc->r_address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
            uint32_t symbolIndex = reloc->r_symbolnum;
            if ( symbolIndex > symCount ) {
                diag.error("external relocation has out of range r_symbolnum");
                break;
            }
            else {
                uint32_t strOffset  = is64Bit ? symbols64[symbolIndex].n_un.n_strx : symbols32[symbolIndex].n_un.n_strx;
                uint16_t n_desc     = is64Bit ? symbols64[symbolIndex].n_desc : symbols32[symbolIndex].n_desc;
                uint8_t  n_type     = is64Bit ? symbols64[symbolIndex].n_type : symbols32[symbolIndex].n_type;
                uint32_t libOrdinal = libOrdinalFromDesc(n_desc);
                if ( strOffset >= poolSize ) {
                    diag.error("external relocation has r_symbolnum=%d which has out of range n_strx", symbolIndex);
                    break;
                }
                else {
                    const char*     symbolName = stringPool + strOffset;
                    bool            weakImport = (n_desc & N_WEAK_REF);
                    const uint8_t*  content    = (uint8_t*)this + segmentsInfo[segIndex].vmaddr - leInfo.layout.textUnslidVMAddr + segOffset;
                    uint64_t        addend     = (reloc->r_length == 3) ? *((uint64_t*)content) : *((uint32_t*)content);
                    // Handle defined weak def symbols which need to get a special ordinal
                    if ( ((n_type & N_TYPE) == N_SECT) && ((n_type & N_EXT) != 0) && ((n_desc & N_WEAK_DEF) != 0) )
                        libOrdinal = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
                    uint8_t type = isBranch ? BIND_TYPE_TEXT_PCREL32 : BIND_TYPE_POINTER;
                    bool targetOrAddendChanged = (lastSymIndx != symbolIndex) || (lastAddend != addend);
                    handler("external relocation", leInfo, segmentsInfo, true, true, dylibCount, libOrdinal,
                             ptrSize, segIndex, segOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    lastSymIndx = symbolIndex;
                    lastAddend  = addend;
                }
            }
        }
        else {
            diag.error("local relocation has out of range r_address");
            break;
        }
    }
    // then process indirect symbols
    forEachIndirectPointer(diag, supportPrivateExternsWorkaround,
                           ^(uint64_t address, bool bind, int bindLibOrdinal,
                             const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
        if ( !bind )
           return;
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
            handler("indirect symbol", leInfo, segmentsInfo, true, true, dylibCount, bindLibOrdinal,
                     ptrSize, segIndex, segOffset, BIND_TYPE_POINTER, bindSymbolName, bindWeakImport, bindLazy, 0, true, indStop);
        }
        else {
            diag.error("indirect symbol has out of range address");
            indStop = true;
        }
    });

    return false;
}
#endif

void MachOAnalyzer::forEachBindLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, unsigned targetIndex, bool& stop),
                                                void (^overrideHandler)(uint64_t runtimeOffset, unsigned overrideBindTargetIndex, bool& stop)) const
{
    this->forEachBindUnified_Opcodes(diag, false, ^(uint64_t runtimeOffset, const BindTargetInfo& targetInfo, bool& stop) {
        handler(runtimeOffset, targetInfo.targetIndex, stop);
    }, ^(uint64_t runtimeOffset, const BindTargetInfo& weakTargetInfo, bool& stop) {
        overrideHandler(runtimeOffset, weakTargetInfo.targetIndex, stop);
    });
}

bool MachOAnalyzer::forEachBind_OpcodesLazy(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[], BindDetailedHandler handler) const
{
    if ( (leInfo.dyldInfo == nullptr) || (leInfo.dyldInfo->lazy_bind_size == 0) )
        return false;

    uint32_t        lazyDoneCount   = 0;
    uint32_t        lazyBindCount   = 0;
    const uint32_t  ptrSize         = this->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = dependentDylibCount();
    const uint8_t*  p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->lazy_bind_off);
    const uint8_t*  end             = p + leInfo.dyldInfo->lazy_bind_size;
    uint8_t         type            = BIND_TYPE_POINTER;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = 0;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = false;
    int64_t         addend          = 0;
    bool            weakImport = false;
    while (  !stop && diag.noError() && (p < end) ) {
        uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        switch (opcode) {
            case BIND_OPCODE_DONE:
                // this opcode marks the end of each lazy pointer binding
                ++lazyDoneCount;
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
                handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, true, addend, true, stop);
                segmentOffset += ptrSize;
                ++lazyBindCount;
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
    if ( lazyDoneCount > lazyBindCount+7 ) {
        // diag.error("lazy bind opcodes missing binds");
    }
    return stop;
}



bool MachOAnalyzer::forEachBind_OpcodesWeak(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[], BindDetailedHandler handler,  void (^strongHandler)(const char* symbolName)) const
{
   if ( (leInfo.dyldInfo == nullptr) || (leInfo.dyldInfo->weak_bind_size == 0) )
        return false;

    const uint32_t  ptrSize         = this->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = dependentDylibCount();
    const uint8_t*  p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->weak_bind_off);
    const uint8_t*  end             = p + leInfo.dyldInfo->weak_bind_size;
    uint8_t         type            = BIND_TYPE_POINTER;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = BIND_SPECIAL_DYLIB_WEAK_LOOKUP;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = true;
    int64_t         addend          = 0;
    bool            weakImport      = false;
    bool            targetOrAddendChanged   = true;
    bool            done            = false;
    uint64_t        count;
    uint64_t        skip;
    while ( !stop && diag.noError() && (p < end) && !done ) {
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
                diag.error("unexpected dylib ordinal in weak_bind");
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                weakImport = ( (immediate & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0 );
                symbolName = (char*)p;
                while (*p != '\0')
                    ++p;
                ++p;
                if ( immediate & BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION ) {
                    strongHandler(symbolName);
                }
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(diag, p, end);
                targetOrAddendChanged = true;
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
                handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                 targetOrAddendChanged = false;
               break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += immediate*ptrSize + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(diag, p, end);
                skip = read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    segmentOffset += skip + ptrSize;
                    targetOrAddendChanged = false;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("bad bind opcode 0x%02X", *p);
        }
    }
    return stop;
}

bool MachOAnalyzer::forEachBind_OpcodesRegular(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[], BindDetailedHandler handler) const
{
    if ( (leInfo.dyldInfo == nullptr) || (leInfo.dyldInfo->bind_size == 0) )
        return false;

    const uint32_t  ptrSize         = this->pointerSize();
    bool            stop            = false;
    const uint32_t  dylibCount      = dependentDylibCount();
    const uint8_t*  p               = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->bind_off);
    const uint8_t*  end             = p + leInfo.dyldInfo->bind_size;
    uint8_t         type            = 0;
    uint64_t        segmentOffset   = 0;
    uint8_t         segmentIndex    = 0;
    const char*     symbolName      = NULL;
    int             libraryOrdinal  = 0;
    bool            segIndexSet     = false;
    bool            libraryOrdinalSet = false;
    bool            targetOrAddendChanged   = false;
    bool            done            = false;
    int64_t         addend          = 0;
    uint64_t        count;
    uint64_t        skip;
    bool            weakImport = false;
    while ( !stop && diag.noError() && (p < end) && !done ) {
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
                targetOrAddendChanged = true;
                break;
            case BIND_OPCODE_SET_TYPE_IMM:
                type = immediate;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(diag, p, end);
                targetOrAddendChanged = true;
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
                handler("BIND_OPCODE_DO_BIND", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += read_uleb128(diag, p, end) + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                handler("BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                        ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                segmentOffset += immediate*ptrSize + ptrSize;
                targetOrAddendChanged = false;
                break;
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(diag, p, end);
                skip = read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, libraryOrdinalSet, dylibCount, libraryOrdinal,
                            ptrSize, segmentIndex, segmentOffset, type, symbolName, weakImport, false, addend, targetOrAddendChanged, stop);
                    segmentOffset += skip + ptrSize;
                    targetOrAddendChanged = false;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("bad bind opcode 0x%02X", *p);
        }
    }
    return stop;
}

bool MachOAnalyzer::forEachRebaseLocation_Opcodes(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return false;

    return this->forEachRebase_Opcodes(diag, leInfo, segmentsInfo, ^(const char* opcodeName, const LinkEditInfo& rleInfo, const Header::SegmentInfo segments[],
                                        bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop) {
        uint64_t rebaseVmOffset = segments[segmentIndex].vmaddr + segmentOffset;
        uint64_t runtimeOffset  = rebaseVmOffset - leInfo.layout.textUnslidVMAddr;
        handler(runtimeOffset, stop);
    });
}

bool MachOAnalyzer::forEachRebase_Opcodes(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[], RebaseDetailHandler handler) const
{
    const Rebase pointerRebaseKind = is64() ? Rebase::pointer64 : Rebase::pointer32;
    assert(leInfo.dyldInfo != nullptr);

    const uint8_t* const start = getLinkEditContent(leInfo.layout, leInfo.dyldInfo->rebase_off);
    const uint8_t* const end   = start + leInfo.dyldInfo->rebase_size;
    const uint8_t* p           = start;
    const uint32_t ptrSize     = pointerSize();
    Rebase   kind = Rebase::unknown;
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
                // Allow some padding, in case rebases were somehow aligned to 16-bytes in size
                if ( (end - p) > 15 )
                    diag.error("rebase opcodes terminated early at offset %d of %d", (int)(p-start), (int)(end-start));
                stop = true;
                break;
            case REBASE_OPCODE_SET_TYPE_IMM:
                switch ( immediate ) {
                    case REBASE_TYPE_POINTER:
                        kind = pointerRebaseKind;
                        break;
                    case REBASE_TYPE_TEXT_ABSOLUTE32:
                        kind = Rebase::textAbsolute32;
                        break;
                    case REBASE_TYPE_TEXT_PCREL32:
                        kind = Rebase::textPCrel32;
                        break;
                    default:
                        kind = Rebase::unknown;
                        break;
                }
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
                segOffset += immediate*ptrSize;
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
                for (int i=0; i < immediate; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_IMM_TIMES", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += ptrSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
                count = read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += ptrSize;
                    if ( stop )
                        break;
                }
                break;
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
                handler("REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                segOffset += read_uleb128(diag, p, end) + ptrSize;
                break;
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
                count = read_uleb128(diag, p, end);
                if ( diag.hasError() )
                    break;
                skip = read_uleb128(diag, p, end);
                for (uint32_t i=0; i < count; ++i) {
                    handler("REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB", leInfo, segmentsInfo, segIndexSet, ptrSize, segIndex, segOffset, kind, stop);
                    segOffset += skip + ptrSize;
                    if ( stop )
                        break;
                }
                break;
            default:
                diag.error("unknown rebase opcode 0x%02X", opcode);
        }
    }
    return stop;
}

#if SUPPORT_CLASSIC_RELOCS
bool MachOAnalyzer::forEachRebaseLocation_Relocations(Diagnostics& diag, void (^handler)(uint64_t runtimeOffset, bool& stop)) const
{
    LinkEditInfo leInfo;
    getLinkEditPointers(diag, leInfo);
    if ( diag.hasError() )
        return false;

    BLOCK_ACCCESSIBLE_ARRAY(Header::SegmentInfo, segmentsInfo, leInfo.layout.lastSegIndex+1);
    getAllSegmentsInfos(diag, segmentsInfo);
    if ( diag.hasError() )
        return false;

    return this->forEachRebase_Relocations(diag, leInfo, segmentsInfo, ^(const char* opcodeName, const LinkEditInfo& rleInfo, const Header::SegmentInfo segments[],
                                           bool segIndexSet, uint32_t pointerSize, uint8_t segmentIndex, uint64_t segmentOffset, Rebase kind, bool& stop) {
        uint64_t rebaseVmOffset = segments[segmentIndex].vmaddr + segmentOffset;
        uint64_t runtimeOffset  = rebaseVmOffset - leInfo.layout.textUnslidVMAddr;
        handler(runtimeOffset, stop);
    });
}

// relocs are normally sorted, we don't want to use qsort because it may switch to mergesort which uses malloc
void MachOAnalyzer::sortRelocations(Array<relocation_info>& relocs) const
{
    // The kernel linker has malloc, and old-style relocations are extremely common.  So use qsort
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
    ::qsort(&relocs[0], (size_t)relocs.count(), sizeof(relocation_info),
            [](const void* l, const void* r) -> int {
                if ( ((relocation_info*)l)->r_address < ((relocation_info*)r)->r_address )
                    return -1;
                else
                    return 1;
    });
#else
    uint64_t count = relocs.count();
    for (uint64_t i=0; i < count-1; ++i) {
        bool done = true;
        for (uint64_t j=0; j < count-i-1; ++j) {
            if ( relocs[j].r_address > relocs[j+1].r_address ) {
                relocation_info temp = relocs[j];
                relocs[j]   = relocs[j+1];
                relocs[j+1] = temp;
                done = false;
            }
        }
        if ( done )
            break;
    }
#endif
}


bool MachOAnalyzer::forEachRebase_Relocations(Diagnostics& diag, const LinkEditInfo& leInfo, const Header::SegmentInfo segmentsInfo[], RebaseDetailHandler handler) const
{
    // old binary, walk relocations
    const uint64_t                  relocsStartAddress = localRelocBaseAddress(segmentsInfo, leInfo.layout.linkeditSegIndex);
    const relocation_info* const    relocsStart = (relocation_info*)getLinkEditContent(leInfo.layout, leInfo.dynSymTab->locreloff);
    const relocation_info* const    relocsEnd   = &relocsStart[leInfo.dynSymTab->nlocrel];
    const uint8_t                   relocSize   = (is64() ? 3 : 2);
    const uint8_t                   ptrSize     = pointerSize();
    bool                            stop        = false;
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(relocation_info, relocs, 2048);
    for (const relocation_info* reloc=relocsStart; (reloc < relocsEnd) && !stop; ++reloc) {
        if ( reloc->r_length != relocSize ) {
            bool shouldEmitError = true;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
            if ( usesClassicRelocationsInKernelCollection() && (reloc->r_length == 2) && (relocSize == 3) )
                shouldEmitError = false;
#endif
            if ( shouldEmitError ) {
                diag.error("local relocation has wrong r_length");
                break;
            }
        }
        if ( reloc->r_type != 0 ) { // 0 == X86_64_RELOC_UNSIGNED == GENERIC_RELOC_VANILLA ==  ARM64_RELOC_UNSIGNED
            diag.error("local relocation has wrong r_type");
            break;
        }
        relocs.push_back(*reloc);
    }
    if ( !relocs.empty() ) {
        sortRelocations(relocs);
        for (relocation_info reloc : relocs) {
            uint32_t addrOff = reloc.r_address;
            uint32_t segIndex  = 0;
            uint64_t segOffset = 0;
            uint64_t addr = 0;
#if BUILDING_APP_CACHE_UTIL || BUILDING_DYLDINFO
            // xnu for x86_64 has __HIB mapped before __DATA, so offsets appear to be
            // negative
            if ( isStaticExecutable() || isFileSet() ) {
                addr = relocsStartAddress + (int32_t)addrOff;
            } else {
                addr = relocsStartAddress + addrOff;
            }
#else
            addr = relocsStartAddress + addrOff;
#endif
            if ( segIndexAndOffsetForAddress(addr, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
                Rebase kind = (reloc.r_length == 2) ? Rebase::pointer32 : Rebase::pointer64;
                if ( this->cputype == CPU_TYPE_I386 ) {
                    if ( segmentsInfo[segIndex].executable() )
                        kind = Rebase::textAbsolute32;
                }
                handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, kind, stop);
            }
            else {
                diag.error("local relocation has out of range r_address");
                break;
            }
        }
    }
    // then process indirect symbols
    const Rebase pointerRebaseKind = is64() ? Rebase::pointer64 : Rebase::pointer32;
    forEachIndirectPointer(diag, false,
                           ^(uint64_t address, bool bind, int bindLibOrdinal,
                             const char* bindSymbolName, bool bindWeakImport, bool bindLazy, bool selfModifyingStub, bool& indStop) {
        if ( bind )
           return;
        uint32_t segIndex  = 0;
        uint64_t segOffset = 0;
        if ( segIndexAndOffsetForAddress(address, segmentsInfo, leInfo.layout.linkeditSegIndex, segIndex, segOffset) ) {
            handler("local relocation", leInfo, segmentsInfo, true, ptrSize, segIndex, segOffset, pointerRebaseKind, indStop);
        }
        else {
            diag.error("local relocation has out of range r_address");
            indStop = true;
        }
    });

    return stop;
}
#endif // SUPPORT_CLASSIC_RELOCS

bool MachOAnalyzer::getLinkeditLayout(Diagnostics& diag, uint64_t linkeditFileOffset,
                                      const uint8_t* linkeditStartAddr, mach_o::LinkeditLayout& layout) const
{
    // Note, in VM layout all linkedit offsets are adjusted from file offsets.
    // It is essential no-one calls this on an object in file layout. It must be in VM layout

    auto getLinkEditContent = [&](uint32_t fileOffset)
    {
        uint64_t offsetInLinkedit = fileOffset - linkeditFileOffset;
        return linkeditStartAddr + offsetInLinkedit;
    };

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
                layout.symbolTable.buffer           = getLinkEditContent(symTabCmd->symoff);
                layout.symbolTable.bufferSize       = (uint32_t)(symTabCmd->nsyms * nlistEntrySize);
                layout.symbolTable.entryCount       = symTabCmd->nsyms;
                layout.symbolTable.hasLinkedit      = true;

                // Symbol strings
                layout.symbolStrings.fileOffset     = symTabCmd->stroff;
                layout.symbolStrings.buffer         = getLinkEditContent(symTabCmd->stroff);
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
                layout.localRelocs.buffer              = getLinkEditContent(dynSymTabCmd->locreloff);
                layout.localRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.localRelocs.entryIndex          = 0;         // Use buffer instead
                layout.localRelocs.entryCount          = dynSymTabCmd->nlocrel;
                layout.localRelocs.hasLinkedit         = true;

                // Extern relocs
                layout.externRelocs.fileOffset          = dynSymTabCmd->extreloff;
                layout.externRelocs.buffer              = getLinkEditContent(dynSymTabCmd->extreloff);
                layout.externRelocs.bufferSize          = 0;         // Use entryCount instead
                layout.externRelocs.entryIndex          = 0;         // Use buffer instead
                layout.externRelocs.entryCount          = dynSymTabCmd->nextrel;
                layout.externRelocs.hasLinkedit         = true;

                // Indirect symbol table
                layout.indirectSymbolTable.fileOffset   = dynSymTabCmd->indirectsymoff;
                layout.indirectSymbolTable.buffer       = getLinkEditContent(dynSymTabCmd->indirectsymoff);
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
                layout.rebaseOpcodes.buffer             = getLinkEditContent(linkeditCmd->rebase_off);
                layout.rebaseOpcodes.bufferSize         = linkeditCmd->rebase_size;
                layout.rebaseOpcodes.hasLinkedit        = true;

                // Bind
                layout.regularBindOpcodes.fileOffset    = linkeditCmd->bind_off;
                layout.regularBindOpcodes.buffer        = getLinkEditContent(linkeditCmd->bind_off);
                layout.regularBindOpcodes.bufferSize    = linkeditCmd->bind_size;
                layout.regularBindOpcodes.hasLinkedit   = true;

                // Lazy bind
                layout.lazyBindOpcodes.fileOffset       = linkeditCmd->lazy_bind_off;
                layout.lazyBindOpcodes.buffer           = getLinkEditContent(linkeditCmd->lazy_bind_off);
                layout.lazyBindOpcodes.bufferSize       = linkeditCmd->lazy_bind_size;
                layout.lazyBindOpcodes.hasLinkedit      = true;

                // Weak bind
                layout.weakBindOpcodes.fileOffset       = linkeditCmd->weak_bind_off;
                layout.weakBindOpcodes.buffer           = getLinkEditContent(linkeditCmd->weak_bind_off);
                layout.weakBindOpcodes.bufferSize       = linkeditCmd->weak_bind_size;
                layout.weakBindOpcodes.hasLinkedit      = true;

                // Export trie
                layout.exportsTrie.fileOffset           = linkeditCmd->export_off;
                layout.exportsTrie.buffer               = getLinkEditContent(linkeditCmd->export_off);
                layout.exportsTrie.bufferSize           = linkeditCmd->export_size;
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_DYLD_CHAINED_FIXUPS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.chainedFixups.fileOffset         = linkeditCmd->dataoff;
                layout.chainedFixups.buffer             = getLinkEditContent(linkeditCmd->dataoff);
                layout.chainedFixups.bufferSize         = linkeditCmd->datasize;
                layout.chainedFixups.entryCount         = 0; // Not needed here
                layout.chainedFixups.hasLinkedit        = true;
                layout.chainedFixups.cmd                = linkeditCmd;
                break;
            }
            case LC_DYLD_EXPORTS_TRIE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.exportsTrie.fileOffset           = linkeditCmd->dataoff;
                layout.exportsTrie.buffer               = getLinkEditContent(linkeditCmd->dataoff);
                layout.exportsTrie.bufferSize           = linkeditCmd->datasize;
                layout.exportsTrie.entryCount           = 0; // Not needed here
                layout.exportsTrie.hasLinkedit          = true;
                break;
            }
            case LC_SEGMENT_SPLIT_INFO: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.splitSegInfo.fileOffset           = linkeditCmd->dataoff;
                layout.splitSegInfo.buffer               = getLinkEditContent(linkeditCmd->dataoff);
                layout.splitSegInfo.bufferSize           = linkeditCmd->datasize;
                layout.splitSegInfo.entryCount           = 0; // Not needed here
                layout.splitSegInfo.hasLinkedit          = true;
                break;
            }
            case LC_FUNCTION_STARTS: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.functionStarts.fileOffset           = linkeditCmd->dataoff;
                layout.functionStarts.buffer               = getLinkEditContent(linkeditCmd->dataoff);
                layout.functionStarts.bufferSize           = linkeditCmd->datasize;
                layout.functionStarts.entryCount           = 0; // Not needed here
                layout.functionStarts.hasLinkedit          = true;
                break;
            }
            case LC_DATA_IN_CODE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.dataInCode.fileOffset    = linkeditCmd->dataoff;
                layout.dataInCode.buffer        = getLinkEditContent(linkeditCmd->dataoff);
                layout.dataInCode.bufferSize    = linkeditCmd->datasize;
                layout.dataInCode.entryCount    = 0; // Not needed here
                layout.dataInCode.hasLinkedit   = true;
                break;
            }
            case LC_CODE_SIGNATURE: {
                const linkedit_data_command* linkeditCmd = (const linkedit_data_command*)cmd;

                layout.codeSignature.fileOffset    = linkeditCmd->dataoff;
                layout.codeSignature.buffer        = getLinkEditContent(linkeditCmd->dataoff);
                layout.codeSignature.bufferSize    = linkeditCmd->datasize;
                layout.codeSignature.entryCount    = 0; // Not needed here
                layout.codeSignature.hasLinkedit   = true;
                break;
            }
        }
    });

    return true;
}

void MachOAnalyzer::withVMLayout(Diagnostics &diag, void (^callback)(const mach_o::Layout &layout)) const
{
    intptr_t slide = this->getSlide();
    __block uint64_t linkeditFileOffset = 0;
    __block const uint8_t* linkeditStartAddr = nullptr;

    const Header* hdr       = ((const Header*)this);
    uint32_t numSegments    = hdr->segmentCount();
    BLOCK_ACCCESSIBLE_ARRAY(mach_o::SegmentLayout, segmentLayout, numSegments);
    hdr->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
        mach_o::SegmentLayout segment;
        segment.vmAddr      = info.vmaddr;
        segment.vmSize      = info.vmsize;
        segment.fileOffset  = info.fileOffset;
        segment.fileSize    = info.fileSize;
        segment.buffer      = (uint8_t*)(info.vmaddr + slide);
        segment.protections = info.initProt;

        segment.kind        = mach_o::SegmentLayout::Kind::unknown;
        if ( info.segmentName == "__TEXT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::text;
        } else if ( info.segmentName == "__LINKEDIT" ) {
            segment.kind    = mach_o::SegmentLayout::Kind::linkedit;
            linkeditFileOffset = info.fileOffset;
            linkeditStartAddr = segment.buffer;
        }

        segmentLayout[info.segmentIndex] = segment;
    });

    mach_o::LinkeditLayout linkedit;
    if ( !this->getLinkeditLayout(diag, linkeditFileOffset, linkeditStartAddr, linkedit) ) {
        diag.error("Couldn't get dylib layout");
        return;
    }

    mach_o::Layout layout(this, { &segmentLayout[0], &segmentLayout[numSegments] }, linkedit);
    callback(layout);
}

} // dyld3


