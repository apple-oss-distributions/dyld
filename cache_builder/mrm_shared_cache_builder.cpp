/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
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

#include "mrm_shared_cache_builder.h"
#include "Defines.h"
#include "BuilderFileSystem.h"
#include "NewSharedCacheBuilder.h"
#include "Error.h"
#include "ClosureFileSystem.h"
#include "FileUtils.h"
#include "JSONReader.h"
#include "JSONWriter.h"
#include <pthread.h>
#include <memory>
#include <vector>
#include <map>
#include <sys/stat.h>


using cache_builder::CacheBuffer;
using cache_builder::SharedCacheBuilder;

using error::Error;

static const uint64_t kMinBuildVersion = 1; //The minimum version BuildOptions struct we can support
static const uint64_t kMaxBuildVersion = 3; //The maximum version BuildOptions struct we can support

static const uint32_t MajorVersion = 1;
static const uint32_t MinorVersion = 3;

struct BuildInstance {
    std::unique_ptr<cache_builder::BuilderOptions>  options;
    std::unique_ptr<SharedCacheBuilder>             builder;
    std::string                                     mainCacheFilePath;
    std::vector<const char*>                        errors;
    std::vector<const char*>                        warnings;
    std::vector<std::string>                        errorStrings;   // Owns the data for the errors
    std::vector<std::string>                        warningStrings; // Owns the data for the warnings
    std::vector<CacheBuffer>                        cacheBuffers;
    std::vector<std::string>                        cachePaths;     // Owns the data for the cache paths
    std::string                                     loggingPrefix;
    std::string                                     jsonMap;
    std::string                                     mainCacheUUID;
    std::optional<std::string>                      customerLoggingPrefix;
    std::optional<std::string>                      customerJsonMap;
    std::optional<std::string>                      customerMainCacheUUID;
    std::string                                     macOSMap;       // For compatibility with update_dyld_shared_cache's .map file
    std::string                                     macOSMapPath;   // Owns the string for the path
    std::string                                     cdHashType;     // Owns the data for the cdHashType
};

struct BuildFileResult {
    std::string                                 path;
    const uint8_t*                              data;
    uint64_t                                    size;
};

struct TranslationResult {
    const uint8_t*   data;
    size_t           size;
    std::string      cdHash;
    std::string      path;
    bool             bufferWasMalloced;
};

struct FileBuffer
{
    void*   buffer;
    size_t  size;
};

struct MappedBuffer
{
    void*       buffer;
    size_t      size;
    int         fd;
    std::string tempPath;
};

struct MRMSharedCacheBuilder {
    MRMSharedCacheBuilder(const BuildOptions_v1* options);
    const BuildOptions_v1*          options;
    cache_builder::FileSystemMRM    fileSystem;

    std::string dylibOrderFileData;
    std::string dirtyDataOrderFileData;
    void* objcOptimizationsFileData;
    size_t objcOptimizationsFileLength;

    // An array of builders and their options as we may have more than one builder for a given device variant.
    std::vector<BuildInstance> builders;

    // The paths in all of the caches
    // We keep this here to own the std::string path data
    std::map<std::string, std::unordered_set<const BuildInstance*>> dylibsInCaches;

    // The results from all of the builders
    // We keep this in a vector to own the data.
    std::vector<FileResult*>                     fileResults;

#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
    // The builder dylib passes back buffers
    std::vector<FileResult_v1>                   fileResultStorage_v1;
#else
    // The builder executable gets file descriptors
    std::vector<FileResult_v2>                   fileResultStorage_v2;
#endif

    // Buffers which were malloc()ed and need free()d
    std::vector<FileBuffer> buffersToFree;

    // Buffers which were vm_allocate()d, and need vm_deallocate()d
    std::vector<FileBuffer> buffersToDeallocate;

#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
    // Buffers which were open()ed and mmap()ed
    std::vector<MappedBuffer> buffersToUnmap;
#endif

    // The results from all of the builders
    // We keep this in a vector to own the data.
    std::vector<CacheResult*>    cacheResults;
    std::vector<CacheResult>     cacheResultStorage;


    // The files to remove.  These are in every copy of the caches we built
    std::vector<const char*> filesToRemove;

    std::vector<const char*> errors;
    std::vector<std::string> errorStorage;
    std::vector<std::string> warningsStorage;
    pthread_mutex_t lock;

    enum State {
        AcceptingFiles,
        Building,
        FinishedBuilding
    };

    State state = AcceptingFiles;

    void runSync(void (^block)()) {
        pthread_mutex_lock(&lock);
        block();
        pthread_mutex_unlock(&lock);
    }

