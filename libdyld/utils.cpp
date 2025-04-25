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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

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

// mach_o
#include "Architecture.h"
#include "Header.h"
#include "Image.h"
#include "Error.h"
#include "Version32.h"
#include "Fixup.h"
#include "Symbol.h"

using dyld3::MachOFile;
using dyld3::FatFile;
using dyld3::GradedArchs;

using mach_o::Header;
using mach_o::Image;
using mach_o::Error;
using mach_o::Version32;
using mach_o::Version64;
using mach_o::LinkedDylibAttributes;
using mach_o::Fixup;
using mach_o::Symbol;
using mach_o::Platform;

// used by unit tests
__attribute__((visibility("hidden")))
int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchs& launchArchs, const GradedArchs& dylibArchs,  bool isOSBinary,
                                    void (^bestSlice)(const struct mach_header* slice, uint64_t sliceOffset, size_t sliceSize));


///
/// arch-name <--> cpu-type
///

bool macho_cpu_type_for_arch_name(const char* archName, cpu_type_t* type, cpu_subtype_t* subtype)
{
    mach_o::Architecture arch = mach_o::Architecture::byName(archName);
    if ( arch == mach_o::Architecture::invalid )
        return false;
    
    *type = arch.cpuType();
    *subtype = arch.cpuSubtype();
    return true;
}

