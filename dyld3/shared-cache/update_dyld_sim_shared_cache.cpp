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
#include <assert.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <rootless.h>
#include <dscsym.h>
#include <dispatch/dispatch.h>
#include <pthread/pthread.h>

#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_set>
#include <iostream>
#include <fstream>

#include "MachOParser.h"
#include "FileUtils.h"
#include "StringUtils.h"
#include "DyldSharedCache.h"



struct MappedMachOsByCategory
{
    std::string                                 archName;
    std::vector<DyldSharedCache::MappedMachO>   dylibsForCache;
    std::vector<DyldSharedCache::MappedMachO>   otherDylibsAndBundles;
    std::vector<DyldSharedCache::MappedMachO>   mainExecutables;
};

static const char* sSearchDirs[] = {
    "/bin",
    "/sbin",
    "/usr",
    "/System",
};

static const char* sSkipDirs[] = {
    "/usr/share",
    "/usr/local/include",
};


static const char* sMacOsAdditions[] = {
    "/usr/lib/system/libsystem_kernel.dylib",
    "/usr/lib/system/libsystem_platform.dylib",
    "/usr/lib/system/libsystem_pthread.dylib",
};


static bool verbose = false;

static bool addIfMachO(const std::string& simRuntimeRootPath, const std::string& runtimePath, const struct stat& statBuf, dyld3::Platform platform, std::vector<MappedMachOsByCategory>& files)
{
    // read start of file to determine if it is mach-o or a fat file
    std::string fullPath = simRuntimeRootPath + runtimePath;
    int fd = ::open(fullPath.c_str(), O_RDONLY);
    if ( fd < 0 )
        return false;
    bool result = false;
    const void* wholeFile = ::mmap(NULL, statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( wholeFile != MAP_FAILED ) {
        Diagnostics diag;
        bool usedWholeFile = false;
        for (MappedMachOsByCategory& file : files) {
            size_t sliceOffset;
            size_t sliceLength;
            bool fatButMissingSlice;
            const void* slice = MAP_FAILED;
            if ( dyld3::FatUtil::isFatFileWithSlice(diag, wholeFile, statBuf.st_size, file.archName, sliceOffset, sliceLength, fatButMissingSlice) ) {
                slice = ::mmap(NULL, sliceLength, PROT_READ, MAP_PRIVATE, fd, sliceOffset);
                if ( slice != MAP_FAILED ) {
                    //fprintf(stderr, "mapped slice at %p size=0x%0lX, offset=0x%0lX for %s\n", p, len, offset, fullPath.c_str());
                    if ( !dyld3::MachOParser::isValidMachO(diag, file.archName, platform, slice, sliceLength, fullPath.c_str(), false) ) {
                        ::munmap((void*)slice, sliceLength);
                        slice = MAP_FAILED;
                    }
                }
            }
            else if ( !fatButMissingSlice && dyld3::MachOParser::isValidMachO(diag, file.archName, platform, wholeFile, statBuf.st_size, fullPath.c_str(), false) ) {
                slice           = wholeFile;
                sliceLength     = statBuf.st_size;
                sliceOffset     = 0;
                usedWholeFile   = true;
                //fprintf(stderr, "mapped whole file at %p size=0x%0lX for %s\n", p, len, inputPath.c_str());
            }
            if ( slice != MAP_FAILED ) {
                const mach_header* mh = (mach_header*)slice;
                dyld3::MachOParser parser(mh);
                if ( parser.platform() != platform ) {
                    fprintf(stderr, "skipped wrong platform binary: %s\n", fullPath.c_str());
                    result = false;
                }
                else {
                    bool sip = true; // assume anything found in the simulator runtime is a platform binary
                    if ( parser.isDynamicExecutable() ) {
                        bool issetuid = (statBuf.st_mode & (S_ISUID|S_ISGID));
                        file.mainExecutables.emplace_back(runtimePath, mh, sliceLength, issetuid, sip, sliceOffset, statBuf.st_mtime, statBuf.st_ino);
                    }
                    else {
                        if ( parser.canBePlacedInDyldCache(runtimePath) ) {
                            file.dylibsForCache.emplace_back(runtimePath, mh, sliceLength, false, sip, sliceOffset, statBuf.st_mtime, statBuf.st_ino);
                        }
                        else {
                            file.otherDylibsAndBundles.emplace_back(runtimePath, mh, sliceLength, false, sip, sliceOffset, statBuf.st_mtime, statBuf.st_ino);
                        }
                    }
                    result = true;
                }
            }
        }
        if ( !usedWholeFile )
            ::munmap((void*)wholeFile, statBuf.st_size);
    }
    ::close(fd);
    return result;
}

static void findAllFiles(const std::string& simRuntimeRootPath, dyld3::Platform platform, std::vector<MappedMachOsByCategory>& files)
{
    std::unordered_set<std::string> skipDirs;
    for (const char* s : sSkipDirs)
        skipDirs.insert(s);

    for (const char* searchDir : sSearchDirs ) {
        iterateDirectoryTree(simRuntimeRootPath, searchDir, ^(const std::string& dirPath) { return (skipDirs.count(dirPath) != 0); }, ^(const std::string& path, const struct stat& statBuf) {
            // ignore files that don't have 'x' bit set (all runnable mach-o files do)
            const bool hasXBit = ((statBuf.st_mode & S_IXOTH) == S_IXOTH);
            if ( !hasXBit && !endsWith(path, ".dylib") )
                return;

            // ignore files too small
            if ( statBuf.st_size < 0x3000 )
                return;

            // if the file is mach-o add to list
            addIfMachO(simRuntimeRootPath, path, statBuf, platform, files);
         });
    }
}

static void addMacOSAdditions(std::vector<MappedMachOsByCategory>& allFileSets)
{
    for (const char* addPath : sMacOsAdditions) {
        struct stat statBuf;
        if ( stat(addPath, &statBuf) == 0 )
            addIfMachO("", addPath, statBuf, dyld3::Platform::macOS, allFileSets);
    }
}


static bool dontCache(const std::string& simRuntimeRootPath, const std::string& archName,
                      const std::unordered_set<std::string>& pathsWithDuplicateInstallName,
                      const DyldSharedCache::MappedMachO& aFile, bool warn)
{
    if ( startsWith(aFile.runtimePath, "/usr/lib/system/introspection/") )
        return true;
    if ( startsWith(aFile.runtimePath, "/usr/local/") )
        return true;

    // anything inside a .app bundle is specific to app, so should be in shared cache
    if ( aFile.runtimePath.find(".app/") != std::string::npos )
        return true;

    if ( aFile.runtimePath.find("//") != std::string::npos ) {
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s double-slash in install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
    }

    dyld3::MachOParser parser(aFile.mh);
    const char* installName = parser.installName();
    if ( (pathsWithDuplicateInstallName.count(aFile.runtimePath) != 0) && (aFile.runtimePath != installName) ) {
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s skipping because of duplicate install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
        return true;
    }

    if ( aFile.runtimePath != installName ) {
        // see if install name is a symlink to actual path
        std::string fullInstall = simRuntimeRootPath + installName;
        char resolvedPath[PATH_MAX];
        if ( realpath(fullInstall.c_str(), resolvedPath) != NULL ) {
            std::string resolvedSymlink = resolvedPath;
            if ( !simRuntimeRootPath.empty() ) {
                resolvedSymlink = resolvedSymlink.substr(simRuntimeRootPath.size());
            }
            if ( aFile.runtimePath == resolvedSymlink ) {
                return false;
            }
        }
        if (warn) fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s skipping because of bad install name %s\n", archName.c_str(), aFile.runtimePath.c_str());
        return true;
    }

    return false;
}

static void pruneCachedDylibs(const std::string& simRuntimeRootPath, MappedMachOsByCategory& fileSet)
{
    std::unordered_set<std::string> pathsWithDuplicateInstallName;

    std::unordered_map<std::string, std::string> installNameToFirstPath;
    for (DyldSharedCache::MappedMachO& aFile : fileSet.dylibsForCache) {
        //fprintf(stderr, "dylib: %s\n", aFile.runtimePath.c_str());
        dyld3::MachOParser parser(aFile.mh);
        const char* installName = parser.installName();
        auto pos = installNameToFirstPath.find(installName);
        if ( pos == installNameToFirstPath.end() ) {
            installNameToFirstPath[installName] = aFile.runtimePath;
        }
        else {
            pathsWithDuplicateInstallName.insert(aFile.runtimePath);
            pathsWithDuplicateInstallName.insert(installNameToFirstPath[installName]);
        }
    }

    for (DyldSharedCache::MappedMachO& aFile : fileSet.dylibsForCache) {
        if ( dontCache(simRuntimeRootPath, fileSet.archName, pathsWithDuplicateInstallName, aFile, true) )
            fileSet.otherDylibsAndBundles.push_back(aFile);
     }
    fileSet.dylibsForCache.erase(std::remove_if(fileSet.dylibsForCache.begin(), fileSet.dylibsForCache.end(),
        [&](const DyldSharedCache::MappedMachO& aFile) { return dontCache(simRuntimeRootPath, fileSet.archName, pathsWithDuplicateInstallName, aFile, false); }),
        fileSet.dylibsForCache.end());
}

static bool existingCacheUpToDate(const std::string& existingCache, const std::vector<DyldSharedCache::MappedMachO>& currentDylibs)
{
    // if no existing cache, it is not up-to-date
    int fd = ::open(existingCache.c_str(), O_RDONLY);
    if ( fd < 0 )
        return false;

    // build map of found dylibs
    std::unordered_map<std::string, const DyldSharedCache::MappedMachO*> currentDylibMap;
    for (const DyldSharedCache::MappedMachO& aFile : currentDylibs) {
        //fprintf(stderr, "0x%0llX 0x%0llX  %s\n", aFile.inode, aFile.modTime, aFile.runtimePath.c_str());
        currentDylibMap[aFile.runtimePath] = &aFile;
    }

    // make sure all dylibs in existing cache have same mtime and inode as found dylib
    __block bool foundMismatch = false;
    const uint64_t cacheMapLen = 0x40000000;
    void *p = ::mmap(NULL, cacheMapLen, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( p != MAP_FAILED ) {
        const DyldSharedCache* cache = (DyldSharedCache*)p;
        cache->forEachImageEntry(^(const char* installName, uint64_t mTime, uint64_t inode) {
            bool foundMatch = false;
            auto pos = currentDylibMap.find(installName);
            if ( pos != currentDylibMap.end() ) {
                const DyldSharedCache::MappedMachO* foundDylib = pos->second;
                if ( (foundDylib->inode == inode) && (foundDylib->modTime == mTime) ) {
                    foundMatch = true;
                }
            }
            if ( !foundMatch ) {
                // use slow path and look for any dylib with a matching inode and mtime
                bool foundSlow = false;
                for (const DyldSharedCache::MappedMachO& aFile : currentDylibs) {
                    if ( (aFile.inode == inode) && (aFile.modTime == mTime) ) {
                        foundSlow = true;
                        break;
                    }
                }
                if ( !foundSlow ) {
                    foundMismatch = true;
                    if ( verbose )
                        fprintf(stderr, "rebuilding dyld cache because dylib changed: %s\n", installName);
                }
            }
         });
        ::munmap(p, cacheMapLen);
    }

    ::close(fd);

    return !foundMismatch;
}


inline uint32_t absolutetime_to_milliseconds(uint64_t abstime)
{
    return (uint32_t)(abstime/1000/1000);
}


#define TERMINATE_IF_LAST_ARG( s )      \
    do {                                \
        if ( i == argc - 1 ) {          \
            fprintf(stderr, s );        \
            return 1;                   \
        }                               \
    } while ( 0 )

int main(int argc, const char* argv[])
{
    std::string                     rootPath;
    std::string                     dylibListFile;
    bool                            force = false;
    std::string                     cacheDir;
    std::unordered_set<std::string> archStrs;

    dyld3::Platform platform = dyld3::Platform::iOS;

    // parse command line options
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "-debug") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-verbose") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-tvOS") == 0) {
            platform = dyld3::Platform::tvOS;
        }
        else if (strcmp(arg, "-iOS") == 0) {
            platform = dyld3::Platform::iOS;
        }
        else if (strcmp(arg, "-watchOS") == 0) {
            platform = dyld3::Platform::watchOS;
        }
        else if ( strcmp(arg, "-runtime_dir") == 0 ) {
            TERMINATE_IF_LAST_ARG("-runtime_dir missing path argument\n");
            rootPath = argv[++i];
        }
        else if (strcmp(arg, "-cache_dir") == 0) {
            TERMINATE_IF_LAST_ARG("-cache_dir missing path argument\n");
            cacheDir = argv[++i];
        }
        else if (strcmp(arg, "-arch") == 0) {
            TERMINATE_IF_LAST_ARG("-arch missing argument\n");
            archStrs.insert(argv[++i]);
        }
        else if (strcmp(arg, "-force") == 0) {
            force = true;
        }
        else {
            //usage();
            fprintf(stderr, "update_dyld_sim_shared_cache: unknown option: %s\n", arg);
            return 1;
        }
    }

    if ( cacheDir.empty() ) {
        fprintf(stderr, "missing -cache_dir <path> option to specify directory in which to write cache file(s)\n");
        return 1;
    }

    if ( rootPath.empty() ) {
        fprintf(stderr, "missing -runtime_dir <path> option to specify directory which is root of simulator runtime)\n");
        return 1;
    }
    else {
        // canonicalize rootPath
        char resolvedPath[PATH_MAX];
        if ( realpath(rootPath.c_str(), resolvedPath) != NULL ) {
            rootPath = resolvedPath;
        }
    }

    int err = mkpath_np(cacheDir.c_str(), S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH);
    if ( (err != 0) && (err != EEXIST) ) {
        fprintf(stderr, "mkpath_np fail: %d", err);
        return 1;
    }

    if ( archStrs.empty() ) {
        switch ( platform ) {
            case dyld3::Platform::iOS:
                archStrs.insert("x86_64");
                break;
            case dyld3::Platform::tvOS:
                archStrs.insert("x86_64");
                break;
            case dyld3::Platform::watchOS:
                archStrs.insert("i386");
                break;
             case dyld3::Platform::unknown:
             case dyld3::Platform::macOS:
                assert(0 && "macOS does not have a simulator");
                break;
             case dyld3::Platform::bridgeOS:
                assert(0 && "bridgeOS does not have a simulator");
                break;
       }
    }

    uint64_t t1 = mach_absolute_time();

    // find all mach-o files for requested architectures
    __block std::vector<MappedMachOsByCategory> allFileSets;
    if ( archStrs.count("x86_64") )
        allFileSets.push_back({"x86_64"});
    if ( archStrs.count("i386") )
        allFileSets.push_back({"i386"});
    findAllFiles(rootPath, platform, allFileSets);
    addMacOSAdditions(allFileSets);
    for (MappedMachOsByCategory& fileSet : allFileSets) {
        pruneCachedDylibs(rootPath, fileSet);
    }

    uint64_t t2 = mach_absolute_time();

    fprintf(stderr, "time to scan file system and construct lists of mach-o files: %ums\n", absolutetime_to_milliseconds(t2-t1));

    // build all caches in parallel
    __block bool cacheBuildFailure = false;
    dispatch_apply(allFileSets.size(), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(size_t index) {
        MappedMachOsByCategory& fileSet = allFileSets[index];
        const std::string outFile = cacheDir + "/dyld_shared_cache_" + fileSet.archName;
        __block std::unordered_set<std::string> knownMissingDylib;

        DyldSharedCache::MappedMachO (^loader)(const std::string&) = ^DyldSharedCache::MappedMachO(const std::string& runtimePath) {
            std::string fullPath = rootPath + runtimePath;
            struct stat statBuf;
            if ( stat(fullPath.c_str(), &statBuf) == 0 ) {
                std::vector<MappedMachOsByCategory> mappedFiles;
                mappedFiles.push_back({fileSet.archName});
                if ( addIfMachO(rootPath, runtimePath, statBuf, platform, mappedFiles) ) {
                    if ( !mappedFiles.back().dylibsForCache.empty() )
                        return mappedFiles.back().dylibsForCache.back();
                }
            }
            if ( knownMissingDylib.count(runtimePath) == 0 ) {
                fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s could not use in dylid cache: %s\n", fileSet.archName.c_str(), runtimePath.c_str());
                knownMissingDylib.insert(runtimePath);
            }
            return DyldSharedCache::MappedMachO();
        };
        size_t startCount = fileSet.dylibsForCache.size();
        std::vector<std::pair<DyldSharedCache::MappedMachO, std::set<std::string>>> excludes;
        DyldSharedCache::verifySelfContained(fileSet.dylibsForCache, loader, excludes);
        for (size_t i=startCount; i < fileSet.dylibsForCache.size(); ++i) {
            fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s not found in initial scan, but adding required dylib %s\n", fileSet.archName.c_str(), fileSet.dylibsForCache[i].runtimePath.c_str());
        }
        for (auto& exclude : excludes) {
            std::string reasons = "(\"";
            for (auto i = exclude.second.begin(); i != exclude.second.end(); ++i) {
                reasons += *i;
                if (i != --exclude.second.end()) {
                    reasons += "\", \"";
                }
            }
            reasons += "\")";
            fprintf(stderr, "update_dyld_shared_cache: warning: %s rejected from cached dylibs: %s (%s)\n", fileSet.archName.c_str(), exclude.first.runtimePath.c_str(), reasons.c_str());
            fileSet.otherDylibsAndBundles.push_back(exclude.first);
        }

        // check if cache is already up to date
        if ( !force ) {
            if ( existingCacheUpToDate(outFile, fileSet.dylibsForCache) )
                return;
        }
        fprintf(stderr, "make %s cache with %lu dylibs, %lu other dylibs, %lu programs\n", fileSet.archName.c_str(), fileSet.dylibsForCache.size(), fileSet.otherDylibsAndBundles.size(), fileSet.mainExecutables.size());

        // build cache new cache file
        DyldSharedCache::CreateOptions options;
        options.archName                     = fileSet.archName;
        options.platform                     = platform;
        options.excludeLocalSymbols          = false;
        options.optimizeStubs                = false;
        options.optimizeObjC                 = true;
        options.codeSigningDigestMode        = DyldSharedCache::SHA256only;
        options.dylibsRemovedDuringMastering = false;
        options.inodesAreSameAsRuntime       = true;
        options.cacheSupportsASLR            = false;
        options.forSimulator                 = true;
        options.verbose                      = verbose;
        options.evictLeafDylibsOnOverflow    = true;
        options.pathPrefixes                 = { rootPath };
        DyldSharedCache::CreateResults results = DyldSharedCache::create(options, fileSet.dylibsForCache, fileSet.otherDylibsAndBundles, fileSet.mainExecutables);

        // print any warnings
        for (const std::string& warn : results.warnings) {
            fprintf(stderr, "update_dyld_sim_shared_cache: warning: %s %s\n", fileSet.archName.c_str(), warn.c_str());
        }
        if ( !results.errorMessage.empty() ) {
            // print error (if one)
            fprintf(stderr, "update_dyld_sim_shared_cache: %s\n", results.errorMessage.c_str());
            cacheBuildFailure = true;
        }
        else {
            // save new cache file to disk and write new .map file
            assert(results.cacheContent != nullptr);
            if ( !safeSave(results.cacheContent, results.cacheLength, outFile) )
                cacheBuildFailure = true;
            if ( !cacheBuildFailure ) {
                std::string mapStr = results.cacheContent->mapFile();
                std::string outFileMap = cacheDir + "/dyld_shared_cache_" + fileSet.archName + ".map";
                safeSave(mapStr.c_str(), mapStr.size(), outFileMap);
            }
            // free created cache buffer
            vm_deallocate(mach_task_self(), (vm_address_t)results.cacheContent, results.cacheLength);
        }
    });

    // we could unmap all input files, but tool is about to quit

    return (cacheBuildFailure ? 1 : 0);
}

