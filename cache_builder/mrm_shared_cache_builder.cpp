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

#include <pthread.h>
#include <memory>
#include <unordered_set>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <mach-o/utils.h>

// mach_o
#include "Architecture.h"
#include "Universal.h"
#include "Misc.h"
#include "Image.h"

// common
#include "Defines.h"
#include "JSONReader.h"
#include "JSONWriter.h"
#include "ClosureFileSystem.h"
#include "ClosureFileSystemNull.h"
#include "SymbolsCache.h"

// other_tools
#include "FileUtils.h"
#include "MiscFileUtils.h"


#include "mrm_shared_cache_builder.h"
#include "BuilderFileSystem.h"
#include "NewSharedCacheBuilder.h"
#include "Error.h"
#include "SharedCacheLinker.h"



using mach_o::UnsafeHeader;
using mach_o::Image;

using cache_builder::CacheBuffer;
using cache_builder::SharedCacheBuilder;

using error::Error;

static const uint64_t kMinBuildVersion = 1; //The minimum version BuildOptions struct we can support
static const uint64_t kMaxBuildVersion = 3; //The maximum version BuildOptions struct we can support

static const uint32_t MajorVersion = 1;
static const uint32_t MinorVersion = 11;

struct BuildInstance {
    std::unique_ptr<cache_builder::BuilderOptions>  options;
    std::vector<cache_builder::FileAlias>           aliases;
    std::vector<cache_builder::FileAlias>           intermediateAliases;
    std::string                                     mainCacheFilePath;
    std::string                                     atlasPath;
    std::vector<const char*>                        errors;
    std::vector<const char*>                        warnings;
    std::vector<std::string>                        errorStrings;   // Owns the data for the errors
    std::vector<std::string>                        warningStrings; // Owns the data for the warnings
    std::vector<CacheBuffer>                        cacheBuffers;
    std::vector<std::string>                        cachePaths;             // Owns the data for the cache paths

    std::vector<std::byte>                          atlas;
    std::string                                     loggingPrefix;
    std::string                                     jsonMap;
    std::string                                     mainCacheUUID;
    std::string                                     customerLoggingPrefix;
    std::string                                     customerJsonMap;
    std::string                                     customerMainCacheUUID;
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

    std::vector<std::span<const uint8_t>> inputFileBuffers;

    std::string dylibOrderFileData;
    std::string dirtyDataOrderFileData;
    std::string swiftGenericMetadataFileData;
    std::string swiftGenericMetadataBuilderPath;
    void* objcOptimizationsFileData;
    size_t objcOptimizationsFileLength;
    std::string prewarmingMetadataFileData;

    // An array of builders and their options as we may have more than one builder for a given device variant.
    std::vector<BuildInstance> builders;

    // The paths in all of the caches
    // We keep this here to own the std::string path data
    std::map<std::string, std::unordered_set<const BuildInstance*>> dylibsInCaches;

    // The results from all of the builders
    // We keep this in a vector to own the data.
    std::vector<FileResult*>                     fileResults;

    // The builder dylib passes back buffers
    std::vector<FileResult_v1>                   fileResultStorage_v1;

    // Buffers which were malloc()ed and need free()d
    std::vector<FileBuffer> buffersToFree;

    // Buffers which were vm_allocate()d, and need vm_deallocate()d
    std::vector<FileBuffer> buffersToDeallocate;

    // The results from all of the builders
    // We keep this in a vector to own the data.
    std::vector<CacheResult*>    cacheResults;
    std::vector<CacheResult>     cacheResultStorage;


    // The files to remove.  These are in every copy of the caches we built
    std::vector<const char*> filesToRemove;

    // 1 JSON string per cache we built, with stats
    std::vector<const char*> stats;
    std::vector<std::string> statsStorage;

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
        diag.error(format, va_list_wrap(list));
        va_end(list);

        errorStorage.push_back(diag.errorMessage());
        errors.push_back(errorStorage.back().data());
    }

    __attribute__((format(printf, 2, 3)))
    void warning(const char* format, ...) {
        va_list list;
        va_start(list, format);
        Diagnostics diag;
        diag.error(format, va_list_wrap(list));
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
        case Disposition::SymbolsCache:
        case Disposition::InternalDevelopmentPlusAOT:
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

    if ( builder.options->disposition == Disposition::InternalDevelopmentPlusAOT ) {
        bool gotX86Cache = false;
        for (uint64_t i = 0; i != builder.options->numArchs; ++i) {
            if ( !strcmp(builder.options->archs[i], "x86_64") ) {
                gotX86Cache = true;
                break;
            }
        }

        if ( !gotX86Cache ) {
            builder.error("x86_64 arch must be present for 'InternalDevelopmentPlusAOT' disposition");
        }
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

static bool addFileImpl(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags,
                        uint64_t inode, uint64_t modTime, const char* projectName) {
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
            // three lib system dylibs are allowed to be null at this point, stubs are instantiated later
            mach_o::Platform plat((int)builder->options->platform);
            if ( !plat.isSimulator() || !UnsafeHeader::isSimulatorSupportDylibPath(path) ) {
                builder->error("Data cannot be null for file: '%s'", path);
                return;
            }
        }
        switch (fileFlags) {
            case NoFlags:
            case MustBeInCache:
            case ShouldBeExcludedFromCacheIfUnusedLeaf:
            case RequiredClosure:
            case CanBeMissing:
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
            case SwiftGenericMetadataFile:
                builder->swiftGenericMetadataFileData = std::string((char*)data, size);
                success = true;
                return;
            case OptimizationFile: {
                // TODO: Remove DylibOrderFile..SwiftGenericMetadataFile once image assembly
                // passes this for all files from the OrderFiles project
                CString leafName = CString(path).leafName();
                if ( leafName == "dylib-order.txt" ) {
                    builder->dylibOrderFileData = std::string((char*)data, size);
                    success = true;
                    return;
                }
                if ( leafName == "dirty-data-segments-order.txt" ) {
                    builder->dirtyDataOrderFileData = std::string((char*)data, size);
                    success = true;
                    return;
                }
                if ( leafName == "shared-cache-objc-optimizations.json" ) {
                    builder->objcOptimizationsFileData = data;
                    builder->objcOptimizationsFileLength = size;
                    success = true;
                    return;
                }
                if ( leafName == "swift-generic-metadata.json" ) {
                    builder->swiftGenericMetadataFileData = std::string((char*)data, size);
                    success = true;
                    return;
                }
                if ( leafName == "prewarming-metadata.json" ) {
                    builder->prewarmingMetadataFileData = std::string((char*)data, size);
                    success = true;
                    return;
                }
                // Skip this file as image assembly will probably just give us all files in a given
                // directory and that might include new/unrelated content
                builder->warning("unknown optimization file path: %s", path);
                success = true;
                return;
            }
            default:
                builder->error("unknown file flags value");
                break;
        }
        Diagnostics diag;
        if (!builder->fileSystem.addFile(path, data, size, diag, fileFlags, inode, modTime, projectName)) {
            builder->errorStorage.push_back(diag.errorMessage());
            builder->errors.push_back(builder->errorStorage.back().data());
            return;
        }
        success = true;
    });
    return success;
}

bool addFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags) {
    return addFileImpl(builder, path, data, size, fileFlags, 0, 0, "");
}

bool addFile_v2(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags, const char* projectName) {
    return addFileImpl(builder, path, data, size, fileFlags, 0, 0, projectName);
}

bool addSimCacheOnDiskFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, FileFlags fileFlags,
                   uint64_t inode, uint64_t modTime) {
    return addFileImpl(builder, path, data, size, fileFlags, inode, modTime, "");
}

bool addOnDiskFile_v1(struct MRMSharedCacheBuilder* builder, const char* onDevicePath, const char* filePath, enum FileFlags fileFlags, const char* projectName)
{
    std::span<const uint8_t> mappedBuffer;
    if ( mach_o::Error err = other_tools::mapFileReadOnly(filePath, mappedBuffer) ) {
        if ( fileFlags == CanBeMissing ) {
            builder->warning("%s", err.message());
            return true;
        } else {
            builder->error("%s", err.message());
            return false;
        }
    }

    builder->inputFileBuffers.push_back(mappedBuffer);
    return addFileImpl(builder, onDevicePath, (uint8_t*)mappedBuffer.data(), mappedBuffer.size(), fileFlags, 0, 0, projectName);
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

// Available in API version 1.6 and later
__API_AVAILABLE(macos(10.12))
bool addPlugin(struct MRMSharedCacheBuilder* builder, const char* path, enum FileFlags fileFlags)
{
    switch ( fileFlags ) {
        case FileFlags::PluginSwiftGenericMetadataBuilder:
            builder->swiftGenericMetadataBuilderPath = path;
            break;
        default:
            builder->warning("unknown plugin file path: %s", path);
            break;
    }

    return true;
}

static cache_builder::LocalSymbolsMode platformExcludeLocalSymbols(Platform platform) {
    if ( mach_o::Platform((uint32_t)platform).isSimulator() )
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
        case Disposition::SymbolsCache:
            return "SymbolsCache";
        case Disposition::InternalDevelopmentPlusAOT:
            return "InternalDevelopmentPlusAOT";
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

static bool debugIMPCaches(const BuildOptions_v1* options) {
    // Old builds just don't print this verbose output
    if ( options->version < 3 ) {
        return false;
    }

    const BuildOptions_v3* v3 = (const BuildOptions_v3*)options;
    return v3->verboseIMPCaches;
}

static bool debugCacheLayout(const BuildOptions_v1* options) {
    // Old builds just don't print this verbose output
    if ( options->version < 3 ) {
        return false;
    }

    const BuildOptions_v3* v3 = (const BuildOptions_v3*)options;
    return v3->verboseCacheLayout;
}

// This is a JSON file containing the list of classes for which
// we should try to build IMP caches.
static json::Node parseObjcOptimizationsFile(Diagnostics& diags, const void* data, size_t length) {
    if ( data == nullptr )
        return json::Node();
    return json::readJSON(diags, data, length, false /* useJSON5 */);
}

static cache_builder::CacheKind getCacheKind(const BuildOptions_v1* options)
{
    // Work out what kind of cache we are building.  macOS/driverKit/exclaveKit are always development
    if (   (options->platform == macOS)
        || (options->platform == driverKit)
        || mach_o::Platform(options->platform).isExclaveKit() )
        return cache_builder::CacheKind::development;

    // Sims are always development
    if ( mach_o::Platform(options->platform).isSimulator() )
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
        case Disposition::InternalDevelopmentPlusAOT:
            return true;
        case Disposition::Customer:
            return false;
        case Disposition::InternalMinDevelopment:
            return true;
        case Disposition::SymbolsCache:
            return false;
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
        case Disposition::InternalDevelopmentPlusAOT:
            return true;
        case Disposition::Customer:
            return true;
        case Disposition::InternalMinDevelopment:
            return false;
        case Disposition::SymbolsCache:
            return false;
    }

    return true;
}

static std::string cacheFileName(const char* arch, bool isSimulator) {
    if ( isSimulator ) {
        return std::string("dyld_sim_shared_cache_") + arch;
    } else {
        return std::string("dyld_shared_cache_") + arch;
    }
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

        // Add a driverKit/exclaveKit suffix.  Note we don't need to add .development suffixes any
        // more as the universal caches don't build customer and development seperately
        const char *loggingSuffix = "";
        if ( builder->options->platform == Platform::driverKit )
            loggingSuffix = ".driverKit";
        if ( mach_o::Platform(builder->options->platform).isExclaveKit() )
            loggingSuffix = ".exclaveKit";

        std::string loggingPrefix = "";
        loggingPrefix += std::string(builder->options->deviceName);
        loggingPrefix += dispositionName(builder->options->disposition);
        loggingPrefix += std::string(".") + builder->options->archs[i];
        loggingPrefix += loggingSuffix;

        bool dylibsRemovedFromDisk  = filesRemovedFromDisk(builder->options);
        bool isLocallyBuiltCache    = builder->options->isLocallyBuiltCache;

        std::string mainCacheFileName;
        std::string runtimePath;
        if ( mach_o::Platform(builder->options->platform).isSimulator()) {
            mainCacheFileName = cacheFileName(builder->options->archs[i], true);
            if ( dylibsRemovedFromDisk ) {
                // B&I built caches go into RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_<arch>
                runtimePath = IPHONE_DYLD_SHARED_CACHE_DIR + mainCacheFileName;
            }
            else {
                // Locally built sim caches are written exactly where instructed, without adding any directory structure
                runtimePath = mainCacheFileName;
            }
        } else {
            mainCacheFileName = cacheFileName(builder->options->archs[i], false);
            if ( builder->options->platform == Platform::macOS )
                runtimePath = MACOSX_MRM_DYLD_SHARED_CACHE_DIR + mainCacheFileName;
            else if ( builder->options->platform == Platform::driverKit )
                runtimePath = DRIVERKIT_DYLD_SHARED_CACHE_DIR + mainCacheFileName;
            else if ( mach_o::Platform(builder->options->platform).isExclaveKit() )
                runtimePath = EXCLAVEKIT_DYLD_SHARED_CACHE_DIR + mainCacheFileName;
            else
                runtimePath = IPHONE_DYLD_SHARED_CACHE_DIR + mainCacheFileName;
        }

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
                                                                       builder->options->platform,
                                                                       dylibsRemovedFromDisk, isLocallyBuiltCache,
                                                                       cacheKind, forceDevelopmentSubCacheSuffix,
                                                                       builder->options->updateName,
                                                                       builder->options->deviceName);

        options->mainCacheFileName           = mainCacheFileName;
        options->logPrefix                   = loggingPrefix;
        options->debug                       = builder->options->verboseDiagnostics;
        options->debugIMPCaches              = debugIMPCaches(builder->options);
        options->debugCacheLayout            = debugCacheLayout(builder->options);
        options->timePasses                  = options->debug ? true : timePasses(builder->options);
        options->stats                       = options->debug ? true : printStats(builder->options);
        options->dylibOrdering               = parseOrderFile(builder->dylibOrderFileData);
        options->dirtyDataSegmentOrdering    = parseOrderFile(builder->dirtyDataOrderFileData);
        options->objcOptimizations           = parseObjcOptimizationsFile(diag, builder->objcOptimizationsFileData,
                                                                          builder->objcOptimizationsFileLength);
        options->localSymbolsMode            = excludeLocalSymbols(builder->options);
        options->prewarmingOptimizations     = builder->prewarmingMetadataFileData;

        options->swiftGenericMetadataFile        = builder->swiftGenericMetadataFileData;
        options->swiftGenericMetadataBuilderPath = builder->swiftGenericMetadataBuilderPath;

        BuildInstance buildInstance;
        buildInstance.options               = std::move(options);
        buildInstance.aliases               = aliases;
        buildInstance.intermediateAliases   = intermediateAliases;
        buildInstance.mainCacheFilePath     = runtimePath;
        buildInstance.atlasPath             = runtimePath + ".atlas";

        builder->builders.push_back(std::move(buildInstance));
    }

    return true;
}

