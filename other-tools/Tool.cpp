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


#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <crt_externs.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/dyld_introspection.h>

// common
#include "DyldSharedCache.h"

// mach_o
#include "UnsafeHeader.h"
#include "Error.h"
#include "Architecture.h"
#include "Universal.h"
#include "Archive.h"

// other_tools
#include "Tool.h"
#include "StringUtils.h"
#include "MiscFileUtils.h"


using mach_o::Universal;
using mach_o::UnsafeHeader;
using mach_o::Architecture;
using mach_o::Error;
using mach_o::Archive;
using mach_o::PlatformAndVersions;

namespace other_tools {

#if BUILDING_UNIT_TESTS
Error Tool::processTestArgs(const std::vector<const char*>& args, const std::vector<const char*>& envVars)
{
    processProgName(args[0]);
    processEnvVars(&envVars[0]);
    return processArgs((int)args.size(), &args[0]);
}
#endif

std::vector<CString> Tool::expandResponseFile(const char* responseFile)
{
    std::vector<CString> result;

    int fd = ::open(responseFile, O_RDONLY);
    struct stat stat;
    if ( fd == -1 || ::fstat(fd, &stat) != 0 ) {
        // ld64 doesn't consider missing response files to be an error.
        // For now lets emit a warning, ld64 doesn't even do that, but lets see if we can
        fprintf(stderr, "%s: warning: response file '%s' could not be opened, errno=%d (%s)", __progname, responseFile, errno, strerror(errno));
        return result;
    }
    if ( stat.st_size == 0 )
        return result;

    std::vector<char> buffer;
    buffer.resize(stat.st_size + 1); // one extra byte to guarantee null termination
    if ( ::read(fd, buffer.data(), stat.st_size) != stat.st_size ) {
        fprintf(stderr, "%s: warning: response file '%s' could not be read, errno=%d (%s)", __progname,  responseFile, errno, strerror(errno));
        return result;
    }
    ::close(fd);

    char* p = buffer.data();
    for (char* arg = get_next_response_option(&p); arg; arg = get_next_response_option(&p))
        result.push_back(CString::dup(arg));

    return result;
}

Error Tool::processArgs(int argc, const char* const argv[])
{
    // convert to std::span<CString> and expand any response files
    std::vector<CString> args;
    args.reserve(argc+1);
    for (int i=0; i < argc; ++i) {
        const char* arg = argv[i];
        if ( arg[0] == '@' ){
            std::vector<CString> newArgs = expandResponseFile(&arg[1]);
            args.insert(args.end(), newArgs.begin(), newArgs.end());
        }
        else {
            args.push_back(arg);
        }
    }

    // process each command line option
    return processArgs(args);
}

Error Tool::processArgs(std::span<CString> args)
{
    std::span<CString> remainingArgs = args.subspan(1); // skip over argv[0] which is prog name
    while ( !remainingArgs.empty() ) {
        if ( remainingArgs.front().starts_with("-") ) {
            if (Error err = handleOption(remainingArgs) )
                return err;
        }
        else {
            handleFile(remainingArgs.front());
        }
        remainingArgs = remainingArgs.subspan(1); // move on to next arg
    }
    return Error::none();
}

CString Tool::versionNumberStr() const
{
    constexpr CString vers = OS_STRINGIFY(LD_VERSION);
    if constexpr ( !vers.empty() )
        return vers;
    return "9000.90.90";
}

int Tool::run(int argc, const char* argv[], const char* const envp[])
{
    this->processProgName(argv[0]);
    this->processEnvVars(envp);
    Error err = this->processArgs(argc, argv);

    // log usage on invalid arguments
    if ( err )
        usage();

    if ( err.noError() )
        err = this->doRun();

    if ( err.hasError() ) {
        this->printErrorMessage(err);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void Tool::printErrorMessage(const Error& err) const
{
    fprintf(stderr, "%s: %s\n", __progname, err.message());
}

Error Tool::printSlice(CString path, std::span<const uint8_t> buffer, bool printPath) const
{
    if ( const UnsafeHeader* hdr = UnsafeHeader::isMachO(buffer) ) {
        return printMachO(path, hdr, buffer.size(), printPath);
    }
    else if ( UnsafeHeader::isBitCodeHeader(buffer) ) {
        return Error("bitcode file '%s' not supported in %s, use llvm-nm", path.c_str(), __progname);
    }
    else {
        return Error("invalid mach-o '%s'", path.c_str());
    }
    return Error::none();
}

Error Tool::printSpecificSlice(CString path, bool printPath, std::span<const Universal::Slice> slices, Architecture arch, bool& found) const
{
    found = false;
    for (const Universal::Slice& slice : slices) {
        if ( slice.arch == arch ) {
            found = true;
            if ( Error err = printSlice(path, slice.buffer, printPath) )
                return err;
            break;
        }
    }
    return Error::none();
}

Error Tool::printHostSpecificSlice(CString path,  bool printPath, std::span<const Universal::Slice> slices, bool& found) const
{
    Error sliceErr;
    found = false;
#if __arm64__
  #if __LP64__
    // try arm64e first
    sliceErr = printSpecificSlice(path, printPath, slices, Architecture::arm64e, found);
    // if no arm64e, try arm64
    if ( !found )
        sliceErr = printSpecificSlice(path, printPath, slices, Architecture::arm64, found);
  #else
    // try arm64_32 first
    sliceErr = printSpecificSlice(path, printPath, slices, Architecture::arm64_32, found);
  #endif
#elif __x86_64__
    // try x86_64h first
    sliceErr = printSpecificSlice(path, printPath, slices, Architecture::x86_64h, found);
    // if no x86_64h, try x86_64
    if ( !found )
        sliceErr = printSpecificSlice(path, printPath, slices, Architecture::x86_64, found);
#endif
    return sliceErr;
}

Tool::ErrorMap Tool::printSlices(std::span<CString> filePaths, std::span<CString> cmdLineArchs) const
{
    // make map to hold any errors
    __block ErrorMap result;

    // iterate over all files and slices
    this->getMachOsFromPaths(filePaths, true /*decendIntoStaticLibs*/, true /*searchDyldCache*/, ^(std::span<const Input> inputs) {
        if ( (inputs.size() == 1) && inputs[0].err.hasError() ) {
            result[inputs[0].path] = Error("%s", inputs[0].err.message());
            return;
        }
        // tool shows only one slice by default
        // you can use '-arch all' to show all archs
        // or use '-arch foo' to show just arch foo
        if ( cmdLineArchs.empty() ) {
            if ( inputs.size() == 1 ) {
                // only one arch in file and no -arch on command line, so print this one
                if (Error err = printSlice(inputs[0].path, inputs[0].slice.buffer, false) ) {
                    result[inputs[0].path] = std::move(err);
                    return;
                }
            }
            else {
                // many slices and no -arch on command line
                for (const Input& input : inputs) {
                    if ( input.err.hasError() ) {
                        result[input.path] = Error("%s", input.err.message());
                    }
                    else if (Error err = printSlice(input.path, input.slice.buffer, true)) {
                        result[input.path] = std::move(err);
                        return;
                    }
                }
            }
        }
        else {
            // -arch specified, print out slices that match
            bool foundSlice = false;
            for (const Input& input : inputs) {
                bool useSlice = false;
                for ( CString cmdLineArch : cmdLineArchs ) {
                    if ( (cmdLineArch == input.slice.arch.name()) || ( cmdLineArch == "all") ) {
                        useSlice   = true;
                        foundSlice = true;
                        break;
                    }
                }
                if ( useSlice ) {
                    if (Error err = printSlice(input.path, input.slice.buffer, true) ) {
                        result[input.path] = std::move(err);
                        return;
                    }
                }
            }
            //if ( !foundSlice ) {
            //    result[input.path] = Error("'%s' does not contain specified arch(s)", input.path.c_str());
            //}
        }
    });
    return std::move(result);
}

void Tool::appendInputs(CString path, std::string_view memberName, const struct stat& statBuf, std::span<const uint8_t> fileBuffer,
                        bool decendIntoStaticLibs, std::vector<Input>& inputs, std::vector<const char*>& stringsToFree,
                        Archive::MisalignHandling misalignedMember)
{
    if ( const Universal* uni = Universal::isUniversal(fileBuffer) ) {
        // if fat file, recurse on each slice
        uni->forEachSlice(^(Universal::Slice slice, bool& stop) {
            appendInputs(path, memberName, statBuf, slice.buffer, decendIntoStaticLibs, inputs, stringsToFree, misalignedMember);
        });
    }
    else if ( const UnsafeHeader* mh = UnsafeHeader::isMachO(fileBuffer) ) {
        // if thin mach-o file, add the one slice
        Universal::Slice sliceInfo{ mh->arch(), fileBuffer, 0, Universal::defaultAlignment(fileBuffer) };
        Archive::Member  memberInfo;
        memberInfo.contents = fileBuffer;
        memberInfo.setFileInfo(statBuf);
        memberInfo.name = memberName.empty() ? (std::string_view)path.leafName() : memberName;
        inputs.push_back({path, Error::none(), sliceInfo, memberInfo});
    }
    else if ( UnsafeHeader::isBitCodeHeader(fileBuffer) ) {
        // if bitcode file, add the one slice if libLTO.dylib is available
        Universal::Slice sliceInfo;
        sliceInfo.buffer    = fileBuffer;
        sliceInfo.alignment = Universal::defaultAlignment(fileBuffer);
        Archive::Member  memberInfo;
        memberInfo.contents = fileBuffer;
        memberInfo.setFileInfo(statBuf);
        memberInfo.name = memberName.empty() ? (std::string_view)path.leafName() : memberName;
#if HAVE_LIBLTO
        Architecture tripleArch;
        if ( getBitCodeFileArch(path, fileBuffer, tripleArch) ) {
            sliceInfo.arch = tripleArch;
        }
        else {
            sliceInfo.buffer    = std::span<const uint8_t>();
            memberInfo.contents = std::span<const uint8_t>();
        }
#endif
        inputs.push_back({path, Error::none(), sliceInfo, memberInfo});
    }
    else if ( std::optional<Archive> ar = Archive::isArchive(fileBuffer) ) {
        if ( decendIntoStaticLibs ) {
            // if static library, recurse on each member
            Error err = ar->forEachMachO(^(const Archive::Member& memberInfo, unsigned int memberIndex, const mach_o::UnsafeHeader*, bool& stop) {
                // synthesize path into archive
                char* memberPath;
                asprintf(&memberPath, "%s(%.*s)", path.c_str(), (int)memberInfo.name.size(), memberInfo.name.data());
                stringsToFree.push_back(memberPath);
                // synthesize statBuf
                struct stat      memberAsStat;
                memberAsStat.st_uid   = memberInfo.uid;
                memberAsStat.st_gid   = memberInfo.gid;
                memberAsStat.st_mtime = memberInfo.mtime;
                memberAsStat.st_mode  = memberInfo.perms;
                appendInputs(memberPath, memberInfo.name, memberAsStat, memberInfo.contents, decendIntoStaticLibs, inputs, stringsToFree, misalignedMember);
            }, misalignedMember);
            if ( err.hasError() ) {
                std::span<const uint8_t> emptyBuffer = std::span<const uint8_t>();
                Universal::Slice emptySlice{ Architecture(), emptyBuffer, 0, 0 };
                Archive::Member memberInfo;
                memberInfo.name     = path.leafName();
                memberInfo.contents = emptyBuffer;
                inputs.push_back({path, std::move(err), emptySlice, memberInfo});
            }
        }
        else {
            // if static library, pass whole buffer to client, but arch from first file
            __block Architecture firstArch;
            __block bool         foundBitCode = false;
            __block bool         anyMacho = false;
            Error err = ar->forEachMachO(^(const Archive::Member& memberInfo, unsigned int memberIndex, const mach_o::UnsafeHeader* hdr, bool& stop) {
                anyMacho = true;
                if ( hdr != nullptr ) {
                    // if mach-o, get arch from mach_header
                    firstArch = hdr->arch();
                    stop = true;
                }
                else if ( UnsafeHeader::isBitCodeHeader(memberInfo.contents) ) {
                    foundBitCode = true;
                    // if bitcode file, use libLTO.dylib to figure out arch
#if HAVE_LIBLTO
                    Architecture tripleArch;
                    if ( getBitCodeFileArch(path, memberInfo.contents, tripleArch) ) {
                        firstArch = tripleArch;
                        stop = true;
                    }
#endif
                }
            });
            Universal::Slice sliceInfo{ firstArch, fileBuffer, 0, 0 };
            Archive::Member  memberInfo;
            memberInfo.name     = path.leafName();
            memberInfo.contents = fileBuffer;
            memberInfo.setFileInfo(statBuf);
            if ( firstArch.empty() ) {
                inputs.push_back({path, Error(anyMacho ? "can't determine architecture for %s" : "empty archive %s", path.c_str()),
                    sliceInfo, memberInfo});
            } else {
                inputs.push_back({path, Error::none(), sliceInfo, memberInfo});
            }
        }
    }
    else {
        // unknown file type
        Universal::Slice emptySlice{ Architecture(), fileBuffer, 0, 0 };
        Archive::Member memberInfo;
        memberInfo.name     = path.leafName();
        memberInfo.contents = fileBuffer;
        memberInfo.setFileInfo(statBuf);
        inputs.push_back({path, Error("not a mach-o '%s'", path.c_str()), emptySlice, memberInfo});
    }
}

void Tool::getMachOsFromPaths(std::span<const CString> paths, bool decendIntoStaticLibs, bool searchDyldCache,
                              void (^callback)(std::span<const Input> inputs), Archive::MisalignHandling misalignedMember)
{
    std::vector<std::span<const uint8_t>>   buffersToUnmap;
    std::vector<const char*>                stringsToFree; // for use with temp strings like "/blah/libfoo.a(foo.o)"
    std::vector<Input>                      inputs;
    __block const DyldSharedCache*          dyldCache = nullptr;
    __block const DyldSharedCache*          dyldCacheDK = nullptr;
    for (CString path : paths) {
        struct stat                 statBuf;
        std::span<const uint8_t>    fileBuffer;
        Error mapErr = mapFileReadOnly(path.c_str(), fileBuffer, &statBuf);
        if ( mapErr.noError() ) {
            buffersToUnmap.push_back(fileBuffer);
            appendInputs(path, /* member name */ "", statBuf, fileBuffer, decendIntoStaticLibs, inputs, stringsToFree, misalignedMember);
        }
        else if ( searchDyldCache ) {
            if (__builtin_available(macOS 12, iOS 15, tvOS 15, watchOS 8, bridgeOS 6, *)) {
                // if path not found, check if the path is in the dyld cache
                const char*  currentArchName = nullptr;
                size_t       cacheLen;
                if ( dyldCache == nullptr )
                    dyldCache   = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLen);
                if ( dyldCache != nullptr )
                    currentArchName = dyldCache->archName();
                if ( path.starts_with("/System/DriverKit/") ) {
                    if ( dyldCacheDK == nullptr ) {
                        dyld_for_each_installed_shared_cache(^(dyld_shared_cache_t cacheRef) {
                            //__block bool firstCacheFile = false;
                            dyld_shared_cache_for_each_file(cacheRef, ^(const char* aCacheFilePath) {
                                // skip non-driverkit caches
                                if ( strncmp(aCacheFilePath, "/System/DriverKit/", 18) != 0 )
                                    return;

                                // skip cache files for all but matching arch with no extension
                                const char* founddk = strstr(aCacheFilePath, currentArchName);
                                if ( founddk == nullptr || strlen(founddk) != strlen(currentArchName) )
                                    return;
                                // FIXME: free private dk cache
                                std::vector<const DyldSharedCache*> dyldCaches = DyldSharedCache::mapCacheFiles(aCacheFilePath);
                                if ( dyldCaches.empty() )
                                    return;
                                dyldCacheDK = dyldCaches.front();
                            });
                        });
                    }
                    if ( dyldCacheDK != nullptr ) {
                        uint32_t imageIndex;
                        if ( dyldCacheDK->hasImagePath(path.c_str(), imageIndex) ) {
                            const UnsafeHeader* hdr = (UnsafeHeader*)dyldCacheDK->getIndexedImageEntry(imageIndex);
                            std::span<const uint8_t> inDyldCacheBuffer = std::span<const uint8_t>((uint8_t*)hdr, 0x100000000);
                            Universal::Slice sliceInfo{ hdr->arch(), inDyldCacheBuffer };
                            Archive::Member mInfo;
                            mInfo.name     = path.leafName();
                            mInfo.contents = inDyldCacheBuffer;
                            inputs.push_back({path, Error::none(), sliceInfo, mInfo});
                            mapErr = Error::none();
                        }
                    }
                }
                else if ( dyldCache != nullptr ) {
                    // see if path is in current dyld shared cache
                    uint32_t imageIndex;
                    if ( dyldCache->hasImagePath(path.c_str(), imageIndex) ) {
                        const UnsafeHeader* hdr = (UnsafeHeader*)dyldCache->getIndexedImageEntry(imageIndex);
                        std::span<const uint8_t> inDyldCacheBuffer = std::span<const uint8_t>((uint8_t*)hdr, 0x100000000);
                        Universal::Slice sliceInfo{ hdr->arch(), inDyldCacheBuffer };
                        Archive::Member mInfo;
                        mInfo.name     = path.leafName();
                        mInfo.contents = inDyldCacheBuffer;
                        inputs.push_back({path, Error::none(), sliceInfo, mInfo});
                        mapErr = Error::none(); // clear file-not-found error
                    }
                }
            }
        }
        if ( mapErr.hasError() ) {
            // record error in inputs vector
            std::span<const uint8_t> emptyBuffer = std::span<const uint8_t>();
            Universal::Slice emptySlice{ Architecture(), emptyBuffer, 0, 0 };
            Archive::Member mInfo;
            mInfo.name     = path.leafName();
            mInfo.contents = emptyBuffer;
            mInfo.setFileInfo(statBuf);
            inputs.push_back({path, std::move(mapErr), emptySlice, mInfo});
        }
    }

    // let client process all slices
    callback(inputs);

    // clean up
    for (std::span<const uint8_t> fileBuffer : buffersToUnmap) {
        unmapFile(fileBuffer);
    }
    for (const char* str : stringsToFree) {
        free((void*)str);
    }
}

#if HAVE_LIBLTO
bool Tool::getBitCodeFileArch(CString path, std::span<const uint8_t> buffer, Architecture& arch)
{
    // Every file gets its own parsing context
    lto_module_t mod = ::lto_module_create_in_local_context(buffer.data(), buffer.size(), path.c_str());

    // lto_module_create_in_codegen_context doesn't handle errors correctly.  It returns the error code in the error case, not NULL.
    // Check for a range of bad values, including NULL
    if ( (uint64_t)mod < 65536 )
        return false;

    // get arch and platform info from bitcode
    const char*         triple = ::lto_module_get_target_triple(mod);
    Architecture        tripleArch;
    PlatformAndVersions triplePvs;
    if ( Error err = triplePvs.setFromTargetTriple(triple, tripleArch) )
        return false;
    arch = tripleArch;
    ::lto_module_dispose(mod);
    return true;
}
#endif


} // namespace other_tools


