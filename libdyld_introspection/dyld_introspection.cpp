/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

#include "dyld_introspection.h"
#include "dyld_cache_format.h"
#include "ProcessAtlas.h"
#include "MachOLoaded.h"
#include "DyldProcessConfig.h"
#include "DyldAPIs.h"
#include "FileManager.h"

#define NO_ULEB
#include "FileAbstraction.hpp"
#include "MachOFileAbstraction.hpp"

using lsl::Allocator;
using lsl::UniquePtr;
using dyld4::FileManager;
using dyld4::Allocator;
using dyld4::Atlas::SharedCache;
using dyld4::Atlas::Image;
using dyld4::Atlas::ProcessSnapshot;
#if BUILDING_LIBDYLD
using dyld4::Atlas::Process;
#include "DyldLegacyInterfaceGlue.h"
namespace dyld4 {
    extern class APIs* gAPIs;
}
using dyld4::gAPIs;
#endif


#if BUILDING_DYLD
FileManager& defaultFileManager();
#else
static
FileManager& defaultFileManager() {
    static FileManager* sFileManager = nullptr;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        sFileManager =  dyld4::MemoryManager::memoryManager().defaultAllocator().makeUnique<FileManager>(dyld4::MemoryManager::memoryManager().defaultAllocator(), nullptr).release();
    });
    return *sFileManager;
}
#endif /* !BUILDING_DYLD */

// This file is essentially glue to bind the public API/SPI to the internal object representations.
// No significant implementation code should be present in this file

#if BUILDING_LIBDYLD
#include <libgen.h>
extern int _NSGetExecutablePath(char* buf, uint32_t* bufsize);
IntrospectionVtable* dyldFrameworkIntrospectionVtable() {
    static IntrospectionVtable* vtable = nullptr;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        bool useAtlas = true;
        char pathBuffer[4096];
        uint32_t legnth = 4096;
        _NSGetExecutablePath(&pathBuffer[0], &legnth);
        const char* baseName = basename(pathBuffer);

        if ((strcmp(baseName, "ReportCrash") == 0)
            || (strcmp(baseName, "sandboxd") == 0)
            || (strcmp(baseName, "com.apple.dt.instruments.dtsecurity") == 0)
            || (strcmp(baseName, "DTServiceHub") == 0)
            || (strcmp(baseName, "trace") == 0)
            || (strcmp(baseName, "trace_internal") == 0)) {
            useAtlas = false;
        }
#if !TARGET_OS_EXCLAVEKIT
        // Check boot-args after builtin overrides so they take precedence
        if (gAPIs->_dyld_commpage().disableAtlasUsage) {
            useAtlas = false;
        }
        // CHheck enablement secons so that if both  disable and enable bits are set it defaults to enabled
        if (gAPIs->_dyld_commpage().enableAtlasUsage) {
            useAtlas = true;
        }
#endif
        if (!useAtlas) {
            return;
        }
        // We want to use the atlas, so try to open up Dyld.framework
        void* handle = dlopen("/System/Library/PrivateFrameworks/Dyld.framework/Dyld", RTLD_LOCAL);
#if TARGET_OS_OSX
        // One OS X with versioned paths if the library is not n the cache the symlink will be in
        // base system and the dylib will be in the cryptex which will result in the above dlopen()
        // failing. Once this gets through a single build and is in the cache that will be never be an issue
        //FIXME: We can remove this after the first submission, once the paths have been added
        if (!handle) {
            handle = dlopen("/System/Library/PrivateFrameworks/Dyld.framework/Versions/A/Dyld", RTLD_LOCAL);
        }
#endif
        vtable = (IntrospectionVtable*)dlsym(handle, "_dyld_legacy_introspection_vtable");
    });
    return vtable;
}

#pragma mark -
#pragma mark Process

dyld_process_t dyld_process_create_for_task(task_t task, kern_return_t *kr) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_create_for_task(task, kr);
    }
    return (dyld_process_t)dyld4::MemoryManager::memoryManager().defaultAllocator().makeUnique<Process>(dyld4::MemoryManager::memoryManager().defaultAllocator(), defaultFileManager(), task, kr).release();
}

dyld_process_t dyld_process_create_for_current_task() {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_create_for_current_task();
    }
    return dyld_process_create_for_task(mach_task_self(), nullptr);
}

void dyld_process_dispose(dyld_process_t process) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_dispose(process);
    }
    UniquePtr<Process> temp((Process*)process);
}