#if !BUILDING_SIM_CACHE_BUILDER
static const char* const libPlatformExports[] = {
    "_OSAtomicAdd64",
    "_OSAtomicCompareAndSwapLong",
    "_OSAtomicDecrement32Barrier",
    "_OSAtomicDequeue",
    "_OSAtomicEnqueue",
    "_OSAtomicIncrement32",
    "_OSAtomicIncrement32Barrier",
    "_OSAtomicIncrement64",
    "_OSMemoryBarrier",
    "_OSSpinLockLock",
    "_OSSpinLockUnlock",
    "___platform_sigaction",
    "__longjmp",
    "__os_alloc_once",
    "__os_nospin_lock_lock",
    "__os_nospin_lock_trylock",
    "__os_nospin_lock_unlock",
    "__os_once",
    "__platform_bzero",
    "__platform_memccpy",
    "__platform_memchr",
    "__platform_memcmp",
    "__platform_memcmp_zero_aligned8",
    "__platform_memmove",
    "__platform_memset",
    "__platform_memset_pattern16",
    "__platform_memset_pattern4",
    "__platform_memset_pattern8",
    "__platform_strchr",
    "__platform_strcmp",
    "__platform_strcpy",
    "__platform_strlcat",
    "__platform_strlcpy",
    "__platform_strlen",
    "__platform_strncmp",
    "__platform_strncpy",
    "__platform_strnlen",
    "__platform_strstr",
    "__setjmp",
    "__simple_asl_log",
    "__simple_asl_log_prog",
    "__simple_asl_msg_new",
    "__simple_asl_msg_set",
    "__simple_asl_send",
    "__simple_dprintf",
    "__simple_esappend",
    "__simple_esprintf",
    "__simple_getenv",
    "__simple_put",
    "__simple_putline",
    "__simple_salloc",
    "__simple_sappend",
    "__simple_sfree",
    "__simple_sprintf",
    "__simple_sresize",
    "__simple_string",
    "__simple_vdprintf",
    "__simple_vesprintf",
    "__simple_vsprintf",
    "_fls",
    "_flsl",
    "_flsll",
    "_longjmp",
    "_os_sync_wait_on_address",
    "_os_sync_wait_on_address_with_deadline",
    "_os_sync_wait_on_address_with_timeout",
    "_os_sync_wake_by_address_all",
    "_os_sync_wake_by_address_any",
    "_os_unfair_lock_assert_not_owner",
    "_os_unfair_lock_assert_owner",
    "_os_unfair_lock_lock",
    "_os_unfair_lock_lock_with_flags",
    "_os_unfair_lock_lock_with_options",
    "_os_unfair_lock_trylock",
    "_os_unfair_lock_unlock",
    "_os_unfair_recursive_lock_lock_with_options",
    "_os_unfair_recursive_lock_trylock",
    "_os_unfair_recursive_lock_tryunlock4objc",
    "_os_unfair_recursive_lock_unlock",
    "_os_unfair_recursive_lock_unlock_forked_child",
    "_setjmp",
    "_siglongjmp",
    "_sigsetjmp",
    "_sys_dcache_flush",
    "_sys_icache_invalidate"
};

static const char* const libPthreadExports[] = {
    "___chkstk_darwin",
    "___pthread_init",
    "___pthread_late_init",
    "___pthread_workqueue_setkill",
    "___unix_conforming",
    "__pthread_atfork_child",
    "__pthread_atfork_child_handlers",
    "__pthread_atfork_parent",
    "__pthread_atfork_parent_handlers",
    "__pthread_atfork_prepare",
    "__pthread_atfork_prepare_handlers",
    "__pthread_clear_qos_tsd",
    "__pthread_exit_if_canceled",
    "__pthread_qos_class_decode",
    "__pthread_qos_class_encode",
    "__pthread_qos_override_end_direct",
    "__pthread_qos_override_start_direct",
    "__pthread_set_properties_self",
    "__pthread_setspecific_static",
    "__pthread_workloop_create",
    "__pthread_workloop_destroy",
    "__pthread_workqueue_add_cooperativethreads",
    "__pthread_workqueue_addthreads",
    "__pthread_workqueue_allow_send_signals",
    "__pthread_workqueue_override_reset",
    "__pthread_workqueue_override_start_direct",
    "__pthread_workqueue_override_start_direct_check_owner",
    "__pthread_workqueue_set_event_manager_priority",
    "__pthread_workqueue_supported",
    "_posix_spawnattr_set_qos_class_np",
    "_pthread_atfork",
    "_pthread_attr_destroy",
    "_pthread_attr_get_qos_class_np",
    "_pthread_attr_getschedparam",
    "_pthread_attr_getschedpolicy",
    "_pthread_attr_getstacksize",
    "_pthread_attr_init",
    "_pthread_attr_set_qos_class_np",
    "_pthread_attr_setcpupercent_np",
    "_pthread_attr_setdetachstate",
    "_pthread_attr_setinheritsched",
    "_pthread_attr_setschedparam",
    "_pthread_attr_setschedpolicy",
    "_pthread_attr_setscope",
    "_pthread_attr_setstacksize",
    "_pthread_attr_setworkinterval_np",
    "_pthread_cancel",
    "_pthread_chdir_np",
    "_pthread_cond_broadcast",
    "_pthread_cond_destroy",
    "_pthread_cond_init",
    "_pthread_cond_signal",
    "_pthread_cond_signal_thread_np",
    "_pthread_cond_timedwait",
    "_pthread_cond_timedwait_relative_np",
    "_pthread_cond_wait",
    "_pthread_condattr_destroy",
    "_pthread_condattr_init",
    "_pthread_cpu_number_np",
    "_pthread_create",
    "_pthread_current_stack_contains_np",
    "_pthread_dependency_fulfill_np",
    "_pthread_dependency_init_np",
    "_pthread_dependency_wait_np",
    "_pthread_detach",
    "_pthread_equal",
    "_pthread_exit",
    "_pthread_fchdir_np",
    "_pthread_from_mach_thread_np",
    "_pthread_get_qos_class_np",
    "_pthread_get_stackaddr_np",
    "_pthread_get_stacksize_np",
    "_pthread_getname_np",
    "_pthread_getschedparam",
    "_pthread_getspecific",
    "_pthread_install_workgroup_functions_np",
    "_pthread_introspection_getspecific_np",
    "_pthread_introspection_hook_install",
    "_pthread_introspection_setspecific_np",
    "_pthread_is_threaded_np",
    "_pthread_join",
    "_pthread_key_create",
    "_pthread_key_delete",
    "_pthread_key_init_np",
    "_pthread_kill",
    "_pthread_mach_thread_np",
    "_pthread_main_np",
    "_pthread_main_thread_np",
    "_pthread_mutex_destroy",
    "_pthread_mutex_init",
    "_pthread_mutex_lock",
    "_pthread_mutex_trylock",
    "_pthread_mutex_unlock",
    "_pthread_mutexattr_destroy",
    "_pthread_mutexattr_init",
    "_pthread_mutexattr_setpolicy_np",
    "_pthread_mutexattr_setprotocol",
    "_pthread_mutexattr_setpshared",
    "_pthread_mutexattr_settype",
    "_pthread_once",
    "_pthread_override_qos_class_end_np",
    "_pthread_override_qos_class_start_np",
    "_pthread_qos_max_parallelism",
    "_pthread_rwlock_destroy",
    "_pthread_rwlock_init",
    "_pthread_rwlock_rdlock",
    "_pthread_rwlock_tryrdlock",
    "_pthread_rwlock_trywrlock",
    "_pthread_rwlock_unlock",
    "_pthread_rwlock_wrlock",
    "_pthread_rwlockattr_destroy",
    "_pthread_rwlockattr_init",
    "_pthread_self",
    "_pthread_self_is_exiting_np",
    "_pthread_set_fixedpriority_self",
    "_pthread_set_qos_class_self_np",
    "_pthread_setcancelstate",
    "_pthread_setcanceltype",
    "_pthread_setname_np",
    "_pthread_setschedparam",
    "_pthread_setspecific",
    "_pthread_sigmask",
    "_pthread_stack_frame_decode_np",
    "_pthread_testcancel",
    "_pthread_threadid_np",
    "_pthread_time_constraint_max_parallelism",
    "_pthread_workqueue_setup",
    "_pthread_yield_np",
    "_qos_class_main",
    "_qos_class_self",
    "_sched_get_priority_max",
    "_sched_get_priority_min",
    "_sched_yield",
    "_sigwait"
};

