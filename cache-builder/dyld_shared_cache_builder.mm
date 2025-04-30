/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <libgen.h>
#include <pthread.h>
#include <fts.h>

#include <vector>
#include <array>
#include <list>
#include <set>
#include <map>
#include <unordered_set>
#include <algorithm>
#include <fstream>
#include <regex>

#include <spawn.h>

#include <Bom/Bom.h>
#include <Foundation/NSData.h>
#include <Foundation/NSDictionary.h>
#include <Foundation/NSPropertyList.h>
#include <Foundation/NSString.h>

#include "Defines.h"
#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "FileUtils.h"
#include "JSONReader.h"
#include "JSONWriter.h"
#include "Platform.h"
#include "StringUtils.h"
#include "mrm_shared_cache_builder.h"

#if !__has_feature(objc_arc)
#error The use of libdispatch in this files requires it to be compiled with ARC in order to avoid leaks
#endif

extern char** environ;

static dispatch_queue_t build_queue;

static int runCommandAndWait(Diagnostics& diags, const char* args[])
{
    pid_t pid;
    int   status;
    int   res = posix_spawn(&pid, args[0], nullptr, nullptr, (char**)args, environ);
    if (res != 0)
        diags.error("Failed to spawn %s: %s (%d)", args[0], strerror(res), res);

    do {
        res = waitpid(pid, &status, 0);
    } while (res == -1 && errno == EINTR);
    if (res != -1) {
        if (WIFEXITED(status)) {
            res = WEXITSTATUS(status);
        } else {
            res = -1;
        }
    }

    return res;
}