    __attribute__((format(printf, 2, 3)))
    void error(const char* format, ...) {
        va_list list;
        va_start(list, format);
        Diagnostics diag;
        diag.error(format, list);
        va_end(list);

        errorStorage.push_back(diag.errorMessage());
        errors.push_back(errorStorage.back().data());
    }

    __attribute__((format(printf, 2, 3)))
    void warning(const char* format, ...) {
        va_list list;
        va_start(list, format);
        Diagnostics diag;
        diag.error(format, list);
        va_end(list);

        warningsStorage.push_back(diag.errorMessage());
    }
};

MRMSharedCacheBuilder::MRMSharedCacheBuilder(const BuildOptions_v1* options)
: options(options)
, objcOptimizationsFileData(nullptr)
, objcOptimizationsFileLength(0)
, lock(PTHREAD_MUTEX_INITIALIZER)
{

}

static void validateBuildOptions(const BuildOptions_v1* options, MRMSharedCacheBuilder& builder) {
    if (options->version < kMinBuildVersion) {
        builder.error("Builder version %llu is less than minimum supported version of %llu", options->version, kMinBuildVersion);
    }
    if (options->version > kMaxBuildVersion) {
        builder.error("Builder version %llu is greater than maximum supported version of %llu", options->version, kMaxBuildVersion);
    }
    if (!options->updateName) {
        builder.error("updateName must not be null");
    }
    if (!options->deviceName) {
        builder.error("deviceName must not be null");
    }
    switch (options->disposition) {
        case Disposition::Unknown:
        case Disposition::InternalDevelopment:
        case Disposition::Customer:
        case Disposition::InternalMinDevelopment:
            break;
        default:
            builder.error("unknown disposition value");
            break;
    }
    if ( options->platform == Platform::unknown ) {
        builder.error("platform must not be unknown");
    }
    if (!options->archs) {
        builder.error("archs must not be null");
    }
    if (!options->numArchs) {
        builder.error("numArchs must not be 0");
    }
}

void getVersion(uint32_t *major, uint32_t *minor) {
    *major = MajorVersion;
    *minor = MinorVersion;
}

struct MRMSharedCacheBuilder* createSharedCacheBuilder(const BuildOptions_v1* options) {
    MRMSharedCacheBuilder* builder = new MRMSharedCacheBuilder(options);

    // Check the option struct values are valid
    validateBuildOptions(options, *builder);

    return builder;
}

bool addFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags) {
    return addOnDiskFile(builder, path, data, size, fileFlags, 0, 0);
}

bool addOnDiskFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags,
                   uint64_t inode, uint64_t modTime) {
    __block bool success = false;
    builder->runSync(^() {
        if (builder->state != MRMSharedCacheBuilder::AcceptingFiles) {
            builder->error("Cannot add file: '%s' as we have already started building", path);
            return;
        }
        size_t pathLength = strlen(path);
        if (pathLength == 0) {
            builder->error("Empty path");
            return;
        }
        if (pathLength >= MAXPATHLEN) {
            builder->error("Path is too long: '%s'", path);
            return;
        }
        if (data == nullptr) {
            builder->error("Data cannot be null for file: '%s'", path);
            return;
        }
        switch (fileFlags) {
            case NoFlags:
            case MustBeInCache:
            case ShouldBeExcludedFromCacheIfUnusedLeaf:
            case RequiredClosure:
                break;
            case DylibOrderFile:
                builder->dylibOrderFileData = std::string((char*)data, size);
                success = true;
                return;
            case DirtyDataOrderFile:
                builder->dirtyDataOrderFileData = std::string((char*)data, size);
                success = true;
                return;
            case ObjCOptimizationsFile:
                builder->objcOptimizationsFileData = data;
                builder->objcOptimizationsFileLength = size;
                success = true;
                return;
            default:
                builder->error("unknown file flags value");
                break;
        }
        Diagnostics diag;
        if (!builder->fileSystem.addFile(path, data, size, diag, fileFlags, inode, modTime)) {
            builder->errorStorage.push_back(diag.errorMessage());
            builder->errors.push_back(builder->errorStorage.back().data());
            return;
        }
        success = true;
    });
    return success;
}

bool addSymlink(struct MRMSharedCacheBuilder* builder, const char* fromPath, const char* toPath) {
    __block bool success = false;
    builder->runSync(^() {
        if (builder->state != MRMSharedCacheBuilder::AcceptingFiles) {
            builder->error("Cannot add file: '%s' as we have already started building", fromPath);
            return;
        }
        size_t pathLength = strlen(fromPath);
        if (pathLength == 0) {
            builder->error("Empty path");
            return;
        }
        if (pathLength >= MAXPATHLEN) {
            builder->error("Path is too long: '%s'", fromPath);
            return;
        }
        Diagnostics diag;
        if (!builder->fileSystem.addSymlink(fromPath, toPath, diag)) {
            builder->errorStorage.push_back(diag.errorMessage());
            builder->errors.push_back(builder->errorStorage.back().data());
            return;
        }
        success = true;
    });
    return success;
}