static const char* const libKernelExports[] = {
    "_NDR_record",
    "_____sigwait_nocancel",
    "____kernelVersionNumber",
    "____kernelVersionString",
    "___abort_with_payload",
    "___accept",
    "___accept_nocancel",
    "___access_extended",
    "___aio_suspend_nocancel",
    "___bind",
    "___bsdthread_create",
    "___bsdthread_ctl",
    "___bsdthread_register",
    "___bsdthread_terminate",
    "___carbon_delete",
    "___channel_get_info",
    "___channel_get_opt",
    "___channel_open",
    "___channel_set_opt",
    "___channel_sync",
    "___chmod",
    "___chmod_extended",
    "___close_nocancel",
    "___coalition",
    "___coalition_info",
    "___coalition_ledger",
    "___commpage_gettimeofday",
    "___connect",
    "___connect_nocancel",
    "___copyfile",
    "___crossarch_trap",
    "___csrctl",
    "___darwin_check_fd_set_overflow",
    "___debug_syscall_reject",
    "___debug_syscall_reject_config",
    "___delete",
    "___disable_threadsignal",
    "___error",
    "___execve",
    "___exit",
    "___fchmod",
    "___fchmod_extended",
    "___fcntl",
    "___fcntl_nocancel",
    "___fork",
    "___fs_snapshot",
    "___fstat",             // arm64 only
    "___fstat64_extended",
    "___fstat_extended",
    "___fstatat",           // arm64 only
    "___fstatfs",           // arm64 only
    "___fsync_nocancel",
    "___get_remove_counter",
    "___getattrlist",
    "___getdirentries64",
    "___getfsstat",         // arm64 only
    "___gethostuuid",
    "___getlogin",
    "___getpeername",
    "___getpid",
    "___getrlimit",
    "___getsgroups",
    "___getsockname",
    "___gettid",
    "___gettimeofday",
    "___getwgroups",
    "___guarded_open_dprotected_np",
    "___guarded_open_np",
    "___identitysvc",
    "___inc_remove_counter",
    "___initgroups",
    "___ioctl",
    "___iopolicysys",
    "___kdebug_trace",
    "___kdebug_trace64",
    "___kdebug_trace_string",
    "___kdebug_typefilter",
    "___kill",
    "___kqueue_workloop_ctl",
    "___lchown",
    "___libkernel_init",
    "___libkernel_init_after_boot_tasks",
    "___libkernel_init_late",
    "___libkernel_platform_init",
    "___libkernel_voucher_init",
    "___listen",
    "___log_data",
    "___lseek",
    "___lstat",                // arm64 only
    "___lstat64_extended",
    "___lstat_extended",
    "___mac_execve",
    "___mac_get_fd",
    "___mac_get_file",
    "___mac_get_link",
    "___mac_get_mount",
    "___mac_get_pid",
    "___mac_get_proc",
    "___mac_getfsstat",
    "___mac_mount",
    "___mac_set_fd",
    "___mac_set_file",
    "___mac_set_link",
    "___mac_set_proc",
    "___mac_syscall",
    "___mach_bridge_remote_time",
    "___mach_eventlink_signal",
    "___mach_eventlink_signal_wait_until",
    "___mach_eventlink_wait_until",
    "___map_with_linking_np",
    "___memorystatus_available_memory",
    "___microstackshot",
    "___mkdir_extended",
    "___mkfifo_extended",
    "___mmap",
    "___mprotect",
    "___msgctl",
    "___msgrcv_nocancel",
    "___msgsnd_nocancel",
    "___msgsys",
    "___msync",
    "___msync_nocancel",
    "___munmap",
    "___nexus_create",
    "___nexus_deregister",
    "___nexus_destroy",
    "___nexus_get_opt",
    "___nexus_open",
    "___nexus_register",
    "___nexus_set_opt",
    "___open",
    "___open_dprotected_np",
    "___open_extended",
    "___open_nocancel",
    "___openat",
    "___openat_dprotected_np",
    "___openat_nocancel",
    "___os_nexus_flow_add",
    "___os_nexus_flow_del",
    "___os_nexus_get_llink_info",
    "___os_nexus_ifattach",
    "___os_nexus_ifdetach",
    "___oslog_coproc",
    "___oslog_coproc_reg",
    "___persona",
    "___pipe",
    "___poll_nocancel",
    "___posix_spawn",
    "___pread_nocancel",
    "___preadv_nocancel",
    "___proc_info",
    "___proc_info_extended_id",
    "___process_policy",
    "___pselect",
    "___pselect_nocancel",
    "___psynch_cvbroad",
    "___psynch_cvclrprepost",
    "___psynch_cvsignal",
    "___psynch_cvwait",
    "___psynch_mutexdrop",
    "___psynch_mutexwait",
    "___psynch_rw_downgrade",
    "___psynch_rw_longrdlock",
    "___psynch_rw_rdlock",
    "___psynch_rw_unlock",
    "___psynch_rw_unlock2",
    "___psynch_rw_upgrade",
    "___psynch_rw_wrlock",
    "___psynch_rw_yieldwrlock",
    "___pthread_canceled",
    "___pthread_chdir",
    "___pthread_fchdir",
    "___pthread_kill",
    "___pthread_markcancel",
    "___pthread_sigmask",
    "___ptrace",
    "___pwrite_nocancel",
    "___pwritev_nocancel",
    "___read_nocancel",
    "___readv_nocancel",
    "___reboot",
    "___record_system_event",
    "___recvfrom",
    "___recvfrom_nocancel",
    "___recvmsg",
    "___recvmsg_nocancel",
    "___rename",
    "___renameat",
    "___renameatx_np",
    "___rmdir",
    "___sandbox_me",
    "___sandbox_mm",
    "___sandbox_ms",
    "___sandbox_msp",
    "___select",
    "___select_nocancel",
    "___sem_open",
    "___sem_wait_nocancel",
    "___semctl",
    "___semsys",
    "___semwait_signal",
    "___semwait_signal_nocancel",
    "___sendmsg",
    "___sendmsg_nocancel",
    "___sendto",
    "___sendto_nocancel",
    "___setattrlist",
    "___setlogin",
    "___setpriority",
    "___setregid",
    "___setreuid",
    "___setrlimit",
    "___setsgroups",
    "___settid",
    "___settid_with_pid",
    "___settimeofday",
    "___setwgroups",
    "___sfi_ctl",
    "___sfi_pidctl",
    "___shared_region_check_np",
    "___shared_region_map_and_slide_2_np",
    "___shm_open",
    "___shmctl",
    "___shmsys",
    "___sigaction",
    "___sigaltstack",
    "___sigreturn",
    "___sigsuspend",
    "___sigsuspend_nocancel",
    "___sigwait",
    "___socketpair",
    "___stack_snapshot_with_config",
    "___stat",                  // arm64 only
    "___stat64_extended",
    "___stat_extended",
    "___statfs",               // arm64 only
    "___syscall",
    "___syscall_logger",
    "___sysctl",
    "___sysctlbyname",
    "___telemetry",
    "___terminate_with_payload",
    "___thread_selfid",
    "___thread_selfusage",
    "___ulock_wait",
    "___ulock_wait2",
    "___ulock_wake",
    "___umask_extended",
    "___unlink",
    "___unlinkat",
    "___vfork",
    "___wait4",
    "___wait4_nocancel",
    "___waitid_nocancel",
    "___work_interval_ctl",
    "___workq_kernreturn",
    "___workq_open",
    "___write_nocancel",
    "___writev_nocancel",
    "__cpu_capabilities",
    "__cpu_has_altivec",
    "__current_pid",         // arm64 only
    "__exclaves_ctl_trap",
    "__exit",
    "__get_cpu_capabilities",
    "__getprivatesystemidentifier",
    "__host_page_size",
    "__init_cpu_capabilities",
    "__kernelrpc_host_create_mach_voucher",
    "__kernelrpc_mach_port_allocate",
    "__kernelrpc_mach_port_allocate_full",
    "__kernelrpc_mach_port_allocate_name",
    "__kernelrpc_mach_port_allocate_qos",
    "__kernelrpc_mach_port_allocate_trap",
    "__kernelrpc_mach_port_assert_attributes",
    "__kernelrpc_mach_port_construct",
    "__kernelrpc_mach_port_construct_trap",
    "__kernelrpc_mach_port_deallocate",
    "__kernelrpc_mach_port_deallocate_trap",
    "__kernelrpc_mach_port_destroy",
    "__kernelrpc_mach_port_destruct",
    "__kernelrpc_mach_port_destruct_trap",
    "__kernelrpc_mach_port_dnrequest_info",
    "__kernelrpc_mach_port_extract_member",
    "__kernelrpc_mach_port_extract_member_trap",
    "__kernelrpc_mach_port_extract_right",
    "__kernelrpc_mach_port_get_attributes",
    "__kernelrpc_mach_port_get_attributes_trap",
    "__kernelrpc_mach_port_get_context",
    "__kernelrpc_mach_port_get_refs",
    "__kernelrpc_mach_port_get_service_port_info",
    "__kernelrpc_mach_port_get_set_status",
    "__kernelrpc_mach_port_get_srights",
    "__kernelrpc_mach_port_guard",
    "__kernelrpc_mach_port_guard_trap",
    "__kernelrpc_mach_port_guard_with_flags",
    "__kernelrpc_mach_port_insert_member",
    "__kernelrpc_mach_port_insert_member_trap",
    "__kernelrpc_mach_port_insert_right",
    "__kernelrpc_mach_port_insert_right_trap",
    "__kernelrpc_mach_port_is_connection_for_service",
    "__kernelrpc_mach_port_kernel_object",
    "__kernelrpc_mach_port_kobject",
    "__kernelrpc_mach_port_kobject_description",
    "__kernelrpc_mach_port_mod_refs",
    "__kernelrpc_mach_port_mod_refs_trap",
    "__kernelrpc_mach_port_move_member",
    "__kernelrpc_mach_port_move_member_trap",
    "__kernelrpc_mach_port_names",
    "__kernelrpc_mach_port_peek",
    "__kernelrpc_mach_port_rename",
    "__kernelrpc_mach_port_request_notification",
    "__kernelrpc_mach_port_request_notification_trap",
    "__kernelrpc_mach_port_set_attributes",
    "__kernelrpc_mach_port_set_context",
    "__kernelrpc_mach_port_set_mscount",
    "__kernelrpc_mach_port_set_seqno",
    "__kernelrpc_mach_port_space_basic_info",
    "__kernelrpc_mach_port_space_info",
    "__kernelrpc_mach_port_special_reply_port_reset_link",
    "__kernelrpc_mach_port_swap_guard",
    "__kernelrpc_mach_port_type",
    "__kernelrpc_mach_port_type_trap",
    "__kernelrpc_mach_port_unguard",
    "__kernelrpc_mach_port_unguard_trap",
    "__kernelrpc_mach_ports_lookup3",
    "__kernelrpc_mach_ports_register3",
    "__kernelrpc_mach_task_is_self",
    "__kernelrpc_mach_vm_allocate",
    "__kernelrpc_mach_vm_allocate_trap",
    "__kernelrpc_mach_vm_deallocate",
    "__kernelrpc_mach_vm_deallocate_trap",
    "__kernelrpc_mach_vm_map",
    "__kernelrpc_mach_vm_map_trap",
    "__kernelrpc_mach_vm_protect",
    "__kernelrpc_mach_vm_protect_trap",
    "__kernelrpc_mach_vm_purgable_control",
    "__kernelrpc_mach_vm_purgable_control_trap",
    "__kernelrpc_mach_vm_read",
    "__kernelrpc_mach_vm_remap",
    "__kernelrpc_mach_vm_remap_new",
    "__kernelrpc_mach_voucher_extract_attr_recipe",
    "__kernelrpc_task_set_port_space",
    "__kernelrpc_thread_policy",
    "__kernelrpc_thread_policy_set",
    "__kernelrpc_thread_set_policy",
    "__kernelrpc_vm_map",
    "__kernelrpc_vm_purgable_control",
    "__kernelrpc_vm_read",
    "__kernelrpc_vm_remap",
    "__kernelrpc_vm_remap_new",
    "__mach_errors",
    "__mach_fork_child",
    "__mach_snprintf",
    "__mach_vsnprintf",
    "__os_alloc_once_table",
    "__os_xbs_chrooted",
    "__register_gethostuuid_callback",
    "__thread_set_tsd_base",
    "_abort_with_payload",
    "_abort_with_reason",
    "_accept",
    "_accept$NOCANCEL",
    "_access",
    "_accessx_np",
    "_acct",
    "_act_get_state",
    "_act_set_state",
    "_adjtime",
    "_aio_cancel",
    "_aio_error",
    "_aio_fsync",
    "_aio_read",
    "_aio_return",
    "_aio_suspend",
    "_aio_suspend$NOCANCEL",
    "_aio_write",
    "_audit",
    "_audit_session_join",
    "_audit_session_port",
    "_audit_session_self",
    "_auditctl",
    "_auditon",
    "_bind",
    "_bootstrap_port",
    "_cerror",
    "_cerror_nocancel",
    "_change_fdguard_np",
    "_chdir",
    "_chflags",
    "_chmod",
    "_chown",
    "_chroot",
    "_clock_alarm",
    "_clock_alarm_reply",
    "_clock_get_attributes",
    "_clock_get_time",
    "_clock_set_attributes",
    "_clock_set_time",
    "_clock_sleep",
    "_clock_sleep_trap",
    "_clonefile",
    "_clonefileat",
    "_close",
    "_close$NOCANCEL",
    "_coalition_create",
    "_coalition_info_debug_info",
    "_coalition_info_resource_usage",
    "_coalition_info_set_efficiency",
    "_coalition_info_set_name",
    "_coalition_ledger_set_logical_writes_limit",
    "_coalition_policy_get",
    "_coalition_policy_set",
    "_coalition_reap",
    "_coalition_terminate",
    "_connect",
    "_connect$NOCANCEL",
    "_connectx",
    "_csops",
    "_csops_audittoken",
    "_csr_check",
    "_csr_get_active_config",
    "_debug_control_port_for_pid",
    "_debug_syscall_reject",
    "_debug_syscall_reject_config",
    "_denap_boost_assertion_token",
    "_disconnectx",
    "_dup",
    "_dup2",
    "_errno",
    "_etap_trace_thread",
    "_exc_server",
    "_exc_server_routine",
    "_exception_raise",
    "_exception_raise_state",
    "_exception_raise_state_identity",
    "_exchangedata",
    "_exclaves_audio_buffer_copyout",
    "_exclaves_audio_buffer_copyout_with_status",
    "_exclaves_audio_buffer_create",
    "_exclaves_boot",
    "_exclaves_endpoint_call",
    "_exclaves_inbound_buffer_copyin",
    "_exclaves_inbound_buffer_create",
    "_exclaves_launch_conclave",
    "_exclaves_lookup_service",
    "_exclaves_named_buffer_copyin",
    "_exclaves_named_buffer_copyout",
    "_exclaves_named_buffer_create",
    "_exclaves_notification_create",
    "_exclaves_outbound_buffer_copyout",
    "_exclaves_outbound_buffer_create",
    "_exclaves_sensor_create",
    "_exclaves_sensor_start",
    "_exclaves_sensor_status",
    "_exclaves_sensor_stop",
    "_execve",
    "_faccessat",
    "_fchdir",
    "_fchflags",
    "_fchmod",
    "_fchmodat",
    "_fchown",
    "_fchownat",
    "_fclonefileat",
    "_fcntl",
    "_fcntl$NOCANCEL",
    "_fdatasync",
    "_ffsctl",
    "_fgetattrlist",
    "_fgetxattr",
    "_fhopen",
    "_fileport_makefd",
    "_fileport_makeport",
    "_flistxattr",
    "_flock",
    "_fmount",
    "_fpathconf",
    "_freadlink",
    "_fremovexattr",
    "_fs_snapshot_create",
    "_fs_snapshot_delete",
    "_fs_snapshot_list",
    "_fs_snapshot_mount",
    "_fs_snapshot_rename",
    "_fs_snapshot_revert",
    "_fs_snapshot_root",
    "_fsctl",
    "_fsetattrlist",
    "_fsetxattr",
    "_fsgetpath",
    "_fsgetpath_ext",
    "_fstat",
    "_fstat$INODE64",       // x86_64 only
    "_fstat64",
    "_fstatat",
    "_fstatat$INODE64",     // x86_64 only
    "_fstatat64",
    "_fstatfs",
    "_fstatfs$INODE64",     // x86_64 only
    "_fstatfs64",
    "_fstatfs_ext",
    "_fsync",
    "_fsync$NOCANCEL",
    "_ftruncate",
    "_futimens",
    "_futimes",
    "_getattrlist",
    "_getattrlistat",
    "_getattrlistbulk",
    "_getaudit",
    "_getaudit_addr",
    "_getauid",
    "_getdirentries",
    "_getdirentriesattr",
    "_getdtablesize",
    "_getegid",
    "_getentropy",
    "_geteuid",
    "_getfh",
    "_getfsstat",
    "_getfsstat$INODE64",   // x86_64 only
    "_getfsstat64",
    "_getgid",
    "_getgroups",
    "_gethostuuid",
    "_getiopolicy_np",
    "_getitimer",
    "_getpeername",
    "_getpgid",
    "_getpgrp",
    "_getpid",
    "_getppid",
    "_getpriority",
    "_getrlimit",
    "_getrusage",
    "_getsgroups_np",
    "_getsid",
    "_getsockname",
    "_getsockopt",
    "_getuid",
    "_getwgroups_np",
    "_getxattr",
    "_grab_pgo_data",
    "_graftdmg",
    "_guarded_close_np",
    "_guarded_kqueue_np",
    "_guarded_open_dprotected_np",
    "_guarded_open_np",
    "_guarded_pwrite_np",
    "_guarded_write_np",
    "_guarded_writev_np",
    "_host_check_multiuser_mode",
    "_host_create_mach_voucher",
    "_host_create_mach_voucher_trap",
    "_host_default_memory_manager",
    "_host_get_UNDServer",
    "_host_get_atm_diagnostic_flag",
    "_host_get_boot_info",
    "_host_get_clock_control",
    "_host_get_clock_service",
    "_host_get_exception_ports",
    "_host_get_io_main",
    "_host_get_io_master",
    "_host_get_multiuser_config_flags",
    "_host_get_special_port",
    "_host_info",
    "_host_kernel_version",
    "_host_lockgroup_info",
    "_host_page_size",
    "_host_priv_statistics",
    "_host_processor_info",
    "_host_processor_set_priv",
    "_host_processor_sets",
    "_host_processors",
    "_host_reboot",
    "_host_register_mach_voucher_attr_manager",
    "_host_register_well_known_mach_voucher_attr_manager",
    "_host_request_notification",
    "_host_security_create_task_token",
    "_host_security_set_task_token",
    "_host_self",
    "_host_self_trap",
    "_host_set_UNDServer",
    "_host_set_atm_diagnostic_flag",
    "_host_set_exception_ports",
    "_host_set_multiuser_config_flags",
    "_host_set_special_port",
    "_host_statistics",
    "_host_statistics64",
    "_host_swap_exception_ports",
    "_host_virtual_physical_table_info",
    "_important_boost_assertion_token",
    "_internal_catch_exc_subsystem",
    "_ioctl",
    "_issetugid",
    "_kas_info",
    "_kdebug_is_enabled",
    "_kdebug_signpost",
    "_kdebug_signpost_end",
    "_kdebug_signpost_start",
    "_kdebug_timestamp",
    "_kdebug_timestamp_from_absolute",
    "_kdebug_timestamp_from_continuous",
    "_kdebug_trace",
    "_kdebug_trace_string",
    "_kdebug_typefilter",
    "_kdebug_using_continuous_time",
    "_kevent",
    "_kevent64",
    "_kevent_id",
    "_kevent_qos",
    "_kext_request",
    "_kill",
    "_kmod_control",
    "_kmod_create",
    "_kmod_destroy",
    "_kmod_get_info",
    "_kpersona_alloc",
    "_kpersona_dealloc",
    "_kpersona_find",
    "_kpersona_find_by_type",
    "_kpersona_get",
    "_kpersona_getpath",
    "_kpersona_info",
    "_kpersona_palloc",
    "_kpersona_pidinfo",
    "_kqueue",
    "_lchown",
    "_ledger",
    "_link",
    "_linkat",
    "_lio_listio",
    "_listen",
    "_listxattr",
    "_lock_set_create",
    "_lock_set_destroy",
    "_log_data_as_kernel",
    "_lseek",
    "_lstat",
    "_lstat$INODE64",               // x86_64 only
    "_lstat64",
    "_mach_absolute_time",
    "_mach_absolute_time_kernel",   // arm64 only
    "_mach_approximate_time",
    "_mach_boottime_usec",
    "_mach_continuous_approximate_time",
    "_mach_continuous_time",
    "_mach_continuous_time_kernel", // arm64_only
    "_mach_error",
    "_mach_error_full_diag",
    "_mach_error_string",
    "_mach_error_type",
    "_mach_eventlink_associate",
    "_mach_eventlink_create",
    "_mach_eventlink_destroy",
    "_mach_eventlink_disassociate",
    "_mach_eventlink_signal",
    "_mach_eventlink_signal_wait_until",
    "_mach_eventlink_wait_until",
    "_mach_generate_activity_id",
    "_mach_get_times",
    "_mach_host_self",
    "_mach_host_special_port_description",
    "_mach_host_special_port_for_id",
    "_mach_init",
    "_mach_make_memory_entry",
    "_mach_make_memory_entry_64",
    "_mach_memory_entry_access_tracking",
    "_mach_memory_entry_ownership",
    "_mach_memory_entry_purgable_control",
    "_mach_memory_info",
    "_mach_memory_object_memory_entry",
    "_mach_memory_object_memory_entry_64",
    "_mach_msg",
    "_mach_msg2_internal",
    "_mach_msg2_trap",
    "_mach_msg_destroy",
    "_mach_msg_overwrite",
    "_mach_msg_overwrite_trap",
    "_mach_msg_priority_encode",
    "_mach_msg_priority_is_pthread_priority",
    "_mach_msg_priority_overide_qos",
    "_mach_msg_priority_qos",
    "_mach_msg_priority_relpri",
    "_mach_msg_receive",
    "_mach_msg_send",
    "_mach_msg_server",
    "_mach_msg_server_importance",
    "_mach_msg_server_once",
    "_mach_msg_trap",
    "_mach_notify_dead_name",
    "_mach_notify_no_senders",
    "_mach_notify_port_deleted",
    "_mach_notify_port_destroyed",
    "_mach_notify_send_once",
    "_mach_port_allocate",
    "_mach_port_allocate_full",
    "_mach_port_allocate_name",
    "_mach_port_allocate_qos",
    "_mach_port_assert_attributes",
    "_mach_port_construct",
    "_mach_port_deallocate",
    "_mach_port_destroy",
    "_mach_port_destruct",
    "_mach_port_dnrequest_info",
    "_mach_port_extract_member",
    "_mach_port_extract_right",
    "_mach_port_get_attributes",
    "_mach_port_get_context",
    "_mach_port_get_refs",
    "_mach_port_get_service_port_info",
    "_mach_port_get_set_status",
    "_mach_port_get_srights",
    "_mach_port_guard",
    "_mach_port_guard_with_flags",
    "_mach_port_insert_member",
    "_mach_port_insert_right",
    "_mach_port_is_connection_for_service",
    "_mach_port_kernel_object",
    "_mach_port_kobject",
    "_mach_port_kobject_description",
    "_mach_port_mod_refs",
    "_mach_port_move_member",
    "_mach_port_names",
    "_mach_port_peek",
    "_mach_port_rename",
    "_mach_port_request_notification",
    "_mach_port_set_attributes",
    "_mach_port_set_context",
    "_mach_port_set_mscount",
    "_mach_port_set_seqno",
    "_mach_port_space_basic_info",
    "_mach_port_space_info",
    "_mach_port_swap_guard",
    "_mach_port_type",
    "_mach_port_unguard",
    "_mach_ports_lookup",
    "_mach_ports_register",
    "_mach_reply_port",
    "_mach_right_recv_construct",
    "_mach_right_recv_destruct",
    "_mach_right_send_create",
    "_mach_right_send_once_consume",
    "_mach_right_send_once_create",
    "_mach_right_send_release",
    "_mach_right_send_retain",
    "_mach_sync_ipc_link_monitoring_start",
    "_mach_sync_ipc_link_monitoring_stop",
    "_mach_task_is_self",
    "_mach_task_self",
    "_mach_task_self_",
    "_mach_task_special_port_description",
    "_mach_task_special_port_for_id",
    "_mach_thread_self",
    "_mach_thread_special_port_description",
    "_mach_thread_special_port_for_id",
    "_mach_timebase_info",
    "_mach_timebase_info_trap",
    "_mach_vm_allocate",
    "_mach_vm_behavior_set",
    "_mach_vm_copy",
    "_mach_vm_deallocate",
    "_mach_vm_deferred_reclamation_buffer_allocate",
    "_mach_vm_deferred_reclamation_buffer_flush",
    "_mach_vm_deferred_reclamation_buffer_resize",
    "_mach_vm_deferred_reclamation_buffer_update_reclaimable_bytes",
    "_mach_vm_inherit",
    "_mach_vm_machine_attribute",
    "_mach_vm_map",
    "_mach_vm_msync",
    "_mach_vm_page_info",
    "_mach_vm_page_query",
    "_mach_vm_page_range_query",
    "_mach_vm_protect",
    "_mach_vm_purgable_control",
    "_mach_vm_range_create",
    "_mach_vm_read",
    "_mach_vm_read_list",
    "_mach_vm_read_overwrite",
    "_mach_vm_reclaim_is_reusable",
    "_mach_vm_reclaim_query_state",
    "_mach_vm_reclaim_ring_allocate",
    "_mach_vm_reclaim_ring_capacity",
    "_mach_vm_reclaim_ring_flush",
    "_mach_vm_reclaim_ring_resize",
    "_mach_vm_reclaim_round_capacity",
    "_mach_vm_reclaim_try_cancel",
    "_mach_vm_reclaim_try_enter",
    "_mach_vm_reclaim_update_kernel_accounting",
    "_mach_vm_region",
    "_mach_vm_region_recurse",
    "_mach_vm_remap",
    "_mach_vm_remap_new",
    "_mach_vm_wire",
    "_mach_vm_write",
    "_mach_voucher_attr_command",
    "_mach_voucher_deallocate",
    "_mach_voucher_debug_info",
    "_mach_voucher_extract_all_attr_recipes",
    "_mach_voucher_extract_attr_content",
    "_mach_voucher_extract_attr_recipe",
    "_mach_voucher_extract_attr_recipe_trap",
    "_mach_wait_until",
    "_mach_zone_force_gc",
    "_mach_zone_get_btlog_records",
    "_mach_zone_get_zlog_zones",
    "_mach_zone_info",
    "_mach_zone_info_for_largest_zone",
    "_mach_zone_info_for_zone",
    "_macx_backing_store_recovery",
    "_macx_backing_store_suspend",
    "_macx_swapoff",
    "_macx_swapon",
    "_macx_triggers",
    "_madvise",
    "_memorystatus_control",
    "_memorystatus_get_level",
    "_mig_allocate",
    "_mig_dealloc_reply_port",
    "_mig_dealloc_special_reply_port",
    "_mig_deallocate",
    "_mig_get_reply_port",
    "_mig_get_special_reply_port",
    "_mig_put_reply_port",
    "_mig_reply_setup",
    "_mig_strncpy",
    "_mig_strncpy_zerofill",
    "_mincore",
    "_minherit",
    "_mk_timer_arm",
    "_mk_timer_arm_leeway",
    "_mk_timer_cancel",
    "_mk_timer_create",
    "_mk_timer_destroy",
    "_mkdir",
    "_mkdirat",
    "_mkfifo",
    "_mkfifoat",
    "_mknod",
    "_mknodat",
    "_mlock",
    "_mlockall",
    "_mmap",
    "_mount",
    "_mprotect",
    "_mremap_encrypted",
    "_msg_receive",
    "_msg_rpc",
    "_msg_send",
    "_msgctl",
    "_msgget",
    "_msgrcv",
    "_msgrcv$NOCANCEL",
    "_msgsnd",
    "_msgsnd$NOCANCEL",
    "_msgsys",
    "_msync",
    "_msync$NOCANCEL",
    "_munlock",
    "_munlockall",
    "_munmap",
    "_necp_client_action",
    "_necp_match_policy",
    "_necp_open",
    "_necp_session_action",
    "_necp_session_open",
    "_net_qos_guideline",
    "_netagent_trigger",
    "_netname_check_in",
    "_netname_check_out",
    "_netname_look_up",
    "_netname_version",
    "_nfsclnt",
    "_nfssvc",
    "_non_boost_assertion_token",
    "_normal_boost_assertion_token",
    "_ntp_adjtime",
    "_ntp_gettime",
    "_objc_bp_assist_cfg_np",
    "_open",
    "_open$NOCANCEL",
    "_open_dprotected_np",
    "_openat",
    "_openat$NOCANCEL",
    "_openat_authenticated_np",
    "_openat_dprotected_np",
    "_openbyid_np",
    "_os_buflet_get_data_address",
    "_os_buflet_get_data_length",
    "_os_buflet_get_data_limit",
    "_os_buflet_get_data_offset",
    "_os_buflet_get_object_address",
    "_os_buflet_get_object_limit",
    "_os_buflet_set_data_length",
    "_os_buflet_set_data_offset",
    "_os_channel_advance_slot",
    "_os_channel_attr_clone",
    "_os_channel_attr_create",
    "_os_channel_attr_destroy",
    "_os_channel_attr_get",
    "_os_channel_attr_get_key",
    "_os_channel_attr_set",
    "_os_channel_attr_set_key",
    "_os_channel_available_slot_count",
    "_os_channel_buflet_alloc",
    "_os_channel_buflet_free",
    "_os_channel_configure_interface_advisory",
    "_os_channel_create",
    "_os_channel_create_extended",
    "_os_channel_destroy",
    "_os_channel_event_free",
    "_os_channel_event_get_event_data",
    "_os_channel_event_get_next_event",
    "_os_channel_flow_admissible",
    "_os_channel_flow_adv_get_ce_count",
    "_os_channel_get_advisory_region",
    "_os_channel_get_fd",
    "_os_channel_get_interface_advisory",
    "_os_channel_get_next_event_handle",
    "_os_channel_get_next_slot",
    "_os_channel_get_stats_region",
    "_os_channel_is_defunct",
    "_os_channel_large_packet_alloc",
    "_os_channel_packet_alloc",
    "_os_channel_packet_free",
    "_os_channel_packet_pool_purge",
    "_os_channel_pending",
    "_os_channel_read_attr",
    "_os_channel_read_nexus_extension_info",
    "_os_channel_ring_id",
    "_os_channel_ring_notify_time",
    "_os_channel_ring_sync_time",
    "_os_channel_rx_ring",
    "_os_channel_set_slot_properties",
    "_os_channel_slot_attach_packet",
    "_os_channel_slot_detach_packet",
    "_os_channel_slot_get_packet",
    "_os_channel_sync",
    "_os_channel_tx_ring",
    "_os_channel_write_attr",
    "_os_copy_and_inet_checksum",
    "_os_cpu_copy_in_cksum",
    "_os_cpu_in_cksum",
    "_os_cpu_in_cksum_mbuf",
    "_os_fault_with_payload",
    "_os_inet_checksum",
    "_os_log_coprocessor_as_kernel",
    "_os_log_coprocessor_register_as_kernel",
    "_os_nexus_attr_clone",
    "_os_nexus_attr_create",
    "_os_nexus_attr_destroy",
    "_os_nexus_attr_get",
    "_os_nexus_attr_set",
    "_os_nexus_controller_add_traffic_rule",
    "_os_nexus_controller_alloc_provider_instance",
    "_os_nexus_controller_bind_provider_instance",
    "_os_nexus_controller_create",
    "_os_nexus_controller_deregister_provider",
    "_os_nexus_controller_destroy",
    "_os_nexus_controller_free_provider_instance",
    "_os_nexus_controller_get_fd",
    "_os_nexus_controller_iterate_traffic_rules",
    "_os_nexus_controller_read_provider_attr",
    "_os_nexus_controller_register_provider",
    "_os_nexus_controller_remove_traffic_rule",
    "_os_nexus_controller_unbind_provider_instance",
    "_os_nexus_flow_set_wake_from_sleep",
    "_os_packet_add_buflet",
    "_os_packet_add_inet_csum_flags",
    "_os_packet_clear_flow_uuid",
    "_os_packet_decrement_use_count",
    "_os_packet_finalize",
    "_os_packet_get_aggregation_type",
    "_os_packet_get_buflet_count",
    "_os_packet_get_compression_generation_count",
    "_os_packet_get_data_length",
    "_os_packet_get_expire_time",
    "_os_packet_get_expiry_action",
    "_os_packet_get_flow_uuid",
    "_os_packet_get_group_end",
    "_os_packet_get_group_start",
    "_os_packet_get_headroom",
    "_os_packet_get_inet_checksum",
    "_os_packet_get_keep_alive",
    "_os_packet_get_link_broadcast",
    "_os_packet_get_link_ethfcs",
    "_os_packet_get_link_header_length",
    "_os_packet_get_link_multicast",
    "_os_packet_get_next_buflet",
    "_os_packet_get_packetid",
    "_os_packet_get_segment_count",
    "_os_packet_get_service_class",
    "_os_packet_get_token",
    "_os_packet_get_trace_id",
    "_os_packet_get_traffic_class",
    "_os_packet_get_transport_retransmit",
    "_os_packet_get_transport_traffic_background",
    "_os_packet_get_transport_traffic_realtime",
    "_os_packet_get_truncated",
    "_os_packet_get_vlan_id",
    "_os_packet_get_vlan_priority",
    "_os_packet_get_vlan_tag",
    "_os_packet_get_wake_flag",
    "_os_packet_increment_use_count",
    "_os_packet_set_app_metadata",
    "_os_packet_set_compression_generation_count",
    "_os_packet_set_expire_time",
    "_os_packet_set_expiry_action",
    "_os_packet_set_flow_uuid",
    "_os_packet_set_group_end",
    "_os_packet_set_group_start",
    "_os_packet_set_headroom",
    "_os_packet_set_inet_checksum",
    "_os_packet_set_keep_alive",
    "_os_packet_set_l4s_flag",
    "_os_packet_set_link_broadcast",
    "_os_packet_set_link_ethfcs",
    "_os_packet_set_link_header_length",
    "_os_packet_set_link_multicast",
    "_os_packet_set_packetid",
    "_os_packet_set_protocol_segment_size",
    "_os_packet_set_service_class",
    "_os_packet_set_token",
    "_os_packet_set_trace_id",
    "_os_packet_set_traffic_class",
    "_os_packet_set_transport_last_packet",
    "_os_packet_set_transport_retransmit",
    "_os_packet_set_transport_traffic_background",
    "_os_packet_set_transport_traffic_realtime",
    "_os_packet_set_tso_flags",
    "_os_packet_set_tx_timestamp",
    "_os_packet_set_vlan_tag",
    "_os_packet_trace_event",
    "_os_proc_available_memory",
    "_panic",
    "_panic_init",
    "_panic_with_data",
    "_pathconf",
    "_peeloff",
    "_pid_for_task",
    "_pid_hibernate",
    "_pid_resume",
    "_pid_shutdown_networking",
    "_pid_shutdown_sockets",
    "_pid_suspend",
    "_pipe",
    "_pivot_root",
    "_pkt_subtype_assert_fail",
    "_pkt_type_assert_fail",
    "_poll",
    "_poll$NOCANCEL",
    "_port_obj_init",
    "_port_obj_table",
    "_port_obj_table_size",
    "_posix_madvise",
    "_posix_spawn",
    "_posix_spawn_file_actions_add_fileportdup2_np",
    "_posix_spawn_file_actions_addchdir_np",
    "_posix_spawn_file_actions_addclose",
    "_posix_spawn_file_actions_adddup2",
    "_posix_spawn_file_actions_addfchdir_np",
    "_posix_spawn_file_actions_addinherit_np",
    "_posix_spawn_file_actions_addopen",
    "_posix_spawn_file_actions_destroy",
    "_posix_spawn_file_actions_init",
    "_posix_spawnattr_destroy",
    "_posix_spawnattr_disable_ptr_auth_a_keys_np",
    "_posix_spawnattr_get_darwin_role_np",
    "_posix_spawnattr_get_qos_clamp_np",
    "_posix_spawnattr_getarchpref_np",
    "_posix_spawnattr_getbinpref_np",
    "_posix_spawnattr_getcpumonitor",
    "_posix_spawnattr_getflags",
    "_posix_spawnattr_getmacpolicyinfo_np",
    "_posix_spawnattr_getpcontrol_np",
    "_posix_spawnattr_getpgroup",
    "_posix_spawnattr_getprocesstype_np",
    "_posix_spawnattr_getsigdefault",
    "_posix_spawnattr_getsigmask",
    "_posix_spawnattr_init",
    "_posix_spawnattr_set_alt_rosetta_np",
    "_posix_spawnattr_set_conclave_id_np",
    "_posix_spawnattr_set_crash_behavior_deadline_np",
    "_posix_spawnattr_set_crash_behavior_np",
    "_posix_spawnattr_set_crash_count_np",
    "_posix_spawnattr_set_csm_np",
    "_posix_spawnattr_set_darwin_role_np",
    "_posix_spawnattr_set_filedesclimit_ext",
    "_posix_spawnattr_set_gid_np",
    "_posix_spawnattr_set_groups_np",
    "_posix_spawnattr_set_importancewatch_port_np",
    "_posix_spawnattr_set_jetsam_ttr_np",
    "_posix_spawnattr_set_kqworklooplimit_ext",
    "_posix_spawnattr_set_launch_type_np",
    "_posix_spawnattr_set_login_np",
    "_posix_spawnattr_set_max_addr_np",
    "_posix_spawnattr_set_persona_gid_np",
    "_posix_spawnattr_set_persona_groups_np",
    "_posix_spawnattr_set_persona_np",
    "_posix_spawnattr_set_persona_uid_np",
    "_posix_spawnattr_set_platform_np",
    "_posix_spawnattr_set_portlimits_ext",
    "_posix_spawnattr_set_ptrauth_task_port_np",
    "_posix_spawnattr_set_qos_clamp_np",
    "_posix_spawnattr_set_registered_ports_np",
    "_posix_spawnattr_set_subsystem_root_path_np",
    "_posix_spawnattr_set_threadlimit_ext",
    "_posix_spawnattr_set_uid_np",
    "_posix_spawnattr_set_use_sec_transition_shims_np",
    "_posix_spawnattr_setarchpref_np",
    "_posix_spawnattr_setauditsessionport_np",
    "_posix_spawnattr_setbinpref_np",
    "_posix_spawnattr_setcoalition_np",
    "_posix_spawnattr_setcpumonitor",
    "_posix_spawnattr_setcpumonitor_default",
    "_posix_spawnattr_setdataless_iopolicy_np",
    "_posix_spawnattr_setexceptionports_np",
    "_posix_spawnattr_setflags",
    "_posix_spawnattr_setjetsam_ext",
    "_posix_spawnattr_setmacpolicyinfo_np",
    "_posix_spawnattr_setnosmt_np",
    "_posix_spawnattr_setpcontrol_np",
    "_posix_spawnattr_setpgroup",
    "_posix_spawnattr_setprocesstype_np",
    "_posix_spawnattr_setsigdefault",
    "_posix_spawnattr_setsigmask",
    "_posix_spawnattr_setspecialport_np",
    "_pread",
    "_pread$NOCANCEL",
    "_preadv",
    "_preadv$NOCANCEL",
    "_proc_clear_cpulimits",
    "_proc_clear_delayidlesleep",
    "_proc_clear_dirty",
    "_proc_clear_vmpressure",
    "_proc_current_thread_schedinfo",
    "_proc_denap_assertion_begin_with_msg",
    "_proc_denap_assertion_complete",
    "_proc_disable_apptype",
    "_proc_disable_cpumon",
    "_proc_disable_wakemon",
    "_proc_donate_importance_boost",
    "_proc_enable_apptype",
    "_proc_get_cpumon_params",
    "_proc_get_dirty",
    "_proc_get_wakemon_params",
    "_proc_importance_assertion_begin_with_msg",
    "_proc_importance_assertion_complete",
    "_proc_kmsgbuf",
    "_proc_libversion",
    "_proc_list_dynkqueueids",
    "_proc_list_uptrs",
    "_proc_listallpids",
    "_proc_listchildpids",
    "_proc_listcoalitions",
    "_proc_listpgrppids",
    "_proc_listpids",
    "_proc_listpidspath",
    "_proc_name",
    "_proc_pid_rusage",
    "_proc_piddynkqueueinfo",
    "_proc_pidfdinfo",
    "_proc_pidfileportinfo",
    "_proc_pidinfo",
    "_proc_pidoriginatorinfo",
    "_proc_pidpath",
    "_proc_pidpath_audittoken",
    "_proc_regionfilename",
    "_proc_reset_footprint_interval",
    "_proc_resume_cpumon",
    "_proc_rlimit_control",
    "_proc_set_cpumon_defaults",
    "_proc_set_cpumon_params",
    "_proc_set_cpumon_params_fatal",
    "_proc_set_csm",
    "_proc_set_delayidlesleep",
    "_proc_set_dirty",
    "_proc_set_no_smt",
    "_proc_set_owner_vmpressure",
    "_proc_set_wakemon_defaults",
    "_proc_set_wakemon_params",
    "_proc_setcpu_percentage",
    "_proc_setpcontrol",
    "_proc_setthread_cpupercent",
    "_proc_setthread_csm",
    "_proc_setthread_no_smt",
    "_proc_signal_delegate",
    "_proc_signal_with_audittoken",
    "_proc_suppress",
    "_proc_terminate",
    "_proc_terminate_all_rsr",
    "_proc_terminate_delegate",
    "_proc_terminate_with_audittoken",
    "_proc_trace_log",
    "_proc_track_dirty",
    "_proc_udata_info",
    "_proc_uuid_policy",
    "_processor_assign",
    "_processor_control",
    "_processor_exit",
    "_processor_get_assignment",
    "_processor_info",
    "_processor_set_create",
    "_processor_set_default",
    "_processor_set_destroy",
    "_processor_set_info",
    "_processor_set_max_priority",
    "_processor_set_policy_control",
    "_processor_set_policy_disable",
    "_processor_set_policy_enable",
    "_processor_set_stack_usage",
    "_processor_set_statistics",
    "_processor_set_tasks",
    "_processor_set_tasks_with_flavor",
    "_processor_set_threads",
    "_processor_start",
    "_pselect",
    "_pselect$1050",
    "_pselect$DARWIN_EXTSN",
    "_pselect$DARWIN_EXTSN$NOCANCEL",
    "_pselect$NOCANCEL",
    "_pthread_getugid_np",
    "_pthread_setugid_np",
    "_ptrace",
    "_pwrite",
    "_pwrite$NOCANCEL",
    "_pwritev",
    "_pwritev$NOCANCEL",
    "_quota",
    "_quotactl",
    "_read",
    "_read$NOCANCEL",
    "_readlink",
    "_readlinkat",
    "_readv",
    "_readv$NOCANCEL",
    "_reboot",
    "_reboot_np",
    "_record_system_event_as_kernel",
    "_recvfrom",
    "_recvfrom$NOCANCEL",
    "_recvmsg",
    "_recvmsg$NOCANCEL",
    "_recvmsg_x",
    "_register_uexc_handler",
    "_removexattr",
    "_rename",
    "_rename_ext",
    "_renameat",
    "_renameatx_np",
    "_renamex_np",
    "_revoke",
    "_rmdir",
    "_searchfs",
    "_select",
    "_select$1050",
    "_select$DARWIN_EXTSN",
    "_select$DARWIN_EXTSN$NOCANCEL",
    "_select$NOCANCEL",
    "_sem_close",
    "_sem_destroy",
    "_sem_getvalue",
    "_sem_init",
    "_sem_open",
    "_sem_post",
    "_sem_trywait",
    "_sem_unlink",
    "_sem_wait",
    "_sem_wait$NOCANCEL",
    "_semaphore_create",
    "_semaphore_destroy",
    "_semaphore_signal",
    "_semaphore_signal_all",
    "_semaphore_signal_all_trap",
    "_semaphore_signal_thread",
    "_semaphore_signal_thread_trap",
    "_semaphore_signal_trap",
    "_semaphore_timedwait",
    "_semaphore_timedwait_signal",
    "_semaphore_timedwait_signal_trap",
    "_semaphore_timedwait_trap",
    "_semaphore_wait",
    "_semaphore_wait_signal",
    "_semaphore_wait_signal_trap",
    "_semaphore_wait_trap",
    "_semctl",
    "_semget",
    "_semop",
    "_semsys",
    "_sendfile",
    "_sendmsg",
    "_sendmsg$NOCANCEL",
    "_sendmsg_x",
    "_sendto",
    "_sendto$NOCANCEL",
    "_setattrlist",
    "_setattrlistat",
    "_setaudit",
    "_setaudit_addr",
    "_setauid",
    "_setegid",
    "_seteuid",
    "_setgid",
    "_setgroups",
    "_setiopolicy_np",
    "_setitimer",
    "_setpgid",
    "_setpriority",
    "_setprivexec",
    "_setquota",
    "_setregid",
    "_setreuid",
    "_setrlimit",
    "_setsgroups_np",
    "_setsid",
    "_setsockopt",
    "_setuid",
    "_setwgroups_np",
    "_setxattr",
    "_sfi_get_class_offtime",
    "_sfi_process_get_flags",
    "_sfi_process_set_flags",
    "_sfi_set_class_offtime",
    "_shm_open",
    "_shm_unlink",
    "_shmat",
    "_shmctl",
    "_shmdt",
    "_shmget",
    "_shmsys",
    "_shutdown",
    "_sigpending",
    "_sigprocmask",
    "_sigsuspend",
    "_sigsuspend$NOCANCEL",
    "_socket",
    "_socket_delegate",
    "_socketpair",
    "_stackshot_capture_with_config",
    "_stackshot_config_create",
    "_stackshot_config_dealloc",
    "_stackshot_config_dealloc_buffer",
    "_stackshot_config_get_stackshot_buffer",
    "_stackshot_config_get_stackshot_size",
    "_stackshot_config_set_delta_timestamp",
    "_stackshot_config_set_flags",
    "_stackshot_config_set_pagetable_mask",
    "_stackshot_config_set_pid",
    "_stackshot_config_set_size_hint",
    "_stat",
    "_stat$INODE64",        // x86_64 only
    "_stat64",
    "_statfs",
    "_statfs$INODE64",      // x86_64 only
    "_statfs64",
    "_statfs_ext",
    "_swapon",
    "_swtch",
    "_swtch_pri",
    "_symlink",
    "_symlinkat",
    "_sync",
    "_syscall",
    "_syscall_thread_switch",
    "_system_get_sfi_window",
    "_system_override",
    "_system_set_sfi_window",
    "_system_version_compat_mode",
    "_task_assign",
    "_task_assign_default",
    "_task_create",
    "_task_create_identity_token",
    "_task_dyld_process_info_notify_deregister",
    "_task_dyld_process_info_notify_get",
    "_task_dyld_process_info_notify_register",
    "_task_for_pid",
    "_task_generate_corpse",
    "_task_get_assignment",
    "_task_get_dyld_image_infos",
    "_task_get_emulation_vector",
    "_task_get_exc_guard_behavior",
    "_task_get_exception_ports",
    "_task_get_exception_ports_info",
    "_task_get_mach_voucher",
    "_task_get_special_port",
    "_task_get_state",
    "_task_identity_token_get_task_port",
    "_task_info",
    "_task_inspect",
    "_task_inspect_for_pid",
    "_task_map_corpse_info",
    "_task_map_corpse_info_64",
    "_task_map_kcdata_object_64",
    "_task_name_for_pid",
    "_task_policy",
    "_task_policy_get",
    "_task_policy_set",
    "_task_purgable_info",
    "_task_read_for_pid",
    "_task_register_dyld_get_process_state",
    "_task_register_dyld_image_infos",
    "_task_register_dyld_set_dyld_state",
    "_task_register_dyld_shared_cache_image_info",
    "_task_register_hardened_exception_handler",
    "_task_restartable_ranges_register",
    "_task_restartable_ranges_synchronize",
    "_task_resume",
    "_task_resume2",
    "_task_sample",
    "_task_self_",
    "_task_self_trap",
    "_task_set_corpse_forking_behavior",
    "_task_set_emulation",
    "_task_set_emulation_vector",
    "_task_set_exc_guard_behavior",
    "_task_set_exception_ports",
    "_task_set_info",
    "_task_set_mach_voucher",
    "_task_set_phys_footprint_limit",
    "_task_set_policy",
    "_task_set_port_space",
    "_task_set_ras_pc",
    "_task_set_special_port",
    "_task_set_state",
    "_task_suspend",
    "_task_suspend2",
    "_task_swap_exception_ports",
    "_task_swap_mach_voucher",
    "_task_terminate",
    "_task_test_async_upcall_propagation",
    "_task_test_sync_upcall",
    "_task_threads",
    "_task_unregister_dyld_image_infos",
    "_task_zone_info",
    "_terminate_with_payload",
    "_terminate_with_reason",
    "_thread_abort",
    "_thread_abort_safely",
    "_thread_adopt_exception_handler",
    "_thread_assign",
    "_thread_assign_default",
    "_thread_convert_thread_state",
    "_thread_create",
    "_thread_create_running",
    "_thread_depress_abort",
    "_thread_destruct_special_reply_port",
    "_thread_get_assignment",
    "_thread_get_exception_ports",
    "_thread_get_exception_ports_info",
    "_thread_get_mach_voucher",
    "_thread_get_register_pointer_values",
    "_thread_get_special_port",
    "_thread_get_special_reply_port",
    "_thread_get_state",
    "_thread_info",
    "_thread_policy",
    "_thread_policy_get",
    "_thread_policy_set",
    "_thread_resume",
    "_thread_sample",
    "_thread_self_trap",
    "_thread_selfcounts",
    "_thread_set_exception_ports",
    "_thread_set_mach_voucher",
    "_thread_set_policy",
    "_thread_set_special_port",
    "_thread_set_state",
    "_thread_suspend",
    "_thread_swap_exception_ports",
    "_thread_swap_mach_voucher",
    "_thread_switch",
    "_thread_terminate",
    "_thread_wire",
    "_tracker_action",
    "_truncate",
    "_umask",
    "_undelete",
    "_ungraftdmg",
    "_unlink",
    "_unlinkat",
    "_unmount",
    "_usrctl",
    "_utimensat",
    "_utimes",
    "_vfs_purge",
    "_vm_allocate",
    "_vm_allocate_cpm",
    "_vm_behavior_set",
    "_vm_copy",
    "_vm_deallocate",
    "_vm_inherit",
    "_vm_kernel_page_mask",
    "_vm_kernel_page_shift",
    "_vm_kernel_page_size",
    "_vm_machine_attribute",
    "_vm_map",
    "_vm_map_page_query",
    "_vm_msync",
    "_vm_page_mask",
    "_vm_page_shift",
    "_vm_page_size",
    "_vm_pressure_monitor",
    "_vm_protect",
    "_vm_purgable_control",
    "_vm_read",
    "_vm_read_list",
    "_vm_read_overwrite",
    "_vm_region_64",
    "_vm_region_recurse_64",
    "_vm_remap",
    "_vm_remap_new",
    "_vm_wire",
    "_vm_write",
    "_voucher_mach_msg_adopt",
    "_voucher_mach_msg_clear",
    "_voucher_mach_msg_revert",
    "_voucher_mach_msg_set",
    "_vprintf_stderr_func",
    "_wait4",
    "_waitid",
    "_waitid$NOCANCEL",
    "_work_interval_copy_port",
    "_work_interval_create",
    "_work_interval_destroy",
    "_work_interval_get_flags_from_port",
    "_work_interval_instance_alloc",
    "_work_interval_instance_clear",
    "_work_interval_instance_finish",
    "_work_interval_instance_free",
    "_work_interval_instance_get_complexity",
    "_work_interval_instance_get_deadline",
    "_work_interval_instance_get_finish",
    "_work_interval_instance_get_id",
    "_work_interval_instance_get_start",
    "_work_interval_instance_get_telemetry_data",
    "_work_interval_instance_set_complexity",
    "_work_interval_instance_set_deadline",
    "_work_interval_instance_set_finish",
    "_work_interval_instance_set_start",
    "_work_interval_instance_start",
    "_work_interval_instance_update",
    "_work_interval_join",
    "_work_interval_join_port",
    "_work_interval_leave",
    "_work_interval_notify",
    "_work_interval_notify_simple",
    "_write",
    "_write$NOCANCEL",
    "_writev",
    "_writev$NOCANCEL"
};