const char* macho_arch_name_for_cpu_type(cpu_type_t type, cpu_subtype_t subtype)
{
    const char* result = mach_o::Architecture(type, subtype).name();
    if ( strcmp(result, "unknown") == 0 )
        return nullptr;
    // Strip any suffix that further specifies the exact arm64e type (.old, .kernel, etc).
    if ( std::string_view(result).starts_with("arm64e") )
        return "arm64e";
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

static bool launchableOnCurrentPlatform(const Header* mh)
{
#if TARGET_OS_OSX
    // macOS is special and can launch macOS, catalyst, and iOS apps
    return ( mh->builtForPlatform(Platform::macOS) || mh->builtForPlatform(Platform::macCatalyst) || mh->builtForPlatform(Platform::iOS) );
#else
    return mh->builtForPlatform(Platform::current());
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
            if ( const Header* mh = Header::isMachO({(const uint8_t*)sliceStart, (size_t)sliceSize}) ) {
                if ( mh->isMainExecutable() ) {
                    int sliceGrade = launchArchs.grade(mh->arch().cpuType(), mh->arch().cpuSubtype(), isOSBinary);
                    if ( (sliceGrade > bestGrade) && launchableOnCurrentPlatform(mh) ) {
                        sliceOffset = (char*)sliceStart - (char*)mappedFile;
                        sliceLen    = sliceSize;
                        bestGrade   = sliceGrade;
                    }
                }
                else {
                    int sliceGrade = dylibArchs.grade(mh->arch().cpuType(), mh->arch().cpuSubtype(), isOSBinary);
                    if ( (sliceGrade > bestGrade) && mh->loadableIntoProcess(platform, "") ) {
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
    else if ( const Header* mh = Header::isMachO({(const uint8_t*)mappedFile, (size_t)statbuf.st_size}) ) {
        if ( mh->isMainExecutable() && (launchArchs.grade(mh->arch().cpuType(), mh->arch().cpuSubtype(), isOSBinary) != 0) && launchableOnCurrentPlatform(mh) )  {
            // the "best" of a main executable must pass grading and be a launchable
            if ( bestSlice )
                bestSlice((const mach_header*)mh, 0, (size_t)statbuf.st_size);
        }
        else if ( (dylibArchs.grade(mh->arch().cpuType(), mh->arch().cpuSubtype(), isOSBinary) != 0) && mh->loadableIntoProcess(platform, "") ) {
            // the "best" of a dylib/bundle must pass grading and match the platform of the current process
            if ( bestSlice )
                bestSlice((const mach_header*)mh, 0, (size_t)statbuf.st_size);
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

#if __arm64e__
static const void* stripPointer(const void* ptr)
{
#if __has_feature(ptrauth_calls)
    return __builtin_ptrauth_strip(ptr, ptrauth_key_asia);
#else
    return ptr;
#endif
}
#endif

int macho_best_slice_in_fd(int fd, void (^bestSlice)(const struct mach_header* slice, uint64_t sliceFileOffset, size_t sliceSize)__MACHO_NOESCAPE)
{
    bool keysOff = true;
#if __arm64e__
    // Test if PAC is enabled
    const void* p = (const void*)&macho_best_slice;
    if ( stripPointer(p) != p )
       keysOff = false;
#endif
    const Platform     platform    = Platform::current();
    const GradedArchs* launchArchs = &GradedArchs::launchCurrentOS();
    const GradedArchs* dylibArchs  = &GradedArchs::forCurrentOS(keysOff, false);
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
///
const char* _Nullable macho_dylib_install_name(const struct mach_header* _Nonnull mh) DYLD_EXCLAVEKIT_UNAVAILABLE
{
    const Header* hdr = (const Header*)mh;
    if ( hdr->hasMachOMagic() )
        return hdr->installName();

    return nullptr;
}

static void iterateDependencies(const Image& image, void (^_Nonnull callback)(const char* _Nonnull loadPath, const char* _Nonnull attributes, bool* _Nonnull stop) )
{
    image.header()->forEachLinkedDylib(^(const char* loadPath, LinkedDylibAttributes kind, Version32 compatVersion, Version32 curVersion, 
                                         bool synthesizedLink, bool& stop) {
        char attrBuf[64];
        kind.toString(attrBuf);
        callback(loadPath, attrBuf, &stop);
    });
}

int macho_for_each_dependent_dylib(const struct mach_header* _Nonnull mh, size_t mappedSize,
                                   void (^ _Nonnull callback)(const char* _Nonnull loadPath, const char* _Nonnull attributes, bool* _Nonnull stop))
{
    if ( mappedSize == 0 ) {
        // Image loaded by dyld
        Image image(mh);
        iterateDependencies(image, callback);
    }
    else {
        // raw mach-o file/slice in memory
        Image image(mh, mappedSize, Image::MappingKind::wholeSliceMapped);
        if ( !image.header()->hasMachOMagic() )
            return EFTYPE;
        if ( Error err = image.validate() )
            return EBADMACHO;
        iterateDependencies(image, callback);
    }
    return 0;
}

static void iterateImportedSymbols(const Image& image, void (^_Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull libraryPath, bool weakImport, bool* _Nonnull stop) )
{
    if ( image.hasChainedFixups() ) {
        image.chainedFixups().forEachBindTarget(^(const Fixup::BindTarget& bindTarget, bool& stop) {
            callback(bindTarget.symbolName.c_str(), image.header()->libOrdinalName(bindTarget.libOrdinal).c_str(), bindTarget.weakImport, &stop);
        });
    }
    else {
        // old opcode based fixups
        if ( image.hasBindOpcodes() ) {
            image.bindOpcodes().forEachBindTarget(^(const Fixup::BindTarget& bindTarget, bool& stop) {
                callback(bindTarget.symbolName.c_str(), image.header()->libOrdinalName(bindTarget.libOrdinal).c_str(), bindTarget.weakImport, &stop);
            }, ^(const char* symbolName) {
            });
        }
        if ( image.hasLazyBindOpcodes() ) {
            image.lazyBindOpcodes().forEachBindTarget(^(const Fixup::BindTarget& bindTarget, bool& stop) {
                callback(bindTarget.symbolName.c_str(), image.header()->libOrdinalName(bindTarget.libOrdinal).c_str(), bindTarget.weakImport, &stop);
            }, ^(const char *symbolName) {
            });
        }
    }
}

int macho_for_each_imported_symbol(const struct mach_header* _Nonnull mh, size_t mappedSize,
                                   void (^ _Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull libraryPath, bool weakImport, bool* _Nonnull stop))
{
    if ( mappedSize == 0 ) {
        // Image loaded by dyld, but sanity check
        if ( !((Header*)mh)->hasMachOMagic() )
            return EFTYPE;
        Image image(mh);
        iterateImportedSymbols(image, callback);
    }
    else {
        // raw mach-o file/slice in memory
        Image image(mh, mappedSize, Image::MappingKind::wholeSliceMapped);
        if ( !image.header()->hasMachOMagic() )
            return EFTYPE;
        if ( Error err = image.validate() )
            return EBADMACHO;
        iterateImportedSymbols(image, callback);
    }
    return 0;
}

static const char* exportSymbolAttrString(const Symbol& symbol)
{
    uint64_t other;
    if ( symbol.isWeakDef() )
        return "weak-def";
    else if ( symbol.isThreadLocal() )
        return "thread-local";
    else if ( symbol.isDynamicResolver(other) )
        return "dynamic-resolver";
    else if ( symbol.isAbsolute(other) )
        return "absolute";
    return "";
}

static void iterateExportedSymbols(const Image& image, void (^_Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull attributes, bool* _Nonnull stop) )
{
    if ( image.hasExportsTrie() ) {
        image.exportsTrie().forEachExportedSymbol(^(const Symbol& symbol, bool& stop) {
            callback(symbol.name().c_str(), exportSymbolAttrString(symbol), &stop);
        });
    }
    else if ( image.hasSymbolTable() ) {
        image.symbolTable().forEachExportedSymbol(^(const Symbol& symbol, uint32_t symbolIndex, bool& stop) {
            callback(symbol.name().c_str(), exportSymbolAttrString(symbol), &stop);
        });
    }
}

int macho_for_each_exported_symbol(const struct mach_header* _Nonnull mh, size_t mappedSize,
                                   void (^ _Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull attributes, bool* _Nonnull stop))
{
    if ( mappedSize == 0 ) {
        // Image loaded by dyld, but sanity check
        if ( !((Header*)mh)->hasMachOMagic() )
            return EFTYPE;
        Image image(mh);
        iterateExportedSymbols(image, callback);
    }
    else {
        // raw mach-o file/slice in memory
        Image image(mh, mappedSize, Image::MappingKind::wholeSliceMapped);
        if ( !image.header()->hasMachOMagic() )
            return EFTYPE;
        if ( Error err = image.validate() )
            return EBADMACHO;
        iterateExportedSymbols(image, callback);
    }
    return 0;
}


int macho_for_each_defined_rpath(const struct mach_header* _Nonnull mh, size_t mappedSize,
                                 void (^ _Nonnull callback)(const char* _Nonnull rpath, bool* _Nonnull stop))
{
    if ( mappedSize == 0 ) {
        // Image loaded by dyld
        Image image(mh);
        image.header()->forEachRPath(^(const char* _Nonnull rpath, bool& stop) {
            callback(rpath, &stop);
        });
    }
    else {
        // raw mach-o file/slice in memory
        Image image(mh, mappedSize, Image::MappingKind::wholeSliceMapped);
        if ( !image.header()->hasMachOMagic() )
            return EFTYPE;
        if ( Error err = image.validate() )
            return EBADMACHO;
        image.header()->forEachRPath(^(const char* _Nonnull rpath, bool& stop) {
            callback(rpath, &stop);
        });
    }
    return 0;
}

bool macho_source_version(const struct mach_header* _Nonnull mh, uint64_t* _Nonnull version)
{
    Header* header = (Header*)mh;
    if ( !header->hasMachOMagic() )
        return false;
    
    Version64 v;
    if ( !header->sourceVersion(v) )
        return false;
    
    *version = v.value();
    return true;
}

#endif // !TARGET_OS_EXCLAVEKIT