static cache_builder::LocalSymbolsMode platformExcludeLocalSymbols(Platform platform) {
    if ( dyld3::MachOFile::isSimulatorPlatform((dyld3::Platform)platform) )
        return cache_builder::LocalSymbolsMode::keep;
    if ( (platform == Platform::macOS) || (platform == Platform::iOSMac) )
        return cache_builder::LocalSymbolsMode::keep;
    // Everything else is based on iOS so just use that value
    return cache_builder::LocalSymbolsMode::unmap;
}

static cache_builder::LocalSymbolsMode excludeLocalSymbols(const BuildOptions_v1* options) {
    if ( options->version >= 2 ) {
        const BuildOptions_v2* v2 = (const BuildOptions_v2*)options;
        if ( v2->optimizeForSize )
            return cache_builder::LocalSymbolsMode::strip;
    }

    // Old build options always use the platform default
    return platformExcludeLocalSymbols(options->platform);
}

static DyldSharedCache::CodeSigningDigestMode platformCodeSigningDigestMode(Platform platform) {
    if ( platform == Platform::watchOS )
        return DyldSharedCache::Agile;
    return DyldSharedCache::SHA256only;
}

static const char* dispositionName(Disposition disposition) {
    switch (disposition) {
        case Disposition::Unknown:
            return "";
        case Disposition::InternalDevelopment:
            return "Internal";
        case Disposition::Customer:
            return "Customer";
        case Disposition::InternalMinDevelopment:
            return "InternalMinDevelopment";
    }
}

static bool filesRemovedFromDisk(const BuildOptions_v1* options) {
    // Old builds are platforms which always remove files from disk
    if ( options->version < 3 ) {
        return true;
    }

    const BuildOptions_v3* v3 = (const BuildOptions_v3*)options;
    return v3->filesRemovedFromDisk;
}

static bool timePasses(const BuildOptions_v1* options) {
    // Old builds just use the verbose flags
    if ( options->version < 3 ) {
        return options->verboseDiagnostics;
    }

    const BuildOptions_v3* v3 = (const BuildOptions_v3*)options;
    return v3->timePasses;
}

static bool printStats(const BuildOptions_v1* options) {
    // Old builds just use the verbose flags
    if ( options->version < 3 ) {
        return options->verboseDiagnostics;
    }

    const BuildOptions_v3* v3 = (const BuildOptions_v3*)options;
    return v3->printStats;
}

// This is a JSON file containing the list of classes for which
// we should try to build IMP caches.
static dyld3::json::Node parseObjcOptimizationsFile(Diagnostics& diags, const void* data, size_t length) {
    if ( data == nullptr )
        return dyld3::json::Node();
    return dyld3::json::readJSON(diags, data, length);
}

static cache_builder::CacheKind getCacheKind(const BuildOptions_v1* options)
{
    // Work out what kind of cache we are building.  macOS/driverKit are always development
    if ( (options->platform == macOS) || (options->platform == driverKit) )
        return cache_builder::CacheKind::development;

    // Sims are always development
    if ( dyld3::MachOFile::isSimulatorPlatform((dyld3::Platform)options->platform) )
        return cache_builder::CacheKind::development;

    // iOS is always universal. If building for InternalMinDevelopment, we'll build universal
    // anyway, then throw away the development pieces
    return cache_builder::CacheKind::universal;
}

static bool shouldEmitDevelopmentCache(const BuildOptions_v1* options)
{
    // Filter dev/customer based on the cache kind and disposition
    switch ( getCacheKind(options) ) {
        case cache_builder::CacheKind::development:
            return true;
        case cache_builder::CacheKind::universal:
            break;
    }

    switch ( options->disposition ) {
        case Disposition::Unknown:
        case Disposition::InternalDevelopment:
            return true;
        case Disposition::Customer:
            return false;
        case Disposition::InternalMinDevelopment:
            return true;
    }

    return true;
}

static bool shouldEmitCustomerCache(const BuildOptions_v1* options)
{
    // Filter dev/customer based on the cache kind and disposition
    switch ( getCacheKind(options) ) {
        case cache_builder::CacheKind::development:
            return false;
        case cache_builder::CacheKind::universal:
            break;
    }

    switch ( options->disposition ) {
        case Disposition::Unknown:
        case Disposition::InternalDevelopment:
            return true;
        case Disposition::Customer:
            return true;
        case Disposition::InternalMinDevelopment:
            return false;
    }

    return true;
}