// look for what symbols are exported by /usr/lib/system/libsystem_blah.dylib on host
static void getHostDylibExports(CString path, std::vector<CString>& symbols)
{
    if ( path == "/usr/lib/system/libsystem_platform.dylib" ) {
        for ( const char* sym : libPlatformExports ) {
            symbols.push_back(sym);
        }
    }
    else if ( path == "/usr/lib/system/libsystem_pthread.dylib" ) {
        for ( const char* sym : libPthreadExports ) {
            symbols.push_back(sym);
        }
    }
    else if ( path == "/usr/lib/system/libsystem_kernel.dylib" ) {
        for ( const char* sym : libKernelExports ) {
            symbols.push_back(sym);
        }
    }
}

static std::string makeJsonDylib(CString installName, std::vector<CString> exportedNames, mach_o::Architecture arch, mach_o::Platform platform)
{
    std::string result = "{\n \"version\": 1,\n \"platformVersion\": \"15.0\",\n";
    result += " \"platform\": ";
    result += std::to_string(platform.value());
    result += ",\n";
    result += " \"arch\": \"";
    result += arch.name();
    result += "\",\n";
    result += " \"installName\": \"";
    result += installName;
    result += "\",\n";
    result += " \"atoms\": [\n";
    for (CString exportName : exportedNames) {
        result += "  {\n";
        result += "    \"name\": \"";
        result += exportName;
        result += "\",\n";
        result += "    \"contentType\": \"function\",\n";
        result += "    \"contents\": [ \"00000000\" ]\n";
        result += "  },\n";
    }
   // result += " { }\n";
    result += " ]\n";
    result += "}\n";
    return result;
}