uint32_t dyld_process_register_for_image_notifications(dyld_process_t process, kern_return_t *kr,
                                                       dispatch_queue_t queue, void (^block)(dyld_image_t image, bool load)) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_register_for_image_notifications(process, kr, queue, block);
    }
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    return ((dyld4::Atlas::Process*)process)->registerAtlasChangedEventHandler(kr, queue, (void (^)(dyld4::Atlas::Image*, bool))block);
}


uint32_t dyld_process_register_for_event_notification(dyld_process_t process, kern_return_t *kr, uint32_t event,
                                                      dispatch_queue_t queue, void (^block)()) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_register_for_event_notification(process, kr, event, queue, block);
    }
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    return ((dyld4::Atlas::Process*)process)->registerEventHandler(kr, event, queue, block);
}

void dyld_process_unregister_for_notification(dyld_process_t process, uint32_t handle) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_unregister_for_notification(process, handle);
    }
    ((dyld4::Atlas::Process*)process)->unregisterEventHandler(handle);
}

#pragma mark -
#pragma mark Process Snaphsot

dyld_process_snapshot_t dyld_process_snapshot_create_for_process(dyld_process_t process, kern_return_t *kr) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_snapshot_create_for_process(process, kr);
    }
    return (dyld_process_snapshot_t)((Process*)process)->getSnapshot(kr).release();
}

dyld_process_snapshot_t dyld_process_snapshot_create_from_data(void* buffer, size_t size, void* reserved1, size_t reserved2) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_snapshot_create_from_data(buffer, size, reserved1, reserved2);
    }
    // Make sure no one uses the reserved parameters
    assert(reserved1 == nullptr);
    assert(reserved2 == 0);
    STACK_ALLOCATOR(ephemeralAllocator, 0);
    auto bytes = std::span((std::byte*)buffer, size);
    return (dyld_process_snapshot_t)dyld4::MemoryManager::memoryManager().defaultAllocator()
                                .makeUnique<ProcessSnapshot>(ephemeralAllocator, defaultFileManager(), false, bytes).release();
}


void dyld_process_snapshot_dispose(dyld_process_snapshot_t snapshot) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_snapshot_dispose(snapshot);
    }
    ProcessSnapshot* processSnapshot = (ProcessSnapshot*)snapshot;
    if ( !processSnapshot->valid() )
        return;
    UniquePtr<ProcessSnapshot> temp(processSnapshot);
}

void dyld_process_snapshot_for_each_image(dyld_process_snapshot_t snapshot, void (^block)(dyld_image_t image)) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_snapshot_for_each_image(snapshot, block);
    }
    ProcessSnapshot* processSnapshot = (ProcessSnapshot*)snapshot;
    if ( !processSnapshot->valid() )
        return;
    processSnapshot->forEachImage(^(Image* image) {
        block((dyld_image_t)image);
    });
}

dyld_shared_cache_t dyld_process_snapshot_get_shared_cache(dyld_process_snapshot_t snapshot) {
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_process_snapshot_get_shared_cache(snapshot);
    }
    ProcessSnapshot* processSnapshot = (ProcessSnapshot*)snapshot;
    if ( !processSnapshot->valid() )
        return nullptr;
    return processSnapshot->sharedCache().withUnsafe([](auto cachePtr) {
        return (dyld_shared_cache_t)cachePtr;
    });
}

#endif /* BUILDING_LIBDYLD */

#pragma mark -
#pragma mark SharedCache

bool dyld_shared_cache_pin_mapping(dyld_shared_cache_t cache) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_pin_mapping(cache);
    }
#endif /* BUILDING_LIBDYLD */
    return ((dyld4::Atlas::SharedCache*)cache)->pin();
}

void dyld_shared_cache_unpin_mapping(dyld_shared_cache_t cache) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_unpin_mapping(cache);
    }
#endif /* BUILDING_LIBDYLD */
    ((dyld4::Atlas::SharedCache*)cache)->unpin();
}

uint64_t dyld_shared_cache_get_base_address(dyld_shared_cache_t cache_atlas) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_get_base_address(cache_atlas);
    }
#endif /* BUILDING_LIBDYLD */
    auto cache = (dyld4::Atlas::SharedCache*)cache_atlas;
    return (uint64_t)cache->rebasedAddress();

}

uint64_t dyld_shared_cache_get_mapped_size(dyld_shared_cache_t cache_atlas) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_get_mapped_size(cache_atlas);
    }