static bool createBuilders(struct MRMSharedCacheBuilder* builder)
{
    if (builder->state != MRMSharedCacheBuilder::AcceptingFiles) {
        builder->error("Builder has already been run");
        return false;
    }
    builder->state = MRMSharedCacheBuilder::Building;
    if (builder->fileSystem.fileCount() == 0) {
        builder->error("Cannot run builder with no files");
    }

    auto symlinkResolverError = ^(const std::string& error) {
        builder->warning("%s", error.c_str());
    };
    std::vector<cache_builder::FileAlias> aliases = builder->fileSystem.getResolvedSymlinks(symlinkResolverError);
    std::vector<cache_builder::FileAlias> intermediateAliases = builder->fileSystem.getIntermediateSymlinks();

    if (!builder->errors.empty()) {
        builder->error("Skipping running shared cache builder due to previous errors");
        return false;
    }

    // Enqueue a cache for each configuration
    Diagnostics diag;
    for (uint64_t i = 0; i != builder->options->numArchs; ++i) {
        // HACK: Skip i386 for macOS
        if ( strcmp(builder->options->archs[i], "i386") == 0 )
            continue;

        // Add a driverKit suffix.  Note we don't need to add .development suffixes any
        // more as the universal caches don't build customer and development seperately
        const char *loggingSuffix = "";
        if ( builder->options->platform == Platform::driverKit )
            loggingSuffix = ".driverKit";

        std::string loggingPrefix = "";
        loggingPrefix += std::string(builder->options->deviceName);
        loggingPrefix += dispositionName(builder->options->disposition);
        loggingPrefix += std::string(".") + builder->options->archs[i];
        loggingPrefix += loggingSuffix;

        std::string runtimePath;
        if ( dyld3::MachOFile::isSimulatorPlatform((dyld3::Platform)builder->options->platform) ) {
            // Sim caches are written exactly where instructed, without adding any directory structure
            runtimePath = runtimePath + "dyld_sim_shared_cache_" + builder->options->archs[i];
        } else {
            if ( builder->options->platform == Platform::macOS )
                runtimePath = MACOSX_MRM_DYLD_SHARED_CACHE_DIR;
            else if ( builder->options->platform == Platform::driverKit )
                runtimePath = DRIVERKIT_DYLD_SHARED_CACHE_DIR;
            else
                runtimePath = IPHONE_DYLD_SHARED_CACHE_DIR;
            runtimePath = runtimePath + "dyld_shared_cache_" + builder->options->archs[i];
        }

        bool dylibsRemovedFromDisk  = filesRemovedFromDisk(builder->options);
        bool isLocallyBuiltCache    = builder->options->isLocallyBuiltCache;

        cache_builder::CacheKind cacheKind = getCacheKind(builder->options);

        // If we have a universal cache, but min development disposition, then we want dev
        // caches only, and should change the names to match.
        // This also lets us install a dev cache on top of a universal one, without breaking
        // the customer bits in the universal cache
        bool forceDevelopmentSubCacheSuffix = false;
        if ( (cacheKind == cache_builder::CacheKind::universal)
            && (builder->options->disposition == InternalMinDevelopment) )
            forceDevelopmentSubCacheSuffix = true;

        auto options = std::make_unique<cache_builder::BuilderOptions>(builder->options->archs[i],
                                                                       (dyld3::Platform)builder->options->platform,
                                                                       dylibsRemovedFromDisk, isLocallyBuiltCache,
                                                                       cacheKind, forceDevelopmentSubCacheSuffix);

        options->logPrefix                   = loggingPrefix;
        options->timePasses                  = timePasses(builder->options);
        options->stats                       = printStats(builder->options);
        options->dylibOrdering               = parseOrderFile(builder->dylibOrderFileData);
        options->dirtyDataSegmentOrdering    = parseOrderFile(builder->dirtyDataOrderFileData);
        options->objcOptimizations           = parseObjcOptimizationsFile(diag, builder->objcOptimizationsFileData,
                                                                          builder->objcOptimizationsFileLength);
        options->localSymbolsMode            = excludeLocalSymbols(builder->options);

        BuildInstance buildInstance;
        buildInstance.options           = std::move(options);
        buildInstance.builder           = std::make_unique<SharedCacheBuilder>(*buildInstance.options.get(), builder->fileSystem);
        buildInstance.mainCacheFilePath = runtimePath;

        builder->builders.push_back(std::move(buildInstance));
    }

    // Add all the input files
    __block std::vector<cache_builder::InputFile> inputFiles;
    builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                          FileFlags fileFlags, uint64_t inode, uint64_t modTime) {
        switch (fileFlags) {
            case FileFlags::NoFlags:
            case FileFlags::MustBeInCache:
            case FileFlags::ShouldBeExcludedFromCacheIfUnusedLeaf:
            case FileFlags::RequiredClosure:
                break;
            case FileFlags::DylibOrderFile:
            case FileFlags::DirtyDataOrderFile:
            case FileFlags::ObjCOptimizationsFile:
                builder->error("Order files should not be in the file system");
                return;
        }

        for (auto& buildInstance : builder->builders) {
            SharedCacheBuilder* cacheBuilder = buildInstance.builder.get();
            cacheBuilder->addFile(buffer, bufferSize, path, inode, modTime);
        }
    });

    // Add resolved aliases (symlinks)
    for (auto& buildInstance : builder->builders) {
        SharedCacheBuilder* cacheBuilder = buildInstance.builder.get();
        cacheBuilder->setAliases(aliases, intermediateAliases);
    }

    return true;
}