static std::span<const uint8_t> synthesizeHostDylib(CString installName, mach_o::Architecture arch, mach_o::Platform platform)
{
    std::vector<CString> exportedNames;
    getHostDylibExports(installName, exportedNames);

    // TODO: support in-memory file buffer
    std::string outPath;
    if ( const char* dir = ::getenv("TMPDIR") ) {
        if ( ::access(dir, W_OK) == 0 ) {
            outPath = dir;
            outPath += "/";
        }
    }
    if ( outPath.empty() )
        outPath = "/tmp/";
    outPath += installName.leafName();
    outPath += "-XXXXXX";

    int outFileFd = ::mkstemp(outPath.data());
    close(outFileFd);

    std::string jsonData = makeJsonDylib(installName, exportedNames, arch, platform);
    //printf("LOG: json: %s\n", jsonData.c_str());
    const char* err = ldMakeDylibFromJSON(jsonData, {}, outPath.c_str());
    if ( err == nullptr ) {
        size_t mappedSize;
        const void* mappedFile = mapFileReadOnly(outPath.c_str(), mappedSize);
        ::unlink(outPath.c_str());
        return std::span<const uint8_t>((const uint8_t*)mappedFile, mappedSize);
    }

    return std::span<const uint8_t>((const uint8_t*)nullptr, 0);
}

