/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2018 Apple Inc. All rights reserved.
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

#include <fstream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>

#include "FileUtils.h"
#include "StringUtils.h"
#include "MachOFile.h"
#include "mrm_shared_cache_builder.h"

struct MappedFile
{
    std::string path;
    uint64_t    inode       = 0;
    uint64_t    mtime       = 0;
    const void* buffer      = nullptr;
    size_t      bufferSize  = 0;
};

static const char* sAllowedPrefixes[] = {
    "/usr/lib",
    "/System/Library",
// don't look at main executables until simulator supports dyld3
//    "/bin",
//    "/sbin",
};

static const char* sDontUsePrefixes[] = {
    "/usr/share",
    "/usr/local",
    "/usr/lib/system/introspection",
};

static const char* sMacOSHostLibs[] = {
    "/usr/lib/system/libsystem_kernel.dylib",
    "/usr/lib/system/libsystem_platform.dylib",
    "/usr/lib/system/libsystem_pthread.dylib",
};

static std::string getOrderFileContent(const std::string& orderFile)
{
    std::ifstream fstream(orderFile);
    std::stringstream stringBuf;
    stringBuf << fstream.rdbuf();
    return stringBuf.str();
}

struct CacheDylibID
{
    std::string installName;
    uint64_t    inode;
    uint64_t    mtime;
};