static void runBuilders(struct MRMSharedCacheBuilder* builder)
{
    for (auto& buildInstance : builder->builders) {
        std::unique_ptr<SharedCacheBuilder> cacheBuilder = std::move(buildInstance.builder);
        Error error = cacheBuilder->build();

        buildInstance.loggingPrefix = cacheBuilder->developmentLoggingPrefix();
        buildInstance.customerLoggingPrefix = cacheBuilder->customerLoggingPrefix();

        // Get result buffers, even if there's an error.  That way we'll free them
        cacheBuilder->getResults(buildInstance.cacheBuffers);

        // Track all buffers to be freed/unmapped (see allocateSubCacheBuffers() for allocation)
        for ( const CacheBuffer& buffer : buildInstance.cacheBuffers ) {
#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
            // In MRM, we vm_allocated
            builder->buffersToDeallocate.emplace_back((FileBuffer){ buffer.bufferData, buffer.bufferSize });
#else
            // In the local builder, we mmap()ed
            builder->buffersToUnmap.emplace_back((MappedBuffer) {
                buffer.bufferData,
                buffer.bufferSize,
                buffer.fd,
                buffer.tempPath
            });
#endif
        }

        if ( error.hasError() ) {
            // First put the error in to a vector to own it
            buildInstance.errorStrings.push_back(error.message());

            // Then copy to a vector to reference the owner
            buildInstance.errors.reserve(buildInstance.errorStrings.size());
            for (const std::string& err : buildInstance.errorStrings)
                buildInstance.errors.push_back(err.c_str());

            // First put the warnings in to a vector to own them.
            if ( builder->options->verboseDiagnostics ) {
                cacheBuilder->forEachWarning(^(const std::string_view &str) {
                    buildInstance.warningStrings.push_back(std::string(str));
                });

                // Then copy to a vector to reference the owner
                buildInstance.warnings.reserve(buildInstance.warningStrings.size());
                for ( const std::string& warning : buildInstance.warningStrings )
                    buildInstance.warnings.push_back(warning.c_str());
            }
        } else {
            // Successfully built a cache

            // Remove buffers we don't need
            bool needDevelopmentCaches = shouldEmitDevelopmentCache(builder->options);
            bool needCustomerCaches = shouldEmitCustomerCache(builder->options);
            std::erase_if(buildInstance.cacheBuffers, [&](const CacheBuffer& buffer) {
                if ( needDevelopmentCaches && buffer.usedByDevelopmentConfig )
                    return false;

                if ( needCustomerCaches && buffer.usedByCustomerConfig )
                    return false;

                return true;
            });

            if ( (buildInstance.options->platform == dyld3::Platform::macOS)
                || dyld3::MachOFile::isSimulatorPlatform(buildInstance.options->platform) ) {
                // For compatibility with update_dyld_shared_cache/update_dyld_sim_shared_cache, put a .map file next to the shared cache
                buildInstance.macOSMap = cacheBuilder->getMapFileBuffer();
                buildInstance.macOSMapPath = buildInstance.mainCacheFilePath + ".map";
            }

            buildInstance.jsonMap = cacheBuilder->developmentJSONMap(builder->options->deviceName);
            buildInstance.mainCacheUUID = cacheBuilder->developmentCacheUUID();

            // If building for universal, we'll have customer JSON maps and UUID
            buildInstance.customerJsonMap = cacheBuilder->customerJSONMap(builder->options->deviceName);
            buildInstance.customerMainCacheUUID = cacheBuilder->customerCacheUUID();

            // Only add warnings if the build was good
            // First put the warnings in to a vector to own them.
            cacheBuilder->forEachWarning(^(const std::string_view &str) {
                buildInstance.warningStrings.push_back(std::string(str));
            });

            // Then copy to a vector to reference the owner
            buildInstance.warnings.reserve(buildInstance.warningStrings.size());
            for ( const std::string& warning : buildInstance.warningStrings )
                buildInstance.warnings.push_back(warning.c_str());

            switch ( platformCodeSigningDigestMode(builder->options->platform) ) {
                case DyldSharedCache::SHA256only:
                    buildInstance.cdHashType = "sha256";
                    break;
                case DyldSharedCache::SHA1only:
                    buildInstance.cdHashType = "sha1";
                    break;
                case DyldSharedCache::Agile:
                    buildInstance.cdHashType = "sha1";
                    break;
            }

            // Track the dylibs which were included in this cache
            cacheBuilder->forEachCacheDylib(^(const std::string_view &path) {
                builder->dylibsInCaches[std::string(path)].insert(&buildInstance);
            });
            cacheBuilder->forEachCacheSymlink(^(const std::string_view &path) {
                builder->dylibsInCaches[std::string(path)].insert(&buildInstance);
            });
        }
    }
}


