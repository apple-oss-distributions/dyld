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

#ifndef mrm_shared_cache_builder_h
#define mrm_shared_cache_builder_h

#include <Availability.h>

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Note, this should match PLATFORM_* values in <mach-o/loader.h>
enum Platform {
    unknown             = 0,
    macOS               = 1,    // PLATFORM_MACOS
    iOS                 = 2,    // PLATFORM_IOS
    tvOS                = 3,    // PLATFORM_TVOS
    watchOS             = 4,    // PLATFORM_WATCHOS
    bridgeOS            = 5,    // PLATFORM_BRIDGEOS
    iOSMac              = 6,    // PLATFORM_MACCATALYST
    iOS_simulator       = 7,    // PLATFORM_IOSIMULATOR
    tvOS_simulator      = 8,    // PLATFORM_TVOSSIMULATOR
    watchOS_simulator   = 9,    // PLATFORM_WATCHOSSIMULATOR
    driverKit           = 10,   // PLATFORM_DRIVERKIT
    macOSExclaveKit     = 16,   // PLATFORM_MACOS_EXCLAVEKIT
    iOSExclaveKit       = 18,   // PLATFORM_IOS_EXCLAVEKIT
};

enum Disposition
{
    Unknown                 = 0,
    InternalDevelopment     = 1,
    Customer                = 2,
    InternalMinDevelopment  = 3,
    SymbolsCache            = 4
};

enum FileFlags
{
    // Note these are for macho inputs
    NoFlags                                     = 0,
    MustBeInCache                               = 1,
    ShouldBeExcludedFromCacheIfUnusedLeaf       = 2,
    RequiredClosure                             = 3,

    // These are for the order files
    DylibOrderFile                              = 100,
    DirtyDataOrderFile                          = 101,
    ObjCOptimizationsFile                       = 102,
    SwiftGenericMetadataFile                    = 103,

    // This replaces all the magic JSON files and order files, ie, 100..103 above
    // The path (or some field in the file if its JSON) will be used later to work
    // out which file it is
    OptimizationFile                            = 1000,
};

struct BuildOptions_v1
{
    uint64_t                                    version;                        // Future proofing, set to 1
    const char *                                updateName;                     // BuildTrain+UpdateNumber
    const char *                                deviceName;
    enum Disposition                            disposition;                    // Internal, Customer, etc.
    enum Platform                               platform;                       // Enum: unknown, macOS, iOS, ...
    const char **                               archs;
    uint64_t                                    numArchs;
    bool                                        verboseDiagnostics;
    bool                                        isLocallyBuiltCache;
};

// This is available when getVersion() returns 1.2 or higher
struct BuildOptions_v2
{
    uint64_t                                    version;                        // Future proofing, set to 2
    const char *                                updateName;                     // BuildTrain+UpdateNumber
    const char *                                deviceName;
    enum Disposition                            disposition;                    // Internal, Customer, etc.
    enum Platform                               platform;                       // Enum: unknown, macOS, iOS, ...
    const char **                               archs;
    uint64_t                                    numArchs;
    bool                                        verboseDiagnostics;
    bool                                        isLocallyBuiltCache;
    // Added in v2
    bool                                        optimizeForSize;
};

// This is available when getVersion() returns 1.3 or higher
struct BuildOptions_v3
{
    uint64_t                                    version;                        // Future proofing, set to 2
    const char *                                updateName;                     // BuildTrain+UpdateNumber
    const char *                                deviceName;
    enum Disposition                            disposition;                    // Internal, Customer, etc.
    enum Platform                               platform;                       // Enum: unknown, macOS, iOS, ...
    const char **                               archs;
    uint64_t                                    numArchs;
    bool                                        verboseDiagnostics;
    bool                                        isLocallyBuiltCache;
    // Added in v2
    bool                                        optimizeForSize;
    // Added in v3
    bool                                        filesRemovedFromDisk;
    bool                                        timePasses;
    bool                                        printStats;
};

