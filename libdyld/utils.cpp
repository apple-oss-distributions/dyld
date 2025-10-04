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
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mach-o/utils.h>
#include <mach-o/utils_priv.h>
#include <mach-o/dyld_priv.h>
#include <TargetConditionals.h>

#include "FileUtils.h"
#include "DyldDelegates.h"

// mach_o
#include "Architecture.h"
#include "Error.h"
#include "Fixup.h"
#include "GradedArchitectures.h"
#include "Header.h"
#include "Image.h"
#include "Symbol.h"
#include "Universal.h"
#include "Version32.h"


using mach_o::Architecture;
using mach_o::Error;
using mach_o::Fixup;
using mach_o::GradedArchitectures;
using mach_o::Header;
using mach_o::Image;
using mach_o::LinkedDylibAttributes;
using mach_o::Platform;
using mach_o::Symbol;
using mach_o::Universal;
using mach_o::Version32;
using mach_o::Version64;
using mach_o::LinkedDylibAttributes;
using mach_o::Fixup;
using mach_o::Symbol;
using mach_o::Platform;
using mach_o::Architecture;

// used by unit tests
__attribute__((visibility("hidden")))
int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchitectures& launchArchs, const GradedArchitectures& dylibArchs,  bool isOSBinary,
                                    void (^bestSlice)(const struct mach_header* slice, uint64_t sliceOffset, size_t sliceSize));


///
/// arch-name <--> cpu-type
///

bool macho_cpu_type_for_arch_name(const char* archName, cpu_type_t* type, cpu_subtype_t* subtype)
{
    Architecture arch = Architecture::byName(archName);
    if ( arch == Architecture::invalid )
        return false;
    
    *type = arch.cpuType();
    *subtype = arch.cpuSubtype();
    
    return true;
}

