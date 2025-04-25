/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#include <Availability.h>
#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT

#include <atomic>

#include <mach/task.h>
#include <mach/machine.h>
#include <mach-o/loader.h>
#include <dispatch/dispatch.h>

#include <mach-o/dyld_introspection.h>
#include <mach-o/dyld_process_info.h>

#include "Defines.h"
#include "Allocator.h"
#include "ByteStream.h"
#include "ProcessScavenger.h"
#include "Introspection.h"
#include "Header.h"
#include "dyld_cache_format.h"

#define NO_ULEB
#include "FileAbstraction.hpp"
#include "MachOFileAbstraction.hpp"

#include "DyldLegacyInterfaceGlue.h"
#include <unordered_map>

typedef struct dyld_process_s*              dyld_process_t;
typedef struct dyld_process_snapshot_s*     dyld_process_snapshot_t;
typedef struct dyld_shared_cache_s*         dyld_shared_cache_t;
typedef struct dyld_image_s*                dyld_image_t;

static uint32_t extractKernReturn(NSError *error) {
    if (![error.domain isEqual:@"com.apple.dyld.snapshot"]) {
        return KERN_FAILURE;
    }
    if (error.code != 1) {
        return KERN_FAILURE;
    }
    NSNumber* code = error.userInfo[@"kern_return_t"];
    if (code) {
        return code.intValue;
    }
    return KERN_FAILURE;
}

dyld_process_t dyld_process_create_for_current_task(void) {
    _DYProcess* result = [_DYProcess processForCurrentTask];
    return (dyld_process_t)CFRetain((void*)result);
}

dyld_process_t dyld_process_create_for_task(task_read_t task, kern_return_t *kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    NSError *error = nil;
    _DYProcess* result = [[_DYProcess alloc] initWithTask:task queue:nil error:&error];
    if (error) {
        *kr = extractKernReturn(error);
        return nil;
    }
    return (dyld_process_t)CFRetain((void*)result);
}

void dyld_process_dispose(dyld_process_t process) {
    CFRelease((void*)process);
}

dyld_process_snapshot_t dyld_process_snapshot_create_for_process(dyld_process_t process, kern_return_t *kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    if (!process) {
        *kr = KERN_FAILURE;
        return nil;
    }
    _DYProcess* processObjc = (__bridge _DYProcess*)process;
    NSError *error = nil;
    _DYSnapshot *snapshot = [processObjc getCurrentSnapshotAndReturnError:&error];
    if (error) {
        *kr = extractKernReturn(error);
        return nil;
    }
    return (dyld_process_snapshot_t)CFRetain((void*)snapshot);
}

dyld_process_snapshot_t dyld_process_snapshot_create_from_data(void* buffer, size_t size, void* reserved1, size_t reserved2) {
    assert(reserved1 == nil);
    assert(reserved2 == 0);
    NSData* data = [NSData dataWithBytesNoCopy:buffer length:size freeWhenDone:YES];
    NSError* error = nil;
    _DYSnapshot* snapshot = [[_DYSnapshot alloc] initWithData:data error:&error];
    if (error) {
        return nil;
    }
    return (dyld_process_snapshot_t)CFRetain((void*)snapshot);
}

void dyld_process_snapshot_dispose(dyld_process_snapshot_t snapshot) {
    CFRelease((void*)snapshot);
}

void dyld_process_snapshot_for_each_image(dyld_process_snapshot_t snapshot, void (^block)(dyld_image_t image)) {
    if (!snapshot) {
        return;
    }
    _DYSnapshot* snapshotObjc = (__bridge _DYSnapshot*)snapshot;
    for (_DYImage* imageObjc in snapshotObjc.images) {
        dyld_image_t image = (__bridge dyld_image_t)imageObjc;
        block(image);
    }
}

dyld_shared_cache_t dyld_process_snapshot_get_shared_cache(dyld_process_snapshot_t snapshot) {
    if (!snapshot) {
        return nil;
    }
    _DYSnapshot* snapshotObjc = (__bridge _DYSnapshot*)snapshot;
    dyld_shared_cache_t sharedCache = (__bridge dyld_shared_cache_t)snapshotObjc.sharedCache;
    return sharedCache;
}