#endif /* BUILDING_LIBDYLD */
    auto cache = (dyld4::Atlas::SharedCache*)cache_atlas;
    return cache->size();
}

bool dyld_shared_cache_is_mapped_private(dyld_shared_cache_t cache_atlas) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_is_mapped_private(cache_atlas);
    }
#endif /* BUILDING_LIBDYLD */
    auto cache = (dyld4::Atlas::SharedCache*)cache_atlas;
    return cache->isPrivateMapped();
}

void dyld_shared_cache_copy_uuid(dyld_shared_cache_t cache_atlas, uuid_t *uuid) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_copy_uuid(cache_atlas, uuid);
    }
#endif /* BUILDING_LIBDYLD */
    auto cache = (dyld4::Atlas::SharedCache*)cache_atlas;
    memcpy((void*)&uuid[0], cache->uuid().begin(), 16);
}

void dyld_shared_cache_for_each_file(dyld_shared_cache_t cache_atlas, void (^block)(const char* file_path)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_for_each_file(cache_atlas, block);
    }
#endif /* BUILDING_LIBDYLD */
    auto cache = (dyld4::Atlas::SharedCache*)cache_atlas;
    cache->forEachFilePath(block);
}

void dyld_shared_cache_for_each_image(dyld_shared_cache_t cache, void (^block)(dyld_image_t image)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_for_each_image(cache, block);
    }
#endif /* BUILDING_LIBDYLD */
    ((dyld4::Atlas::SharedCache*)cache)->forEachImage(^(dyld4::Atlas::Image* image) {
        block((dyld_image_t)image);
    });
}

void dyld_for_each_installed_shared_cache_with_system_path(const char* root_path, void (^block)(dyld_shared_cache_t atlas)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_for_each_installed_shared_cache_with_system_path(root_path, block);
    }
#endif /* BUILDING_LIBDYLD */
    STACK_ALLOCATOR(ephemeralAllocator, 0);
    dyld4::Atlas::SharedCache::forEachInstalledCacheWithSystemPath(ephemeralAllocator, defaultFileManager(), root_path, ^(dyld4::Atlas::SharedCache* cache){
        block((dyld_shared_cache_t)cache);
    });
}

void dyld_for_each_installed_shared_cache(void (^block)(dyld_shared_cache_t cache)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_for_each_installed_shared_cache(block);
    }
#endif /* BUILDING_LIBDYLD */
    STACK_ALLOCATOR(ephemeralAllocator, 0);
    dyld4::Atlas::SharedCache::forEachInstalledCacheWithSystemPath(ephemeralAllocator, defaultFileManager(), "/", ^(dyld4::Atlas::SharedCache* cache){
        block((dyld_shared_cache_t)cache);
    });
}

bool dyld_shared_cache_for_file(const char* filePath, void (^block)(dyld_shared_cache_t cache)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_shared_cache_for_file(filePath, block);
    }
#endif /* BUILDING_LIBDYLD */
    STACK_ALLOCATOR(ephemeralAllocator, 0);
    auto cacheFile = defaultFileManager().fileRecordForPath(ephemeralAllocator, filePath);
    auto cache = SharedCache::createForFileRecord(ephemeralAllocator, std::move(cacheFile));
    if (cache) {
        cache.withUnsafe([&](auto cachePtr) {
            block((dyld_shared_cache_t)cachePtr);
        });
        return true;
    }
    return false;
}

bool dyld_image_content_for_segment(dyld_image_t image, const char* segmentName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_content_for_segment(image, segmentName, contentReader);
    }
#endif /* BUILDING_LIBDYLD */
    return ((Image*)image)->contentForSegment(segmentName, contentReader);
}

bool dyld_image_content_for_section(dyld_image_t image, const char* segmentName, const char* sectionName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_content_for_section(image, segmentName, sectionName, contentReader);
    }
#endif /* BUILDING_LIBDYLD */
    return ((Image*)image)->contentForSection(segmentName, sectionName, contentReader);
}

bool dyld_image_copy_uuid(dyld_image_t image, uuid_t* uuid) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_copy_uuid(image, uuid);
    }
#endif /* BUILDING_LIBDYLD */
    using lsl::UUID;

    UUID imageUUID = ((dyld4::Atlas::Image*)image)->uuid();
    if (imageUUID.empty()) {
        return false;
    }
    std::copy((uint8_t*)imageUUID.begin(), (uint8_t*)imageUUID.end(), (uint8_t*)&uuid[0]);
    return true;
}