// there are three lib system dylibs where are not part of sim runtime, instead they come from the mac host
// when building the dyld cache in B&I we synthesize dummy dylibs for this which are patched
// into the sim dyld cache at runtime
static void addHostSimDylib(SharedCacheBuilder* cacheBuilder, const BuildInstance& buildInstance, const char* path)
{
    std::span<const uint8_t> synthBuff = synthesizeHostDylib(path, buildInstance.options->arch, buildInstance.options->platform);
    cacheBuilder->addFile(synthBuff.data(), synthBuff.size(), path, 0, 0, 0);
}
#endif // !BUILDING_SIM_CACHE_BUILDER


static void runBuilders(struct MRMSharedCacheBuilder* builder)
{
    for (auto& buildInstance : builder->builders) {
        // The build might overflow, so loop until we don't error from overflow
        std::string              swiftPrespecializedDylibBuildError;
        std::vector<std::string> evictedDylibs;
        std::unordered_set<std::string> evictedDylibsSet;
        __block std::unique_ptr<SharedCacheBuilder> cacheBuilder;
        Error error;

        // Note down when we start so we can emit it in stats().
        // This is outside the loop to include potential time in eviction and rebuilds
        uint64_t startTimeNanos = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        while ( true ) {
            cacheBuilder = std::make_unique<SharedCacheBuilder>(*buildInstance.options.get(), builder->fileSystem);
#if !BUILDING_SIM_CACHE_BUILDER
            if ( buildInstance.options->platform.isSimulator() ) {
                // there are three lib system dylibs where are not part of sim runtime, instead they come from the mac host
                // when building the dyld cache in B&I we synthesize dummy dylibs for this which are patched
                // into the sim dyld cache at runtime
                addHostSimDylib(cacheBuilder.get(), buildInstance, "/usr/lib/system/libsystem_platform.dylib");
                addHostSimDylib(cacheBuilder.get(), buildInstance, "/usr/lib/system/libsystem_kernel.dylib");
                addHostSimDylib(cacheBuilder.get(), buildInstance, "/usr/lib/system/libsystem_pthread.dylib");
            }
#endif
            // Add all the input files
            __block std::vector<cache_builder::InputFile> inputFiles;
            builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                                  FileFlags fileFlags, uint64_t inode, uint64_t modTime,
                                                  const char* projectName) {
                switch (fileFlags) {
                    case FileFlags::NoFlags:
                    case FileFlags::MustBeInCache:
                    case FileFlags::ShouldBeExcludedFromCacheIfUnusedLeaf:
                    case FileFlags::RequiredClosure:
                    case FileFlags::CanBeMissing:
                        break;
                    case FileFlags::DylibOrderFile:
                    case FileFlags::DirtyDataOrderFile:
                    case FileFlags::ObjCOptimizationsFile:
                    case FileFlags::SwiftGenericMetadataFile:
                    case FileFlags::OptimizationFile:
                        builder->error("Order files should not be in the file system");
                        return;
                    case FileFlags::PluginSwiftGenericMetadataBuilder:
                        builder->error("Plugins should not be in the file system");
                        return;
                }
                cacheBuilder->addFile(buffer, bufferSize, path, inode, modTime, evictedDylibsSet.count(path));
            });

            // Add resolved aliases (symlinks)
            cacheBuilder->setAliases(buildInstance.aliases, buildInstance.intermediateAliases);

            error = cacheBuilder->build();

            // Get result buffers, even if there's an error.  That way we'll free them
            cacheBuilder->getResults(buildInstance.cacheBuffers, buildInstance.atlas);

            if ( !error.hasError() )
                break;

            // try without Swift metadata dylib if building it failed
            bool retryWithoutSwiftMetadata = false;
            if ( !cacheBuilder->getSwiftPrespecializedDylibBuildError().empty() ) {
                assert(!buildInstance.options->swiftGenericMetadataFile.empty()
                        && "Swift prespecialization build error even with an empty metadata file");
                swiftPrespecializedDylibBuildError = std::string(cacheBuilder->getSwiftPrespecializedDylibBuildError());
                buildInstance.options->swiftGenericMetadataFile.clear();
                retryWithoutSwiftMetadata = true;
            }

            // We have an error. If its cache overflow, then we can try again, with some evicted dylibs
            bool retryEvictedDylibs = false;
            std::span<const std::string_view> newEvictedDylibs = cacheBuilder->getEvictedDylibs();
            if ( !newEvictedDylibs.empty() ) {
                evictedDylibs.insert(evictedDylibs.end(), newEvictedDylibs.begin(), newEvictedDylibs.end());
                evictedDylibsSet.insert(newEvictedDylibs.begin(), newEvictedDylibs.end());
                retryEvictedDylibs = true;
            }

            bool retry = retryEvictedDylibs || retryWithoutSwiftMetadata;
            if ( !retry ) {
                // Error wasn't eviction, nor Swift metadata build. Break out an handle it as a fatal error
                break;
            }

            // Cache eviction happened. Note down the bad dylibs, and try again
            // Note we should never have buffer data to free at this point as eviction should be
            // determined before buffers are allocated
            for ( const CacheBuffer& buffer : buildInstance.cacheBuffers ) {
                assert(buffer.bufferData == nullptr);
            }
            buildInstance.cacheBuffers.clear();
        }

        buildInstance.loggingPrefix = cacheBuilder->developmentLoggingPrefix();
        buildInstance.customerLoggingPrefix = cacheBuilder->customerLoggingPrefix();

        // Track all buffers to be freed/unmapped (see allocateSubCacheBuffers() for allocation)
        for ( const CacheBuffer& buffer : buildInstance.cacheBuffers ) {
            // In MRM, we vm_allocated
            builder->buffersToDeallocate.emplace_back((FileBuffer){ buffer.bufferData, buffer.bufferSize });
        }

        if ( error.hasError() ) {
            // First put the error in to a vector to own it
            buildInstance.errorStrings.push_back(error.message());

            cacheBuilder->forEachError(^(const std::string_view &str) {
                buildInstance.errorStrings.push_back(std::string(str));
            });

            // Then copy to a vector to reference the owner
            buildInstance.errors.reserve(buildInstance.errorStrings.size());
            for (const std::string& err : buildInstance.errorStrings)
                buildInstance.errors.push_back(err.c_str());

            // First put the warnings in to a vector to own them.
            if ( builder->options->verboseDiagnostics ) {
                // Add cache eviction warnings, if any
                for ( std::string path : evictedDylibs ) {
                    std::string reason = "Dylib located at '" + path + "' not placed in shared cache because: cache overflow";
                    buildInstance.warningStrings.push_back(reason);
                }
                if ( !swiftPrespecializedDylibBuildError.empty() )
                    buildInstance.warningStrings.push_back("Couldn't build Swift prespecialized metadata dylib: " + swiftPrespecializedDylibBuildError);

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

            if ( (buildInstance.options->platform == Platform::macOS)
                || buildInstance.options->platform.isSimulator() ) {
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

            // Add cache eviction warnings, if any
            for ( std::string path : evictedDylibs ) {
                std::string reason = "Dylib located at '" + path + "' not placed in shared cache because: cache overflow";
                buildInstance.warningStrings.push_back(reason);
            }
            if ( !swiftPrespecializedDylibBuildError.empty() )
                buildInstance.warningStrings.push_back("Couldn't build Swift prespecialized metadata dylib: " + swiftPrespecializedDylibBuildError);

            cacheBuilder->forEachWarning(^(const std::string_view &str) {
                buildInstance.warningStrings.push_back(std::string(str));
            });

            // Then copy to a vector to reference the owner
            buildInstance.warnings.reserve(buildInstance.warningStrings.size());
            for ( const std::string& warning : buildInstance.warningStrings )
                buildInstance.warnings.push_back(warning.c_str());

            buildInstance.cdHashType = "sha256";

            // Track the dylibs which were included in this cache
            cacheBuilder->forEachCacheDylib(^(const std::string_view &path) {
                builder->dylibsInCaches[std::string(path)].insert(&buildInstance);
            });
            cacheBuilder->forEachCacheSymlink(^(const std::string_view &path) {
                builder->dylibsInCaches[std::string(path)].insert(&buildInstance);
            });

            // Track cache stats
            builder->statsStorage.push_back(cacheBuilder->stats(startTimeNanos));
        }
    }

    // Make the stats vector from the storage
    if ( !builder->statsStorage.empty() ) {
        for ( std::string& str : builder->statsStorage )
            builder->stats.push_back(str.data());
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
            cacheBuildResult.mapJSON                = buildInstance.jsonMap.empty() ? "" : buildInstance.jsonMap.c_str();

            builder->cacheResultStorage.emplace_back(cacheBuildResult);

            emittedWarningsAndErrors = true;
        }

        if ( shouldEmitCustomerCache(builder->options) ) {
            CacheResult cacheBuildResult;
            cacheBuildResult.version              = 1;
            cacheBuildResult.loggingPrefix        = buildInstance.customerLoggingPrefix.c_str();
            cacheBuildResult.deviceConfiguration  = buildInstance.customerLoggingPrefix.c_str();
            cacheBuildResult.warnings             = nullptr;
            cacheBuildResult.numWarnings          = 0;
            cacheBuildResult.errors               = nullptr;
            cacheBuildResult.numErrors            = 0;
            cacheBuildResult.uuidString           = buildInstance.customerMainCacheUUID.empty() ? "" : buildInstance.customerMainCacheUUID.c_str();
            cacheBuildResult.mapJSON              = buildInstance.customerJsonMap.empty() ? "" : buildInstance.customerJsonMap.c_str();

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

        for ( const CacheBuffer& buffer : buildInstance.cacheBuffers ) {
            buildInstance.cachePaths.push_back(buildInstance.mainCacheFilePath + buffer.cacheFileSuffix);
        }

        uint32_t cacheIndex = 0;
        for ( const CacheBuffer& cacheBuffer : buildInstance.cacheBuffers ) {
            {
                FileResult_v1 cacheFileResult;
                cacheFileResult.version          = 1;
                cacheFileResult.path             = buildInstance.cachePaths[cacheIndex].c_str();
                cacheFileResult.behavior         = AddFile;
                cacheFileResult.data             = cacheBuffer.bufferData;
                cacheFileResult.size             = cacheBuffer.bufferSize;
                cacheFileResult.hashArch         = buildInstance.options->arch.name();
                cacheFileResult.hashType         = buildInstance.cdHashType.c_str();
                cacheFileResult.hash             = cacheBuffer.cdHash.c_str();

                builder->fileResultStorage_v1.emplace_back(cacheFileResult);
            }
            ++cacheIndex;
        }

        FileResult_v1 arlasFileResult;
        arlasFileResult.version     = 1;
        arlasFileResult.path        = buildInstance.atlasPath.c_str();
        arlasFileResult.behavior    = AddFile;
        arlasFileResult.data        = (const uint8_t*)&buildInstance.atlas[0];
        arlasFileResult.size        = buildInstance.atlas.size();
        arlasFileResult.hashArch    = buildInstance.options->arch.name();
        arlasFileResult.hashType    = buildInstance.cdHashType.c_str();
        arlasFileResult.hash        = buildInstance.cacheBuffers.front().cdHash.c_str();;

        builder->fileResultStorage_v1.emplace_back(arlasFileResult);

        // Add a file result for the .map file
        // FIXME: We only emit a single map file right now.
        if ( !buildInstance.macOSMap.empty() ) {
            FileResult_v1 cacheFileResult;
            cacheFileResult.version     = 1;
            cacheFileResult.path        = buildInstance.macOSMapPath.c_str();
            cacheFileResult.behavior    = AddFile;
            cacheFileResult.data        = (const uint8_t*)buildInstance.macOSMap.data();
            cacheFileResult.size        = buildInstance.macOSMap.size();
            cacheFileResult.hashArch    = buildInstance.options->arch.name();
            cacheFileResult.hashType    = buildInstance.cdHashType.c_str();
            cacheFileResult.hash        = buildInstance.cacheBuffers.front().cdHash.c_str();;

            builder->fileResultStorage_v1.emplace_back(cacheFileResult);
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

        switch ( builder->options->platform ) {
            // macOS has to leave the simulator support binaries on disk
            case Platform::macOS:
            // simulator caches in B&I never had sim support dylibs as input, so nothing to remove
            case Platform::iOS_simulator:
            case Platform::tvOS_simulator:
            case Platform::watchOS_simulator:
                if ( UnsafeHeader::isSimulatorSupportDylibPath(pathToRemove) )
                    continue;
                break;
            default:
                break;
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
                const mach_o::GradedArchitectures& archs = buildInstance.options->gradedArchs;
                mach_o::Platform platform = buildInstance.options->platform;
                uint64_t sliceOffset = 0;
                uint64_t sliceSize = 0;
                if ( const auto* mf = dyld3::MachOFile::compatibleSlice(loaderDiag, sliceOffset, sliceSize, buffer, bufferSize,
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
                if ( (platform == mach_o::Platform::macOS) && loaderDiag.hasError() ) {
                    loaderDiag.clearError();

                    if ( const auto* mf = dyld3::MachOFile::compatibleSlice(loaderDiag, sliceOffset, sliceSize, buffer, bufferSize,
                                                                            pathToRemove, mach_o::Platform::macCatalyst,
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

static bool runSymbolsCacheBuilder(struct MRMSharedCacheBuilder* builder) {
    __block bool success = false;
    builder->runSync(^() {
        __block SymbolsCache::ArchPlatforms archPlatforms;
        if ( builder->options->platform == driverKit ) {
            // Note Image Assembly might not know which archs/platforms to build as a single DylibCache could
            // have different archs for different platforms, like userland vs driverKit vs exclaves.
            builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                                  FileFlags fileFlags, uint64_t inode, uint64_t modTime,
                                                  const char* projectName) {
                if ( !strcmp(path, "/System/DriverKit/usr/lib/libSystem.dylib")
                    || !strcmp(path, "/System/DriverKit/usr/lib/libSystem.B.dylib") ) {
                    const std::span<uint8_t> bufferSpan = { (uint8_t*)buffer, bufferSize };
                    mach_o::Error parseErr = mach_o::forEachHeader(bufferSpan, path,
                                                                   ^(const mach_o::UnsafeHeader* mh, size_t sliceHeader, bool &stop) {
                        mach_o::PlatformAndVersions pvs = mh->platformAndVersions();
                        if ( pvs.platform.empty() )
                            return;

                        archPlatforms[mh->archName()].push_back(pvs.platform);
                    });
                    if ( parseErr ) {
                        builder->error("Cannot build symbols cache because: %s", parseErr.message());
                    }
                }
            });
        } else if ( mach_o::Platform(builder->options->platform).isExclaveKit() ) {
            // Note Image Assembly might not know which archs/platforms to build as a single DylibCache could
            // have different archs for different platforms, like userland vs driverKit vs exclaves.
            builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                                  FileFlags fileFlags, uint64_t inode, uint64_t modTime,
                                                  const char* projectName) {
                if ( !strcmp(path, "/System/ExclaveKit/usr/lib/libSystem.dylib") ) {
                    const std::span<uint8_t> bufferSpan = { (uint8_t*)buffer, bufferSize };
                    mach_o::Error parseErr = mach_o::forEachHeader(bufferSpan, path,
                                                                   ^(const mach_o::UnsafeHeader* mh, size_t sliceHeader, bool &stop) {
                        mach_o::PlatformAndVersions pvs = mh->platformAndVersions();
                        if ( pvs.platform.empty() )
                            return;

                        archPlatforms[mh->archName()].push_back(pvs.platform);
                    });
                    if ( parseErr ) {
                        builder->error("Cannot build symbols cache because: %s", parseErr.message());
                    }
                }
            });
        } else {
            // Note Image Assembly might not know which archs/platforms to build as a single DylibCache could
            // have different archs for different platforms, like userland vs driverKit vs exclaves.
            builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                                  FileFlags fileFlags, uint64_t inode, uint64_t modTime,
                                                  const char* projectName) {
                if ( !strcmp(path, "/usr/lib/libSystem.dylib")
                    || !strcmp(path, "/usr/lib/libSystem.B.dylib")
                    || !strcmp(path, "/System/DriverKit/usr/lib/libSystem.dylib")
                    || !strcmp(path, "/System/DriverKit/usr/lib/libSystem.B.dylib")
                    || !strcmp(path, "/System/ExclaveKit/usr/lib/libSystem.dylib") ) {
                    const std::span<uint8_t> bufferSpan = { (uint8_t*)buffer, bufferSize };
                    mach_o::Error parseErr = mach_o::forEachHeader(bufferSpan, path,
                                                                   ^(const mach_o::UnsafeHeader* mh, size_t sliceHeader, bool &stop) {
                        mach_o::PlatformAndVersions pvs = mh->platformAndVersions();
                        if ( pvs.platform.empty() )
                            return;

                        // HACK: Pretend zippered are macOS, so that the database doesn't have to care about zippering
                        mach_o::Platform platform;
                        if ( (pvs.platform == mach_o::Platform::zippered) || (pvs.platform == mach_o::Platform::macCatalyst) )
                            platform = mach_o::Platform::macOS;
                        else
                            platform = pvs.platform;

                        archPlatforms[mh->archName()].push_back(platform);
                    });
                    if ( parseErr ) {
                        builder->error("Cannot build symbols cache because: %s", parseErr.message());
                    }
                }
            });
        }

        __block class SymbolsCache cache;

        if ( mach_o::Error err = cache.create() ) {
            builder->error("Cannot create symbols cache because: %s", err.message());
            return;
        }

        __block bool gotFileErr = false;
        __block std::vector<SymbolsCacheBinary> binaries;
        builder->fileSystem.forEachFileInfo(^(const char* path, const void* buffer, size_t bufferSize,
                                              FileFlags fileFlags, uint64_t inode, uint64_t modTime,
                                              const char* projectName) {
            if ( gotFileErr )
                return;
            switch (fileFlags) {
                case FileFlags::NoFlags:
                case FileFlags::MustBeInCache:
                case FileFlags::ShouldBeExcludedFromCacheIfUnusedLeaf:
                case FileFlags::RequiredClosure:
                case FileFlags::CanBeMissing:
                    break;
                case FileFlags::DylibOrderFile:
                case FileFlags::DirtyDataOrderFile:
                case FileFlags::ObjCOptimizationsFile:
                case FileFlags::SwiftGenericMetadataFile:
                case FileFlags::OptimizationFile:
                    builder->error("Order files should not be in the file system");
                    return;
                case FileFlags::PluginSwiftGenericMetadataBuilder:
                    builder->error("Plugins should not be in the file system");
                    return;
            }

            if ( mach_o::Error err = cache.makeBinaries(archPlatforms, builder->fileSystem, 
                                                        buffer, bufferSize, path, projectName,
                                                        binaries) ) {
                builder->error("Cannot build symbols cache because: %s", err.message());
                gotFileErr = true;
                return;
            }
        });

        if ( gotFileErr )
            return;

        if ( mach_o::Error err = cache.addBinaries(binaries) ) {
            builder->error("Cannot build symbols cache because: %s", err.message());
            return;
        }

        const uint8_t* buffer = nullptr;
        uint64_t bufferSize = 0;
        if ( mach_o::Error err = cache.serialize(buffer, bufferSize) ) {
            builder->error("Cannot serialize symbols cache because: %s", err.message());
            return;
        }

        CacheResult cacheBuildResult;
        cacheBuildResult.version              = 1;
        cacheBuildResult.loggingPrefix        = "symbols-cache";
        cacheBuildResult.deviceConfiguration  = "symbols-cache";
        cacheBuildResult.warnings             = nullptr;
        cacheBuildResult.numWarnings          = 0;
        cacheBuildResult.errors               = nullptr;
        cacheBuildResult.numErrors            = 0;
        cacheBuildResult.uuidString           = "";
        cacheBuildResult.mapJSON              = "";
        cacheBuildResult.warnings             = nullptr;
        cacheBuildResult.numWarnings          = 0;
        cacheBuildResult.errors               = builder->errors.empty() ? nullptr : builder->errors.data();
        cacheBuildResult.numErrors            = builder->errors.size();

        builder->cacheResultStorage.push_back(cacheBuildResult);
        builder->cacheResults.push_back(&builder->cacheResultStorage.back());

        const char* resultPath = MACOSX_MRM_DYLD_SHARED_CACHE_DIR "dyld_symbols.db";;
        if ( builder->options->platform == driverKit )
            resultPath = DRIVERKIT_DYLD_SHARED_CACHE_DIR "dyld_symbols.db";
        else if ( mach_o::Platform(builder->options->platform).isExclaveKit() )
            resultPath = EXCLAVEKIT_DYLD_SHARED_CACHE_DIR "dyld_symbols.db";

        FileResult_v1 cacheFileResult;
        cacheFileResult.version          = 1;
        cacheFileResult.path             = resultPath;
        cacheFileResult.behavior         = AddFile;
        cacheFileResult.data             = buffer;
        cacheFileResult.size             = bufferSize;
        cacheFileResult.hashArch         = "x86_64";
        cacheFileResult.hashType         = "sha256";
        cacheFileResult.hash             = "";

        builder->fileResultStorage_v1.emplace_back(cacheFileResult);
        builder->fileResults.push_back((FileResult*)&builder->fileResultStorage_v1.back());

        builder->buffersToFree.emplace_back((FileBuffer) { (void*)buffer, bufferSize });

        success = true;
    });
    return success;
}

bool runSharedCacheBuilder(struct MRMSharedCacheBuilder* builder) {
    if ( builder->options->disposition == SymbolsCache )
        return runSymbolsCacheBuilder(builder);

    __block bool success = false;
    builder->runSync(^() {
        if ( !createBuilders(builder) )
            return;
        
        runBuilders(builder);


        createBuildResults(builder);
        calculateDylibsToDelete(builder);

        // Copy from the storage to the vector we can return to the API.
        for (auto &fileResult : builder->fileResultStorage_v1)
            builder->fileResults.push_back((FileResult*)&fileResult);
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

const char* const* getCacheStats(const struct MRMSharedCacheBuilder* builder, uint64_t* resultCount) {
    if ( builder->stats.empty() ) {
        *resultCount = 0;
        return nullptr;
    }
    *resultCount = builder->stats.size();
    return builder->stats.data();
}

void destroySharedCacheBuilder(struct MRMSharedCacheBuilder* builder) {

    for ( FileBuffer& buffer : builder->buffersToFree ) {
        free((void*)buffer.buffer);
    }

    for ( FileBuffer& buffer : builder->buffersToDeallocate ) {
        vm_deallocate(mach_task_self(), (vm_address_t)buffer.buffer, buffer.size);
    }

    for ( std::span<const uint8_t> buffer : builder->inputFileBuffers ) {
        ::munmap((void*)buffer.data(), buffer.size());
    }

    delete builder;
}


// rdar://146678211 (slc_builder.dylib is using libcpp_verbose_abort() which does not exist on builder)
// This can be removed when IA builders update to a host whose libc++.dylib has the symbol __ZNSt3__122__libcpp_verbose_abortEPKcz
__attribute__((__noreturn__, visibility("hidden")))
void _libcpp_verbose_abort(const char* msg, ...) __asm("__ZNSt3__122__libcpp_verbose_abortEPKcz");
void _libcpp_verbose_abort(const char* msg, ...)
{
    abort();
}