void dyld_for_each_installed_shared_cache(void (^block)(dyld_shared_cache_t cache)) {
    @autoreleasepool {
        NSArray<_DYSharedCache *>* caches = [_DYSharedCache installedSharedCaches];
        for (_DYSharedCache* cache in caches) {
            dyld_shared_cache_t cacheObjc = (__bridge dyld_shared_cache_t)cache;
            block(cacheObjc);
        }
    }
}

void dyld_for_each_installed_shared_cache_with_system_path(const char* rootPath, void (^block)(dyld_shared_cache_t cache)) {
    if (!rootPath) {
        rootPath = "";
    }
    NSArray<_DYSharedCache *>* caches = [_DYSharedCache installedSharedCachesForSystemPath:[NSString stringWithUTF8String:rootPath]];
    for (_DYSharedCache* cache in caches) {
        dyld_shared_cache_t cacheObjc = (__bridge dyld_shared_cache_t)cache;
        block(cacheObjc);
    }
}

bool dyld_shared_cache_for_file(const char* filePath, void (^block)(dyld_shared_cache_t cache)) {
    NSError *error = nil;
    _DYSharedCache* cache = [[_DYSharedCache alloc] initWithPath:[NSString stringWithUTF8String:filePath] error:&error];
    if (error) {
        return false;
    }
    dyld_shared_cache_t cacheObjc = (__bridge dyld_shared_cache_t)cache;
    block(cacheObjc);
    return true;
}