static void processRoots(std::list<std::string>& roots, const char *tempRootsDir)
{
    std::list<std::string>  processedRoots;
    struct stat             sb;
    int                     res = 0;
    const char*             args[8];

    for (const auto& root : roots) {
        res = stat(root.c_str(), &sb);

        if (res == 0 && S_ISDIR(sb.st_mode)) {
            processedRoots.push_back(root);
            continue;
        }

        char tempRootDir[MAXPATHLEN];
        strlcpy(tempRootDir, tempRootsDir, MAXPATHLEN);
        strlcat(tempRootDir, "/XXXXXXXX", MAXPATHLEN);
        mkdtemp(tempRootDir);

        if (endsWith(root, ".cpio") || endsWith(root, ".cpio.gz") || endsWith(root, ".cpgz") || endsWith(root, ".cpio.bz2") || endsWith(root, ".cpbz2") || endsWith(root, ".pax") || endsWith(root, ".pax.gz") || endsWith(root, ".pgz") || endsWith(root, ".pax.bz2") || endsWith(root, ".pbz2")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-x";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else if (endsWith(root, ".tar")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.gz") || endsWith(root, ".tgz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xzf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.bz2")
            || endsWith(root, ".tbz2")
            || endsWith(root, ".tbz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xjf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".zip")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-xk";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else {
            fprintf(stderr, "unknown archive type: %s\n", root.c_str());
            exit(EX_DATAERR);
        }

        Diagnostics diags;
        if (res != runCommandAndWait(diags, args)) {
            fprintf(stderr, "could not expand archive %s: %s (%d) because '%s'\n",
                    root.c_str(), strerror(res), res,
                    diags.hasError() ? diags.errorMessageCStr() : "unknown error");
            exit(EX_DATAERR);
        }
        for (auto& existingRoot : processedRoots) {
            if (existingRoot == tempRootDir)
                continue;
        }

        processedRoots.push_back(tempRootDir);
    }

    roots = processedRoots;
}

static void writeRootList(const std::string& dstRoot, const std::list<std::string>& roots)
{
    if (roots.size() == 0)
        return;

    std::string rootFile = dstRoot + "/roots.txt";
    FILE*       froots = ::fopen(rootFile.c_str(), "w");
    if (froots == NULL)
        return;

    for (auto& root : roots) {
        fprintf(froots, "%s\n", root.c_str());
    }

    ::fclose(froots);
}

struct FilteredCopyOptions {
    Diagnostics*            diags               = nullptr;
    std::set<std::string>*  cachePaths          = nullptr;
    std::set<std::string>*  dylibsFoundInRoots  = nullptr;
};

static BOMCopierCopyOperation filteredCopyIncludingPaths(BOMCopier copier, const char* path, BOMFSObjType type, off_t size)
{
    std::string absolutePath = &path[1];
    const FilteredCopyOptions *userData = (const FilteredCopyOptions*)BOMCopierUserData(copier);

    // Don't copy from the artifact if the dylib is actally in a -root
    if ( userData->dylibsFoundInRoots->count(absolutePath) != 0 ) {
        userData->diags->verbose("Skipping copying dylib from shared cache artifact as it is in a -root: '%s'\n", absolutePath.c_str());
        return BOMCopierSkipFile;
    }

    for (const std::string& cachePath : *userData->cachePaths) {
        if (startsWith(cachePath, absolutePath)) {
            userData->diags->verbose("Copying dylib from shared cache artifact: '%s'\n", absolutePath.c_str());
            return BOMCopierContinue;
        }
    }
    if (userData->cachePaths->count(absolutePath)) {
        userData->diags->verbose("Copying dylib from shared cache artifact: '%s'\n", absolutePath.c_str());
        return BOMCopierContinue;
    }
    return BOMCopierSkipFile;
}

static Disposition stringToDisposition(Diagnostics& diags, const std::string& str) {
    if (diags.hasError())
        return Unknown;
    if (str == "Unknown")
        return Unknown;
    if (str == "InternalDevelopment")
        return InternalDevelopment;
    if (str == "Customer")
        return Customer;
    if (str == "InternalMinDevelopment")
        return InternalMinDevelopment;
    if (str == "SymbolsCache")
        return SymbolsCache;
    return Unknown;
}

static Platform stringToPlatform(Diagnostics& diags, const std::string& str) {
    if (diags.hasError())
        return unknown;
    if (str == "unknown")
        return unknown;
    if ( (str == "macOS") || (str == "osx") )
        return macOS;
    if (str == "iOS")
        return iOS;
    if (str == "tvOS")
        return tvOS;
    if (str == "watchOS")
        return watchOS;
    if (str == "bridgeOS")
        return bridgeOS;
    if (str == "iOSMac")
        return iOSMac;
    if (str == "UIKitForMac")
        return iOSMac;
    if (str == "iOS_simulator")
        return iOS_simulator;
    if (str == "tvOS_simulator")
        return tvOS_simulator;
    if (str == "watchOS_simulator")
        return watchOS_simulator;
    if (str == "driverKit")
        return driverKit;
    if (str == "macOSExclaveKit")
        return macOSExclaveKit;
    if (str == "iOSExclaveKit")
        return iOSExclaveKit;
    if ( std::isdigit(str.front()) ) {
        // Also allow platforms to be specified as an integer
        return (Platform)atoi(str.c_str());
    }
    if ( startsWith(str, "platform") ) {
        std::string_view strView = str;
        strView.remove_prefix(8);
        if ( std::isdigit(strView.front()) ) {
            // Also allow platforms to be specified as an integer
            return (Platform)atoi(strView.data());
        }
    }
    return unknown;
}

static FileFlags stringToFileFlags(Diagnostics& diags, const std::string& str) {
    if (diags.hasError())
        return NoFlags;
    if (str == "NoFlags")
        return NoFlags;
    if (str == "MustBeInCache")
        return MustBeInCache;
    if (str == "ShouldBeExcludedFromCacheIfUnusedLeaf")
        return ShouldBeExcludedFromCacheIfUnusedLeaf;
    if (str == "RequiredClosure")
        return RequiredClosure;
    if (str == "DylibOrderFile")
        return DylibOrderFile;
    if (str == "DirtyDataOrderFile")
        return DirtyDataOrderFile;
    if (str == "ObjCOptimizationsFile")
        return ObjCOptimizationsFile;
    if (str == "SwiftGenericMetadataFile")
        return SwiftGenericMetadataFile;
    if (str == "OptimizationFile")
        return OptimizationFile;
    return NoFlags;
}

struct SharedCacheBuilderOptions {
    Diagnostics                 diags;
    std::list<std::string>      roots;
    std::string                 dylibCacheDir;
    std::string                 artifactDir;
    std::string                 release;
    bool                        emitDevCaches = true;
    bool                        emitCustomerCaches = true;
    bool                        emitElidedDylibs = true;
    bool                        listConfigs = false;
    bool                        copyRoots = false;
    bool                        debug = false;
    bool                        useMRM = false;
    bool                        timePasses = false;
    bool                        printStats = false;
    bool                        printRemovedFiles = false;
    bool                        emitJSONMap = false;
    std::string                 dstRoot;
    std::string                 buildAllPath;
    std::string                 resultPath;
    std::string                 baselineDifferenceResultPath;
    std::list<std::string>      baselineCacheMapPaths;
    std::string                 baselineCacheMapDirPath;
    bool                        baselineCopyRoots = false;
    bool                        emitMapFiles = false;
    std::set<std::string>       cmdLineArchs;
};

typedef std::tuple<std::string, std::string, FileFlags, std::string> InputFile;

static void loadMRMFiles(Diagnostics& diags,
                         MRMSharedCacheBuilder* sharedCacheBuilder,
                         const std::vector<InputFile>& inputFiles,
                         std::vector<std::pair<const void*, size_t>>& mappedFiles,
                         const std::set<std::string>& baselineCacheFiles) {

    for (const InputFile& inputFile : inputFiles) {
        const std::string& buildPath   = std::get<0>(inputFile);
        const std::string& runtimePath = std::get<1>(inputFile);
        FileFlags          fileFlags   = std::get<2>(inputFile);
        const std::string& projectName = std::get<3>(inputFile);

        struct stat stat_buf;
        int fd = ::open(buildPath.c_str(), O_RDONLY, 0);
        if (fd == -1) {
            if (baselineCacheFiles.count(runtimePath)) {
                diags.error("can't open file '%s', errno=%d\n", buildPath.c_str(), errno);
                return;
            } else {
                // Don't spam with paths we know will be missing
                if ( buildPath.starts_with("./System/Library/Templates/Data/") )
                    continue;
                diags.verbose("can't open file '%s', errno=%d\n", buildPath.c_str(), errno);
                continue;
            }
        }

        if (fstat(fd, &stat_buf) == -1) {
            if (baselineCacheFiles.count(runtimePath)) {
                diags.error("can't stat open file '%s', errno=%d\n", buildPath.c_str(), errno);
                ::close(fd);
                return;
            } else {
                diags.verbose("can't stat open file '%s', errno=%d\n", buildPath.c_str(), errno);
                ::close(fd);
                continue;
            }
        }

        const void* buffer = mmap(NULL, (size_t)stat_buf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (buffer == MAP_FAILED) {
            diags.error("mmap() for file at %s failed, errno=%d\n", buildPath.c_str(), errno);
            ::close(fd);
        }
        ::close(fd);

        mappedFiles.emplace_back(buffer, (size_t)stat_buf.st_size);

        addFile_v2(sharedCacheBuilder, runtimePath.c_str(), (uint8_t*)buffer, (size_t)stat_buf.st_size, fileFlags, projectName.c_str());
    }
}

static void unloadMRMFiles(std::vector<std::pair<const void*, size_t>>& mappedFiles) {
    for (auto mappedFile : mappedFiles)
        ::munmap((void*)mappedFile.first, mappedFile.second);
}

static ssize_t write64(int fildes, const void *buf, size_t nbyte)
{
    unsigned char* uchars = (unsigned char*)buf;
    ssize_t total = 0;

    while (nbyte)
    {
        /*
         * If we were writing socket- or stream-safe code we'd chuck the
         * entire buf to write(2) and then gracefully re-request bytes that
         * didn't get written. But write(2) will return EINVAL if you ask it to
         * write more than 2^31-1 bytes. So instead we actually need to throttle
         * the input to write.
         *
         * Historically code using write(2) to write to disk will assert that
         * that all of the requested bytes were written. It seems harmless to
         * re-request bytes as one does when writing to streams, with the
         * compromise that we will return immediately when write(2) returns 0
         * bytes written.
         */
        size_t limit = 0x7FFFFFFF;
        size_t towrite = nbyte < limit ? nbyte : limit;
        ssize_t wrote = write(fildes, uchars, towrite);
        if (-1 == wrote)
        {
            return -1;
        }
        else if (0 == wrote)
        {
            break;
        }
        else
        {
            nbyte -= wrote;
            uchars += wrote;
            total += wrote;
        }
    }

    return total;
}

static void printRemovedFiles(bool cacheBuildSuccess, MRMSharedCacheBuilder* sharedCacheBuilder,
                              const SharedCacheBuilderOptions& options)
{
    if ( !cacheBuildSuccess || !options.printRemovedFiles )
        return;

    uint64_t fileResultCount = 0;
    if (const char* const* fileResults = getFilesToRemove(sharedCacheBuilder, &fileResultCount)) {
        for (uint64_t i = 0; i != fileResultCount; ++i)
            printf("Removed: %s\n", fileResults[i]);
    }
}

static void writeMRMResults(bool cacheBuildSuccess, MRMSharedCacheBuilder* sharedCacheBuilder,
                            const SharedCacheBuilderOptions& options)
{
    if (!cacheBuildSuccess) {
        uint64_t errorCount = 0;
        if (const char* const* errors = getErrors(sharedCacheBuilder, &errorCount)) {
            for (uint64_t i = 0, e = errorCount; i != e; ++i) {
                const char* errorMessage = errors[i];
                fprintf(stderr, "ERROR: %s\n", errorMessage);
            }
        }
    }

    // Now emit each cache we generated, or the errors for them.
    uint64_t cacheResultCount = 0;
    if (const CacheResult* const* cacheResults = getCacheResults(sharedCacheBuilder, &cacheResultCount)) {
        for (uint64_t i = 0, e = cacheResultCount; i != e; ++i) {
            const CacheResult& result = *(cacheResults[i]);
            // Always print the warnings if we have roots, even if there are errors
            // But not if we have -build_all, as its too noisy
            bool emitWarnings = (result.numErrors == 0) || !options.roots.empty() || options.debug;
            if ( options.dstRoot.empty() )
                emitWarnings = false;
            if ( emitWarnings ) {
                for (uint64_t warningIndex = 0; warningIndex != result.numWarnings; ++warningIndex) {
                    fprintf(stderr, "[%s] WARNING: %s\n", result.loggingPrefix, result.warnings[warningIndex]);
                }
            }
            if (result.numErrors) {
                for (uint64_t errorIndex = 0; errorIndex != result.numErrors; ++errorIndex) {
                    fprintf(stderr, "[%s] ERROR: %s\n", result.loggingPrefix, result.errors[errorIndex]);
                }
                cacheBuildSuccess = false;
            }
        }
    }

    if (!cacheBuildSuccess) {
        return;
    }

    // If we built caches, then write everything out.
    // TODO: Decide if we should we write any good caches anyway?
    if ( cacheBuildSuccess ) {
        uint64_t fileResultCount = 0;
        if (const FileResult* const* fileResults = getFileResults(sharedCacheBuilder, &fileResultCount)) {
            for (uint64_t i = 0, e = fileResultCount; i != e; ++i) {
                const FileResult* fileResultPtr = fileResults[i];

#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
                assert(fileResultPtr->version == 1);
                const FileResult_v1& fileResult = *(const FileResult_v1*)fileResultPtr;
#else
                assert(fileResultPtr->version == 2);
                const FileResult_v2& fileResult = *(const FileResult_v2*)fileResultPtr;
#endif

                switch (fileResult.behavior) {
                    case AddFile:
                        break;
                    case ChangeFile:
                        continue;
                }

                // Use the fd if we have one
#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
                // HACK: when building a macOS dyld cache on macOS it is not immediately usable because
                // the file was just written to and marked tainted.  So, use the slow path below
                // and copy to a new file that is not tainted.
                if ( (fileResult.fd != 0) && (strncmp(fileResult.path, MACOSX_MRM_DYLD_SHARED_CACHE_DIR, strlen(MACOSX_MRM_DYLD_SHARED_CACHE_DIR)) != 0) ) {
                    // If are building all caches, then we don't have a dst_root, and instead
                    // want to just drop this file
                    if ( options.dstRoot.empty() )
                        continue;

                    // Try link() the file, which will fail if crossing file systems,
                    // so fall back to the regular write() path in that case
                    const std::string path = options.dstRoot + fileResult.path;

                    // mkstemp() makes file "rw-------", switch it to "rw-r--r--"
                    ::fchmod(fileResult.fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                    ::unlink(path.c_str());
                    int result = ::link(fileResult.tempFilePath, path.c_str());
                    if ( result == 0 )
                        continue;
                    // Fall though to regular path
                }
#endif

                if ( (fileResult.data != nullptr) && !options.dstRoot.empty() ) {
                    const std::string path = options.dstRoot + fileResult.path;
                    std::string pathTemplate = path + "-XXXXXX";
                    size_t templateLen = strlen(pathTemplate.c_str())+2;
                    char pathTemplateSpace[templateLen];
                    strlcpy(pathTemplateSpace, pathTemplate.c_str(), templateLen);
                    int fd = mkstemp(pathTemplateSpace);
                    if ( fd != -1 ) {
                        ::ftruncate(fd, fileResult.size);
                        uint64_t writtenSize = write64(fd, fileResult.data, fileResult.size);
                        if ( writtenSize == fileResult.size ) {
                            ::fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH); // mkstemp() makes file "rw-------", switch it to "rw-r--r--"
                            if ( ::rename(pathTemplateSpace, path.c_str()) == 0) {
                                ::close(fd);
                                continue; // success
                            }
                        }
                        else {
                            fprintf(stderr, "ERROR: could not write file %s\n", pathTemplateSpace);
                            cacheBuildSuccess = false;
                        }
                        ::close(fd);
                        ::unlink(pathTemplateSpace);
                    }
                    else {
                        fprintf(stderr, "ERROR: could not open file %s\n", pathTemplateSpace);
                        cacheBuildSuccess = false;
                    }
                }
            }
        }

        // Give up if we couldn't write the caches
        if (!cacheBuildSuccess) {
            return;
        }
    }

    if ( options.emitJSONMap && !options.dstRoot.empty() ) {
        if (const CacheResult* const* cacheResults = getCacheResults(sharedCacheBuilder, &cacheResultCount)) {
            for (uint64_t i = 0, e = cacheResultCount; i != e; ++i) {
                const CacheResult& result = *(cacheResults[i]);

                const std::string path = options.dstRoot + "/" + result.loggingPrefix + ".json";
                std::string pathTemplate = path + "-XXXXXX";
                size_t templateLen = strlen(pathTemplate.c_str())+2;
                char pathTemplateSpace[templateLen];
                strlcpy(pathTemplateSpace, pathTemplate.c_str(), templateLen);
                int fd = mkstemp(pathTemplateSpace);
                if ( fd != -1 ) {
                    size_t jsonLength = strlen(result.mapJSON) + 1;
                    ::ftruncate(fd, jsonLength);
                    uint64_t writtenSize = write64(fd, result.mapJSON, jsonLength);
                    if ( writtenSize == jsonLength ) {
                        ::fchmod(fd, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH); // mkstemp() makes file "rw-------", switch it to "rw-r--r--"
                        if ( ::rename(pathTemplateSpace, path.c_str()) == 0) {
                            ::close(fd);
                            continue; // success
                        }
                    }
                    else {
                        fprintf(stderr, "ERROR: could not write map file %s\n", pathTemplateSpace);
                        cacheBuildSuccess = false;
                    }
                    ::close(fd);
                    ::unlink(pathTemplateSpace);
                }
                else {
                    fprintf(stderr, "ERROR: could not open map file %s\n", pathTemplateSpace);
                    cacheBuildSuccess = false;
                }
            }
        }
    }
}

static void buildCacheFromJSONManifest(Diagnostics& diags, const SharedCacheBuilderOptions& options,
                                       const std::string& jsonManifestPath) {
    json::Node manifestNode = json::readJSON(diags, jsonManifestPath.c_str(), false /* useJSON5 */);
    if (diags.hasError())
        return;

    // Top level node should be a map of the options, files, and symlinks.
    if (manifestNode.map.empty()) {
        diags.error("Expected map for JSON manifest node\n");
        return;
    }

    // Parse the nodes in the top level manifest node
    const json::Node& versionNode          = json::getRequiredValue(diags, manifestNode, "version");
    uint64_t manifestVersion               = json::parseRequiredInt(diags, versionNode);
    if (diags.hasError())
        return;

    const uint64_t supportedManifestVersion = 1;
    if (manifestVersion != supportedManifestVersion) {
        diags.error("JSON manfiest version of %lld is unsupported.  Supported version is %lld\n",
                    manifestVersion, supportedManifestVersion);
        return;
    }
    const json::Node& buildOptionsNode     = json::getRequiredValue(diags, manifestNode, "buildOptions");
    const json::Node& filesNode            = json::getRequiredValue(diags, manifestNode, "files");
    const json::Node* symlinksNode         = json::getOptionalValue(diags, manifestNode, "symlinks");

    // Parse the archs
    const json::Node& archsNode = json::getRequiredValue(diags, buildOptionsNode, "archs");
    if (diags.hasError())
        return;
    if (archsNode.array.empty()) {
        diags.error("Build options archs node is not an array\n");
        return;
    }
    std::set<std::string> jsonArchs;
    const char* archs[archsNode.array.size()];
    uint64_t numArchs = 0;
    for (const json::Node& archNode : archsNode.array) {
        const char* archName = json::parseRequiredString(diags, archNode).c_str();
        jsonArchs.insert(archName);
        if ( options.cmdLineArchs.empty() || options.cmdLineArchs.count(archName) ) {
            archs[numArchs++] = archName;
        }
    }

    // Check that the command line archs are in the JSON list
    if ( !options.cmdLineArchs.empty() ) {
        for (const std::string& cmdLineArch : options.cmdLineArchs) {
            if ( !jsonArchs.count(cmdLineArch) ) {
                std::string validArchs = "";
                for (const std::string& jsonArch : jsonArchs) {
                    if ( !validArchs.empty() ) {
                        validArchs += ", ";
                    }
                    validArchs += jsonArch;
                }
                diags.error("Command line -arch '%s' is not valid for this device.  Valid archs are (%s)\n", cmdLineArch.c_str(), validArchs.c_str());
                return;
            }
        }
    }

    // Parse the rest of the options node.
    BuildOptions_v3 buildOptions;
    buildOptions.version                            = json::parseRequiredInt(diags, json::getRequiredValue(diags, buildOptionsNode, "version"));
    buildOptions.updateName                         = json::parseRequiredString(diags, json::getRequiredValue(diags, buildOptionsNode, "updateName")).c_str();
    buildOptions.deviceName                         = json::parseRequiredString(diags, json::getRequiredValue(diags, buildOptionsNode, "deviceName")).c_str();
    buildOptions.disposition                        = stringToDisposition(diags, json::parseRequiredString(diags, json::getRequiredValue(diags, buildOptionsNode, "disposition")));
    buildOptions.platform                           = stringToPlatform(diags, json::parseRequiredString(diags, json::getRequiredValue(diags, buildOptionsNode, "platform")));
    buildOptions.archs                              = archs;
    buildOptions.numArchs                           = numArchs;
    buildOptions.verboseDiagnostics                 = options.debug;
    buildOptions.isLocallyBuiltCache                = true;

    // optimizeForSize was added in version 2
    buildOptions.optimizeForSize = false;
    if ( buildOptions.version >= 2 ) {
        buildOptions.optimizeForSize                = json::parseRequiredBool(diags, json::getRequiredValue(diags, buildOptionsNode, "optimizeForSize"));
    }

    // timePasses was added in version 3
    buildOptions.filesRemovedFromDisk = true;
    buildOptions.timePasses = false;
    buildOptions.printStats = false;
    if ( buildOptions.version == 2 ) {
        // HACK:! Bump to version 3 so that timePasses/printStats are picked up.
        buildOptions.version = 3;
        buildOptions.timePasses = options.timePasses;
        buildOptions.printStats = options.printStats;
    } else if ( buildOptions.version >= 3 ) {
        const json::Node* filesRemovedNode = json::getOptionalValue(diags, buildOptionsNode, "filesRemovedFromDisk");
        const json::Node* timePassesNode = json::getOptionalValue(diags, buildOptionsNode, "timePasses");
        const json::Node* printStatsNode = json::getOptionalValue(diags, buildOptionsNode, "printStats");
        if ( filesRemovedNode != nullptr )
            buildOptions.filesRemovedFromDisk = json::parseRequiredBool(diags, *filesRemovedNode);
        if ( timePassesNode != nullptr )
            buildOptions.timePasses = json::parseRequiredBool(diags, *timePassesNode);
        if ( printStatsNode != nullptr )
            buildOptions.printStats = json::parseRequiredBool(diags, *printStatsNode);
    }

    if (diags.hasError())
        return;

    // Override the disposition if we don't want certain caches.
    switch (buildOptions.disposition) {
        case Unknown:
            // Nothing we can do here as we can't assume what caches are built here.
            break;
        case InternalDevelopment:
            if (!options.emitDevCaches && !options.emitCustomerCaches) {
                diags.error("both -no_customer_cache and -no_development_cache passed\n");
                break;
            }
            if (!options.emitDevCaches) {
                // This builds both caches, but we don't want dev
                buildOptions.disposition = Customer;
            }
            if (!options.emitCustomerCaches) {
                // This builds both caches, but we don't want customer
                buildOptions.disposition = InternalMinDevelopment;
            }
            break;
        case Customer:
            if (!options.emitCustomerCaches) {
                diags.error("Cannot request no customer cache for Customer as that is already only a customer cache\n");
            }
            break;
        case InternalMinDevelopment:
            if (!options.emitDevCaches) {
                diags.error("Cannot request no dev cache for InternalMinDevelopment as that is already only a dev cache\n");
            }
            break;
        case SymbolsCache:
            break;
    }

    if (diags.hasError())
        return;

    struct MRMSharedCacheBuilder* sharedCacheBuilder = createSharedCacheBuilder((const BuildOptions_v1*)&buildOptions);

    // Parse the files
    if (filesNode.array.empty()) {
        diags.error("Build options files node is not an array\n");
        return;
    }

    std::vector<InputFile> inputFiles;
    std::set<std::string> dylibsFoundInRoots;
    for (const json::Node& fileNode : filesNode.array) {
        std::string path = json::parseRequiredString(diags, json::getRequiredValue(diags, fileNode, "path")).c_str();
        FileFlags fileFlags     = stringToFileFlags(diags, json::parseRequiredString(diags, json::getRequiredValue(diags, fileNode, "flags")));

        std::string_view projectName;
        if ( const json::Node* projectNode = json::getOptionalValue(diags, fileNode, "project") )
            projectName = projectNode->value;

        // We can optionally have a sourcePath entry which is the path to get the source content from instead of the install path
        std::string sourcePath;
        const json::Node* sourcePathNode = json::getOptionalValue(diags, fileNode, "sourcePath");
        if ( sourcePathNode != nullptr ) {
            if (!sourcePathNode->array.empty()) {
                diags.error("sourcePath node cannot be an array\n");
                return;
            }
            if (!sourcePathNode->map.empty()) {
                diags.error("sourcePath node cannot be a map\n");
                return;
            }
            sourcePath = sourcePathNode->value;
        } else {
            sourcePath = path;
        }

        std::string buildPath = sourcePath;

        // Check if one of the -root's has this path
        bool foundInOverlay = false;
        for (const std::string& overlay : options.roots) {
            struct stat sb;
            std::string filePath = overlay + path;
            if (!stat(filePath.c_str(), &sb)) {
                foundInOverlay = true;
                diags.verbose("Taking '%s' from overlay '%s' instead of dylib cache\n", path.c_str(), overlay.c_str());
                inputFiles.push_back({ filePath, path, fileFlags, std::string(projectName) });
                dylibsFoundInRoots.insert(path);
                break;
            }
        }

        if (foundInOverlay)
            continue;

        // Build paths are relative to the build artifact root directory.
        switch (fileFlags) {
            case NoFlags:
            case MustBeInCache:
            case ShouldBeExcludedFromCacheIfUnusedLeaf:
            case RequiredClosure:
            case DylibOrderFile:
            case DirtyDataOrderFile:
            case ObjCOptimizationsFile:
            case SwiftGenericMetadataFile:
            case OptimizationFile:
                buildPath = "." + buildPath;
                break;
        }
        inputFiles.push_back({ buildPath, path, fileFlags, std::string(projectName) });
    }

    if (diags.hasError())
        return;

    // Parse the baseline from the map(s) if we have it
    BaselineCachesChecker baselineCaches({ &archs[0], &archs[numArchs] }, mach_o::Platform(buildOptions.platform));

    // If we have a maps directory, use it
    if ( !options.baselineCacheMapDirPath.empty() ) {
        if ( mach_o::Error err = baselineCaches.addBaselineMaps(options.baselineCacheMapDirPath) ) {
            diags.error("%s", err.message());
            return;
        }
    } else {
        for ( const std::string& baselineCacheMapPath : options.baselineCacheMapPaths ) {
            if ( mach_o::Error err = baselineCaches.addBaselineMap(baselineCacheMapPath) ) {
                diags.error("%s", err.message());
                return;
            }
        }
    }

    std::vector<std::pair<const void*, size_t>> mappedFiles;
    {
        uint64_t startTimeNanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        loadMRMFiles(diags, sharedCacheBuilder, inputFiles, mappedFiles, baselineCaches.unionBaselineDylibs());
        uint64_t endTimeNanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

        if ( options.timePasses ) {
            uint64_t timeMillis = (endTimeNanos - startTimeNanos) / 1000000;
            fprintf(stderr, "loadMRMFiles: time = %lldms\n", timeMillis);
        }
    }

    if (diags.hasError())
        return;

    // Parse the symlinks if we have them
    if (symlinksNode) {
        if (symlinksNode->array.empty()) {
            diags.error("Build options symlinks node is not an array\n");
            return;
        }
        for (const json::Node& symlinkNode : symlinksNode->array) {
            std::string fromPath = json::parseRequiredString(diags, json::getRequiredValue(diags, symlinkNode, "path")).c_str();
            const std::string& toPath   = json::parseRequiredString(diags, json::getRequiredValue(diags, symlinkNode, "target")).c_str();
            addSymlink(sharedCacheBuilder, fromPath.c_str(), toPath.c_str());
        }
    }

    if (diags.hasError())
        return;

    // Don't create a directory if we are skipping writes, which means we have no dstRoot set
    if (!options.dstRoot.empty()) {
        if ( buildOptions.platform == macOS ) {
            (void)mkpath_np((options.dstRoot + MACOSX_MRM_DYLD_SHARED_CACHE_DIR).c_str(), 0755);
        } else if (buildOptions.platform == driverKit ) {
            (void)mkpath_np((options.dstRoot + DRIVERKIT_DYLD_SHARED_CACHE_DIR).c_str(), 0755);
        } else if ( mach_o::Platform(buildOptions.platform).isExclaveKit() ) {
            (void)mkpath_np((options.dstRoot + EXCLAVEKIT_DYLD_SHARED_CACHE_DIR).c_str(), 0755);
        } else if ( buildOptions.disposition == SymbolsCache ) {
            // symbols cache always uses /System/Library/dyld, even on iOS
            (void)mkpath_np((options.dstRoot + MACOSX_MRM_DYLD_SHARED_CACHE_DIR).c_str(), 0755);
        } else {
            (void)mkpath_np((options.dstRoot + IPHONE_DYLD_SHARED_CACHE_DIR).c_str(), 0755);
        }
    }

    // Actually build the cache.
    bool cacheBuildSuccess = runSharedCacheBuilder(sharedCacheBuilder);

    // Compare this cache to the baseline cache and see if we have any roots to copy over
    if (!options.baselineDifferenceResultPath.empty() || options.baselineCopyRoots) {
        std::set<std::string> dylibsInNewCaches;
        if (cacheBuildSuccess) {
            uint64_t fileResultCount = 0;
            if (const char* const* fileResults = getFilesToRemove(sharedCacheBuilder, &fileResultCount)) {
                for (uint64_t i = 0; i != fileResultCount; ++i)
                    dylibsInNewCaches.insert(fileResults[i]);
            }
        }

        if ( options.baselineCopyRoots && cacheBuildSuccess ) {
            uint64_t cacheResultCount = 0;
            if ( const CacheResult* const* cacheResults = getCacheResults(sharedCacheBuilder, &cacheResultCount) ) {
                for ( uint64_t i = 0; i != cacheResultCount; ++i ) {
                    const CacheResult& result = *(cacheResults[i]);
                    if ( result.mapJSON == nullptr )
                        continue;
                    std::string_view mapString = result.mapJSON;
                    if ( mapString.empty() )
                        continue;

                    if ( mach_o::Error err = baselineCaches.addNewMap(mapString) ) {
                        diags.error("%s", err.message());
                        return;
                    }
                }
            }

            uint64_t fileResultCount = 0;
            if ( const char* const* fileResults = getFilesToRemove(sharedCacheBuilder, &fileResultCount) )
                baselineCaches.setFilesFromNewCaches({ fileResults, fileResultCount });

            // Work out the set of dylibs in the old caches but not the new ones
            std::set<std::string> dylibsMissingFromNewCaches = baselineCaches.dylibsMissingFromNewCaches();
            if ( !dylibsMissingFromNewCaches.empty() ) {
                BOMCopier copier = BOMCopierNewWithSys(BomSys_default());
                FilteredCopyOptions userData = { &diags, &dylibsMissingFromNewCaches, &dylibsFoundInRoots };
                BOMCopierSetUserData(copier, (void*)&userData);
                BOMCopierSetCopyFileStartedHandler(copier, filteredCopyIncludingPaths);
                std::string dylibCacheRootDir = realFilePath(options.dylibCacheDir);
                if (dylibCacheRootDir == "") {
                    fprintf(stderr, "Could not find dylib Root directory to copy baseline roots from\n");
                    exit(EX_NOINPUT);
                }
                BOMCopierCopy(copier, dylibCacheRootDir.c_str(), options.dstRoot.c_str());
                BOMCopierFree(copier);

                for (const std::string& dylibMissingFromNewCache : dylibsMissingFromNewCaches) {
                    diags.verbose("Dylib missing from new cache: '%s'\n", dylibMissingFromNewCache.c_str());
                }
            }
        }

        if (!options.baselineDifferenceResultPath.empty()) {
            auto cppToObjStr = [](const std::string& str) {
                return [NSString stringWithUTF8String:str.c_str()];
            };

            // Work out the set of dylibs in the cache and taken from any -roots
            NSMutableArray<NSString*>* dylibsFromRoots = [NSMutableArray array];
            for (auto& root : options.roots) {
                for (const std::string& dylibInstallName : dylibsInNewCaches) {
                    struct stat sb;
                    std::string filePath = root + "/" + dylibInstallName;
                    if (!stat(filePath.c_str(), &sb)) {
                        [dylibsFromRoots addObject:cppToObjStr(dylibInstallName)];
                    }
                }
            }

            // Work out the set of dylibs in the new cache but not in the baseline cache.
            NSMutableArray<NSString*>* dylibsMissingFromBaselineCache = [NSMutableArray array];
            for (const std::string& newDylib : dylibsInNewCaches) {
                if ( !baselineCaches.unionBaselineDylibs().count(newDylib) )
                    [dylibsMissingFromBaselineCache addObject:cppToObjStr(newDylib)];
            }

            NSMutableDictionary* cacheDict = [[NSMutableDictionary alloc] init];
            cacheDict[@"root-paths-in-cache"] = dylibsFromRoots;
            cacheDict[@"device-paths-to-delete"] = dylibsMissingFromBaselineCache;

            NSError* error = nil;
            NSData*  outData = [NSPropertyListSerialization dataWithPropertyList:cacheDict
                                                                          format:NSPropertyListBinaryFormat_v1_0
                                                                         options:0
                                                                           error:&error];
            (void)[outData writeToFile:cppToObjStr(options.baselineDifferenceResultPath) atomically:YES];
        }
    }

    printRemovedFiles(cacheBuildSuccess, sharedCacheBuilder, options);

    writeMRMResults(cacheBuildSuccess, sharedCacheBuilder, options);

    destroySharedCacheBuilder(sharedCacheBuilder);

    unloadMRMFiles(mappedFiles);

    // On failure, add an error to the diagnostic so that the caller can see that the build failed
    if ( !cacheBuildSuccess ) {
        diags.error("see other errors");
    }
}

static std::string realPathOrExit(const char* argName, const char* argValue)
{
    std::string realpath = realPath(argValue);
    if ( realpath.empty() || !fileExists(realpath) ) {
        fprintf(stderr, "%s path doesn't exist: %s\n", argName, argValue);
        exit(EX_NOINPUT);
    }
    return realpath;
}

static const char* leafName(std::string_view str)
{
    const char* start = strrchr(str.data(), '/');
    if ( start != nullptr )
        return &start[1];
    else
        return str.data();
}

int main(int argc, const char* argv[])
{
    SharedCacheBuilderOptions options;
    std::string jsonManifestPath;
    char* tempRootsDir = strdup("/tmp/dyld_shared_cache_builder.XXXXXX");

    mkdtemp(tempRootsDir);

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] == '-') {
            if (strcmp(arg, "-debug") == 0) {
                options.debug = true;
            } else if (strcmp(arg, "-list_configs") == 0) {
                options.listConfigs = true;
            } else if (strcmp(arg, "-root") == 0) {
                std::string realpath = realPathOrExit("-root", argv[++i]);
                if ( std::find(options.roots.begin(), options.roots.end(), realpath) == options.roots.end() ) {
                    // Push roots on to the front so that each -root overrides previous entries
                    options.roots.push_front(realpath);
                }
            } else if (strcmp(arg, "-copy_roots") == 0) {
                options.copyRoots = true;
            } else if (strcmp(arg, "-dylib_cache") == 0) {
                options.dylibCacheDir = realPathOrExit("-dylib_cache", argv[++i]);
            } else if (strcmp(arg, "-artifact") == 0) {
                options.artifactDir = realPathOrExit("-artifact", argv[++i]);
            } else if (strcmp(arg, "-no_overflow_dylibs") == 0) {
                options.emitElidedDylibs = false;
            } else if (strcmp(arg, "-no_development_cache") == 0) {
                options.emitDevCaches = false;
            } else if (strcmp(arg, "-development_cache") == 0) {
                options.emitDevCaches = true;
            } else if (strcmp(arg, "-no_customer_cache") == 0) {
                options.emitCustomerCaches = false;
            } else if (strcmp(arg, "-customer_cache") == 0) {
                options.emitCustomerCaches = true;
            } else if (strcmp(arg, "-overflow_dylibs") == 0) {
                options.emitElidedDylibs = true;
            } else if (strcmp(arg, "-mrm") == 0) {
                options.useMRM = true;
            } else if (strcmp(arg, "-time-passes") == 0) {
                options.timePasses = true;
            } else if (strcmp(arg, "-stats") == 0) {
                options.printStats = true;
            } else if (strcmp(arg, "-removed_files") == 0) {
                options.printRemovedFiles = true;
            } else if (strcmp(arg, "-emit_json") == 0) {
                // unused
            } else if (strcmp(arg, "-emit_json_map") == 0) {
                options.emitJSONMap = true;
            } else if (strcmp(arg, "-json_manifest") == 0) {
                jsonManifestPath = realPathOrExit("-json_manifest", argv[++i]);
            } else if (strcmp(arg, "-build_all") == 0) {
                options.buildAllPath = realPathOrExit("-build_all", argv[++i]);
            } else if (strcmp(arg, "-dst_root") == 0) {
                options.dstRoot = realPath(argv[++i]);
            } else if (strcmp(arg, "-release") == 0) {
                options.release = argv[++i];
            } else if (strcmp(arg, "-results") == 0) {
                options.resultPath = realPath(argv[++i]);
            } else if (strcmp(arg, "-baseline_diff_results") == 0) {
                options.baselineDifferenceResultPath = realPath(argv[++i]);
            } else if (strcmp(arg, "-baseline_copy_roots") == 0) {
                options.baselineCopyRoots = true;
            } else if (strcmp(arg, "-baseline_cache_map") == 0) {
                std::string path = realPathOrExit("-baseline_cache_map", argv[++i]);
                options.baselineCacheMapPaths.push_back(path);
            } else if (strcmp(arg, "-baseline_cache_maps") == 0) {
                std::string path = realPathOrExit("-baseline_cache_maps", argv[++i]);
                options.baselineCacheMapDirPath = path;
            } else if (strcmp(arg, "-arch") == 0) {
                if ( ++i < argc ) {
                    options.cmdLineArchs.insert(argv[i]);
                }
                else {
                    fprintf(stderr, "-arch missing architecture name");
                    exit(EX_USAGE);
                }
            } else if (strcmp(arg, "-help") == 0) {
                // no usage() to show, but having this allows clients to probe
                // whether flags are supported by seeing if `-flag2check -help`
                // exits with EXIT_SUCCESS or EX_USAGE
                exit(EXIT_SUCCESS);
            } else {
                //usage();
                fprintf(stderr, "unknown option: %s\n", arg);
                exit(EX_USAGE);
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            exit(EX_USAGE);
        }
    }
    (void)options.emitElidedDylibs; // not implemented yet

    time_t mytime = time(0);
    fprintf(stderr, "Started: %s", asctime(localtime(&mytime)));
    processRoots(options.roots, tempRootsDir);

    struct rlimit rl = { OPEN_MAX, OPEN_MAX };
    (void)setrlimit(RLIMIT_NOFILE, &rl);

    if (options.dylibCacheDir.empty() && options.artifactDir.empty() && options.release.empty()) {
        fprintf(stderr, "you must specify either -dylib_cache, -artifact or -release\n");
        exit(EX_USAGE);
    } else if (!options.dylibCacheDir.empty() && !options.release.empty()) {
        fprintf(stderr, "you may not use -dylib_cache and -release at the same time\n");
        exit(EX_USAGE);
    } else if (!options.dylibCacheDir.empty() && !options.artifactDir.empty()) {
        fprintf(stderr, "you may not use -dylib_cache and -artifact at the same time\n");
        exit(EX_USAGE);
    }

    if (jsonManifestPath.empty() && options.buildAllPath.empty()) {
        fprintf(stderr, "Must specify a -json_manifest path OR a -build_all path\n");
        exit(EX_USAGE);
    }

    if (!options.buildAllPath.empty()) {
        if (!options.dstRoot.empty()) {
            fprintf(stderr, "Cannot combine -dst_root and -build_all\n");
            exit(EX_USAGE);
        }
        if (!jsonManifestPath.empty()) {
            fprintf(stderr, "Cannot combine -json_manifest and -build_all\n");
            exit(EX_USAGE);
        }
        if (!options.baselineDifferenceResultPath.empty()) {
            fprintf(stderr, "Cannot combine -baseline_diff_results and -build_all\n");
            exit(EX_USAGE);
        }
        if (options.baselineCopyRoots) {
            fprintf(stderr, "Cannot combine -baseline_copy_roots and -build_all\n");
            exit(EX_USAGE);
        }
        if (!options.baselineCacheMapPaths.empty()) {
            fprintf(stderr, "Cannot combine -baseline_cache_map and -build_all\n");
            exit(EX_USAGE);
        }
        if (!options.baselineCacheMapDirPath.empty()) {
            fprintf(stderr, "Cannot combine -baseline_cache_maps and -build_all\n");
            exit(EX_USAGE);
        }
    } else if (!options.listConfigs) {
        if (options.dstRoot.empty()) {
            fprintf(stderr, "Must specify a valid -dst_root OR -list_configs\n");
            exit(EX_USAGE);
        }

        if (jsonManifestPath.empty()) {
            fprintf(stderr, "Must specify a -json_manifest path OR -list_configs\n");
            exit(EX_USAGE);
        }
    }

    // Some options don't work with a JSON manifest
    if (!jsonManifestPath.empty()) {
        if (!options.resultPath.empty()) {
            fprintf(stderr, "Cannot use -results with -json_manifest\n");
            exit(EX_USAGE);
        }
        if (!options.baselineDifferenceResultPath.empty() && options.baselineCacheMapPaths.empty() && options.baselineCacheMapDirPath.empty()) {
            fprintf(stderr, "Must use -baseline_cache_map/-baseline_cache_maps with -baseline_diff_results when using -json_manifest\n");
            exit(EX_USAGE);
        }
        if (options.baselineCopyRoots && options.baselineCacheMapPaths.empty() && options.baselineCacheMapDirPath.empty()) {
            fprintf(stderr, "Must use -baseline_cache_map/-baseline_cache_maps with -baseline_copy_roots when using -json_manifest\n");
            exit(EX_USAGE);
        }
    } else {
        if (!options.baselineCacheMapPaths.empty()) {
            fprintf(stderr, "Cannot use -baseline_cache_map without -json_manifest\n");
            exit(EX_USAGE);
        }
        if (!options.baselineCacheMapDirPath.empty()) {
            fprintf(stderr, "Cannot use -baseline_cache_maps without -json_manifest\n");
            exit(EX_USAGE);
        }
    }

    if (!options.baselineCacheMapPaths.empty()) {
        if (options.baselineDifferenceResultPath.empty() && !options.baselineCopyRoots) {
            fprintf(stderr, "Must use -baseline_cache_map with -baseline_diff_results or -baseline_copy_roots\n");
            exit(EX_USAGE);
        }
    }

    if (!options.baselineCacheMapDirPath.empty()) {
        if (options.baselineDifferenceResultPath.empty() && !options.baselineCopyRoots) {
            fprintf(stderr, "Must use -baseline_cache_maps with -baseline_diff_results or -baseline_copy_roots\n");
            exit(EX_USAGE);
        }
    }

    // Find all the JSON files if we use -build_all
    __block std::vector<std::string> jsonPaths;
    if (!options.buildAllPath.empty()) {
        struct stat stat_buf;
        if (stat(options.buildAllPath.c_str(), &stat_buf) != 0) {
            fprintf(stderr, "Could not find -build_all path '%s'\n", options.buildAllPath.c_str());
            exit(EX_NOINPUT);
        }

        if ( (stat_buf.st_mode & S_IFMT) != S_IFDIR ) {
            fprintf(stderr, "-build_all path is not a directory '%s'\n", options.buildAllPath.c_str());
            exit(EX_DATAERR);
        }

        auto processFile = ^(const std::string& path, const struct stat& statBuf) {
            if ( !endsWith(path, ".json") )
                return;

            jsonPaths.push_back(path);
        };

        iterateDirectoryTree("", options.buildAllPath,
                             ^(const std::string& dirPath) { return false; },
                             processFile, true /* process files */, true /* recurse */);

        if (jsonPaths.empty()) {
            fprintf(stderr, "Didn't find any .json files inside -build_all path: %s\n", options.buildAllPath.c_str());
            exit(EX_DATAERR);
        }

        if (options.listConfigs) {
            for (const std::string& path : jsonPaths) {
                fprintf(stderr, "Found config: %s\n", path.c_str());
            }
            exit(EXIT_SUCCESS);
        }
    }

    if (!options.artifactDir.empty()) {
        // Find the dylib cache dir from inside the artifact dir
        struct stat stat_buf;
        if (stat(options.artifactDir.c_str(), &stat_buf) != 0) {
            fprintf(stderr, "Could not find artifact path '%s'\n", options.artifactDir.c_str());
            exit(EX_NOINPUT);
        }
        std::string dir = options.artifactDir + "/AppleInternal/Developer/DylibCaches";
        if (stat(dir.c_str(), &stat_buf) != 0) {
            fprintf(stderr, "Could not find artifact path '%s'\n", dir.c_str());
            exit(EX_DATAERR);
        }

        if (!options.release.empty()) {
            // Use the given release
            options.dylibCacheDir = dir + "/" + options.release + ".dlc";
        } else {
            // Find a release directory
            __block std::vector<std::string> subDirectories;
            iterateDirectoryTree("", dir, ^(const std::string& dirPath) {
                subDirectories.push_back(dirPath);
                return false;
            }, nullptr, false, false);

            if (subDirectories.empty()) {
                fprintf(stderr, "Could not find dlc subdirectories inside '%s'\n", dir.c_str());
                exit(EX_DATAERR);
            }

            if (subDirectories.size() > 1) {
                fprintf(stderr, "Found too many subdirectories inside artifact path '%s'.  Use -release to select one\n", dir.c_str());
                exit(EX_DATAERR);
            }

            options.dylibCacheDir = subDirectories.front();
        }
    }

    if (options.dylibCacheDir.empty()) {
        options.dylibCacheDir = std::string("/AppleInternal/Developer/DylibCaches/") + options.release + ".dlc";
    }

    //Move into the dir so we can use relative path manifests
    if ( int result = chdir(options.dylibCacheDir.c_str()); result == -1 ) {
        fprintf(stderr, "Couldn't cd in to dylib cache directory of '%s' because: %s\n",
                options.dylibCacheDir.c_str(), strerror(errno));
    }

    if (!options.buildAllPath.empty()) {
        bool requiresConcurrencyLimit = false;
        dispatch_semaphore_t concurrencyLimit = NULL;
        // Try build 1 cache per 8GB of RAM
        uint64_t memSize = 0;
        size_t sz = sizeof(memSize);
        if ( sysctlbyname("hw.memsize", &memSize, &sz, NULL, 0) == 0 ) {
            uint64_t maxThreads = std::max(memSize / 0x200000000ULL, 1ULL);
            fprintf(stderr, "Detected %lldGb or less of memory, limiting concurrency to %lld threads\n",
                    memSize / (1 << 30), maxThreads);
            requiresConcurrencyLimit = true;
            concurrencyLimit = dispatch_semaphore_create(maxThreads);
        }

        __block int finishedCount = 0;
        std::atomic_bool failedToBuildCache = { false };
        __block auto& failedToBuildCacheRef = failedToBuildCache;
        dispatch_apply(jsonPaths.size(), DISPATCH_APPLY_AUTO, ^(size_t index) {
            // Horrible hack to limit concurrency in low spec build machines.
            if (requiresConcurrencyLimit) { dispatch_semaphore_wait(concurrencyLimit, DISPATCH_TIME_FOREVER); }

            const std::string& jsonPath = jsonPaths[index];
            Diagnostics diags(options.debug);
            buildCacheFromJSONManifest(diags, options, jsonPath);

            if (diags.hasError()) {
                fprintf(stderr, "dyld_shared_cache_builder: error: %s\n", diags.errorMessage().c_str());
                failedToBuildCacheRef = true;
            }

            time_t endTime = time(0);
            std::string timeString = asctime(localtime(&endTime));
            timeString.pop_back();
            fprintf(stderr, "Finished[% 4d/% 4d]: %s %s\n",
                    ++finishedCount, (int)jsonPaths.size(), timeString.c_str(), leafName(jsonPath));

            if (requiresConcurrencyLimit) { dispatch_semaphore_signal(concurrencyLimit); }
        });

        if ( failedToBuildCacheRef )
            return EXIT_FAILURE;
    } else {
        Diagnostics diags(options.debug);
        buildCacheFromJSONManifest(diags, options, jsonManifestPath);

        if (diags.hasError()) {
            fprintf(stderr, "dyld_shared_cache_builder: error: %s\n", diags.errorMessage().c_str());
            return EXIT_FAILURE;
        }
    }

    Diagnostics diags;
    const char* args[8];
    args[0] = (char*)"/bin/rm";
    args[1] = (char*)"-rf";
    args[2] = (char*)tempRootsDir;
    args[3] = nullptr;
    (void)runCommandAndWait(diags, args);

    if (diags.hasError()) {
        // errors from our final rm -rf should just be warnings
        fprintf(stderr, "dyld_shared_cache_builder: warning: %s\n", diags.errorMessage().c_str());
    }

    for (const std::string& warn : diags.warnings()) {
        fprintf(stderr, "dyld_shared_cache_builder: warning: %s\n", warn.c_str());
    }

    // Finally, write the roots.txt to tell us which roots we pulled in
    if (!options.dstRoot.empty())
        writeRootList(options.dstRoot + "/System/Library/Caches/com.apple.dyld", options.roots);

    return EXIT_SUCCESS;
}
