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



#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fsgetpath.h>
#include <sys/syscall.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach-o/fat.h>

#include <mach-o/loader.h>
#include <mach-o/ldsyms.h>
#include <mach/shared_region.h>
#include <mach/mach.h>
#include <optional>
#include <Availability.h>
#include <TargetConditionals.h>

#include "Defines.h"
#include "dyld_cache_format.h"
#include "SharedCacheRuntime.h"
#include "DyldRuntimeState.h"
#include "Utils.h"

#define ENABLE_DYLIBS_TO_OVERRIDE_CACHE_SIZE 1024
#define MAX_SUBCACHES 64
#define SHARED_CACHE_PATH_MAX 256

// should be in mach/shared_region.h
extern "C" int __shared_region_check_np(uint64_t* startaddress);
extern "C" int __shared_region_map_and_slide_np(int fd, uint32_t count, const shared_file_mapping_np mappings[], long slide, const dyld_cache_slide_info2* slideInfo, size_t slideInfoSize);
extern "C" int __shared_region_map_and_slide_2_np(uint32_t files_count, const shared_file_np files[], uint32_t mappings_count, const shared_file_mapping_slide_np mappings[]);

#ifndef VM_PROT_NOAUTH
#define VM_PROT_NOAUTH  0x40  /* must not interfere with normal prot assignments */
#endif