bool dyld_shared_cache_pin_mapping(dyld_shared_cache_t cache) {
    if (!cache) {
        return false;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    return [cacheObjc pinMappings];
}

void dyld_shared_cache_unpin_mapping(dyld_shared_cache_t cache) {
    if (!cache) {
        return;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    return [cacheObjc unpinMappings];
}

uint64_t dyld_shared_cache_get_base_address(dyld_shared_cache_t cache) {
    if (!cache) {
        return 0;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    return cacheObjc.address;
}

uint64_t dyld_shared_cache_get_mapped_size(dyld_shared_cache_t cache) {
    if (!cache) {
        return 0;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    return cacheObjc.vmsize;
}

bool dyld_shared_cache_is_mapped_private(dyld_shared_cache_t cache) {
    if (!cache) {
        return false;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    return cacheObjc.mappedPrivate;
}

void dyld_shared_cache_copy_uuid(dyld_shared_cache_t cache, uuid_t* uuid) {
    if (!cache) {
        return;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    [cacheObjc.uuid getUUIDBytes:*uuid];
}

void dyld_shared_cache_for_each_file(dyld_shared_cache_t cache, void (^block)(const char* file_path)) {
    if (!cache) {
        return;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    for (NSString* filePath in cacheObjc.filePaths) {
        block(filePath.UTF8String);
    }
    if ( NSString* symbolsPath = cacheObjc.localSymbolPath )
        block(symbolsPath.UTF8String);
}

void dyld_shared_cache_for_each_image(dyld_shared_cache_t cache, void (^block)(dyld_image_t image)) {
    if (!cache) {
        return;
    }
    _DYSharedCache* cacheObjc = (__bridge _DYSharedCache*)cache;
    for (_DYImage* imageObjc in cacheObjc.images) {
        dyld_image_t image = (__bridge dyld_image_t)imageObjc;
        block(image);
    }
}

bool dyld_image_copy_uuid(dyld_image_t image, uuid_t* uuid) {
    if (!image) {
        return false;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    if (!imageObjc.uuid) {
        return false;
    }
    [imageObjc.uuid getUUIDBytes:*uuid];
    return true;
}

const char* dyld_image_get_installname(dyld_image_t image) {
    if (!image) {
        return nil;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    if (imageObjc.installname) {
        return imageObjc.installname.UTF8String;
    }
    return nil;
}

const char* dyld_image_get_file_path(dyld_image_t image) {
    if (!image) {
        return nil;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    if (imageObjc.filePath) {
        return imageObjc.filePath.UTF8String;
    }
    return nil;
}

bool dyld_image_for_each_segment_info(dyld_image_t image, void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm)) {
    if (!image) {
        return false;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    for (_DYSegment* segment in imageObjc.segments) {
        block(segment.name.UTF8String, segment.address, segment.vmsize, (int)segment.permissions);
    }
    return true;
}

bool dyld_image_content_for_segment(dyld_image_t image, const char* segmentName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
    if (!image) {
        return false;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    for (_DYSegment* segment in imageObjc.segments) {
        if (strcmp(segment.name.UTF8String, segmentName) == 0) {
            @autoreleasepool {
                return [segment withSegmentData:^(NSData* segmentData) {
                    contentReader(segmentData.bytes, segment.address, segment.vmsize);
                }];
            }
        }
    }
    return false;
}

bool dyld_image_for_each_section_info(dyld_image_t image, void (^block)(const char* segmentName, const char* sectionName, uint64_t vmAddr, uint64_t vmSize)) {
    if (!image) {
        return false;
    }
    _DYImage*   imageObjc   = (__bridge _DYImage*)image;
    _DYSegment* textSegment = nil;
    for (_DYSegment* segment in imageObjc.segments) {
        if (strcmp(segment.name.UTF8String, "__TEXT") == 0) {
            textSegment = segment;
            break;
        }
    }
    uint64_t slide = textSegment.address - textSegment.preferredLoadAddress;
    return [textSegment withSegmentData:^(NSData* data) {
        std::span<const uint8_t> mhSpan((const uint8_t*)data.bytes, data.length);
        if ( const mach_o::Header* mh = mach_o::Header::isMachO(mhSpan) ) {
            mh->forEachSection(^(const mach_o::Header::SectionInfo& info, bool& stop) {
                const char* segName  = &info.segmentName[0];
                const char* sectName = &info.sectionName[0];
                char        longSegName[18];
                char        longSectName[18];
                if ( info.segmentName.size() > 15 ) {
                    memcpy(longSegName, segName, 16);
                    longSegName[16] = '\0';
                    segName = longSegName;
                }
                if ( info.sectionName.size() > 15 ) {
                    memcpy(longSectName, sectName, 16);
                    longSectName[16] = '\0';
                    sectName = longSectName;
                }
                block(segName, sectName, info.address+slide, info.size);
            });
        }
    }];
}

bool dyld_image_content_for_section(dyld_image_t image, const char* segmentName, const char* sectionName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
    if (!image) {
        return false;
    }
    _DYImage*   imageObjc   = (__bridge _DYImage*)image;
    _DYSegment* textSegment = nil;
    _DYSegment* hostSegment = nil;
    for (_DYSegment* segment in imageObjc.segments) {
        if (strcmp(segment.name.UTF8String, "__TEXT") == 0) {
            textSegment = segment;
        }
        if (strcmp(segment.name.UTF8String, segmentName) == 0) {
            hostSegment = segment;
        }
        if (textSegment && hostSegment) {
            break;
        }
    }
    __block uint64_t sectionOffset = 0;
    __block uint64_t sectionSize   = 0;
    bool textSegmentReadable = [textSegment withSegmentData:^(NSData* data) {
        std::span<const uint8_t> mhSpan((const uint8_t*)data.bytes, data.length);
        if ( const mach_o::Header* mh = mach_o::Header::isMachO(mhSpan) ) {
            mh->forEachSection(^(const mach_o::Header::SectionInfo& info, bool& stop) {
                if ( (info.segmentName == segmentName) && (info.sectionName == sectionName) ) {
                    sectionOffset = info.address - hostSegment.preferredLoadAddress;
                    sectionSize   = info.size;
                }
            });
        }
    }];
    if (!textSegmentReadable) {
        return false;
    }
    return [hostSegment withSegmentData:^(NSData* data) {
        contentReader((const void*)(((uint8_t*)data.bytes)+sectionOffset), sectionOffset+hostSegment.address, sectionSize);
    }];
}

// FIXME: These functions are part of DyldSharedCache.cpp and we should use that, but we can't until we factor out libdyld_introspection
static
const void* getLocalNlistEntries(const dyld_cache_local_symbols_info* localInfo) {
    return (uint8_t*)localInfo + localInfo->nlistOffset;
}

static
const char* getLocalStrings(const dyld_cache_local_symbols_info* localInfo)
{
    return (char*)localInfo + localInfo->stringsOffset;
}

static
void forEachLocalSymbolEntry(const dyld_cache_local_symbols_info* localInfo,
                             void (^handler)(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop))
{
    // On new caches, the dylibOffset is 64-bits, and is a VM offset
    const auto* localEntries = (dyld_cache_local_symbols_entry_64*)((uint8_t*)localInfo + localInfo->entriesOffset);
    bool stop = false;
    for (uint32_t i = 0; i < localInfo->entriesCount; i++) {
        const dyld_cache_local_symbols_entry_64& localEntry = localEntries[i];
        handler(localEntry.dylibOffset, localEntry.nlistStartIndex, localEntry.nlistCount, stop);
        if ( stop )
            break;
    }
}

bool dyld_image_local_nlist_content_4Symbolication(dyld_image_t image,
                                                 void (^contentReader)(const void* nlistStart, uint64_t nlistCount,
                                                                       const char* stringTable)) {
    if (!image) {
        return false;
    }
    _DYImage* imageObjc = (__bridge _DYImage*)image;
    _DYSharedCache* sharedCache = imageObjc.sharedCache;
    if (!sharedCache) {
        return false;
    }

    __block bool result = true;
    NSData* localSymbolData = sharedCache.localSymbolData;
    if ( localSymbolData ) {
        uint64_t textOffsetInCache = imageObjc.address - sharedCache.address;
        const dyld_cache_header* header = (const dyld_cache_header*)localSymbolData.bytes;
        const dyld_cache_local_symbols_info* localInfo = (dyld_cache_local_symbols_info*)((const uint8_t*)localSymbolData.bytes + header->localSymbolsOffset);
        forEachLocalSymbolEntry(localInfo, ^(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop){
            if ( dylibCacheVMOffset == textOffsetInCache ) {
                if ( imageObjc.pointerSize == 8 ) {
                    typedef Pointer64<LittleEndian> P;
                    const macho_nlist<P>* allLocalNlists = (macho_nlist<P>*)getLocalNlistEntries(localInfo);
                    const macho_nlist<P>* dylibNListsStart = &allLocalNlists[nlistStartIndex];
                    contentReader(dylibNListsStart, nlistCount, getLocalStrings(localInfo));
                } else if ( imageObjc.pointerSize == 4 ) {
                    typedef Pointer32<LittleEndian> P;
                    const macho_nlist<P>* allLocalNlists = (macho_nlist<P>*)getLocalNlistEntries(localInfo);
                    const macho_nlist<P>* dylibNListsStart = &allLocalNlists[nlistStartIndex];
                    contentReader(dylibNListsStart, nlistCount, getLocalStrings(localInfo));
                } else {
                    result = false;
                }
                stop = true;
            }
        });
        return result;
    }
    return result;
}

#if TARGET_OS_OSX
bool dyld_shared_cache_for_each_subcache4Rosetta(dyld_shared_cache_t cache, void (^block)(const void* cacheBuffer, size_t size)) {
    if (!cache) {
        return false;
    }
    _DYSharedCache* sharedCacheObjc = (__bridge _DYSharedCache*)cache;
    for (_DYSubCache* subCache in sharedCacheObjc.subCaches) {
        __block bool done = false;
        [subCache withVMLayoutData:^(NSData * subCacheData) {
            const dyld_cache_header* header = (const dyld_cache_header*)subCacheData.bytes;
            if (strcmp(header->magic, "dyld_v1  x86_64") != 0) {
                done = true;
                return;
            }
            block((const void*)subCacheData.bytes, subCacheData.length);
        }];
        if (done) { break; }
    }
    return true;
}
#endif /* TARGET_OS_OSX */

dyld_process_info _dyld_process_info_create(task_t task, uint64_t timestamp, kern_return_t* kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    NSError* error = nil;
    _DYProcess* process = [[_DYProcess alloc] initWithTask:task queue:nil error:&error];
    if (error) {
        *kr = extractKernReturn(error);
        return nil;
    }
    _DYSnapshot* snapshot = [process getCurrentSnapshotAndReturnError:&error];
    if (error) {
        *kr = extractKernReturn(error);
        return nil;
    }
    if (snapshot.timestamp && snapshot.timestamp == timestamp) {
        return nullptr;
    }
    return (dyld_process_info)CFRetain((void*)snapshot);
}

// retain/release dyld_process_info for specified task
extern void _dyld_process_info_release(dyld_process_info info);
void _dyld_process_info_release(dyld_process_info info) {
    CFRelease((void*)info);
}
extern void _dyld_process_info_retain(dyld_process_info info);
void _dyld_process_info_retain(dyld_process_info info){
    CFRetain((void*)info);
}

// fill in struct with basic info about dyld in the process
void _dyld_process_info_get_state(dyld_process_info info, dyld_process_state_info* stateInfo) {
    if (!info || !stateInfo) {
        return;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    stateInfo->timestamp = snapshot.timestamp;
    stateInfo->imageCount = (uint32_t)snapshot.images.count;
    stateInfo->initialImageCount = (uint32_t)snapshot.initialImageCount;
    stateInfo->dyldState = snapshot.state;
}


// fill in struct with info about dyld cache in use by process
void  _dyld_process_info_get_cache(dyld_process_info info, dyld_process_cache_info* cacheInfo) {
    if (!info || !cacheInfo) {
        return;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    if (!snapshot.sharedCache) {
        uuid_copy(cacheInfo->cacheUUID, UUID_NULL);
        cacheInfo->cacheBaseAddress = 0;
        cacheInfo->noCache          = true;
        cacheInfo->privateCache     = false;
        return;
    }
    [snapshot.sharedCache.uuid getUUIDBytes:cacheInfo->cacheUUID];
    cacheInfo->cacheBaseAddress = snapshot.sharedCache.address;
    cacheInfo->noCache          = false;
    cacheInfo->privateCache     = snapshot.sharedCache.mappedPrivate;
}
//
//// fill in struct with info about aot cache in use by process
void  _dyld_process_info_get_aot_cache(dyld_process_info info, dyld_process_aot_cache_info* aotCacheInfo) {
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    if (!aotCacheInfo || snapshot || !snapshot.sharedCache) {
        return;
    }
    if (snapshot.sharedCache.aotAddress) {
        aotCacheInfo->cacheBaseAddress = snapshot.sharedCache.aotAddress;
        [snapshot.sharedCache.uuid getUUIDBytes:aotCacheInfo->cacheUUID];
    } else {
        aotCacheInfo->cacheBaseAddress = 0;
    }
}

void _dyld_process_info_for_each_image(dyld_process_info info, void (^callback)(uint64_t machHeaderAddress, const uuid_t uuid, const char* path)) {
    if (!info) {
        return;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    const char* rootPath = nullptr;
    size_t rootPathLen = 0;
    if (snapshot.environment.rootPath) {
        rootPath = snapshot.environment.rootPath.UTF8String;
        rootPathLen = strlen(rootPath);
    }
    for (_DYImage* image in snapshot.images) {
        struct _DYImageFastPathData fastPathInfo = { nil, 0, nil, 0, {0}, 0, false, false, false};
        [image getFastPathData:&fastPathInfo];
        char path[MAXPATHLEN];
        size_t prefixSize = 0;
        const char* pathPtr = &path[0];
        if (rootPath && fastPathInfo.sharedCacheImage) {
            // If this is a cache image and we have a root path pre-pend it so that we emulate the existing behaviour for simulator LLDBs
            strlcpy(path, rootPath, MAXPATHLEN);
            prefixSize = rootPathLen;
        } else {
            // Set the first bit to NULL so subsequent `strlcat` calls start at the beginning
            path[0] = 0;
        }
        if (fastPathInfo.filePathPtr) {
            strlcat(path, (const char*)fastPathInfo.filePathPtr, (size_t)MIN(MAXPATHLEN, prefixSize+fastPathInfo.filePathSize+1));
        } else if (fastPathInfo.unicodeFilePath) {
            strlcat(path, image.filePath.UTF8String, MAXPATHLEN);
        } else if (fastPathInfo.installNamePtr) {
            strlcat(path, (const char*)fastPathInfo.installNamePtr, (size_t)MIN(MAXPATHLEN, prefixSize+fastPathInfo.installNameSize+1));
        } else if (fastPathInfo.unicodeInstallname) {
            strlcat(path, image.installname.UTF8String, MAXPATHLEN);
        } else {
            pathPtr = nil;
        }
        callback(fastPathInfo.address, fastPathInfo.uuid, pathPtr);
    }
}

void _dyld_process_info_for_each_aot_image(dyld_process_info info, bool (^callback)(uint64_t x86Address, uint64_t aotAddress, uint64_t aotSize, uint8_t* aotImageKey, size_t aotImageKeySize)) {
    if (!info) {
        return;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    if (snapshot.aotImages) {
        return;
    }
    for (_DYAOTImage* image in snapshot.aotImages) {
        callback(image.x86Address, image.aotAddress, image.aotSize, (uint8_t*)image.aotImageKey.bytes, image.aotImageKey.length);
    }
}

// iterate all segments in an image
void _dyld_process_info_for_each_segment(dyld_process_info info, uint64_t machHeaderAddress, void (^callback)(uint64_t segmentAddress, uint64_t segmentSize, const char* segmentName)) {
    if (!info) {
        return;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    for (_DYImage* image in snapshot.images) {
        if (image.address != machHeaderAddress) {
            continue;
        }
        for (_DYSegment* segment in image.segments) {
            callback(segment.address, segment.vmsize, segment.name.UTF8String);
        }
        break;
    }
}

dyld_platform_t _dyld_process_info_get_platform(dyld_process_info info) {
    if (!info) {
        return PLATFORM_UNKNOWN;
    }
    _DYSnapshot* snapshot = (__bridge _DYSnapshot*)info;
    return (dyld_platform_t)snapshot.platform;
}

uint32_t dyld_process_register_for_image_notifications(dyld_process_t process, kern_return_t *kr,
                                                       dispatch_queue_t queue, void (^block)(dyld_image_t image, bool load)) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    if (!process) {
        *kr = KERN_FAILURE;
        return 0;
    }
    _DYProcess* processObjc = (__bridge _DYProcess*)process;
    processObjc.queue = queue;
    NSError* error = nil;
    _DYEventHandlerToken* result = [processObjc registerChangeNotificationsWithError:&error handler:^(_DYImage* imageObjc, bool load) {
        dyld_image_t image = (__bridge dyld_image_t)imageObjc;
        block(image, load);
    }];
    if (error) {
        *kr = extractKernReturn(error);
        return 0;
    }
    return result.value;
}


uint32_t dyld_process_register_for_event_notification(dyld_process_t process, kern_return_t *kr, uint32_t event,
                                                      dispatch_queue_t queue, void (^block)()) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    if (!process) {
        *kr = KERN_FAILURE;
        return 0;
    }
    _DYProcess* processObjc = (__bridge _DYProcess*)process;
    processObjc.queue = queue;
    NSError* error = nil;
    _DYEventHandlerToken* token = [processObjc registerForEvent:event error:&error handler:block];
    if (error) {
        *kr = extractKernReturn(error);
        return 0;
    }
    return token.value;
}

void dyld_process_unregister_for_notification(dyld_process_t process, uint32_t handle) {
    _DYProcess* processObjc = (__bridge _DYProcess*)process;
    [processObjc unregisterForEvent:[[_DYEventHandlerToken alloc] initWithValue:handle]];
}

struct dyld_process_info_notify_base {
    dyld_process_info_notify_base(task_t task, dispatch_queue_t queue,
                               void (^notify)(bool unload, uint64_t timestamp, uint64_t machHeader, const uuid_t uuid, const char* path),
                               void (^notifyExit)(void),
                        kern_return_t* kr) {
        kern_return_t krSink = KERN_SUCCESS;
        if (kr == nullptr) {
            kr = &krSink;
        }
        *kr = KERN_SUCCESS;
        NSError* error = nil;
        _process = [[_DYProcess alloc] initWithTask:task queue:queue error:&error];
        if (error) {
            *kr = extractKernReturn(error);
            return;
        }
        if (notify) {
            _changeToken = [_process registerChangeNotificationsWithError:&error handler:^(_DYImage * _Nonnull image,
                                                                                                     bool load) {
                // The old notifier interface does not call the block for already loaded images, which forced the clients
                // to synchronized, which is complex and error prone. All the newer interfaces handle this for the clients
                // by calling the handler for each loaded image before returned from the registration call. Updating the
                // old SPI behaviour would break clients, so instead we swallow allow the notifications that happen before
                // we return from registerChangeNotificationsOnQueue:error:handler: to emulate the old (bad) behavior.
                if (!_registered) { return; }
                uuid_t uuid = {0};
                if (image.uuid) {
                    [image.uuid getUUIDBytes:uuid];
                }
                NSError *snapshotError = nil;
                _DYSnapshot* snapshot = [_process getCurrentSnapshotAndReturnError:&snapshotError];
                if (!snapshot || snapshotError) { return; }
                const char* path = image.filePath.UTF8String;
                if (image.sharedCache != nil) {
                    // If this is shared cache image replace the file path with an installname
                    path = image.installname.UTF8String;
                }
                char crashMessage[1024];
                snprintf(crashMessage, 1024,
                         "Notifier (%s):\n"
                         "\tPath: %s\n"
                         "\tAddress: 0x%llx\n"
                         "\tTimestamp: %llu\n",
                         load ? "Load" : "Unload",
                         path ? path : "(null)", image.address, snapshot.timestamp);
                CRSetCrashLogMessage(crashMessage);
                // fprintf(stderr, "%s  0x%llx %s", load ? "Load:  " : "Unload: ",  image.address, path ? path : "(null)");
                notify(!load, snapshot.timestamp, image.address, uuid, path);
                CRSetCrashLogMessage(nil);

            }];
            _registered = true;
        }
        if (notifyExit) {
            pid_t pid = 0;
            *kr = pid_for_task(task, &pid);
            if (*kr != KERN_SUCCESS) {
                // teadown down
            }
            dispatch_source_t exitSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (pid_t)pid,
                                                                    DISPATCH_PROC_EXIT, queue);
            dispatch_source_set_event_handler(exitSource, ^{
                notifyExit();
                dispatch_source_cancel(exitSource);
            });
            dispatch_resume(exitSource);
        }
    }
    ~dyld_process_info_notify_base() {
        if (_process) {
            if (_changeToken) {
                [_process unregisterForEvent:_changeToken];
            }
            if (_mainToken) {
                [_process unregisterForEvent:_mainToken];
            }
        }
    }
    void  registerNotifyMain(void (^notifyMain)(void)) const {
        _mainToken = [_process registerForEvent:DYLD_REMOTE_EVENT_MAIN error:nil handler:notifyMain];
    }

    void incrementRefCount() const {
        _refCount.fetch_add(1, std::memory_order_relaxed);
    }
    void decrementRefCount() const {
        if (_refCount.fetch_sub(1, std::memory_order_acq_rel) == 0) {
            this->~dyld_process_info_notify_base();
            free((void*)this);
        }
    }
    bool isValid() const {
        return (_process != nil);
    }
private:
    _DYProcess*                      _process        = nil;
    _DYEventHandlerToken*            _changeToken    = nil;
    // We are using mutable here because const was exposed in the old typedef and we don't want to risk breaking
    // mangled names. We can fix it later with SWBs.
    mutable _DYEventHandlerToken*    _mainToken      = nil;
    mutable bool                    _registered     = false;
    mutable std::atomic<uint32_t>   _refCount       = 0;
};

dyld_process_info_notify _dyld_process_info_notify(task_t task, dispatch_queue_t queue,
                                                          void (^notify)(bool unload, uint64_t timestamp, uint64_t machHeader, const uuid_t uuid, const char* path),
                                                          void (^notifyExit)(void),
                                                   kern_return_t* kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    *kr = KERN_SUCCESS;
    void* resultBuffer = malloc(sizeof(dyld_process_info_notify_base));
    if (!resultBuffer) {
        *kr = KERN_FAILURE;
        return nullptr;
    }
    dyld_process_info_notify result = new (resultBuffer) dyld_process_info_notify_base(task, queue, notify, notifyExit, kr);
    if (*kr != KERN_SUCCESS) {
        result->decrementRefCount();
        return nullptr;
    }
    if (!result->isValid()) {
        *kr = KERN_FAILURE;
        return nullptr;
    }
    return result;
}
void  _dyld_process_info_notify_release(dyld_process_info_notify object) {
    object->decrementRefCount();
}
void  _dyld_process_info_notify_retain(dyld_process_info_notify object){
    object->incrementRefCount();
}

void  _dyld_process_info_notify_main(dyld_process_info_notify objc, void (^notifyMain)(void)) {
    objc->registerNotifyMain(notifyMain);
}

struct IntrospectionVtable _dyld_legacy_introspection_vtable = {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
    0,
    &dyld_process_create_for_task,
    &dyld_process_create_for_current_task,
    &dyld_process_dispose,
    &dyld_process_snapshot_create_for_process,
    &dyld_process_snapshot_create_from_data,
    &dyld_process_snapshot_dispose,
    &dyld_process_snapshot_for_each_image,
    &dyld_shared_cache_pin_mapping,
    &dyld_shared_cache_unpin_mapping,
    &dyld_shared_cache_get_base_address,
    &dyld_shared_cache_get_mapped_size,
    &dyld_process_snapshot_get_shared_cache,
    &dyld_shared_cache_is_mapped_private,
    &dyld_shared_cache_copy_uuid,
    &dyld_shared_cache_for_each_file,
    &dyld_shared_cache_for_each_image,
    &dyld_for_each_installed_shared_cache_with_system_path,
    &dyld_for_each_installed_shared_cache,
    &dyld_shared_cache_for_file,
    &dyld_image_content_for_segment,
    &dyld_image_content_for_section,
    &dyld_image_copy_uuid,
    &dyld_image_for_each_segment_info,
    &dyld_image_for_each_section_info,
    &dyld_image_get_installname,
    &dyld_image_get_file_path,
    &dyld_image_local_nlist_content_4Symbolication,
    &dyld_process_register_for_image_notifications,
    &dyld_process_register_for_event_notification,
    &dyld_process_unregister_for_notification,
    &_dyld_process_info_create,
    &_dyld_process_info_get_state,
    &_dyld_process_info_get_cache,
    &_dyld_process_info_get_aot_cache,
    &_dyld_process_info_retain,
    &_dyld_process_info_get_platform,
    &_dyld_process_info_release,
    &_dyld_process_info_for_each_image,
#if TARGET_OS_OSX
    &_dyld_process_info_for_each_aot_image,
#endif
    &_dyld_process_info_for_each_segment,
    &_dyld_process_info_notify,
    &_dyld_process_info_notify_main,
    &_dyld_process_info_notify_retain,
    &_dyld_process_info_notify_release
#pragma clang diagnostic pop
};

#endif // !TARGET_OS_EXCLAVEKIT