bool dyld_image_for_each_segment_info(dyld_image_t image, void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_for_each_segment_info(image, block);
    }
#endif /* BUILDING_LIBDYLD */
    // FIXME: Make a temporary null terminated buffer for the segment name
    return ((dyld4::Atlas::Image*)image)->forEachSegment(block);
}

bool dyld_image_for_each_section_info(dyld_image_t image,
                                 void (^block)(const char* segmentName, const char* sectionName, uint64_t vmAddr, uint64_t vmSize)) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_for_each_section_info(image, block);
    }
#endif /* BUILDING_LIBDYLD */
    // FIXME: Make a temporary null terminated buffer for the segment and section names
    return ((dyld4::Atlas::Image*)image)->forEachSection(block);
}

const char* dyld_image_get_installname(dyld_image_t image) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_get_installname(image);
    }
#endif /* BUILDING_LIBDYLD */
    return ((dyld4::Atlas::Image*)image)->installname();
}

#if !BUILDING_CACHE_BUILDER
const char* dyld_image_get_file_path(dyld_image_t image) {
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_get_file_path(image);
    }
#endif /* BUILDING_LIBDYLD */
    return ((Image*)image)->filename();
}
#endif


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
                             bool use64BitDylibOffsets,
                             void (^handler)(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop))
{
    if ( use64BitDylibOffsets ) {
        // On new caches, the dylibOffset is 64-bits, and is a VM offset
        const auto* localEntries = (dyld_cache_local_symbols_entry_64*)((uint8_t*)localInfo + localInfo->entriesOffset);
        bool stop = false;
        for (uint32_t i = 0; i < localInfo->entriesCount; i++) {
            const dyld_cache_local_symbols_entry_64& localEntry = localEntries[i];
            handler(localEntry.dylibOffset, localEntry.nlistStartIndex, localEntry.nlistCount, stop);
            if ( stop )
                break;
        }
    } else {
        // On old caches, the dylibOffset is 64-bits, and is a file offset
        // Note, as we are only looking for mach_header's, a file offset is a VM offset in this case
        const auto* localEntries = (dyld_cache_local_symbols_entry*)((uint8_t*)localInfo + localInfo->entriesOffset);
        bool stop = false;
        for (uint32_t i = 0; i < localInfo->entriesCount; i++) {
            const dyld_cache_local_symbols_entry& localEntry = localEntries[i];
            handler(localEntry.dylibOffset, localEntry.nlistStartIndex, localEntry.nlistCount, stop);
            if ( stop )
                break;
        }
    }
}


bool dyld_image_local_nlist_content_4Symbolication(dyld_image_t image,
                                                   void (^contentReader)(const void* nListStart, uint64_t nListCount,
                                                                         const char* stringTable))
{
#if BUILDING_LIBDYLD
    if ( const IntrospectionVtable* vtable = dyldFrameworkIntrospectionVtable() ) {
        return vtable->dyld_image_local_nlist_content_4Symbolication(image, contentReader);
    }
#endif /* BUILDING_LIBDYLD */
    Image* atlasImage = (dyld4::Atlas::Image*)image;
    const SharedCache* sharedCache = atlasImage->sharedCache();
    if ( !sharedCache )
        return false;

    __block bool result = true;
    if ( auto localsFileData = sharedCache->localSymbols() ) {
        uint64_t textOffsetInCache = atlasImage->sharedCacheVMOffset();

        const dyld_cache_local_symbols_info* localInfo = localsFileData->localInfo();
        forEachLocalSymbolEntry(localInfo, localsFileData->use64BitDylibOffsets(),
                                ^(uint64_t dylibCacheVMOffset, uint32_t nlistStartIndex, uint32_t nlistCount, bool& stop){
            if ( dylibCacheVMOffset == textOffsetInCache ) {
                if ( atlasImage->pointerSize() == 8 ) {
                    typedef Pointer64<LittleEndian> P;
                    const macho_nlist<P>* allLocalNlists = (macho_nlist<P>*)getLocalNlistEntries(localInfo);
                    const macho_nlist<P>* dylibNListsStart = &allLocalNlists[nlistStartIndex];
                    contentReader(dylibNListsStart, nlistCount, getLocalStrings(localInfo));
                } else if ( atlasImage->pointerSize() == 4 ) {
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


#endif // !TARGET_OS_EXCLAVEKIT