namespace dyld3 {


struct CacheInfo
{
    shared_file_mapping_slide_np            mappings[DyldSharedCache::MaxMappings];
    uint32_t                                mappingsCount;
    bool                                    isTranslated;
    bool                                    hasCacheSuffixes = false;
    // All mappings come from the same file
    int                                     fd               = 0;
    uint64_t                                sharedRegionStart;
    uint64_t                                sharedRegionSize;
    uint64_t                                maxSlide;
    uint32_t                                cacheFileCount;
    uint32_t                                suffixIndexes[MAX_SUBCACHES];
    char                                    cacheSuffixes[MAX_SUBCACHES * 32];
    uint64_t                                dynamicConfigAddress;
    uint64_t                                dynamicConfigMaxSize;
};



#if __i386__
    #define ARCH_NAME            "i386"
    #define ARCH_CACHE_MAGIC     "dyld_v1    i386"
#elif __x86_64__
    #define ARCH_NAME            "x86_64"
    #define ARCH_CACHE_MAGIC     "dyld_v1  x86_64"
    #define ARCH_NAME_H          "x86_64h"
    #define ARCH_CACHE_MAGIC_H   "dyld_v1 x86_64h"
#elif __ARM_ARCH_7K__
    #define ARCH_NAME            "armv7k"
    #define ARCH_CACHE_MAGIC     "dyld_v1  armv7k"
#elif __ARM_ARCH_7A__
    #define ARCH_NAME            "armv7"
    #define ARCH_CACHE_MAGIC     "dyld_v1   armv7"
#elif __ARM_ARCH_7S__
    #define ARCH_NAME            "armv7s"
    #define ARCH_CACHE_MAGIC     "dyld_v1  armv7s"
#elif __arm64e__
    #define ARCH_NAME            "arm64e"
    #define ARCH_CACHE_MAGIC     "dyld_v1  arm64e"
#elif __arm64__
    #if __LP64__
        #define ARCH_NAME            "arm64"
        #define ARCH_CACHE_MAGIC     "dyld_v1   arm64"
    #else
        #define ARCH_NAME            "arm64_32"
        #define ARCH_CACHE_MAGIC     "dyld_v1arm64_32"
    #endif
#endif


#if TARGET_OS_OSX
static bool getMacOSCachePath(int cacheDirFD, bool useHaswell,
                              char filePathBuffer[SHARED_CACHE_PATH_MAX]) {
#if __x86_64__
    if ( useHaswell ) {
        struct stat haswellStatBuf;
        strlcpy(filePathBuffer, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME_H, SHARED_CACHE_PATH_MAX);
        if ( dyld3::fstatat(cacheDirFD, filePathBuffer, &haswellStatBuf, 0) == 0 )
            return true;
    }
#endif

    struct stat statBuf;
    strlcpy(filePathBuffer, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, SHARED_CACHE_PATH_MAX);
    if ( dyld3::fstatat(cacheDirFD, filePathBuffer, &statBuf, 0) == 0 )
        return true;

    return false;
}
#endif // TARGET_OS_OSX

static bool getCachePath(int cacheDirFD, bool useHaswell,
                         bool preferCustomerCache, bool forceDevCache,
                         char pathBuffer[SHARED_CACHE_PATH_MAX],
                         char basePathBuffer[SHARED_CACHE_PATH_MAX])
{
#if TARGET_OS_OSX
    if ( getMacOSCachePath(cacheDirFD, useHaswell, pathBuffer) ) {
        strlcpy(basePathBuffer, pathBuffer, SHARED_CACHE_PATH_MAX);
        return true;
    }

#else // TARGET_OS_OSX

    strlcpy(pathBuffer, DYLD_SHARED_CACHE_BASE_NAME ARCH_NAME, SHARED_CACHE_PATH_MAX);
    strlcpy(basePathBuffer, pathBuffer, SHARED_CACHE_PATH_MAX);

#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    // use .development cache if it exists
    if ( preferCustomerCache) {
        return true;
    }

    // If only one or the other caches exists, then use the one we have
    char potentialDevPath[SHARED_CACHE_PATH_MAX];
    strlcpy(potentialDevPath, pathBuffer, SHARED_CACHE_PATH_MAX);
    strlcat(potentialDevPath, DYLD_SHARED_CACHE_DEVELOPMENT_EXT, PATH_MAX);
    struct stat devCacheStatBuf;
    bool devCacheExists = (dyld3::fstatat(cacheDirFD, potentialDevPath, &devCacheStatBuf, 0) == 0);
    if ( !devCacheExists ) {
        // If the dev cache doesn't exist, then use the customer cache
        return true;
    }
    struct stat optCacheStatBuf;
    bool optCacheExists = (dyld3::fstatat(cacheDirFD, pathBuffer, &optCacheStatBuf, 0) == 0);
    if ( !optCacheExists || forceDevCache ) {
        // If the customer cache doesn't exist (or dyld_flags=4), then use the development cache
        strlcpy(pathBuffer, potentialDevPath, SHARED_CACHE_PATH_MAX);
        return true;
    }

    // Finally, check for the sentinels
    struct stat enableStatBuf;
    //struct stat sentinelStatBuf;
    bool enableFileExists = (dyld3::stat(IPHONE_DYLD_SHARED_CACHE_DIR "enable-dylibs-to-override-cache", &enableStatBuf) == 0);
    // FIXME: rdar://problem/59813537 Re-enable once automation is updated to use boot-arg
    bool sentinelFileExists = false;
    //bool sentinelFileExists = (dyld3::stat(MACOSX_MRM_DYLD_SHARED_CACHE_DIR "enable_development_mode", &sentinelStatBuf) == 0);
    if ( enableFileExists && (enableStatBuf.st_size < ENABLE_DYLIBS_TO_OVERRIDE_CACHE_SIZE) ) {
        // if the old enable file exists, use the development cache
        strlcat(pathBuffer, DYLD_SHARED_CACHE_DEVELOPMENT_EXT, SHARED_CACHE_PATH_MAX);
        return true;
    }
    if ( sentinelFileExists ) {
        // If the new sentinel exists, then use the development cache
        strlcat(pathBuffer, DYLD_SHARED_CACHE_DEVELOPMENT_EXT, SHARED_CACHE_PATH_MAX);
        return true;
    }
#endif

#endif //!TARGET_OS_OSX

    return false;
}

// <rdar://problem/10111032> wrap calls to stat() with check for EAGAIN
static int openat(int fd, const char* path)
{
    int result;
    do {
        result = ::openat(fd, path, O_RDONLY, 0);
    } while ((result == -1) && ((errno == EAGAIN) || (errno == EINTR)));

    return result;
}

static int openMainSharedCacheFile(const SharedCacheOptions& options,
                                   char basePath[SHARED_CACHE_PATH_MAX])
{
    char path[SHARED_CACHE_PATH_MAX];
    getCachePath(options.cacheDirFD, options.useHaswell,
                 options.preferCustomerCache, options.forceDevCache,
                 path, basePath);
    int fd = openat(options.cacheDirFD, path);
    if ( fd != -1 ) {
        return fd;
    }
    return -1;
}

#if !TARGET_OS_SIMULATOR
static int openSubSharedCacheFile(int cacheDirFD, char basePath[SHARED_CACHE_PATH_MAX],
                                  char* suffix)
{
    char path[SHARED_CACHE_PATH_MAX];
    strlcpy(path, basePath, SHARED_CACHE_PATH_MAX);
    strlcat(path, suffix, SHARED_CACHE_PATH_MAX);
    return openat(cacheDirFD, path);
}
#endif // !TARGET_OS_SIMULATOR

static bool validMagic(const SharedCacheOptions& options, const DyldSharedCache* cache)
{
    if ( strcmp(cache->header.magic, ARCH_CACHE_MAGIC) == 0 )
        return true;

#if __x86_64__
    if ( options.useHaswell ) {
        if ( strcmp(cache->header.magic, ARCH_CACHE_MAGIC_H) == 0 )
            return true;
    }
#endif
    return false;
}


static bool validPlatform(const SharedCacheOptions& options, const DyldSharedCache* cache)
{
    // grandfather in old cache that does not have platform in header
    if ( cache->header.mappingOffset < 0xE0 )
        return true;

    uint32_t platform = (uint32_t)options.platform;

    if ( cache->header.platform != platform ) {
        // rdar://74501167 (Marzicaches don't work in private mode)
        if ( cache->header.altPlatform != 0 && ( cache->header.altPlatform == platform ) ) {
            return true;
        }
        return false;
    }

#if TARGET_OS_SIMULATOR
    if ( cache->header.simulator == 0 )
        return false;
#else
    if ( cache->header.simulator != 0 )
        return false;
#endif

    return true;
}

static void verboseSharedCacheMappings(const DyldSharedCache* dyldCache)
{
    intptr_t slide = dyldCache->slide();
    dyldCache->forEachRange(^(const char *mappingName, uint64_t unslidVMAddr, uint64_t vmSize,
                              uint32_t cacheFileIndex, uint64_t fileOffset,
                              uint32_t initProt, uint32_t maxProt, bool& stopRange) {
        dyld4::console("        0x%08llX->0x%08llX init=%x, max=%x %s\n",
                       unslidVMAddr + slide, unslidVMAddr + slide + vmSize - 1,
                       initProt, maxProt, mappingName);
    });
}


static bool preflightCacheFile(const SharedCacheOptions& options, SharedCacheLoadInfo* results,
                               CacheInfo* info, int fd)
{
    struct stat cacheStatBuf;
    if ( ::fstat(fd, &cacheStatBuf) != 0 ) {
        results->errorMessage = "shared cache file stat() failed";
        ::close(fd);
        return false;
    }
    size_t cacheFileLength = (size_t)(cacheStatBuf.st_size);
    results->FSID = cacheStatBuf.st_dev;
    results->FSObjID = cacheStatBuf.st_ino;

    // sanity check header and mappings
    uint8_t firstPage[0x4000];
    if ( ::pread(fd, firstPage, sizeof(firstPage), 0) != sizeof(firstPage) ) {
        results->errorMessage = "shared cache file pread() failed";
        ::close(fd);
        return false;
    }
    const DyldSharedCache* cache = (DyldSharedCache*)firstPage;
    if ( !validMagic(options, cache) ) {
        results->errorMessage = "shared cache file has wrong magic";
        ::close(fd);
        return false;
    }
    if ( !validPlatform(options, cache) ) {
        results->errorMessage = "shared cache file is for a different platform";
        ::close(fd);
        return false;
    }
    if ( (cache->header.mappingCount == 0) || (cache->header.mappingCount > DyldSharedCache::MaxMappings) /*|| (cache->header.mappingOffset > 0x168)*/ ) {
        results->errorMessage = "shared cache file mappings are invalid";
        ::close(fd);
        return false;
    }
    const dyld_cache_mapping_info* const fileMappings = (dyld_cache_mapping_info*)&firstPage[cache->header.mappingOffset];
    const dyld_cache_mapping_info* textMapping = &fileMappings[0];
    std::optional<const dyld_cache_mapping_info*> firstDataMapping;
    std::optional<const dyld_cache_mapping_info*> linkeditMapping;

    // Split caches may not have __DATA/__LINKEDIT
    if ( cache->header.mappingCount > 1 ) {
        if ( fileMappings[1].maxProt == (VM_PROT_READ|VM_PROT_WRITE) ) {
            firstDataMapping = &fileMappings[1];
        } else if ( cache->header.mappingCount > 2 ) {
            // We have even more than __TEXT and __LINKEDIT, so mapping[1] should have been __DATA
            results->errorMessage = "shared cache data mapping was expected";
        }

        // The last mapping should be __LINKEDIT, so long as we have > 1 mapping in total
        linkeditMapping = &fileMappings[cache->header.mappingCount - 1];
    }

    if ( textMapping->fileOffset != 0 ) {
        results->errorMessage = "shared cache text file offset is invalid";
    } else if ( (cache->header.codeSignatureOffset + cache->header.codeSignatureSize) != cacheFileLength ) {
        results->errorMessage = "shared cache code signature size is invalid";
    } else if ( linkeditMapping.has_value() && (linkeditMapping.value()->maxProt != VM_PROT_READ) ) {
        results->errorMessage = "shared cache linkedit permissions are invalid";
    }

    // Regular cache files have TEXT first
    // The LINKEDIT only cache is allowed to have raed-only TEXT as it contains no code
    if ( linkeditMapping.has_value() ) {
        if ( (textMapping->maxProt != (VM_PROT_READ|VM_PROT_EXECUTE)) && (textMapping->maxProt != VM_PROT_READ) ) {
            results->errorMessage = "shared cache text permissions are invalid";
        }
    } else {
        if ( textMapping->maxProt != (VM_PROT_READ|VM_PROT_EXECUTE) ) {
            results->errorMessage = "shared cache text permissions are invalid";
        }
    }

    if ( results->errorMessage != nullptr ) {
        ::close(fd);
        return false;
    }

    // Check mappings don't overlap
    for (unsigned i = 0; i != (cache->header.mappingCount - 1); ++i) {
        if ( ((fileMappings[i].address + fileMappings[i].size) > fileMappings[i + 1].address)
          || ((fileMappings[i].fileOffset + fileMappings[i].size) != fileMappings[i + 1].fileOffset) ) {
            results->errorMessage = "shared cache mappings overlap";
            break;
        }
    }

    if ( results->errorMessage != nullptr ) {
        ::close(fd);
        return false;
    }

    // Check the __DATA mappings
    if ( firstDataMapping.has_value() ) {
        for (unsigned i = 1; i != (cache->header.mappingCount - 1); ++i) {
            if ( fileMappings[i].maxProt != (VM_PROT_READ|VM_PROT_WRITE) ) {
                results->errorMessage = "shared cache data mappings have wrong permissions";
                break;
            }
        }
    }

    if ( results->errorMessage != nullptr ) {
        ::close(fd);
        return false;
    }

    // Make this check real in the new world.
#ifdef NOT_NOW
    if ( (textMapping->address != cache->header.sharedRegionStart) || ((linkeditMapping->address + linkeditMapping->size) > (cache->header.sharedRegionStart+cache->header.sharedRegionSize)) ) {
        results->errorMessage = "shared cache file mapping addressses invalid";
        ::close(fd);
        return false;
    }
#endif

    // register code signature of cache file
    fsignatures_t siginfo;
    siginfo.fs_file_start = 0;  // cache always starts at beginning of file
    siginfo.fs_blob_start = (void*)cache->header.codeSignatureOffset;
    siginfo.fs_blob_size  = (size_t)(cache->header.codeSignatureSize);
    int result = fcntl(fd, F_ADDFILESIGS_RETURN, &siginfo);
    if ( result == -1 ) {
        results->errorMessage = "code signature registration for shared cache failed";
        ::close(fd);
        return false;
    }

    // <rdar://problem/23188073> validate code signature covers entire shared cache
    uint64_t codeSignedLength = siginfo.fs_file_start;
    if ( codeSignedLength < cache->header.codeSignatureOffset ) {
        results->errorMessage = "code signature does not cover entire shared cache file";
        ::close(fd);
        return false;
    }
    void* mappedData = ::mmap(NULL, sizeof(firstPage), PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0);
    if ( mappedData == MAP_FAILED ) {
        results->errorMessage = "first page of shared cache not mmap()able";
        ::close(fd);
        return false;
    }
    if ( memcmp(mappedData, firstPage, sizeof(firstPage)) != 0 ) {
        results->errorMessage = "first page of mmap()ed shared cache not valid";
        ::close(fd);
        return false;
    }
    ::munmap(mappedData, sizeof(firstPage));

    // fill out results
    info->mappingsCount = cache->header.mappingCount;
    // We have to emit the mapping for the __LINKEDIT before the slid mappings
    // This is so that the kernel has already mapped __LINKEDIT in to its address space
    // for when it copies the slid info for each __DATA mapping
    for (int i=0; i < cache->header.mappingCount; ++i) {
        uint64_t    slideInfoFileOffset = 0;
        uint64_t    slideInfoFileSize   = 0;
        vm_prot_t   authProt            = 0;
        vm_prot_t   initProt            = fileMappings[i].initProt;
        if ( cache->header.mappingOffset <= __offsetof(dyld_cache_header, mappingWithSlideOffset) ) {
            // Old cache without the new slid mappings
            if ( i == 1 ) {
                // Add slide info to the __DATA mapping
                slideInfoFileOffset = cache->header.slideInfoOffsetUnused;
                slideInfoFileSize   = cache->header.slideInfoSizeUnused;
                // Don't set auth prot to anything interseting on the old mapppings
                authProt = 0;
            }
        } else {
            // New cache where each mapping has a corresponding slid mapping
            const dyld_cache_mapping_and_slide_info* slidableMappings = (const dyld_cache_mapping_and_slide_info*)&firstPage[cache->header.mappingWithSlideOffset];
            slideInfoFileOffset = slidableMappings[i].slideInfoFileOffset;
            slideInfoFileSize   = slidableMappings[i].slideInfoFileSize;
            if ( (slidableMappings[i].flags & DYLD_CACHE_MAPPING_AUTH_DATA) == 0 )
                authProt = VM_PROT_NOAUTH;
            if ( (slidableMappings[i].flags & DYLD_CACHE_MAPPING_CONST_DATA) != 0 ) {
                // The cache was built with __DATA_CONST being read-only.  We can override that with a boot-arg
                if ( !options.enableReadOnlyDataConst )
                    initProt |= VM_PROT_WRITE;
            }
        }

        // Add a file for each mapping
        info->fd                        = fd;
        info->mappings[i].sms_address               = fileMappings[i].address;
        info->mappings[i].sms_size                  = fileMappings[i].size;
        info->mappings[i].sms_file_offset           = fileMappings[i].fileOffset;
        info->mappings[i].sms_slide_size            = 0;
        info->mappings[i].sms_slide_start           = 0;
        info->mappings[i].sms_max_prot              = fileMappings[i].maxProt;
        info->mappings[i].sms_init_prot             = initProt;
        if ( slideInfoFileSize != 0 ) {
            uint64_t offsetInLinkEditRegion = (slideInfoFileOffset - linkeditMapping.value()->fileOffset);
            info->mappings[i].sms_slide_start   = (user_addr_t)(linkeditMapping.value()->address + offsetInLinkEditRegion);
            info->mappings[i].sms_slide_size    = (user_addr_t)slideInfoFileSize;
            info->mappings[i].sms_init_prot    |= (VM_PROT_SLIDE | authProt);
            info->mappings[i].sms_max_prot     |= (VM_PROT_SLIDE | authProt);
        }
    }

    info->hasCacheSuffixes = (cache->header.mappingOffset > __offsetof(dyld_cache_header, cacheSubType));
    if ( info->hasCacheSuffixes ) {
        uint8_t subCacheArray[MAX_SUBCACHES*sizeof(dyld_subcache_entry)];
        if ( ::pread(fd, subCacheArray, sizeof(subCacheArray), cache->header.subCacheArrayOffset) != sizeof(subCacheArray) ) {
            results->errorMessage = "shared cache file pread() failed, could not read subcache entries";
            ::close(fd);
            return false;
        }
        const dyld_subcache_entry* subCacheEntries = (dyld_subcache_entry*)subCacheArray;
        uint32_t suffixIndex = 0;
        for (uint32_t i = 0; i != cache->header.subCacheArrayCount; ++i) {
            info->suffixIndexes[i] = suffixIndex;
            memcpy(&(info->cacheSuffixes[suffixIndex]), subCacheEntries[i].fileSuffix, 32);
            suffixIndex += 32;
        }
    }

    info->sharedRegionStart     = cache->header.sharedRegionStart;
    info->sharedRegionSize      = cache->header.sharedRegionSize;
    info->maxSlide              = cache->header.maxSlide;
    info->isTranslated          = options.isTranslated;
    info->cacheFileCount        = cache->numSubCaches() + 1;
    info->dynamicConfigAddress  = cache->header.sharedRegionStart + cache->header.dynamicDataOffset;
    info->dynamicConfigMaxSize  = cache->header.dynamicDataMaxSize;

    bool isUniversal = cache->header.cacheType == kDyldSharedCacheTypeUniversal;
    bool isUniversalDev = isUniversal && cache->header.cacheSubType == kDyldSharedCacheTypeDevelopment;
    results->development = cache->header.cacheType == kDyldSharedCacheTypeDevelopment || isUniversalDev;

    return true;
}

static bool preflightMainCacheFile(const SharedCacheOptions& options, SharedCacheLoadInfo* results,
                                   CacheInfo* info, char basePath[SHARED_CACHE_PATH_MAX])
{
    // find and open shared cache file
    int fd = openMainSharedCacheFile(options, basePath);
    if ( fd == -1 ) {
        if ( errno == ENOENT ) {
            results->cacheFileFound = false;
            results->errorMessage = "no shared cache file";
        }
        else
            results->errorMessage = "shared cache file open() failed";
        return false;
    }
    results->cacheFileFound = true;

    return preflightCacheFile(options, results, info, fd);
}

#if !TARGET_OS_SIMULATOR
static void rebaseChainV2(uint8_t* pageContent, uint16_t startOffset, uintptr_t slideAmount, const dyld_cache_slide_info2* slideInfo)
{
    const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
    const uintptr_t   valueMask    = ~deltaMask;
    const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
    const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

    uint32_t pageOffset = startOffset;
    uint32_t delta = 1;
    while ( delta != 0 ) {
        uint8_t* loc = pageContent + pageOffset;
        uintptr_t rawValue = *((uintptr_t*)loc);
        delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
        uintptr_t value = (rawValue & valueMask);
        if ( value != 0 ) {
            value += valueAdd;
            value += slideAmount;
        }
        *((uintptr_t*)loc) = value;
        //dyld::log("         pageOffset=0x%03X, loc=%p, org value=0x%08llX, new value=0x%08llX, delta=0x%X\n", pageOffset, loc, (uint64_t)rawValue, (uint64_t)value, delta);
        pageOffset += delta;
    }
}
#endif

#if !__LP64__ && !TARGET_OS_SIMULATOR
static void rebaseChainV4(uint8_t* pageContent, uint16_t startOffset, uintptr_t slideAmount, const dyld_cache_slide_info4* slideInfo)
{
    const uintptr_t   deltaMask    = (uintptr_t)(slideInfo->delta_mask);
    const uintptr_t   valueMask    = ~deltaMask;
    const uintptr_t   valueAdd     = (uintptr_t)(slideInfo->value_add);
    const unsigned    deltaShift   = __builtin_ctzll(deltaMask) - 2;

    uint32_t pageOffset = startOffset;
    uint32_t delta = 1;
    while ( delta != 0 ) {
        uint8_t* loc = pageContent + pageOffset;
        uintptr_t rawValue = *((uintptr_t*)loc);
        delta = (uint32_t)((rawValue & deltaMask) >> deltaShift);
        uintptr_t value = (rawValue & valueMask);
        if ( (value & 0xFFFF8000) == 0 ) {
           // small positive non-pointer, use as-is
        }
        else if ( (value & 0x3FFF8000) == 0x3FFF8000 ) {
           // small negative non-pointer
           value |= 0xC0000000;
        }
        else {
            value += valueAdd;
            value += slideAmount;
        }
        *((uintptr_t*)loc) = value;
        //dyld::log("         pageOffset=0x%03X, loc=%p, org value=0x%08llX, new value=0x%08llX, delta=0x%X\n", pageOffset, loc, (uint64_t)rawValue, (uint64_t)value, delta);
        pageOffset += delta;
    }
}
#endif

#if !TARGET_OS_SIMULATOR
static bool preflightSubCacheFile(const SharedCacheOptions& options, SharedCacheLoadInfo* results,
                                  CacheInfo* info, char basePath[SHARED_CACHE_PATH_MAX], char* suffix)
{

    // find and open shared cache file
    int fd = openSubSharedCacheFile(options.cacheDirFD, basePath, suffix);
    if ( fd == -1 ) {
        if ( errno == ENOENT ) {
            results->cacheFileFound = false;
            results->errorMessage = "no shared cache file";
        }
        else
            results->errorMessage = "shared cache file open() failed";
        return false;
    }
    results->cacheFileFound = true;

    return preflightCacheFile(options, results, info, fd);
}
#endif // !TARGET_OS_SIMULATOR

static void configureDynamicData(void* address, SharedCacheLoadInfo* results) {
    dyld_cache_dynamic_data_header* dynamicData = (dyld_cache_dynamic_data_header*)address;
    strlcpy(dynamicData->magic, DYLD_SHARED_CACHE_DYNAMIC_DATA_MAGIC, 16);
    dynamicData->fsId = results->FSID;
    dynamicData->fsObjId = results->FSObjID;
}

#if !TARGET_OS_SIMULATOR

static void closeSplitCacheFiles(CacheInfo infoArray[16], uint32_t numFiles) {
    for ( unsigned i = 0; i < numFiles; ++i ) {
        int subCachefd = infoArray[i].fd;
        if ( subCachefd != -1 )
            ::close(subCachefd);
    }
}


// update all __DATA pages with slide info
static bool rebaseDataPages(bool isVerbose, const dyld_cache_slide_info* slideInfo, const uint8_t *dataPagesStart,
                            SharedCacheLoadInfo* results)
{
    const dyld_cache_slide_info* slideInfoHeader = slideInfo;
    if ( slideInfoHeader != nullptr ) {
        if ( slideInfoHeader->version == 2 ) {
            const dyld_cache_slide_info2* slideHeader = (dyld_cache_slide_info2*)slideInfo;
            const uint32_t  page_size = slideHeader->page_size;
            const uint16_t* page_starts = (uint16_t*)((long)(slideInfo) + slideHeader->page_starts_offset);
            const uint16_t* page_extras = (uint16_t*)((long)(slideInfo) + slideHeader->page_extras_offset);
            for (int i=0; i < slideHeader->page_starts_count; ++i) {
                uint8_t* page = (uint8_t*)(long)(dataPagesStart + (page_size*i));
                uint16_t pageEntry = page_starts[i];
                //dyld4::log("page[%d]: page_starts[i]=0x%04X\n", i, pageEntry);
                if ( pageEntry == DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE )
                    continue;
                if ( pageEntry & DYLD_CACHE_SLIDE_PAGE_ATTR_EXTRA ) {
                    uint16_t chainIndex = (pageEntry & 0x3FFF);
                    bool done = false;
                    while ( !done ) {
                        uint16_t pInfo = page_extras[chainIndex];
                        uint16_t pageStartOffset = (pInfo & 0x3FFF)*4;
                        //dyld4::log("     chain[%d] pageOffset=0x%03X\n", chainIndex, pageStartOffset);
                        rebaseChainV2(page, pageStartOffset, results->slide, slideHeader);
                        done = (pInfo & DYLD_CACHE_SLIDE_PAGE_ATTR_END);
                        ++chainIndex;
                    }
                }
                else {
                    uint32_t pageOffset = pageEntry * 4;
                    //dyld::log("     start pageOffset=0x%03X\n", pageOffset);
                    rebaseChainV2(page, pageOffset, results->slide, slideHeader);
                }
            }
        }
#if __LP64__
        else if ( slideInfoHeader->version == 3 ) {
             const dyld_cache_slide_info3* slideHeader = (dyld_cache_slide_info3*)slideInfo;
             const uint32_t                pageSize    = slideHeader->page_size;
             for (int i=0; i < slideHeader->page_starts_count; ++i) {
                 uint8_t* page = (uint8_t*)(dataPagesStart + (pageSize*i));
                 uint64_t delta = slideHeader->page_starts[i];
                 //dyld::log("page[%d]: page_starts[i]=0x%04X\n", i, delta);
                 if ( delta == DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE )
                     continue;
                 delta = delta/sizeof(uint64_t); // initial offset is byte based
                 dyld_cache_slide_pointer3* loc = (dyld_cache_slide_pointer3*)page;
                 do {
                     loc += delta;
                     delta = loc->plain.offsetToNextPointer;
                     if ( loc->auth.authenticated ) {
#if __has_feature(ptrauth_calls)
                        uint64_t target = slideHeader->auth_value_add + loc->auth.offsetFromSharedCacheBase + results->slide;
                        MachOLoaded::ChainedFixupPointerOnDisk ptr;
                        ptr.raw64 = *((uint64_t*)loc);
                        loc->raw = ptr.arm64e.signPointer(loc, target);
#else
                        results->errorMessage = "invalid pointer kind in cache file";
                        return false;
#endif
                     }
                     else {
                        MachOLoaded::ChainedFixupPointerOnDisk ptr;
                        ptr.raw64 = *((uint64_t*)loc);
                        loc->raw = ptr.arm64e.unpackTarget() + results->slide;
                     }
                } while (delta != 0);
            }
        }
#else
        else if ( slideInfoHeader->version == 1 ) {
            const dyld_cache_slide_info* slideHeader = (dyld_cache_slide_info*)slideInfo;
            const uint32_t page_size = 4096;

            const dyld_cache_slide_info_entry* entries = (dyld_cache_slide_info_entry*)((char*)slideHeader + slideHeader->entries_offset);
            const uint16_t* tocs = (uint16_t*)((char*)slideHeader + slideHeader->toc_offset);
            for(int i=0; i < slideHeader->toc_count; ++i) {
                const dyld_cache_slide_info_entry* entry = &entries[tocs[i]];
                uint8_t* page = (uint8_t*)(long)(dataPagesStart + (page_size * i));
                for(int j = 0; j < slideHeader->entries_size; ++j) {
                    uint8_t bitmask = entry->bits[j];
                    for (unsigned k = 0; k != 8; ++k) {
                        if ( bitmask & (1 << k) ) {
                            uint32_t pageOffset = ((j * 8) + k) * 4;
                            uint32_t* loc = (uint32_t*)(page + pageOffset);
                            *loc = *loc + results->slide;
                        }
                    }
                }
            }
        }
        else if ( slideInfoHeader->version == 4 ) {
            const dyld_cache_slide_info4* slideHeader = (dyld_cache_slide_info4*)slideInfo;
            const uint32_t  page_size = slideHeader->page_size;
            const uint16_t* page_starts = (uint16_t*)((long)(slideInfo) + slideHeader->page_starts_offset);
            const uint16_t* page_extras = (uint16_t*)((long)(slideInfo) + slideHeader->page_extras_offset);
            for (int i=0; i < slideHeader->page_starts_count; ++i) {
                uint8_t* page = (uint8_t*)(long)(dataPagesStart + (page_size*i));
                uint16_t pageEntry = page_starts[i];
                //dyld::log("page[%d]: page_starts[i]=0x%04X\n", i, pageEntry);
                if ( pageEntry == DYLD_CACHE_SLIDE4_PAGE_NO_REBASE )
                    continue;
                if ( pageEntry & DYLD_CACHE_SLIDE4_PAGE_USE_EXTRA ) {
                    uint16_t chainIndex = (pageEntry & DYLD_CACHE_SLIDE4_PAGE_INDEX);
                    bool done = false;
                    while ( !done ) {
                        uint16_t pInfo = page_extras[chainIndex];
                        uint16_t pageStartOffset = (pInfo & DYLD_CACHE_SLIDE4_PAGE_INDEX)*4;
                        //dyld::log("     chain[%d] pageOffset=0x%03X\n", chainIndex, pageStartOffset);
                        rebaseChainV4(page, pageStartOffset, results->slide, slideHeader);
                        done = (pInfo & DYLD_CACHE_SLIDE4_PAGE_EXTRA_END);
                        ++chainIndex;
                    }
                }
                else {
                    uint32_t pageOffset = pageEntry * 4;
                    //dyld::log("     start pageOffset=0x%03X\n", pageOffset);
                    rebaseChainV4(page, pageOffset, results->slide, slideHeader);
                }
            }
        }
#endif // LP64
        else {
            results->errorMessage = "invalid slide info in cache file";
            return false;
        }
    }
    return true;
}

static bool reuseExistingCache(const SharedCacheOptions& options, SharedCacheLoadInfo* results)
{
    uint64_t cacheBaseAddress;
#if __i386__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ( syscall(294, &cacheBaseAddress) == 0 ) {
#pragma clang diagnostic pop
#else
    if ( __shared_region_check_np(&cacheBaseAddress) == 0 ) {
#endif
        const DyldSharedCache* existingCache = (DyldSharedCache*)cacheBaseAddress;
        if ( validMagic(options, existingCache) ) {
            results->loadAddress = existingCache;
            results->slide = (long)existingCache->slide();
            bool isUniversal = existingCache->header.cacheType == kDyldSharedCacheTypeUniversal;
            bool isUniversalDev = isUniversal && existingCache->header.cacheSubType == kDyldSharedCacheTypeDevelopment;
            results->development = existingCache->header.cacheType == kDyldSharedCacheTypeDevelopment || isUniversalDev;
            dyld_cache_dynamic_data_header* dynamicData = (dyld_cache_dynamic_data_header*)((uintptr_t)results->loadAddress + existingCache->header.dynamicDataOffset);
            if (strncmp((char*)dynamicData, DYLD_SHARED_CACHE_DYNAMIC_DATA_MAGIC, 16) == 0) {
                results->FSID = dynamicData->fsId;
                results->FSObjID = dynamicData->fsObjId;
            } else {
                dyld4::console("mapped cache does not contain dynamic config data\n");
            }
            if (options.verbose) {
                char path[MAXPATHLEN];
                if (::fsgetpath(&path[0], MAXPATHLEN, (fsid_t*)(&results->FSID), results->FSObjID) > 0) {
                    dyld4::console("re-using existing shared cache (%s):\n", path);
                }
                verboseSharedCacheMappings(existingCache);
            }
        }
        else {
            results->errorMessage = "existing shared cache in memory is not compatible";
            return false;
        }

        return true;
    }
    return false;
}

// this has a large stack frame size, so don't inline into loadDyldCache()
__attribute__((noinline))
static bool mapSplitCacheSystemWide(const SharedCacheOptions& options, SharedCacheLoadInfo* results) {
    CacheInfo firstFileInfo;
    // Try to map the first file to see how many other files we need to map.
    char baseSharedCachePath[SHARED_CACHE_PATH_MAX] = { 0 };
    if ( !preflightMainCacheFile(options, results, &firstFileInfo, baseSharedCachePath) )
        return false;

    uint32_t numFiles = firstFileInfo.cacheFileCount;

    if ( (numFiles != 0) && !firstFileInfo.hasCacheSuffixes ) {
        results->errorMessage = "shared cache is too old, missing cache suffixes";
        return false;
    }

    CacheInfo infoArray[MAX_SUBCACHES];
    for (unsigned i = 1; i < numFiles; ++i) {
        SharedCacheLoadInfo subCacheResults;
        char* suffix = &(firstFileInfo.cacheSuffixes[firstFileInfo.suffixIndexes[i-1]]);
        if ( !preflightSubCacheFile(options, &subCacheResults, &infoArray[i],
                                    baseSharedCachePath, suffix) ) {
            return false;
        }
    }

    vm_address_t    dynamicConfigData   = 0;
    vm_size_t       dynamicConfigSize   = ((sizeof(dyld_cache_dynamic_data_header)+PAGE_SIZE-1) & (-PAGE_SIZE));
    kern_return_t kr = vm_allocate(mach_task_self(), &dynamicConfigData, dynamicConfigSize, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        results->errorMessage = "Could not vm_allocate fixed range for dynamic config data";
        return false;
    }
    configureDynamicData((void*)dynamicConfigData, results);

    infoArray[0] = firstFileInfo;

    shared_file_np files[numFiles + 1];
    uint32_t totalMappings = 0;

    for (unsigned i = 0; i < numFiles; ++i) {
        files[i].sf_fd = infoArray[i].fd;
        uint32_t numMappings = infoArray[i].mappingsCount;
        files[i].sf_mappings_count = numMappings;
        totalMappings += numMappings;

        // The first cache file has the maxSlide for all subCaches.
        files[i].sf_slide = (i == 0) ? (uint32_t)infoArray[0].maxSlide : 0;
    }

    // Set up the dynamic config region (for now just allocate something we can test)
    files[numFiles] = { -1, 1, 0 };

    shared_file_mapping_slide_np mappings[totalMappings + 1];
    uint32_t mappingIdx = 0;

    if ( options.verbose ) {
        dyld4::console("Mapping the shared cache system wide\n");
    }

    for (unsigned i = 0; i < numFiles; ++i) {
        uint32_t numMappings = infoArray[i].mappingsCount;
        shared_file_mapping_slide_np *segmentMappings = infoArray[i].mappings;
      
        for (unsigned j = 0; j < numMappings; ++j) {
            mappings[mappingIdx++] = segmentMappings[j];
        }
    }

    mappings[totalMappings] = {
        firstFileInfo.dynamicConfigAddress,
        dynamicConfigSize,
        (mach_vm_offset_t)dynamicConfigData,
        0,
        0,
        VM_PROT_READ,
        VM_PROT_READ
    };

    int ret = __shared_region_map_and_slide_2_np(numFiles + 1, files, totalMappings + 1, mappings);
    (void)vm_deallocate(mach_task_self(), dynamicConfigData, dynamicConfigSize);

    // <rdar://problem/75293466> don't leak file descriptors.
    closeSplitCacheFiles(infoArray, numFiles);

    if ( ret == 0 ) {
        // We don't know our own slide any more as the kernel owns it, so ask for it again now
        if ( reuseExistingCache(options, results) ) {
            return true;
        }
    }
    else {
        // could be another process beat us to it
        if ( reuseExistingCache(options, results) )
            return true;
        // if cache does not exist, then really is an error
        if ( results->errorMessage == nullptr )
            results->errorMessage = "syscall to map cache into shared region failed";
        return false;
    }

    if ( options.verbose ) {
        dyld4::console("mapped dyld cache file system wide\n");
    }
    return true;
}

#endif // TARGET_OS_SIMULATOR

#if !TARGET_OS_SIMULATOR
// this has a large stack frame size, so don't inline into loadDyldCache()
__attribute__((noinline))
static bool mapSplitCachePrivate(const SharedCacheOptions& options, SharedCacheLoadInfo* results) {
    CacheInfo firstFileInfo;
    // Try to map the first file to see how many other files we need to map.
    char baseSharedCachePath[SHARED_CACHE_PATH_MAX] = { 0 };
    if ( !preflightMainCacheFile(options, results, &firstFileInfo, baseSharedCachePath) )
        return false;

    if (options.verbose) {
        char path[PATH_MAX];
        if (::fsgetpath(&path[0], PATH_MAX, (fsid_t*)(&results->FSID), results->FSObjID) > 0) {
            dyld4::console("mapped dyld cache file private to process (%s):\n", path);
        }
    }

    uint32_t numFiles = firstFileInfo.cacheFileCount;

    if ( (numFiles != 0) && !firstFileInfo.hasCacheSuffixes ) {
        results->errorMessage = "shared cache is too old, missing cache suffixes";
        return false;
    }

    CacheInfo infoArray[MAX_SUBCACHES];
    for (unsigned i = 1; i < numFiles; ++i) {
        SharedCacheLoadInfo subCacheResults;
        char* suffix = &(firstFileInfo.cacheSuffixes[firstFileInfo.suffixIndexes[i-1]]);
        if ( !preflightSubCacheFile(options, &subCacheResults, &infoArray[i],
                                    baseSharedCachePath, suffix) ) {
            return false;
        }
        if ( options.verbose ) {
            char pathBuffer[PATH_MAX] = { 0 };
            if (::fsgetpath(&pathBuffer[0], PATH_MAX, (fsid_t*)(&subCacheResults.FSID), subCacheResults.FSObjID) > 0) {
                dyld4::console("mapped dyld cache file private to process (%s):\n", pathBuffer);
            }
        }
    }
    
    infoArray[0] = firstFileInfo;
    
    uint8_t* buffer = (uint8_t*)SHARED_REGION_BASE;
    uint64_t baseCacheUnslidAddress = firstFileInfo.mappings[0].sms_address;
    uint64_t subCacheBufferOffset = 0;
   
    // deallocate any existing system wide shared cache
    deallocateExistingSharedCache();

    // TODO: implement real ASLR. For now just use 0
    results->slide = (long)0;

    for (unsigned i = 0; i < numFiles; ++i) {
        CacheInfo& subcache = infoArray[i];
        int num_mappings = subcache.mappingsCount;
        shared_file_mapping_slide_np* mappings = (shared_file_mapping_slide_np*)subcache.mappings;
        for (unsigned j = 0; j < num_mappings; ++j) {
            mappings[j].sms_address += results->slide;
            if ( mappings[j].sms_slide_size != 0 )
                mappings[j].sms_slide_start += results->slide;
        }
    }
    results->loadAddress = (const DyldSharedCache*)(infoArray[0].mappings[0].sms_address);

    for (unsigned i = 0; i < numFiles; ++i) {
        const CacheInfo& subcache = infoArray[i];
        int num_mappings = subcache.mappingsCount;
        const shared_file_mapping_slide_np* mappings = (shared_file_mapping_slide_np*)subcache.mappings;
        int cache_fd = subcache.fd;

        // Recompute the cache offset for the main cache and the other subcaches.
        subCacheBufferOffset = mappings[0].sms_address - baseCacheUnslidAddress;

        for (unsigned j = 0; j < num_mappings; ++j) {
            uint64_t mappingAddressOffset = mappings[j].sms_address - mappings[0].sms_address;
            int protection = 0;
            if ( mappings[j].sms_init_prot & VM_PROT_EXECUTE )
                protection   |= PROT_EXEC;
            if ( mappings[j].sms_init_prot & VM_PROT_READ )
                protection   |= PROT_READ;
            if ( mappings[j].sms_init_prot & VM_PROT_WRITE )
                protection   |= PROT_WRITE;
            void* mapped_cache = ::mmap((void*)(buffer + subCacheBufferOffset + mappingAddressOffset + results->slide), (size_t)mappings[j].sms_size,
                                        protection, MAP_FIXED | MAP_PRIVATE, cache_fd, mappings[j].sms_file_offset);
            if (mapped_cache == MAP_FAILED) {
                if ( results->errorMessage == nullptr )
                    results->errorMessage = "mmap() the shared cache region failed";
                closeSplitCacheFiles(infoArray, numFiles);
                return false;
            }
        }
    }
    closeSplitCacheFiles(infoArray, numFiles);

    void*   dynamicConfigData   = (void*)(firstFileInfo.dynamicConfigAddress + results->slide);
    size_t  dynamicConfigSize   = ((sizeof(dyld_cache_dynamic_data_header)+PAGE_SIZE-1) & (-PAGE_SIZE));
    if (::mmap(dynamicConfigData, dynamicConfigSize, VM_PROT_READ | VM_PROT_WRITE, MAP_ANON | MAP_FIXED | MAP_PRIVATE, -1, 0) != dynamicConfigData) {
        // clear shared region
        ::mmap((void*)((long)SHARED_REGION_BASE), SHARED_REGION_SIZE, PROT_NONE, MAP_FIXED | MAP_PRIVATE| MAP_ANON, 0, 0);
        // return failure
        results->loadAddress        = nullptr;
        results->errorMessage       = "could not mmap() dynamic config memory";
        return false;
    }
    configureDynamicData(dynamicConfigData, results);
    ::mprotect(dynamicConfigData, dynamicConfigSize, VM_PROT_READ);


    if ( options.verbose ) {
        verboseSharedCacheMappings(results->loadAddress);
    }

    bool success = true;
    for (unsigned i = 0; i < numFiles; ++i) {
            CacheInfo subcache = infoArray[i];

            // Change __DATA_CONST to read-write while fixup chains are applied
            if ( options.enableReadOnlyDataConst ) {
                const DyldSharedCache* subCache = (const DyldSharedCache*)(subcache.mappings[0].sms_address);
                subCache->forEachRegion(^(const void*, uint64_t vmAddr, uint64_t size, uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
                    if ( flags & DYLD_CACHE_MAPPING_CONST_DATA ) {
                        ::vm_protect(mach_task_self(), (vm_address_t)vmAddr + results->slide, (vm_size_t)size, false, VM_PROT_WRITE | VM_PROT_READ | VM_PROT_COPY);
                    }
                });
            }

            for (unsigned j = 0; j < subcache.mappingsCount; ++j) {
                if ( subcache.mappings[j].sms_slide_size == 0 )
                    continue;
                const dyld_cache_slide_info* slideInfoHeader = (const dyld_cache_slide_info*)subcache.mappings[j].sms_slide_start;
                const uint8_t* mappingPagesStart = (const uint8_t*)subcache.mappings[j].sms_address;
                success &= rebaseDataPages(options.verbose, slideInfoHeader, mappingPagesStart, results);
            }

            // Change __DATA_CONST back to read-only
            if ( options.enableReadOnlyDataConst ) {
                const DyldSharedCache* subCache = (const DyldSharedCache*)(subcache.mappings[0].sms_address);
                subCache->forEachRegion(^(const void*, uint64_t vmAddr, uint64_t size, uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
                    if ( flags & DYLD_CACHE_MAPPING_CONST_DATA ) {
                        ::vm_protect(mach_task_self(), (vm_address_t)vmAddr + results->slide, (vm_size_t)size, false, VM_PROT_READ);
                    }
                });
            }
    }

    return success;
}
#endif // !TARGET_OS_SIMULATOR

#if TARGET_OS_SIMULATOR
static bool mapCachePrivate(const SharedCacheOptions& options, SharedCacheLoadInfo* results)
{
    // open and validate cache file
    CacheInfo info;
    char baseSharedCachePath[SHARED_CACHE_PATH_MAX] = { 0 };
    if ( !preflightMainCacheFile(options, results, &info, baseSharedCachePath) )
        return false;

    // compute ALSR slide
    results->slide = 0;

    // update mappings
    for (uint32_t i=0; i < info.mappingsCount; ++i) {
        info.mappings[i].sms_address += (uint32_t)results->slide;
        if ( info.mappings[i].sms_slide_size != 0 )
            info.mappings[i].sms_slide_start += (uint32_t)results->slide;
    }
    results->loadAddress = (const DyldSharedCache*)(info.mappings[0].sms_address);
    // deallocate any existing system wide shared cache
    deallocateExistingSharedCache();

#if TARGET_OS_SIMULATOR && TARGET_OS_WATCH
    // <rdar://problem/50887685> watchOS 32-bit cache does not overlap macOS dyld cache address range
    // mmap() of a file needs a vm_allocation behind it, so make one
    vm_address_t loadAddress = 0x40000000;
    ::vm_allocate(mach_task_self(), &loadAddress, 0x40000000, VM_FLAGS_FIXED);
#endif

    // map cache just for this process with mmap()
    for (int i=0; i < info.mappingsCount; ++i) {
        void* mmapAddress = (void*)(uintptr_t)(info.mappings[i].sms_address);
        size_t size = (size_t)(info.mappings[i].sms_size);
        //dyld::log("dyld: mapping address %p with size 0x%08lX\n", mmapAddress, size);
        int protection = 0;
        if ( info.mappings[i].sms_init_prot & VM_PROT_EXECUTE )
            protection   |= PROT_EXEC;
        if ( info.mappings[i].sms_init_prot & VM_PROT_READ )
            protection   |= PROT_READ;
        if ( info.mappings[i].sms_init_prot & VM_PROT_WRITE )
            protection   |= PROT_WRITE;
        off_t offset = info.mappings[i].sms_file_offset;
//        dyld4::console("mapped dyld cache file private to process (%s):\n", pathBuffer);
        if ( ::mmap(mmapAddress, size, protection, MAP_FIXED | MAP_PRIVATE, info.fd, offset) != mmapAddress ) {
            // failed to map some chunk of this shared cache file
            // clear shared region
            ::mmap((void*)((long)SHARED_REGION_BASE), SHARED_REGION_SIZE, PROT_NONE, MAP_FIXED | MAP_PRIVATE| MAP_ANON, 0, 0);
            // return failure
            results->loadAddress        = nullptr;
            results->errorMessage       = "could not mmap() part of dyld cache";
            ::close(info.fd);
            return false;
        }
    }

    if ( options.verbose ) {
        char cachePath[PATH_MAX];
        if ( fcntl(options.cacheDirFD, F_GETPATH, cachePath) == 0 )
            dyld4::Utils::concatenatePaths(cachePath, baseSharedCachePath, PATH_MAX);
        else
            strcpy(cachePath, baseSharedCachePath);
        dyld4::console("mapped dyld sim cache file private to process (%s):\n", cachePath);
        verboseSharedCacheMappings(results->loadAddress);
    }
    ::close(info.fd);

    void*   dynamicConfigData   = (void*)(info.dynamicConfigAddress + results->slide);
    size_t  dynamicConfigSize   = ((sizeof(dyld_cache_dynamic_data_header)+PAGE_SIZE-1) & (-PAGE_SIZE));
    if (::mmap(dynamicConfigData, dynamicConfigSize, VM_PROT_READ | VM_PROT_WRITE, MAP_ANON | MAP_FIXED | MAP_PRIVATE, -1, 0) != dynamicConfigData) {
        // clear shared region
        ::mmap((void*)((long)SHARED_REGION_BASE), SHARED_REGION_SIZE, PROT_NONE, MAP_FIXED | MAP_PRIVATE| MAP_ANON, 0, 0);
        // return failure
        results->loadAddress        = nullptr;
        results->errorMessage       = "could not mmap() dynamic config memory";
        return false;
    }
    configureDynamicData(dynamicConfigData, results);
    ::mprotect(dynamicConfigData, dynamicConfigSize, VM_PROT_READ);

    return true;
}
#endif // TARGET_OS_SIMULATOR


bool loadDyldCache(const SharedCacheOptions& options, SharedCacheLoadInfo* results)
{
    bool result                 = false;
    results->loadAddress        = 0;
    results->slide              = 0;
    results->errorMessage       = nullptr;
#if TARGET_OS_SIMULATOR
    // simulator only supports mmap()ing cache privately into process
    result = mapCachePrivate(options, results);
#else
    if ( options.forcePrivate ) {
        // mmap cache into this process only
        result = mapSplitCachePrivate(options, results);
        //return mapCachePrivate(options, results);
    }
    else {
        // fast path: when cache is already mapped into shared region
        bool hasError = false;
        if ( reuseExistingCache(options, results) ) {
            hasError = (results->errorMessage != nullptr);
        } else {
            // slow path: this is first process to load cache
            hasError = mapSplitCacheSystemWide(options, results);
        }
        result = hasError;
    }
    //TODO: This does not require simulator support for now since this is only being used for ordering
#endif
    return result;
}

/*
bool findInSharedCacheImage(const SharedCacheLoadInfo& loadInfo, const char* dylibPathToFind, SharedCacheFindDylibResults* results)
{
    if ( loadInfo.loadAddress == nullptr )
        return false;

    if ( loadInfo.loadAddress->header.formatVersion != dyld3::closure::kFormatVersion ) {
        // support for older cache with a different Image* format
#if TARGET_OS_IPHONE
        uint64_t hash = 0;
        for (const char* s=dylibPathToFind; *s != '\0'; ++s)
                hash += hash*4 + *s;
#endif
        const dyld_cache_image_info* const start = (dyld_cache_image_info*)((uint8_t*)loadInfo.loadAddress + loadInfo.loadAddress->header.imagesOffset);
        const dyld_cache_image_info* const end = &start[loadInfo.loadAddress->header.imagesCount];
        for (const dyld_cache_image_info* p = start; p != end; ++p) {
#if TARGET_OS_IPHONE
            // on iOS, inode is used to hold hash of path
            if ( (p->modTime == 0) && (p->inode != hash) )
                continue;
#endif
            const char* aPath = (char*)loadInfo.loadAddress + p->pathFileOffset;
            if ( strcmp(aPath, dylibPathToFind) == 0 ) {
                results->mhInCache    = (const mach_header*)(p->address+loadInfo.slide);
                results->pathInCache  = aPath;
                results->slideInCache = loadInfo.slide;
                results->image        = nullptr;
                return true;
            }
        }
        return false;
    }

    const dyld3::closure::ImageArray* images = loadInfo.loadAddress->cachedDylibsImageArray();
    results->image = nullptr;
    uint32_t imageIndex;
    if ( loadInfo.loadAddress->hasImagePath(dylibPathToFind, imageIndex) ) {
        results->image = images->imageForNum(imageIndex+1);
    }

    if ( results->image == nullptr )
        return false;

    results->mhInCache    = (const mach_header*)((uintptr_t)loadInfo.loadAddress + results->image->cacheOffset());
    results->pathInCache  = results->image->path();
    results->slideInCache = loadInfo.slide;
    return true;
}
*/

bool pathIsInSharedCacheImage(const SharedCacheLoadInfo& loadInfo, const char* dylibPathToFind)
{
    if ( (loadInfo.loadAddress == nullptr) )
        return false;

    uint32_t imageIndex;
    return loadInfo.loadAddress->hasImagePath(dylibPathToFind, imageIndex);
}

void deallocateExistingSharedCache()
{
#if TARGET_OS_SIMULATOR
    // dyld deallocated macOS shared cache before jumping into dyld_sim
#else
    // <rdar://problem/50773474> remove the shared region sub-map
    uint64_t existingCacheAddress = 0;
    if ( __shared_region_check_np(&existingCacheAddress) == 0 ) {
        // <rdar://problem/73957993>
        (void)__shared_region_check_np(NULL);
    }
#endif

}

} // namespace dyld3