static void createBuildResults(struct MRMSharedCacheBuilder* builder)
{
    // Now that we have run all of the builds, collect the results
    // First push file results for each of the shared caches we built
    for (auto& buildInstance : builder->builders) {
        bool emittedWarningsAndErrors = false;
        if ( shouldEmitDevelopmentCache(builder->options) ) {
            CacheResult cacheBuildResult;
            cacheBuildResult.version                = 1;
            cacheBuildResult.loggingPrefix          = buildInstance.loggingPrefix.c_str();
            cacheBuildResult.deviceConfiguration    = buildInstance.loggingPrefix.c_str();
            cacheBuildResult.warnings               = buildInstance.warnings.empty() ? nullptr : buildInstance.warnings.data();
            cacheBuildResult.numWarnings            = buildInstance.warnings.size();
            cacheBuildResult.errors                 = buildInstance.errors.empty() ? nullptr : buildInstance.errors.data();
            cacheBuildResult.numErrors              = buildInstance.errors.size();
            cacheBuildResult.uuidString             = buildInstance.mainCacheUUID.empty() ? "" : buildInstance.mainCacheUUID.c_str();
            cacheBuildResult.mapJSON                = buildInstance.jsonMap.c_str();

            builder->cacheResultStorage.emplace_back(cacheBuildResult);

            emittedWarningsAndErrors = true;
        }

        if ( shouldEmitCustomerCache(builder->options) ) {
            CacheResult cacheBuildResult;
            cacheBuildResult.version              = 1;
            cacheBuildResult.loggingPrefix        = buildInstance.customerLoggingPrefix->c_str();
            cacheBuildResult.deviceConfiguration  = buildInstance.customerLoggingPrefix->c_str();
            cacheBuildResult.warnings             = nullptr;
            cacheBuildResult.numWarnings          = 0;
            cacheBuildResult.errors               = nullptr;
            cacheBuildResult.numErrors            = 0;
            cacheBuildResult.uuidString           = buildInstance.customerMainCacheUUID->empty() ? "" : buildInstance.customerMainCacheUUID->c_str();
            cacheBuildResult.mapJSON              = buildInstance.customerJsonMap->c_str();

            if ( !emittedWarningsAndErrors ) {
                cacheBuildResult.warnings         = buildInstance.warnings.empty() ? nullptr : buildInstance.warnings.data();
                cacheBuildResult.numWarnings      = buildInstance.warnings.size();
                cacheBuildResult.errors           = buildInstance.errors.empty() ? nullptr : buildInstance.errors.data();
                cacheBuildResult.numErrors        = buildInstance.errors.size();
            }

            builder->cacheResultStorage.emplace_back(cacheBuildResult);
        }

        if (!buildInstance.errors.empty())
            continue;

        for ( const CacheBuffer& buffer : buildInstance.cacheBuffers )
            buildInstance.cachePaths.push_back(buildInstance.mainCacheFilePath + buffer.cacheFileSuffix);

        uint32_t cacheIndex = 0;
        for ( const CacheBuffer& cacheBuffer : buildInstance.cacheBuffers ) {
#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
            FileResult_v1 cacheFileResult;
            cacheFileResult.version          = 1;
            cacheFileResult.path             = buildInstance.cachePaths[cacheIndex].c_str();
            cacheFileResult.behavior         = AddFile;
            cacheFileResult.data             = cacheBuffer.bufferData;
            cacheFileResult.size             = cacheBuffer.bufferSize;
            cacheFileResult.hashArch         = buildInstance.options->archs.name();
            cacheFileResult.hashType         = buildInstance.cdHashType.c_str();
            cacheFileResult.hash             = cacheBuffer.cdHash.c_str();

            builder->fileResultStorage_v1.emplace_back(cacheFileResult);
#else
            FileResult_v2 cacheFileResult;
            cacheFileResult.version          = 2;
            cacheFileResult.path             = buildInstance.cachePaths[cacheIndex].c_str();
            cacheFileResult.behavior         = AddFile;
            cacheFileResult.data             = cacheBuffer.bufferData;
            cacheFileResult.size             = cacheBuffer.bufferSize;
            cacheFileResult.hashArch         = buildInstance.options->archs.name();
            cacheFileResult.hashType         = buildInstance.cdHashType.c_str();
            cacheFileResult.hash             = cacheBuffer.cdHash.c_str();
            cacheFileResult.fd               = cacheBuffer.fd;
            cacheFileResult.tempFilePath     = cacheBuffer.tempPath.c_str();

            builder->fileResultStorage_v2.emplace_back(cacheFileResult);
#endif
            ++cacheIndex;
        }

        // Add a file result for the .map file
        // FIXME: We only emit a single map file right now.
        if ( !buildInstance.macOSMap.empty() ) {
#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
            FileResult_v1 cacheFileResult;
            cacheFileResult.version     = 1;
            cacheFileResult.path        = buildInstance.macOSMapPath.c_str();
            cacheFileResult.behavior    = AddFile;
            cacheFileResult.data        = (const uint8_t*)buildInstance.macOSMap.data();
            cacheFileResult.size        = buildInstance.macOSMap.size();
            cacheFileResult.hashArch    = buildInstance.options->archs.name();
            cacheFileResult.hashType    = buildInstance.cdHashType.c_str();
            cacheFileResult.hash        = buildInstance.cacheBuffers.front().cdHash.c_str();;

            builder->fileResultStorage_v1.emplace_back(cacheFileResult);
#else
            FileResult_v2 cacheFileResult;
            cacheFileResult.version          = 2;
            cacheFileResult.path             = buildInstance.macOSMapPath.c_str();
            cacheFileResult.behavior         = AddFile;
            cacheFileResult.data             = (const uint8_t*)buildInstance.macOSMap.data();
            cacheFileResult.size             = buildInstance.macOSMap.size();
            cacheFileResult.hashArch         = buildInstance.options->archs.name();
            cacheFileResult.hashType         = buildInstance.cdHashType.c_str();
            cacheFileResult.hash             = buildInstance.cacheBuffers.front().cdHash.c_str();
            cacheFileResult.fd               = 0;
            cacheFileResult.tempFilePath     = nullptr;

            builder->fileResultStorage_v2.emplace_back(cacheFileResult);
#endif
        }
    }
}