const char* macho_arch_name_for_cpu_type(cpu_type_t type, cpu_subtype_t subtype)
{
    const char* result = Architecture(type, subtype).name();
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

    // when running as "root", calling open() on a file with no read-permissions actually succeeds, but we need it to fail
    if ( ((statbuf.st_mode & (S_IRUSR|S_IROTH)) == 0) && (geteuid() == 0) )
        return EACCES;

    const void* mappedFile = ::mmap(nullptr, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( mappedFile == MAP_FAILED )
        return errno;

    int result = 0;
    std::span<const uint8_t> content = {(uint8_t*)mappedFile, (size_t)statbuf.st_size};
    if ( const Universal* uni = Universal::isUniversal(content) ) {
        Error error = uni->valid(content.size());
        if ( error.hasError() )
            return EBADMACHO;
        
        uni->forEachSlice(^(Universal::Slice slice, bool &stop) {
            if ( callback ) {
                uint64_t fileOffset = slice.buffer.data() - content.data();
                callback((const mach_header*)slice.buffer.data(), fileOffset, slice.buffer.size(), &stop);
            }
        });
    }
    else if ( const Header* hdr = Header::isMachO(content) ) {
        bool stop;
        if ( callback )
            callback((const mach_header*)hdr, 0, content.size(), &stop);
    }
    else {
        // not a universal file nor a mach-o file
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

static bool launchableOnCurrentPlatform(const Header* hdr)
{
#if TARGET_OS_OSX
    // macOS is special and can launch macOS, catalyst, and iOS apps
    return ( hdr->builtForPlatform(Platform::macOS) || hdr->builtForPlatform(Platform::macCatalyst) || hdr->builtForPlatform(Platform::iOS) );
#else
    return hdr->builtForPlatform(Platform::current());
#endif
}

// use directly by unit tests
int macho_best_slice_fd_internal(int fd, Platform platform, const GradedArchitectures& launchArchs, const GradedArchitectures& dylibArchs, bool isOSBinary,
                                    void (^ _Nullable bestSlice)(const struct mach_header* slice, uint64_t fileOffset, size_t sliceSize))
{
    struct stat statbuf;
    if ( ::fstat(fd, &statbuf) == -1 )
        return errno;

    // FIXME: possible that mmap() of the whole file will fail on memory constrainted devices (e.g. watch), so may need to read() instead
    const void* mappedFile = ::mmap(nullptr, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( mappedFile == MAP_FAILED )
        return errno;
 
    int result = 0;
    std::span<const uint8_t> content = {(uint8_t*)mappedFile, (size_t)statbuf.st_size};
    if ( const Universal* uni = Universal::isUniversal(content) ) {
        Error error = uni->valid(content.size());
        if ( error.hasError() )
            return EBADMACHO;
        
        Universal::Slice launchSlice, dylibSlice;
        uni->bestSlice(launchArchs, isOSBinary, launchSlice);
        uni->bestSlice(dylibArchs, isOSBinary, dylibSlice);
        const Header* exe = Header::isMachO(launchSlice.buffer);
        const Header* dylib = Header::isMachO(dylibSlice.buffer);
        if ( exe && exe->isMainExecutable() ) {
            if ( bestSlice ) {
                uint64_t fileOffset = launchSlice.buffer.data() - content.data();
                bestSlice((const mach_header*)launchSlice.buffer.data(), fileOffset, launchSlice.buffer.size());
            }
        }
        else if ( dylib && !dylib->isMainExecutable() ) {
            if ( bestSlice ) {
                uint64_t fileOffset = dylibSlice.buffer.data() - content.data();
                bestSlice((const mach_header*)dylibSlice.buffer.data(), fileOffset, dylibSlice.buffer.size());
            }
        }
        else
            result = EBADARCH;
    }
    else if ( const Header* hdr = Header::isMachO(content) ) {
        if (    hdr->isMainExecutable()
            &&  (launchArchs.isCompatible(hdr->arch(), isOSBinary) != 0) && launchableOnCurrentPlatform(hdr) )  {
            // the "best" of a main executable must pass grading and be a launchable
            if ( bestSlice )
                bestSlice((const mach_header*)hdr, 0, content.size());
        }
        else if ( (dylibArchs.isCompatible(hdr->arch(), isOSBinary) != 0) && hdr->loadableIntoProcess(platform, "") ) {
            // the "best" of a dylib/bundle must pass grading and match the platform of the current process
            if ( bestSlice )
                bestSlice((const mach_header*)hdr, 0, content.size());
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
    const Platform              platform    = Platform::current();
    const GradedArchitectures*  launchArchs = &GradedArchitectures::currentLaunch();
    const GradedArchitectures&  dylibArchs  = GradedArchitectures::currentLoad(keysOff, false);
#if TARGET_OS_SIMULATOR
    const char* simArchNames = getenv("SIMULATOR_ARCHS");
    if ( simArchNames == nullptr )
        simArchNames = "x86_64";
    launchArchs = &GradedArchitectures::currentLaunch(simArchNames);
#endif
    return macho_best_slice_fd_internal(fd, platform, *launchArchs, dylibArchs, false, bestSlice);
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

#if !TARGET_OS_SIMULATOR
static bool hasFile(const char* path)
{
    struct stat statBuf;
    if ( stat(path, &statBuf) == 0 )
        return true;
    char devPath[PATH_MAX];
    strlcpy(devPath, path, sizeof(devPath));
    strlcat(devPath, ".development", sizeof(devPath));
    return ( stat(devPath, &statBuf) == 0 );
}
#endif


void macho_for_each_runnable_arch_name(void (^ _Nonnull callback)(const char* _Nonnull archName, bool* _Nonnull stop))
{
    bool stop = false;
#if TARGET_OS_SIMULATOR
    // FIXME:  Should $SIMULATOR_ARCHS be used? Or is this API only about what the current simulator instance can run?
    // fallthrough to arch specific handling
#elif TARGET_OS_OSX
    if ( hasFile("/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_arm64e") ) {
        // Apple Silicon mac
        callback("arm64e", &stop);
        if ( stop )
            return;
        callback("arm64", &stop);
        if ( stop )
            return;
        if ( hasFile("/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64") ) {
            // has Rosetta support
            callback("x86_64", &stop);
        }
        return;
    }
    else if ( hasFile("/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h") ) {
        // Intel mac
        callback("x86_64h", &stop);
        if ( stop )
            return;
        callback("x86_64", &stop);
        return;
    }
    else if ( hasFile("/System/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64") ) {
        // Old Intel mac
        callback("x86_64", &stop);
        return;
    }
#elif TARGET_OS_IOS
    if ( hasFile("/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e") ) {
        // iPhone or iPad
        callback("arm64e", &stop);
        if ( stop )
            return;
        callback("arm64", &stop);
        return;
    }
    else if ( hasFile("/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64") ) {
        // old iPhone or ipad
        callback("arm64", &stop);
        return;
    }
#elif TARGET_OS_WATCH
    // gather grading for architectures
    struct NameAndGrade { const char* name; int grade=0; };
    NameAndGrade gn[3] = { { "arm64e"}, { "arm64"}, { "arm64_32"} };
    if ( hasFile("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e") ) {
        // M11 or later watch that supports 64-bit userland
        gn[0].grade = Architecture::arm64e.kernelGrade();
        gn[1].grade = Architecture::arm64.kernelGrade();
    }
    if ( hasFile("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64_32") ) {
        gn[2].grade = Architecture::arm64_32.kernelGrade();
    }
    // sort three entries by grade (unrolled bubble sort)
    if ( gn[0].grade < gn[1].grade )
        std::swap(gn[0], gn[1]);
    if ( gn[1].grade < gn[2].grade )
        std::swap(gn[1], gn[2]);
    if ( gn[0].grade < gn[1].grade )
        std::swap(gn[0], gn[1]);
    // return them in grading order
    int usableCount = 0;
    for ( int i=0; i < 3; ++i ) {
        // skip NameAndGrade elements with no grade value
        if ( gn[i].grade != 0 ) {
            ++usableCount;
            callback(gn[i].name, &stop);
            if ( stop )
                return;
        }
    }
    if ( usableCount > 0 )
        return;
    // otherwise fall into no-dyld-cache case below
#else
    if ( hasFile("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e") ) {
        // Apple TV or other device with arm64e support
        callback("arm64e", &stop);
        if ( stop )
            return;
        callback("arm64", &stop);
        return;
    }
    else if ( hasFile("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64") ) {
        callback("arm64", &stop);
        return;
    }
#endif
    if ( stop )
        return;

    // no dyld cache, must be RAMDisk
#if __arm64e__
    callback("arm64e", &stop);
    if ( stop )
        return;
    callback("arm64", &stop);
#elif __arm64__
  #if __LP64__
    callback("arm64", &stop);
  #else
    callback("arm64_32", &stop);
  #endif
#elif __x86_64__
    callback("x86_64", &stop);
#endif
}




#endif // !TARGET_OS_EXCLAVEKIT