static void getCacheDylibIDs(const std::string& existingCache, std::vector<CacheDylibID>& cacheFiles)
{
    // if no existing cache, it is not up-to-date
    int fd = ::open(existingCache.c_str(), O_RDONLY);
    if ( fd < 0 )
        return;
    struct stat statbuf;
    if ( ::fstat(fd, &statbuf) == -1 ) {
        ::close(fd);
        return;
    }

    const uint64_t cacheMapLen = statbuf.st_size;
    void *p = ::mmap(NULL, cacheMapLen, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( p != MAP_FAILED ) {
        const DyldSharedCache* cache = (DyldSharedCache*)p;
        cache->forEachImageEntry(^(const char* installName, uint64_t mTime, uint64_t inode) {
            cacheFiles.push_back((CacheDylibID){ installName, inode, mTime });
        });
        ::munmap(p, cacheMapLen);
    }
    ::close(fd);
}

static bool allCachesUpToDate(const std::vector<const char*>& buildArchs, const std::string& cacheDir,
                              const std::vector<MappedFile>& mappedFiles, bool verbose)
{
    // Get all the inode/mtimes from the cache(s)
    std::vector<CacheDylibID> cacheDylibIDs;
    for ( const char* arch : buildArchs ) {
        std::string cachePath = cacheDir + "/" + "dyld_sim_shared_cache_" + arch;
        getCacheDylibIDs(cachePath, cacheDylibIDs);
    }

    if ( cacheDylibIDs.empty() )
        return false;

    // Get all the inode/mtimes from the on-disk files we've loaded
    struct FileID
    {
        uint64_t    inode;
        uint64_t    mtime;
    };
    std::unordered_map<std::string_view, FileID> onDiskFileIDs;
    for ( const MappedFile& mappedFile : mappedFiles )
        onDiskFileIDs[mappedFile.path] = { mappedFile.inode, mappedFile.mtime };

    // Compare to see if anything is out of date
    for ( CacheDylibID& cacheDylibID : cacheDylibIDs ) {
        auto it = onDiskFileIDs.find(cacheDylibID.installName);
        if ( it == onDiskFileIDs.end() ) {
            // The file is missing?  Should we rebuild?  Perhaps its a symlink
            continue;
        }

        if ( it->second.inode != cacheDylibID.inode ) {
            if ( verbose )
                fprintf(stderr, "rebuilding dyld cache because dylib changed: %s\n", cacheDylibID.installName.c_str());
            return false;
        }

        if ( it->second.mtime != cacheDylibID.mtime ) {
            if ( verbose )
                fprintf(stderr, "rebuilding dyld cache because dylib changed: %s\n", cacheDylibID.installName.c_str());
            return false;
        }
    }

    return true;
}

static std::vector<const char*> getArchs(std::unordered_set<std::string>& requestedArchs)
{
    std::unordered_set<std::string> allowedArchs;
    allowedArchs.insert("x86_64");
    allowedArchs.insert("arm64");

    if ( requestedArchs.empty() ) {
#if __arm64__
        requestedArchs.insert("arm64");
#elif __x86_64__
        requestedArchs.insert("x86_64");
#else
    #error unknown platform
#endif
    }

    std::vector<const char*> buildArchs;
    for ( auto& requested : requestedArchs ) {
        if ( allowedArchs.find(requested) != allowedArchs.end() ) {
            buildArchs.push_back(requested.c_str());
        }
    }

    return buildArchs;
}

static std::optional<MappedFile> loadFile(Diagnostics& diags, const char* path)
{
    int fd = ::open(path, O_RDONLY, 0);
    if ( fd == -1 ) {
        diags.error("can't open file '%s', errno=%d\n", path, errno);
        return std::nullopt;
    }

    struct stat statBuf;
    if ( fstat(fd, &statBuf) == -1 ) {
        diags.error("can't stat open file '%s', errno=%d\n", path, errno);
        ::close(fd);
        return std::nullopt;
    }

    const void* buffer = mmap(NULL, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( buffer == MAP_FAILED ) {
        diags.error("mmap() for file at %s failed, errno=%d\n", path, errno);
        ::close(fd);
        return std::nullopt;
    }
    ::close(fd);

    uint64_t inode = statBuf.st_ino;
    uint64_t mtime = statBuf.st_mtime;

    return (MappedFile){ path, inode, mtime, buffer, (size_t)statBuf.st_size };
}

static void unloadFile(const MappedFile& mappedFile)
{
    ::munmap((void*)mappedFile.buffer, mappedFile.bufferSize);
}

static void loadMRMFiles(Diagnostics& diags, struct MRMSharedCacheBuilder* sharedCacheBuilder,
                         const std::string& rootPath, std::vector<MappedFile>& mappedFiles)
{
    for (const char* path : sMacOSHostLibs) {
        std::optional<MappedFile> mappedFile = loadFile(diags, path);
        if ( !mappedFile.has_value() )
            continue;

        mappedFiles.push_back(mappedFile.value());

        addOnDiskFile(sharedCacheBuilder, path, (uint8_t*)mappedFile->buffer, mappedFile->bufferSize,
                      NoFlags, mappedFile->inode, mappedFile->mtime);
    }

    // Find files by walking the RuntimeRoot
    std::unordered_set<std::string> skipDirs;
    for (const char* s : sDontUsePrefixes)
        skipDirs.insert(s);

    // get all files from overlay for this search dir
    for (const char* searchDir : sAllowedPrefixes ) {
        iterateDirectoryTree(rootPath, searchDir,
                             ^(const std::string& dirPath) { return (skipDirs.count(dirPath) != 0); },
                             ^(const std::string& path, const struct stat& statBuf) {
            // ignore files that don't have 'x' bit set (all runnable mach-o files do)
            const bool hasXBit = ((statBuf.st_mode & S_IXOTH) == S_IXOTH);
            if ( !hasXBit && !endsWith(path, ".dylib") )
                return;

            // ignore files too small (must have at least a page of TEXT and LINKEDIT)
            if ( statBuf.st_size < 0x2000 )
                return;

            // if the file is mach-o, add to list
            std::string fullPath = rootPath + "/" + path;
            int fd = ::open(fullPath.c_str(), O_RDONLY, 0);
            if ( fd == -1 ) {
                // Should we warn here?  We might not are about arbitrary files not being able to be opened
                // diags.warning("can't open file '%s', errno=%d\n", path, errno);
                return;
            }

            const void* buffer = mmap(NULL, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if ( buffer == MAP_FAILED ) {
                // Should we warn here?  We might not are about arbitrary files not being able to be mapped
                // diags.warning("mmap() for file at %s failed, errno=%d\n", path.c_str(), errno);
                ::close(fd);
                return;
            }
            ::close(fd);

            uint64_t inode = statBuf.st_ino;
            uint64_t mtime = statBuf.st_mtime;
            mappedFiles.push_back((MappedFile){ path, inode, mtime, buffer, (size_t)statBuf.st_size });

            addOnDiskFile(sharedCacheBuilder, path.c_str(), (uint8_t*)buffer, (size_t)statBuf.st_size, NoFlags, inode, mtime);
        });
    }
}

static void unloadMRMFiles(const std::vector<MappedFile>& mappedFiles) {
    for ( const MappedFile& mappedFile : mappedFiles )
        unloadFile(mappedFile);
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

static bool writeMRMResults(bool cacheBuildSuccess, MRMSharedCacheBuilder* sharedCacheBuilder,
                            const std::string& dstRoot, bool verbose) {
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
            if ( (result.numErrors == 0) || verbose ) {
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
        return false;
    }

    // If we built caches, then write everything out.
    uint64_t fileResultCount = 0;
    if (const FileResult* const* fileResults = getFileResults(sharedCacheBuilder, &fileResultCount)) {
        for (uint64_t i = 0, e = fileResultCount; i != e; ++i) {
            const FileResult* fileResultPtr = fileResults[i];

            assert(fileResultPtr->version == 1);
            const FileResult_v1& fileResult = *(const FileResult_v1*)fileResultPtr;

            switch (fileResult.behavior) {
                case AddFile:
                    break;
                case ChangeFile:
                    continue;
            }

            // We don't have an FD on the sim caches.  See rdar://66598213
#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
#error Invalid config
#endif
            if ( fileResult.data != nullptr ) {
                const std::string path = dstRoot + "/" + fileResult.path;
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
        return false;
    }

    return true;
}

static dyld3::Platform getPlatform(Diagnostics& diags, std::string_view rootPath)
{
    // Infer the platform from dyld_sim
    std::string dyldSimPath = std::string(rootPath) + "/usr/lib/dyld_sim";

    std::optional<MappedFile> mappedFile = loadFile(diags, dyldSimPath.c_str());
    if ( !mappedFile.has_value() )
        return dyld3::Platform::unknown;

    __block dyld3::Platform platform = dyld3::Platform::unknown;
    if ( dyld3::FatFile::isFatFile(mappedFile->buffer) ) {
        const dyld3::FatFile* ff = (dyld3::FatFile*)mappedFile->buffer;
        ff->forEachSlice(diags, mappedFile->bufferSize,
                         ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
            const dyld3::MachOFile* mf = (dyld3::MachOFile*)sliceStart;
            mf->forEachSupportedPlatform(^(dyld3::Platform plat, uint32_t minOS, uint32_t sdk) {
                if ( platform == dyld3::Platform::unknown)
                    platform = plat;
            });
        });
    } else {
        const dyld3::MachOFile* mf = (dyld3::MachOFile*)mappedFile->buffer;
        if ( mf->isMachO(diags, mappedFile->bufferSize) ) {
            mf->forEachSupportedPlatform(^(dyld3::Platform plat, uint32_t minOS, uint32_t sdk) {
                if ( platform == dyld3::Platform::unknown)
                    platform = plat;
            });
        }
    }

    unloadFile(mappedFile.value());

    return platform;
}


#define TERMINATE_IF_LAST_ARG( s )      \
    do {                                \
        if ( i == argc - 1 ) {          \
            fprintf(stderr, s );        \
            return 1;                   \
        }                               \
    } while ( 0 )


int main(int argc, const char* argv[], const char* envp[])
{
    std::string                     rootPath;
    bool                            verbose = false;
    bool                            timePasses = false;
    bool                            printStats = false;
    bool                            force = false;
    bool                            dylibsRemoved = false;
    std::string                     cacheDir;
    std::string                     dylibOrderFile;
    std::string                     dirtyDataOrderFile;
    std::unordered_set<std::string> skipDylibs;
    std::unordered_set<std::string> requestedArchs;
    
    // parse command line options
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "-debug") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-verbose") == 0) {
            verbose = true;
        }
        else if (strcmp(arg, "-time-passes") == 0) {
            timePasses = true;
        }
        else if (strcmp(arg, "-stats") == 0) {
            printStats = true;
        }
        else if ((strcmp(arg, "-root") == 0) || (strcmp(arg, "--root") == 0)) {
            TERMINATE_IF_LAST_ARG("-root missing path argument\n");
            rootPath = argv[++i];
        }
        else if (strcmp(arg, "-cache_dir") == 0) {
            TERMINATE_IF_LAST_ARG("-cache_dir missing path argument\n");
            cacheDir = argv[++i];
        }
        else if (strcmp(arg, "-iOS") == 0) {
            // Unused.  We infer the platform from dyld_sim now
        }
        else if (strcmp(arg, "-watchOS") == 0) {
            // Unused.  We infer the platform from dyld_sim now
        }
        else if (strcmp(arg, "-tvOS") == 0) {
            // Unused.  We infer the platform from dyld_sim now
        }
        else if (strcmp(arg, "-dylibs_removed_in_mastering") == 0) {
            dylibsRemoved = true;
        }
        else if (strcmp(arg, "-dylib_order_file") == 0) {
            TERMINATE_IF_LAST_ARG("-dylib_order_file missing path argument\n");
            dylibOrderFile = argv[++i];
        }
        else if (strcmp(arg, "-dirty_data_order_file") == 0) {
            TERMINATE_IF_LAST_ARG("-dirty_data_order_file missing path argument\n");
            dirtyDataOrderFile = argv[++i];
        }
        else if (strcmp(arg, "-arch") == 0) {
            TERMINATE_IF_LAST_ARG("-arch missing arch argument\n");
            requestedArchs.insert(argv[++i]);
        }
        else if (strcmp(arg, "-force") == 0) {
            force = true;
        }
        else if (strcmp(arg, "-skip") == 0) {
            TERMINATE_IF_LAST_ARG("-skip missing argument\n");
            skipDylibs.insert(argv[++i]);
        }
        else {
            //usage();
            fprintf(stderr, "update_dyld_sim_shared_cache: unknown option: %s\n", arg);
            return 1;
        }
    }
    
    if ( rootPath.empty() ) {
        fprintf(stderr, "-root should be specified\n");
        return 1;
    }
    if ( cacheDir.empty() ) {
        fprintf(stderr, "-cache_dir should be specified\n");
        return 1;
    }
    // canonicalize rootPath
    char resolvedPath[PATH_MAX];
    if ( realpath(rootPath.c_str(), resolvedPath) != NULL ) {
        rootPath = resolvedPath;
    }
    
    // canonicalize cacheDir.
    // Later, path is checked against real path name before writing cache file to avoid TOCTU race condition.
    if ( realpath(cacheDir.c_str(), resolvedPath) != NULL ) {
        cacheDir = resolvedPath;
    }

    // Find the boot volume so that we can ensure all overlays are on the same volume
    struct stat rootStatBuf;
    if ( stat(rootPath.c_str(), &rootStatBuf) != 0 ) {
        fprintf(stderr, "update_dyld_sim_shared_cache: error: could not stat root file system because '%s'\n", strerror(errno));
        return 1;
    }

    int err = mkpath_np(cacheDir.c_str(), S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH);
    if ( (err != 0) && (err != EEXIST) ) {
        fprintf(stderr, "update_dyld_sim_shared_cache: could not access cache dir: mkpath_np(%s) failed errno=%d\n", cacheDir.c_str(), err);
        return 1;
    }

    std::vector<const char*> buildArchs = getArchs(requestedArchs);
    if ( buildArchs.empty() ) {
        fprintf(stderr, "update_dyld_sim_shared_cache: error: no valid architecture specified\n");
        return 1;
    }

    // The platform comes from dyld_sim now
    Diagnostics diags;
    dyld3::Platform platform = getPlatform(diags, rootPath);
    if ( diags.hasError() ) {
        fprintf(stderr, "update_dyld_sim_shared_cache: error: could not find sim platform because: %s\n",
                diags.errorMessageCStr());
        return -1;
    }

    // Make a cache builder and run it
    BuildOptions_v3 buildOptions;
    buildOptions.version                            = 3;
    buildOptions.updateName                         = "sim";
    buildOptions.deviceName                         = "sim";
    buildOptions.disposition                        = InternalMinDevelopment;
    buildOptions.platform                           = (Platform)platform;
    buildOptions.archs                              = buildArchs.data();
    buildOptions.numArchs                           = buildArchs.size();
    buildOptions.verboseDiagnostics                 = verbose;
    buildOptions.isLocallyBuiltCache                = true;
    buildOptions.optimizeForSize                    = false;
    buildOptions.filesRemovedFromDisk               = false;
    buildOptions.timePasses                         = timePasses;
    buildOptions.printStats                         = printStats;

    struct MRMSharedCacheBuilder* sharedCacheBuilder = createSharedCacheBuilder((const BuildOptions_v1*)&buildOptions);

    std::vector<MappedFile> mappedFiles;

    {
        uint64_t startTimeNanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        loadMRMFiles(diags, sharedCacheBuilder, rootPath, mappedFiles);
        uint64_t endTimeNanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

        if ( buildOptions.timePasses ) {
            uint64_t timeMillis = (endTimeNanos - startTimeNanos) / 1000000;
            fprintf(stderr, "loadMRMFiles: time = %lldms\n", timeMillis);
        }
    }

    if ( diags.hasError() ) {
        fprintf(stderr, "update_dyld_sim_shared_cache: error: %s", diags.errorMessage().c_str());
        return -1;
    }

    std::string dylibOrderFileContent;
    if ( !dylibOrderFile.empty() ) {
        dylibOrderFileContent = getOrderFileContent(dylibOrderFile);
        addFile(sharedCacheBuilder, "dyld internal dirty data file", (uint8_t*)dylibOrderFileContent.data(), (size_t)dylibOrderFileContent.size(), DylibOrderFile);
    }

    std::string dirtyDataOrderFileContent;
    if ( !dirtyDataOrderFile.empty() ) {
        dirtyDataOrderFileContent = getOrderFileContent(dirtyDataOrderFile);
        addFile(sharedCacheBuilder, "dyld internal dirty data order file", (uint8_t*)dirtyDataOrderFileContent.data(), (size_t)dirtyDataOrderFileContent.size(), DirtyDataOrderFile);
    }

    // check if cache is already up to date
    if ( !force ) {
        if ( allCachesUpToDate(buildArchs, cacheDir, mappedFiles, verbose) )
            return 0;
    }

    bool cacheBuildSuccess = runSharedCacheBuilder(sharedCacheBuilder);

    writeMRMResults(cacheBuildSuccess, sharedCacheBuilder, cacheDir, verbose);

    destroySharedCacheBuilder(sharedCacheBuilder);

    unloadMRMFiles(mappedFiles);
    
    return (cacheBuildSuccess ? 0 : 1);
}