static void calculateDylibsToDelete(struct MRMSharedCacheBuilder* builder)
{
    // Add entries to tell us to remove all of the dylibs from disk which are in every cache.
    const size_t numCaches = builder->builders.size();
    for (const auto& dylibAndCount : builder->dylibsInCaches) {
        const char* pathToRemove = dylibAndCount.first.c_str();
        // Mastering should not remove dyld from disk
        if ( strcmp(pathToRemove, "/usr/lib/dyld") == 0 )
            continue;

        if ( builder->options->platform == Platform::macOS ) {
            // macOS has to leave the simulator support binaries on disk
            if ( strcmp(pathToRemove, "/usr/lib/system/libsystem_kernel.dylib") == 0 )
                continue;
            if ( strcmp(pathToRemove, "/usr/lib/system/libsystem_platform.dylib") == 0 )
                continue;
            if ( strcmp(pathToRemove, "/usr/lib/system/libsystem_pthread.dylib") == 0 )
                continue;
        }

        if (dylibAndCount.second.size() == numCaches) {
            builder->filesToRemove.push_back(pathToRemove);
        } else {
            // File is not in every cache, so likely has perhaps only x86_64h slice
            // but we built both x86_64 and x86_64h caches.
            // We may still delete it if its in all caches it's eligible for, ie, we
            // assume the cache builder knows about all possible arch's on the system and
            // can delete anything it knows can't run
            bool canDeletePath = true;
            for (auto& buildInstance : builder->builders) {
                if ( dylibAndCount.second.count(&buildInstance) != 0 )
                    continue;

                dyld3::closure::LoadedFileInfo fileInfo;
                char realerPath[MAXPATHLEN];
                auto errorHandler = ^(const char*, ...) { };
                if ( !builder->fileSystem.loadFile(pathToRemove, fileInfo, realerPath, errorHandler) ) {
                    // Somehow the file isn't loadable any more
                    continue;
                }

                if ( fileInfo.fileContent == nullptr )
                    continue;

                const void* buffer = fileInfo.fileContent;
                size_t bufferSize = fileInfo.fileContentLen;

                // This builder didn't get this image.  See if the image was ineligible
                // based on slice, ie, that dyld at runtime couldn't load this anyway, so
                // so removing it from disk won't hurt
                Diagnostics loaderDiag;
                const bool  isOSBinary = false;
                const dyld3::GradedArchs& archs = buildInstance.options->archs;
                dyld3::Platform platform = buildInstance.options->platform;
                if ( const auto* mf = dyld3::MachOFile::compatibleSlice(loaderDiag, buffer, bufferSize,
                                                                        pathToRemove, platform,
                                                                        isOSBinary, archs) ) {
                    // This arch was compatible, so the dylib was rejected from this cache for some other reason, eg,
                    // cache overflow.  We need to keep it on-disk
                    if ( !loaderDiag.hasError() ) {
                        canDeletePath = false;
                        break;
                    }
                }

                // Check iOSMac, just in case we couldn't load the slice on macOS
                if ( (platform == dyld3::Platform::macOS) && loaderDiag.hasError() ) {
                    loaderDiag.clearError();

                    if ( const auto* mf = dyld3::MachOFile::compatibleSlice(loaderDiag, buffer, bufferSize,
                                                                            pathToRemove, dyld3::Platform::iOSMac,
                                                                            isOSBinary, archs) ) {
                        // This arch was compatible, so the dylib was rejected from this cache for some other reason, eg,
                        // cache overflow.  We need to keep it on-disk
                        if ( !loaderDiag.hasError() ) {
                            canDeletePath = false;
                            break;
                        }
                    }
                }
            }
            if ( canDeletePath )
                builder->filesToRemove.push_back(pathToRemove);
        }
    }
}