enum FileBehavior
{
    AddFile                                     = 0,        // New file: uid, gid, mode, data, cdhash fields must be set
    ChangeFile                                  = 1,        // Change the data of file: data, size, and cdhash fields must be set
};

struct FileResult
{
    uint64_t                                    version;
};

struct FileResult_v1
{
    uint64_t                                    version;            // Future proofing, set to 1
    const char*                                 path;
    enum FileBehavior                           behavior;
    const uint8_t*                              data;               // Owned by the cache builder.  Destroyed by destroySharedCacheBuilder
    uint64_t                                    size;
    // CDHash, must be set for new or modified files
    const char*                                 hashArch;
    const char*                                 hashType;
    const char*                                 hash;
};

struct FileResult_v2
{
    uint64_t                                    version;            // Future proofing, set to 2
    const char*                                 path;
    enum FileBehavior                           behavior;
    const uint8_t*                              data;               // May be null.  Owned by the cache builder.  Destroyed by destroySharedCacheBuilder
    uint64_t                                    size;
    // CDHash, must be set for new or modified files
    const char*                                 hashArch;
    const char*                                 hashType;
    const char*                                 hash;
    int                                         fd;
    const char*                                 tempFilePath;
};

struct CacheResult
{
    uint64_t                                    version;            // Future proofing, set to 1
    const char*                                 loggingPrefix;      // needed?
    const char*                                 deviceConfiguration;
    const char **                               warnings;           // should this be per-result?
    uint64_t                                    numWarnings;
    const char **                               errors;             // should this be per-result?
    uint64_t                                    numErrors;
    const char*                                 uuidString;
    const char*                                 mapJSON;
};

struct MRMSharedCacheBuilder;

__API_AVAILABLE(macos(10.12))
void getVersion(uint32_t *major, uint32_t *minor);

__API_AVAILABLE(macos(10.12))
struct MRMSharedCacheBuilder* createSharedCacheBuilder(const struct BuildOptions_v1* options);

// Add a file.  Returns true on success.
__API_AVAILABLE(macos(10.12))
bool addFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, enum FileFlags fileFlags);

// Add a file.  Returns true on success.
// Available in API version 1.6 and later
__API_AVAILABLE(macos(10.12))
bool addFile_v2(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, enum FileFlags fileFlags, const char* projectName);

// Add an on-disk file (ie, a file which won't be removed by MRM).  Returns true on success.
__API_AVAILABLE(macos(10.12))
bool addOnDiskFile(struct MRMSharedCacheBuilder* builder, const char* path, uint8_t* data, uint64_t size, enum FileFlags fileFlags,
                   uint64_t inode, uint64_t modTime);

__API_AVAILABLE(macos(10.12))
bool addSymlink(struct MRMSharedCacheBuilder* builder, const char* fromPath, const char* toPath);

__API_AVAILABLE(macos(10.12))
bool runSharedCacheBuilder(struct MRMSharedCacheBuilder* builder);

__API_AVAILABLE(macos(10.12))
const char* const* getErrors(const struct MRMSharedCacheBuilder* builder, uint64_t* errorCount);

__API_AVAILABLE(macos(10.12))
const struct FileResult* const* getFileResults(struct MRMSharedCacheBuilder* builder, uint64_t* resultCount);

__API_AVAILABLE(macos(10.12))
const struct CacheResult* const* getCacheResults(struct MRMSharedCacheBuilder* builder, uint64_t* resultCount);

__API_AVAILABLE(macos(10.12))
const char* const* getFilesToRemove(const struct MRMSharedCacheBuilder* builder, uint64_t* fileCount);

__API_AVAILABLE(macos(10.12))
void destroySharedCacheBuilder(struct MRMSharedCacheBuilder* builder);

#ifdef __cplusplus
}
#endif

#endif /* mrm_shared_cache_builder_h */