bool runSharedCacheBuilder(struct MRMSharedCacheBuilder* builder) {
    __block bool success = false;
    builder->runSync(^() {
        if ( !createBuilders(builder) )
            return;
        
        runBuilders(builder);


        createBuildResults(builder);
        calculateDylibsToDelete(builder);

        // Copy from the storage to the vector we can return to the API.
#if SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
        for (auto &fileResult : builder->fileResultStorage_v1)
            builder->fileResults.push_back((FileResult*)&fileResult);
#else
        for (auto &fileResult : builder->fileResultStorage_v2)
            builder->fileResults.push_back((FileResult*)&fileResult);
#endif
        for (auto &cacheResult : builder->cacheResultStorage)
            builder->cacheResults.push_back(&cacheResult);

        // Quit if we had any errors.
        for (auto& buildInstance : builder->builders) {
            if (!buildInstance.errors.empty())
                return;
        }

        builder->state = MRMSharedCacheBuilder::FinishedBuilding;
        success = true;
    });
    return success;
}

const char* const* getErrors(const struct MRMSharedCacheBuilder* builder, uint64_t* errorCount) {
    if (builder->errors.empty())
        return nullptr;
    *errorCount = builder->errors.size();
    return builder->errors.data();
}

const struct FileResult* const* getFileResults(struct MRMSharedCacheBuilder* builder, uint64_t* resultCount) {
    if (builder->fileResults.empty())
        return nullptr;
    *resultCount = builder->fileResults.size();
    return builder->fileResults.data();
}

const struct CacheResult* const* getCacheResults(struct MRMSharedCacheBuilder* builder, uint64_t* resultCount) {
    if (builder->cacheResults.empty())
        return nullptr;
    *resultCount = builder->cacheResults.size();
    return builder->cacheResults.data();
}

const char* const* getFilesToRemove(const struct MRMSharedCacheBuilder* builder, uint64_t* fileCount) {
    if (builder->filesToRemove.empty())
        return nullptr;
    *fileCount = builder->filesToRemove.size();
    return builder->filesToRemove.data();
}

void destroySharedCacheBuilder(struct MRMSharedCacheBuilder* builder) {

    for ( FileBuffer& buffer : builder->buffersToFree ) {
        free((void*)buffer.buffer);
    }

    for ( FileBuffer& buffer : builder->buffersToDeallocate ) {
        vm_deallocate(mach_task_self(), (vm_address_t)buffer.buffer, buffer.size);
    }

#if !SUPPORT_CACHE_BUILDER_MEMORY_BUFFERS
    for ( MappedBuffer& buffer : builder->buffersToUnmap ) {
        ::munmap(buffer.buffer, buffer.size);
        ::close(buffer.fd);

        // The builder tool will link this temp path to the new location, if needed.  We then
        // remove the old path with this unlink().
        ::unlink(buffer.tempPath.c_str());
    }
#endif

    delete builder;
}
