/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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
#include "Defines.h"

#if HAS_EXTERNAL_STATE

#if !TARGET_OS_EXCLAVEKIT
  #include <libproc_internal.h>
  #include <mach/mach_time.h> // mach_absolute_time()
  #include <mach-o/loader.h> // include to avoid type issues betewen mach_header and dyld4::mach_header
  #include <sys/fsgetpath.h>
  #include "Header.h"
#endif // !TARGET_OS_EXCLAVEKIT

#include <mach-o/dyld_images.h>
#include <stdint.h>
#include "dyld_process_info.h"
#include "DyldProcessConfig.h"
#include "Header.h"
#include "Tracing.h"

#include "ExternallyViewableState.h"
#include "DyldRuntimeState.h"
#include "AAREncoder.h"

#if DYLD_FEATURE_COMPACT_INFO_GENERATION
#include "FileManager.h"
#include "ProcessAtlas.h"
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION */

#if DYLD_FEATURE_ATLAS_GENERATION
#include "AtlasShared.h"
#endif /* DYLD_FEATURE_ATLAS_GENERATION */

using lsl::Allocator;
using mach_o::Header;
using mach_o::Platform;
using lsl::Vector;

extern struct mach_header __dso_handle;

#define STR(s) # s
#define XSTR(s) STR(s)

#if defined(__cplusplus) && (BUILDING_LIBDYLD || BUILDING_DYLD)
    #define MAYBE_ATOMIC(x) {x}
#else
    #define MAYBE_ATOMIC(x)  x
#endif



#if DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS
extern "C" void lldb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]);
// These functions need to be noinline because their precise calling semantics need to be maintained for an
// external observer (lldb). The compiler does not know that, and may try to inline or optimize them away.
__attribute__((noinline))
void lldb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[])
{
//    fprintf(stderr, "REAL notifiers:\n");
//    for (auto i = 0; i < infoCount; ++i) {
//        fprintf(stderr, "\t%s\n", info[i].imageFilePath);
//    }
}

#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
// Older simulators call the notifier pointer directly before the call the host dyld, which violates
// the ordering requirements that all updates happen before all notifications. To fix this we point the
// notiifer in the all image infos to a dummy lldb does not know about, so the simulator calls that,
// then call the function directly after we have updated the info.
extern "C" void lldb_image_notifier_sim_trap(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]);
__attribute__((noinline))
void lldb_image_notifier_sim_trap(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[])
{
//    fprintf(stderr, "SIM notifiers:\n");
//    for (auto i = 0; i < infoCount; ++i) {
//        fprintf(stderr, "\t%s\n", info[i].imageFilePath);
//    }
}
#endif
#endif


#if !TARGET_OS_SIMULATOR
struct dyld_all_image_infos dyld_all_image_infos __attribute__ ((section ("__DATA,__all_image_info")))
                            = {
                                17, 0, MAYBE_ATOMIC(NULL), &lldb_image_notifier, false, false, (const mach_header*)&__dso_handle, NULL,
                                XSTR(DYLD_VERSION), NULL, 0, NULL, 0, 0, NULL, &dyld_all_image_infos,
                                0, 0, NULL, NULL, NULL, 0, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,},
                                0, MAYBE_ATOMIC(0), "/usr/lib/dyld", {0}, {0}, 0, 0, NULL, 0
                            };

// if rare case that we switch to dyld-in-cache but cannot transfer to using dyld_all_image_infos from cache
static struct dyld_all_image_infos* sProcessInfo = &dyld_all_image_infos;
#endif // !TARGET_OS_SIMULATOR


namespace dyld4 {

Vector<std::byte> ExternallyViewableState::generateCompactInfo(Allocator& allocator, AAREncoder& encoder) {
#if DYLD_FEATURE_COMPACT_INFO_GENERATION
    Atlas::ProcessSnapshot snapshot(allocator, _runtimeState->fileManager, true);
    // This has been busted for ages and we will get rid of it soon
    snapshot.setInitialImageCount(2);
    snapshot.setDyldState(_dyldState);
    snapshot.setPlatform((uint32_t)_runtimeState->config.process.platform.value());

    const DyldSharedCache* cache        = _runtimeState->config.dyldCache.addr;
    if (cache) {
        uint64_t sharedCacheLoadAddress = (uint64_t)cache;
        // Technically this wrong, but private caches are mostly broken right now and this is a temporary path until we turn on ATLAS generation
        auto cacheFile = _runtimeState->fileManager.fileRecordForPath(allocator, cache->dynamicRegion()->cachePath());
        Atlas::SharedCache atlasCache(allocator, std::move(cacheFile), snapshot.identityMapper(), sharedCacheLoadAddress, false);
        snapshot.addSharedCache(std::move(atlasCache));
    }

    std::span<const uint8_t> dyldHeaderSpan((const uint8_t*)&__dso_handle, sizeof(mach_header));
    const mach_o::Header* dyldHeader = mach_o::Header::isMachO(dyldHeaderSpan);
    if (dyldHeader->inDyldCache()) {
        snapshot.addSharedCacheImage((const struct mach_header*)&__dso_handle);
    } else {
        uuid_t rawUUID;
        if (!dyldHeader->getUuid(rawUUID)) {
            halt("dyld must have a UUID");
        }
        lsl::UUID dyldUUID = rawUUID;
        auto dyldFile = _runtimeState->fileManager.fileRecordForPath(allocator, _runtimeState->config.process.dyldPath);
        Atlas::Image dyldImage(allocator, std::move(dyldFile), snapshot.identityMapper(), (uint64_t)&__dso_handle, dyldUUID);
        snapshot.addImage(std::move(dyldImage));
    }

#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    // Add dyld_sim
    if ( _dyldSimLoadAddress != 0 ) {
        auto dyldFile = _runtimeState->fileManager.fileRecordForPath(allocator, _dyldSimPath);
        auto simImage = Atlas::Image(allocator, std::move(dyldFile),  snapshot.identityMapper(), (uint64_t)_dyldSimLoadAddress);
        snapshot.addImage(std::move(simImage));
    }
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */

    snapshot.addImages(_runtimeState, _runtimeState->loaded);
    return snapshot.serialize();
#else
    return Vector<std::byte>(allocator);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION */
}

// We leave a global here to support halt() stashing termination info via setExternallyViewableStateToTerminated()
static ExternallyViewableState* sExternallyViewableState = nullptr;

ExternallyViewableState::ExternallyViewableState(Allocator& allocator) : _persistentAllocator(&allocator) {
    _imageInfos = Vector<dyld_image_info>::make(allocator);
    _imageUUIDs = Vector<dyld_uuid_info>::make(allocator);
#if SUPPORT_ROSETTA
    _aotImageInfos = Vector<dyld_aot_image_info>::make(allocator);
#endif
    sExternallyViewableState = this;
#if !TARGET_OS_SIMULATOR
    _allImageInfo = sProcessInfo;
#endif /* !TARGET_OS_SIMULATOR */
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    _dyldState = dyld_process_state_dyld_initialized;
#endif
}

#if TARGET_OS_SIMULATOR
ExternallyViewableState::ExternallyViewableState(Allocator& allocator, const dyld::SyscallHelpers* syscalls): ExternallyViewableState(allocator) {
    _syscallHelpers = syscalls;
    // make old all_image_infos, using all_image_info from host dyld
    _allImageInfo = (struct dyld_all_image_infos*)(_syscallHelpers->getProcessInfo());
    _imageInfos     = Vector<dyld_image_info>::make(*_persistentAllocator);
    _imageUUIDs     = Vector<dyld_uuid_info>::make(*_persistentAllocator);

    // Copy images from host
    for (uint32_t i = 0; i < _allImageInfo->infoArrayCount; i++) {
        _imageInfos->push_back({_allImageInfo->infoArray[i].imageLoadAddress,
            _allImageInfo->infoArray[i].imageFilePath,
            _allImageInfo->infoArray[i].imageFileModDate});
    }
    _allImageInfo->infoArrayCount           = (uint32_t)_imageInfos->size();
    _allImageInfo->infoArrayChangeTimestamp = mach_absolute_time();
    _allImageInfo->infoArray                = _imageInfos->data();

    // Copy uuids from host
    for (uint32_t i = 0; i < _allImageInfo->uuidArrayCount; i++) {
        dyld_uuid_info uuidInfo;
        uuidInfo.imageLoadAddress = _allImageInfo->uuidArray[i].imageLoadAddress;
        memcpy(uuidInfo.imageUUID, _allImageInfo->uuidArray[i].imageUUID, 16);
        _imageUUIDs->push_back(uuidInfo);
    }
    _allImageInfo->uuidArrayCount = (uint32_t)_imageUUIDs->size();
    _allImageInfo->uuidArray      = _imageUUIDs->data();
}
#endif

// State updates are significant, so they update the atlas
void ExternallyViewableState::setDyldState(uint8_t dyldState) {
    _dyldState = dyldState;
    // State updates are significant, so they update the atlas
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    STACK_ALLOCATOR(allocator, 0);
    auto newAtlas = generateAtlas(allocator);
    activateAtlas(*_persistentAllocator, newAtlas);
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
}

// used in macOS host dyld to support old dyld_sim that need access to host dyld_all_image_info
// also used for dyld_process_info_create to get dyld_all_image_infos for current process
struct dyld_all_image_infos* ExternallyViewableState::getProcessInfo()
{
#if TARGET_OS_SIMULATOR
    return nullptr; // FIXME: 
#else
    return sProcessInfo;
#endif
}

#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
void ExternallyViewableState::addDyldSimInfo(const char* path, uint64_t loadAddress) {
    _dyldSimPath           = _persistentAllocator->strdup(path);
    _dyldSimLoadAddress    = loadAddress;
    notifyMonitorOfImageListChangesSim(false, 1, (const struct mach_header**)&loadAddress, &_dyldSimPath);
    // Stop dyld from directly issuing break point requests. The only other user of this
    // function pointer is the transition to dyld in the cache, and we don't do that
    // in simulators.
    sProcessInfo->notification = &lldb_image_notifier_sim_trap;
}

void ExternallyViewableState::setSharedCacheInfo(uint64_t cacheSlide, const ImageInfo& cacheInfo, bool privateCache)
{
    // update cache info in old all_image_infos
    sProcessInfo->sharedCacheSlide                = (uintptr_t)cacheSlide;
    sProcessInfo->sharedCacheBaseAddress          = (uintptr_t)cacheInfo.loadAddress;
    sProcessInfo->sharedCacheFSID                 = cacheInfo.fsID;
    sProcessInfo->sharedCacheFSObjID              = cacheInfo.fsObjID;
    sProcessInfo->processDetachedFromSharedRegion = privateCache;
    if ( const DyldSharedCache* dyldCache = (DyldSharedCache*)cacheInfo.loadAddress )
        dyldCache->getUUID(sProcessInfo->sharedCacheUUID);
}
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */

void ExternallyViewableState::setSharedCacheAddress(uintptr_t cacheSlide, uintptr_t cacheAddress)
{
    // update cache info in old all_image_infos
    _allImageInfo->sharedCacheSlide                = cacheSlide;
    _allImageInfo->sharedCacheBaseAddress          = cacheAddress;
    if ( const DyldSharedCache* cache = (DyldSharedCache*)cacheAddress )
        cache->getUUID(_allImageInfo->sharedCacheUUID);
}

#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
// Storage for the host to keep track of images added by the simulator
static Vector<dyld_image_info>* loadedImagesInfos = nullptr;
#endif

// This routine builds a minimal atlas with just dyld,  then calls notifiers, and finally. It is only used to when transitioning
// from on disk dyld to n cache dyld. As such the info needs the following:
//
// 1. The main executable
// 2. The on disk dyld
// 3. The shared cache
// 4. An entry in the cache bitmap for in cache dyld
//
// That describes all the memory address that may execute code or be read during the transition and while the in cache dyld starts up.
void ExternallyViewableState::createMinimalInfo(Allocator& allocator, uint64_t dyldLoadAddress, const char* dyldPath, uint64_t mainExecutableAddress, const char* mainExecutablePath, const DyldSharedCache* cache) {
#if DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION
    STACK_ALLOCATOR(ephemeralAllocator, 0);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION */
    // fprintf(stderr, "minimalAlas\n\t0x%llx %s\n\t0x%llx %s\n", dyldLoadAddress, dyldPath, mainExecutableAddress, mainExecutablePath);
    // Set up legacy all image info fields
    updateTimestamp();
    _allImageInfo->initialImageCount = 1; // This has been set to 1 for years, hardcoding
    _imageInfos->clear();
    _imageUUIDs->clear();
    dyld_uuid_info  dyldUuidInfo;
    std::span<const uint8_t> dyldHeaderSpan((const uint8_t*)dyldLoadAddress, sizeof(mach_header));
    const mach_o::Header* dyldHeader = mach_o::Header::isMachO(dyldHeaderSpan);
    if (!dyldHeader->getUuid(dyldUuidInfo.imageUUID)) {
        halt("dyld must have a UUID");
    }
#if DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION
    ByteStream outputStream(allocator);
    AAREncoder aarEncoder(ephemeralAllocator);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION */
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    // 1. Set dyld's uuid
    if (!dyldHeader->inDyldCache()) {
        // Stackshot assumes memory regions have a single UUID. If dyld is in the cache the region has the cache UUID attached, so
        // only record dyld's uuid if it is not in the cache.
        dyldUuidInfo.imageLoadAddress = (const struct mach_header*)dyldLoadAddress;
        _imageUUIDs->push_back(dyldUuidInfo);
    }

    // 2. Set the main executable's uuid
    dyld_uuid_info  mainUuidInfo;
    dyld_image_info mainImageInfo;
    std::span<const uint8_t> mainHeaderSpan((const uint8_t*)mainExecutableAddress, sizeof(mach_header));
    const mach_o::Header* mainHeader = mach_o::Header::isMachO(mainHeaderSpan);
    if (mainHeader->getUuid(mainUuidInfo.imageUUID)) {
        mainUuidInfo.imageLoadAddress =  (const struct mach_header*)mainExecutableAddress;;
        _imageUUIDs->push_back(mainUuidInfo);
    }
    // This is a pointer back to the string passed by the kernel, it will not be released.
    mainImageInfo.imageFilePath     = mainExecutablePath;
    mainImageInfo.imageLoadAddress  = (const struct mach_header*)mainExecutableAddress;
    mainImageInfo.imageFileModDate  = 0;
    _imageInfos->push_back(mainImageInfo);
    _allImageInfo->infoArrayCount = (uint32_t)_imageInfos->size();

    // Create the initial legacy infos
    _allImageInfo->infoArray = nullptr;   // set infoArray to NULL to denote it is in-use
    _allImageInfo->uuidArray = nullptr;   // set uuidArray to NULL to denote it is in-use
    _allImageInfo->infoArrayCount           = (uint32_t)_imageInfos->size();
    _allImageInfo->uuidArrayCount           = (uint32_t)_imageUUIDs->size();
    _allImageInfo->infoArray                = _imageInfos->data();
    _allImageInfo->uuidArray                = _imageUUIDs->data();
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
#if DYLD_FEATURE_COMPACT_INFO_GENERATION
    FileManager fileManager(ephemeralAllocator);
    Atlas::ProcessSnapshot snapshot(ephemeralAllocator, _runtimeState->fileManager, true);
    // This has been busted for ages and we will get rid of it soon
    snapshot.setInitialImageCount(1);
    snapshot.setDyldState(dyld_process_state_dyld_initialized);

    if (cache) {
        uint64_t sharedCacheLoadAddress = (uint64_t)cache;
        // Technically this wrong, but private caches are mostly broken right now and this is a temporary path until we turn on ATLAS generation
        auto cacheFile = fileManager.fileRecordForPath(ephemeralAllocator, cache->dynamicRegion()->cachePath());
        Atlas::SharedCache atlasCache(ephemeralAllocator, std::move(cacheFile), snapshot.identityMapper(), sharedCacheLoadAddress, false);
        snapshot.addSharedCache(std::move(atlasCache));
        // Unconditionally add dyld in the cache. Either we are about to transition into it, or we are already in the cache
        snapshot.addSharedCacheImage((const struct mach_header*)(cache->header.dyldInCacheMH + cache->slide()));
    }

    uuid_t rawUUID;
    if (!dyldHeader->inDyldCache()) {
        auto dyldFile = fileManager.fileRecordForPath(ephemeralAllocator, dyldPath);
        lsl::UUID dyldUUID;
        if (!dyldHeader->getUuid(rawUUID)) {
            halt("dyld must have a UUID");
        }
        dyldUUID = rawUUID;
        Atlas::Image dyldImage(ephemeralAllocator, std::move(dyldFile), snapshot.identityMapper(), (uint64_t)&__dso_handle , dyldUUID);
        snapshot.addImage(std::move(dyldImage));
    }

    auto mainFile = fileManager.fileRecordForPath(ephemeralAllocator, dyldPath);
    lsl::UUID mainUUID;
    if (((Header*)mainExecutableAddress)->getUuid(rawUUID)) {
        mainUUID = rawUUID;
        Atlas::Image dyldImage(ephemeralAllocator, std::move(mainFile), snapshot.identityMapper(), (uint64_t)mainExecutableAddress, mainUUID);
        snapshot.addImage(std::move(dyldImage));
    } else {
        Atlas::Image dyldImage(ephemeralAllocator, std::move(mainFile), snapshot.identityMapper(), (uint64_t)mainExecutableAddress);
        snapshot.addImage(std::move(dyldImage));
    }

    // Wrap the compact info into an AAR
    auto serializedCompactInfo = snapshot.serialize();
    aarEncoder.addFile("process.cinfo", serializedCompactInfo);
#endif
#if DYLD_FEATURE_ATLAS_GENERATION
    using Array         = PropertyList::Array;
    using Dictionary    = PropertyList::Dictionary;

    auto propertyListEncoder            = PropertyList(allocator);
    auto& rootDictionary                = propertyListEncoder.rootDictionary();
    auto& images                        = rootDictionary.addObjectForKey<Array>(kDyldAtlasSnapshotImagesArrayKey);
    PropertyList::Bitmap* cacheBitmap   = gatherAtlasProcessInfo(mainExecutableAddress, cache, rootDictionary);
    auto& mainExecutableImage           = images.addObject<Dictionary>();
    atlasAddImage(mainExecutableImage, mainExecutableAddress, mainExecutablePath);

    if (!dyldHeader->inDyldCache()) {
        auto& dyldImage                     = images.addObject<Dictionary>();
        atlasAddImage(dyldImage, dyldLoadAddress, dyldPath);
    }

    if (cacheBitmap) {
        uint64_t sharedCacheLoadAddress = (uint64_t)cache;
        std::span<const dyld_cache_image_text_info> textInfos = std::span((const dyld_cache_image_text_info*)(sharedCacheLoadAddress+cache->header.imagesTextOffset),(size_t)cache->header.imagesTextCount);
        for (auto& textInfo : textInfos) {
            if (strcmp((const char*)(textInfo.pathOffset + sharedCacheLoadAddress), "/usr/lib/dyld") == 0) {
                uint64_t index = &textInfo - &textInfos[0];
                cacheBitmap->setBit(index);
                break;
            }
        }
    }

    ByteStream newAtlas(allocator);
    ByteStream fileStream(allocator);
    propertyListEncoder.encode(fileStream);
    //    aarEncoder.setAlgorithm(COMPRESSION_LZFSE);
    aarEncoder.addFile("process.plist", fileStream);
    aarEncoder.encode(newAtlas);
    // Set the timestamp in case anyone tries to sync with it between the old and new interfaces
    activateAtlas(allocator, newAtlas);
#endif /* DYLD_FEATURE_ATLAS_GENERATION */
#if DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION
    aarEncoder.encode(outputStream);
    ByteStream result(ephemeralAllocator);
    result.insert(result.begin(), outputStream.begin(), outputStream.end());
    activateAtlas(allocator, result);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION || DYLD_FEATURE_ATLAS_GENERATION */
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    // The simulator host support keeps track of images as they are added. dyld and dyld_sim are special cased
    // initialize the storage and insert the main executable here
    loadedImagesInfos = Vector<dyld_image_info>::make(MemoryManager::defaultAllocator());
    dyld_image_info mainInfo = { (const struct mach_header*)mainExecutableAddress, mainExecutablePath, 0};
    loadedImagesInfos->push_back(mainInfo);
#endif
    triggerNotifications(dyld_image_adding, _allImageInfo->infoArrayCount, _allImageInfo->infoArray);
}

void ExternallyViewableState::setLibSystemInitialized()
{
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    setDyldState(dyld_process_state_libSystem_initialized);
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
    _allImageInfo->libSystemInitialized = true;
}

// In addImages and removeImages we interweave the updates to the legacy images and the compact info. We do that
// so that we can deallocate everything before we allocate the new structures ont he persistent allocator, which lets
// it collapse from its high water mark.

void ExternallyViewableState::addImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& imageInfos) {
    // 1. Update timestamp
    updateTimestamp();
    // 2. Generate new info on the ephemeral allocator
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    // Copy the existing vectors into new vectors in the ephemeral allocator
    lsl::Vector<dyld_image_info> newImageList(ephemeralAllocator);
    lsl::Vector<dyld_uuid_info> newUuidList(ephemeralAllocator);
    newImageList.insert(newImageList.begin(), _imageInfos->begin(), _imageInfos->end());
    newUuidList.insert(newUuidList.begin(), _imageUUIDs->begin(), _imageUUIDs->end());
    for (const ImageInfo& imageInfo : imageInfos) {
        const Header* mh = (const Header*)imageInfo.loadAddress;
        //fprintf(stderr, "ExternallyViewableState::addImages(): mh=%p, path=%s\n", mf, imageInfo.path);
        newImageList.push_back({(mach_header*)imageInfo.loadAddress, imageInfo.path, 0});
        if ( !imageInfo.inSharedCache ) {
            dyld_uuid_info uuidAndAddr;
            uuidAndAddr.imageLoadAddress = (const struct mach_header*)mh;
            mh->getUuid(uuidAndAddr.imageUUID);
            newUuidList.push_back(uuidAndAddr);
        }
    }
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
    // 3. Generate  atlases
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    auto newAtlas = generateAtlas(ephemeralAllocator);
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
    // 4. Clear the old info
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    // append old style additions to all image infos array
    _allImageInfo->infoArray = nullptr;   // set infoArray to NULL to denote it is in-use
    _allImageInfo->uuidArray = nullptr;   // set uuidArray to NULL to denote it is in-use
    uint32_t oldInfoCount                   = _allImageInfo->infoArrayCount;
    _imageInfos->clear();
    _imageUUIDs->clear();
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    // 4. Atomically swap atlases
    // Activate atlas both clears the old info and allocates a new one. It needs to do both to
    // guarantee the atomicity for the atlas.
    activateAtlas(*_persistentAllocator, newAtlas);
#endif
    // 5. Setup the new info
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    _imageInfos->reserve(newImageList.size());
    _imageUUIDs->reserve(newUuidList.size());
    _imageInfos->insert(_imageInfos->begin(), newImageList.begin(), newImageList.end());
    _imageUUIDs->insert(_imageUUIDs->begin(), newUuidList.begin(), newUuidList.end());
    _allImageInfo->infoArrayCount           = (uint32_t)newImageList.size();
    _allImageInfo->uuidArrayCount           = (uint32_t)newUuidList.size();
    _allImageInfo->infoArrayChangeTimestamp = _timestamp;
    _allImageInfo->infoArray                = _imageInfos->data();
    _allImageInfo->uuidArray                = _imageUUIDs->data();
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
    // 6. Trigger notifications
    triggerNotifications(dyld_image_adding, (uint32_t)newImageList.size()-oldInfoCount, &_allImageInfo->infoArray[oldInfoCount]);
}

void ExternallyViewableState::removeImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, std::span<const mach_header*>& mhs)
{
    // 1. Get the update timestamp
    updateTimestamp();
    auto removedInfos = Vector<dyld_image_info>(ephemeralAllocator);
    // 2. Figure out the new image lists
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    // Create two Vectors, one of images to remove, and one of images to keep
    removedInfos.reserve(mhs.size());
    auto remaingInfos = Vector<dyld_image_info>(ephemeralAllocator);
    remaingInfos.reserve(_imageInfos->size() - mhs.size());
    for (const auto& it : *_imageInfos) {
        bool removed = false;
        for (const mach_header* mh : mhs) {
            if (it.imageLoadAddress == mh) {
                removed = true;
                break;
            }
        }
        if (removed) {
            removedInfos.push_back(it);
        } else {
            remaingInfos.push_back(it);
        }
    }

    // Go through the uuid array and filter it down based on the libraries being removed
    auto remaingUuids = Vector<dyld_uuid_info>(ephemeralAllocator);
    for (const auto& it : *_imageUUIDs) {
        bool removed = false;
        for (const auto& removedInfo : removedInfos) {
            if ( it.imageLoadAddress == removedInfo.imageLoadAddress ) {
                removed = true;
                break;
            }
        }
        if (!removed) {
            remaingUuids.push_back(it);
        }
    }
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
    // 3. Create the atlase
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    auto newAtlas = generateAtlas(ephemeralAllocator);
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
    // 4. Clear the old info
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    _allImageInfo->infoArray = nullptr;   // set infoArray to NULL to denote it is in-use
    _allImageInfo->uuidArray = nullptr;   // set uuidArray to NULL to denote it is in-use
    _imageInfos->clear();
    _imageUUIDs->clear();
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
    // 5. Atomically update atlases
    // Activate atlas both clears the old info and allocates a new one. It nees to do both to
    // guarantee the atomicity for the atlas.
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    activateAtlas(*_persistentAllocator, newAtlas);
#endif 
    // 6. Setup the new info
#if DYLD_FEATURE_LEGACY_IMAGE_INFO
    _imageInfos->reserve(remaingInfos.size());
    _imageUUIDs->reserve(remaingUuids.size());
    _imageInfos->insert(_imageInfos->begin(), remaingInfos.begin(), remaingInfos.end());
    _imageUUIDs->insert(_imageUUIDs->begin(), remaingUuids.begin(), remaingUuids.end());
    _allImageInfo->infoArrayCount           = (uint32_t)remaingInfos.size();
    _allImageInfo->uuidArrayCount           = (uint32_t)remaingUuids.size();
    _allImageInfo->infoArrayChangeTimestamp = _timestamp;
    _allImageInfo->infoArray                = _imageInfos->data();
    _allImageInfo->uuidArray                = _imageUUIDs->data();
#endif /* DYLD_FEATURE_LEGACY_IMAGE_INFO */
    // 5. Trigger notifications
    // if there are any changes and some other process is monitoring this one, notify it
    triggerNotifications(dyld_image_removing, (unsigned int)removedInfos.size(), removedInfos.data());
}

void ExternallyViewableState::triggerNotifications(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]) {
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
#if DYLD_FEATURE_SIMULATOR_NOTIFICATIONS
    // Simualtor notifications must go first since the host shim may actually update the info and we need all updates to happen
    // before all externally viewable notifications
    if ( _syscallHelpers->version >= 11 ) {
        STACK_ALLOCATOR(epehemeralAllocator, 0);
        // notify any other processing inspecting this one
        // notify any processes tracking loads in this process
        Vector<const char*> pathsBuffer(epehemeralAllocator);
        pathsBuffer.reserve(infoCount);
        Vector<const mach_header*> mhBuffer(epehemeralAllocator);
        mhBuffer.reserve(infoCount);

        for (auto i = 0; i < infoCount; ++i) {
            pathsBuffer.push_back(info[i].imageFilePath);
            mhBuffer.push_back((mach_header*)info[i].imageLoadAddress);
        }
        _syscallHelpers->notifyMonitorOfImageListChanges(mode == dyld_image_removing, (unsigned int)infoCount, &mhBuffer[0], &pathsBuffer[0]);
    }
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATIONS */
#if DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS
#if TARGET_OS_SIMULATOR
    if (_syscallHelpers->version < 18) {
        // Newer dylds call the break point function on dyld_sims behalf, so only call it if this is an old dyld
        _allImageInfo->notification(mode, infoCount, info);
    }
#else
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    if (_allImageInfo->notification == lldb_image_notifier_sim_trap) {
        // We set the simulator to the trap, but we need to switch it back before we trigger the notification so LLDB
        // does not update the notifier when it reads the all image infos. Compiler barriers are necessary to prevent
        // the compiler from optimizing these away, as the order must be observable to an external agent (lldb).
        _allImageInfo->notification = &lldb_image_notifier;
        os_compiler_barrier();
        // Call the real notifier
        _allImageInfo->notification(mode, infoCount, info);
        os_compiler_barrier();
        // Switch back to the trap
        _allImageInfo->notification = &lldb_image_notifier_sim_trap;
    } else {
        _allImageInfo->notification(mode, infoCount, info);
    }
#else
    _allImageInfo->notification(mode, infoCount, info);
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */
#endif /* TARGET_OS_SIMULATOR */
#endif /* DYLD_FEATURE_BREAKPOINT_NOTIFICATIONS */
#if DYLD_FEATURE_MACH_PORT_NOTIFICATIONS || DYLD_FEATURE_LEGACY_MACH_PORT_NOTIFICATIONS
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if (!responder.active()) return;
#endif /* DYLD_FEATURE_MACH_PORT_NOTIFICATIONS || DYLD_FEATURE_LEGACY_MACH_PORT_NOTIFICATIONS */
#if DYLD_FEATURE_MACH_PORT_NOTIFICATIONS
    responder.blockOnSynchronousEvent(DYLD_REMOTE_EVENT_ATLAS_CHANGED);
#endif /* DYLD_FEATURE_MACH_PORT_NOTIFICATIONS */
#if DYLD_FEATURE_LEGACY_MACH_PORT_NOTIFICATIONS
    STACK_ALLOCATOR(allocator, 0);
    Vector<const struct mach_header*> loadAddresses(allocator);
    Vector<const char*> imagePaths(allocator);
    loadAddresses.reserve(infoCount);
    imagePaths.reserve(infoCount);
    for (auto i = 0; i < infoCount; ++i) {
        loadAddresses.push_back(info[i].imageLoadAddress);
        imagePaths.push_back(info[i].imageFilePath);
    }
    responder.notifyMonitorOfImageListChanges(mode == dyld_image_removing, (unsigned int)infoCount, &loadAddresses[0], &imagePaths[0], _timestamp);
#endif
}

// Unlike the scavenger, this function avoids using any content from the allImageInfos, so that we have the option of
// removing the all image infos. The one exception for now is Rosetta AOT infos, which is not currently represented
// in the loaders.
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
ByteStream ExternallyViewableState::generateAtlas(Allocator& allocator) {
    ByteStream outputStream(allocator);
    AAREncoder aarEncoder(allocator);
    // We stub out and call the legacy compact info enocoder here. Though it is a bit counter intuitive, we do it here since they share the same
    // AAREncoder, and the other option requires making every callsite contain all the AAREncoder setup
    auto compactInfo = generateCompactInfo(allocator, aarEncoder);
    if (compactInfo.size() > 0) {
        aarEncoder.addFile("process.cinfo", compactInfo);
    }
#if DYLD_FEATURE_ATLAS_GENERATION
    using Array         = PropertyList::Array;
    using Dictionary    = PropertyList::Dictionary;

    auto propertyListEncoder            = PropertyList(allocator);
    auto& rootDictionary                = propertyListEncoder.rootDictionary();
    auto& images                        = rootDictionary.addObjectForKey<Array>(kDyldAtlasSnapshotImagesArrayKey);
    const DyldSharedCache* cache        = _runtimeState->config.dyldCache.addr;
    PropertyList::Bitmap* cacheBitmap   = gatherAtlasProcessInfo((uint64_t)_runtimeState->config.process.mainExecutableMF, cache, rootDictionary);
    std::span<const dyld_cache_image_text_info> textInfos;
    if (cache) {
        uint64_t sharedCacheLoadAddress = (uint64_t)cache;
        textInfos = std::span((const dyld_cache_image_text_info*)(sharedCacheLoadAddress+cache->header.imagesTextOffset),(size_t)cache->header.imagesTextCount);
    }

    for (const Loader* ldr :  _runtimeState->loaded) {
        const MachOLoaded* ml = ldr->loadAddress(*_runtimeState);
        if (ldr->dylibInDyldCache && cacheBitmap) {
            cacheBitmap->setBit(ldr->ref.index);
            continue;
        }
        const char* filePath = ldr->path(*_runtimeState);
        if (filePath) {
            auto& image = images.addObject<Dictionary>();
            atlasAddImage(image, (uint64_t)ml, filePath);
        }
    }
    if (__dso_handle.flags & MH_DYLIB_IN_CACHE) {
        // dyld is in the cache
        for (auto& textInfo : textInfos) {
            if (textInfo.loadAddress + _runtimeState->config.dyldCache.slide == (uint64_t)&__dso_handle) {
                uint64_t index = &textInfo - &textInfos[0];
                cacheBitmap->setBit(index);
                break;
            }
        }
    } else {
        auto& image = images.addObject<Dictionary>();
        atlasAddImage(image, (uint64_t)&__dso_handle, _runtimeState->config.process.dyldPath);
    }

#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    if ( _dyldSimLoadAddress != 0 ) {
        // Handle dyld_sim
        auto& dyldSimImage = images.addObject<Dictionary>();
        atlasAddImage(dyldSimImage, (uint64_t)_dyldSimLoadAddress, _dyldSimPath);
    }
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */

#if SUPPORT_ROSETTA
    using Data          = PropertyList::Data;
    if (_allImageInfo->aotInfoCount > 0) {
        std::span<dyld_aot_image_info> aotInfos = std::span((dyld_aot_image_info*)_allImageInfo->aotInfoArray, _allImageInfo->aotInfoCount);
        auto& aotImages                = rootDictionary.addObjectForKey<Array>(kDyldAtlasSnapshotAotImagesArrayKey);
        for (const auto& aotInfo : aotInfos) {
            auto& aotImage = aotImages.addObject<Dictionary>();
            aotImage.addObjectForKey<PropertyList::Integer>(kDyldAtlasAOTImageX86AddrKey, (uint64_t)aotInfo.x86LoadAddress);
            aotImage.addObjectForKey<PropertyList::Integer>(kDyldAtlasAOTImageNativeAddrKey, (uint64_t)aotInfo.aotLoadAddress);
            aotImage.addObjectForKey<PropertyList::Integer>(kDyldAtlasAOTImageSizeKey, aotInfo.aotImageSize);
            std::span<std::byte> keySpan = std::span((std::byte*)&aotInfo.aotImageKey[0], DYLD_AOT_IMAGE_KEY_SIZE);
            aotImage.addObjectForKey<Data>(kDyldAtlasAOTImageImageKeyKey, keySpan);
        }
    }
#endif /* SUPPORT_ROSETTA */
    ByteStream newAtlas(allocator);
    ByteStream fileStream(allocator);
    propertyListEncoder.encode(fileStream);
    aarEncoder.addFile("process.plist", fileStream);
#endif /* DYLD_FEATURE_ATLAS_GENERATION */
    aarEncoder.encode(outputStream);
    ByteStream result(allocator);
    result.insert(result.begin(), outputStream.begin(), outputStream.end());
    return result;
}

std::byte* ExternallyViewableState::swapActiveAtlas(std::byte* begin, std::byte* end, struct dyld_all_image_infos* allImageInfos) {
    // atomically update compactInfo addr/size in all_image_infos
    struct CompactInfoDescriptor {
        uintptr_t   addr;
        size_t      size;
    } __attribute__((aligned(16)));
    CompactInfoDescriptor newDescriptor;
    newDescriptor.addr = (uintptr_t)begin;
    newDescriptor.size = (size_t)(end-begin);
    uintptr_t oldCompactInfo = allImageInfos->compact_dyld_image_info_addr;
#if !__LP64__
    // armv32 archs are missing the atomic primitive, but we only need to be guaraantee the write does not sheer, as the only thing
    // accessing this outside of a lock is the kernel or a remote process
    uint64_t* currentDescriptor = (uint64_t*)&sProcessInfo->compact_dyld_image_info_addr;
    *currentDescriptor = *((uint64_t*)&newDescriptor);
#else
    // We do not need a compare and swap since we are under a lock, but we do need the updates to be atomic to out of process observers
    std::atomic<CompactInfoDescriptor>* currentDescriptor = (std::atomic<CompactInfoDescriptor>*)&allImageInfos->compact_dyld_image_info_addr;
    currentDescriptor->store(newDescriptor, std::memory_order_relaxed);
#endif
    return (std::byte*)oldCompactInfo;
}
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */

void ExternallyViewableState::activateAtlas(Allocator& allocator, ByteStream& newAtlas) {
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    // Swap the activeSnapshot to the one we just created on the ephemeralAllocator
    std::byte* oldAtlas = swapActiveAtlas(newAtlas.begin(), newAtlas.end(), _allImageInfo);
    if (oldAtlas && allocator.owned((void*)oldAtlas, 8)) {
        // We swapped the info, if there is space update the old one in place and swap back
        if (allocator.size((const void *)oldAtlas) >= newAtlas.size()) {
            std::copy(newAtlas.begin(), newAtlas.end(), oldAtlas);
            swapActiveAtlas(oldAtlas, oldAtlas+newAtlas.size(), _allImageInfo);
            // If there is not enough space, can we realloc() to get enough space?
        } else if (allocator.realloc((void *)oldAtlas, newAtlas.size())) {
            std::copy(newAtlas.begin(), newAtlas.end(), oldAtlas);
            swapActiveAtlas(oldAtlas, oldAtlas+newAtlas.size(), _allImageInfo);
        } else {
            allocator.free((void*)oldAtlas);
            std::byte* newAtlasStorage = (std::byte*)allocator.malloc(newAtlas.size());
            std::copy(newAtlas.begin(), newAtlas.end(), &newAtlasStorage[0]);
            (void)swapActiveAtlas(newAtlasStorage, newAtlasStorage+newAtlas.size(), _allImageInfo);
        }
    } else {
        // This might be info setup by the dyld runtime state, and if so we don't know the tpro state.
        // If the oldCompactInfo is not owned by the persistentAllocator then purposefully leak it.
        std::byte* newAtlasStorage = (std::byte*)allocator.malloc(newAtlas.size());
        std::copy(newAtlas.begin(), newAtlas.end(), &newAtlasStorage[0]);
        (void)swapActiveAtlas(newAtlasStorage, newAtlasStorage+newAtlas.size(), _allImageInfo);
    }
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
    _allImageInfo->infoArrayChangeTimestamp = _timestamp;
}

#if DYLD_FEATURE_ATLAS_GENERATION
void ExternallyViewableState::atlasAddImage(PropertyList::Dictionary& image, uint64_t loadAddress, const char *filePath) {
    using String        = PropertyList::String;
    using Integer       = PropertyList::Integer;
    using UUID          = PropertyList::UUID;
    using Array         = PropertyList::Array;
    using Dictionary    = PropertyList::Dictionary;

    image.addObjectForKey<String>(kDyldAtlasImageFilePathKey, filePath);
    std::span<const uint8_t> headerSpan((const uint8_t*)loadAddress, sizeof(mach_header));
    const mach_o::Header* header = mach_o::Header::isMachO(headerSpan);
    if (!header || header->inDyldCache()) {
        return;
    }
    image.addObjectForKey<Integer>(kDyldAtlasImageLoadAddressKey, loadAddress);
    uint64_t preferredLoadAddress = header->preferredLoadAddress();
    if (preferredLoadAddress) {
        image.addObjectForKey<Integer>(kDyldAtlasImagePreferredLoadAddressKey, preferredLoadAddress);
    }
    const char* installname = header->installName();
    if (installname) {
        image.addObjectForKey<String>(kDyldAtlasImageInstallnameKey, installname);
    }
    uuid_t uuid;
    if (header->getUuid(uuid)) {
        image.addObjectForKey<UUID>(kDyldAtlasImageUUIDKey, uuid);
    }
    __block Array* segments = nullptr;
    header->forEachSegment(^(const mach_o::Header::SegmentInfo& info, bool& stop) {
        if (info.segmentName == "__PAGEZERO") {
            return;
        }
        if (!segments) {
            segments = &image.addObjectForKey<Array>(kDyldAtlasImageSegmentArrayKey);
        }
        auto segment = &segments->addObject<Dictionary>();
        segment->addObjectForKey<String>(kDyldAtlasSegmentNameKey, info.segmentName);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentPreferredLoadAddressKey, info.vmaddr);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentSizeKey, info.vmsize);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentFileOffsetKey, info.fileOffset);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentFileSizeKey, info.fileSize);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentPermissionsKey, info.initProt);
    });
}

PropertyList::Bitmap* ExternallyViewableState::gatherAtlasProcessInfo(uint64_t mainExecutableAddress, const DyldSharedCache *cache, PropertyList::Dictionary &rootDictionary) {
    PropertyList::Bitmap* result = nullptr;

    auto snapshotFlags          = rootDictionary.addObjectForKey<PropertyList::Flags<SnapshotFlags>>(kDyldAtlasSnapshotFlagsKey);
    rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotTimestampKey, _timestamp);
    rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotPidKey, getpid());
    rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotState, _dyldState);
    rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotInitialImageCount, 1);

    if (_runtimeState) {
        // The runtime state is the canonical source of info for what type of process this is
        rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotPlatformTypeKey,  (int64_t)_runtimeState->config.process.platform.value());
    } else {
        // The runtime state is not available yet, infer the the process type from the main executable. For certain types of processes this may change
        // shortly after bootstrap (in particular, those that use `DYLD_FORCE_PLATFORM`)
        std::span<const uint8_t> mainHeaderSpan((const uint8_t*)mainExecutableAddress, sizeof(mach_header));
        const mach_o::Header* mainHeader = mach_o::Header::isMachO(mainHeaderSpan);
        rootDictionary.addObjectForKey<PropertyList::Integer>(kDyldAtlasSnapshotPlatformTypeKey,  (int64_t)mainHeader->platformAndVersions().platform.value());
    }

    if (PAGE_SIZE == 4096) {
        snapshotFlags.setFlag(SnapshotFlagsPageSize4k, true);
    }
#if !__LP64__
    snapshotFlags.setFlag(SnapshotFlagsPointerSize4Bytes, false);
#endif
    if (cache) {
        // Add the cache so we can include the in cache dyld
        const DyldSharedCache::DynamicRegion* dynamicRegion = cache->dynamicRegion();
        uint64_t sharedCacheLoadAddress     = (uint64_t)cache;
        auto& cacheAtlas                    = rootDictionary.addObjectForKey<PropertyList::Dictionary>(kDyldAtlasSnapshotSharedCacheKey);
        std::span<const dyld_cache_image_text_info> textInfos = std::span((const dyld_cache_image_text_info*)(sharedCacheLoadAddress+cache->header.imagesTextOffset),(size_t)cache->header.imagesTextCount);
        result = &cacheAtlas.addObjectForKey<PropertyList::Bitmap>(kDyldAtlasSharedCacheBitmapArrayKey, textInfos.size());
        cacheAtlas.addObjectForKey<PropertyList::String>(kDyldAtlasSharedCacheFilePathKey, dynamicRegion->cachePath());
        cacheAtlas.addObjectForKey<PropertyList::Integer>(kDyldAtlasSharedCacheLoadAddressKey, sharedCacheLoadAddress);

        uuid_t cacheUUID;
        cache->getUUID(cacheUUID);
        cacheAtlas.addObjectForKey<PropertyList::UUID>(kDyldAtlasSharedCacheUUIDKey, cacheUUID);
#if SUPPORT_ROSETTA
        if (_allImageInfo->aotSharedCacheBaseAddress) {
            cacheAtlas.addObjectForKey<PropertyList::Integer>(kDyldAtlasSharedCacheAotLoadAddressKey, _allImageInfo->aotSharedCacheBaseAddress);
            cacheAtlas.addObjectForKey<PropertyList::UUID>(kDyldAtlasSharedCacheAotLoadAddressKey, _allImageInfo->aotSharedCacheUUID);
        }
#endif /* SUPPORT_ROSETTA */
    }
    return result;
}
#endif /* DYLD_FEATURE_ATLAS_GENERATION */

void ExternallyViewableState::updateTimestamp() {
#if !TARGET_OS_EXCLAVEKIT
    uint64_t timestamp = mach_absolute_time();
    if (timestamp > _timestamp) {
        _timestamp = timestamp;
    } else {
        // We updated before the clock ticked. Chnage the timstamp manually
        ++_timestamp;
    }
#else
    _timestamp++;
#endif
}

// used by host dyld before calling into dyld_sim
void ExternallyViewableState::detachFromSharedRegion()
{
    _allImageInfo->processDetachedFromSharedRegion  = true;
    _allImageInfo->sharedCacheSlide                 = 0;
    _allImageInfo->sharedCacheBaseAddress           = 0;
    ::bzero(_allImageInfo->sharedCacheUUID,sizeof(uuid_t));
}

uint64_t ExternallyViewableState::imageInfoCount()
{
    return _imageInfos->size();
}

void ExternallyViewableState::disableCrashReportBacktrace()
{
    // update old all_image_infos with flag that means termination is by dyld for missing dylib
    _allImageInfo->terminationFlags = 1; // don't show back trace, because nothing interesting
}

void ExternallyViewableState::fork_child()
{
    // If dyld is sending load/unload notices to CoreSymbolication, the shared memory
    // page is not copied on fork. <rdar://problem/6797342>
    _allImageInfo->coreSymbolicationShmPage = nullptr;
    // for safety, make sure child starts with clean systemOrderFlag
    _allImageInfo->systemOrderFlag = 0;
}

#if DYLD_FEATURE_MACH_PORT_NOTIFICATIONS
mach_port_t ExternallyViewableState::notifyPortValue()
{
    return _allImageInfo->notifyPorts[0];
}

#endif

uint64_t ExternallyViewableState::lastImageListUpdateTime()
{
    return _allImageInfo->infoArrayChangeTimestamp;
}

/* Since legacy simulators call notifyMonitorOfImageListChangesSim we can use it as a choke point to have the host dyld generate compact info
   for them. Since we don't have access to the runtime state here and we can't change the call signature need to just materialize
   everything it needs here.

  The simulator directly manipulates the all image info, and it currently does that after calling this function, but it may have done other
  things in the past. We need to just handle everything here, so this works by maintaining its own image list based on what it has been passed, that way
  it will remain consistent.
 */

void ExternallyViewableState::notifyMonitorOfImageListChangesSim(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[])
{
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    MemoryManager::withWritableMemory(^{
        if (unloading) {
            // We don't need to be clever here, the sim is an edge case, and unloading is also very rare due to the shared cache and objc
            for (auto i = 0; i < imageCount; ++i) {
                for (auto j = loadedImagesInfos->begin(); j != loadedImagesInfos->end(); ++j) {
                    if (j->imageLoadAddress != loadAddresses[i]) { continue; }
                    // Remove the image
                    loadedImagesInfos->erase(j);
                    break;
                }
            }
        } else {
            for (auto i = 0; i < imageCount; ++i) {
                dyld_image_info info = { loadAddresses[i], imagePaths[i], 0 };
                loadedImagesInfos->push_back(info);
            }
        }
        STACK_ALLOCATOR(ephemeralAllocator, 0)
        AAREncoder aarEncoder(ephemeralAllocator);
        const DyldSharedCache* cache        = (const DyldSharedCache*)_allImageInfo->sharedCacheBaseAddress;

        // Synthesize the _dyldState from things dyld_sim has done
        if ( _allImageInfo->libSystemInitialized != 0 ) {
            _dyldState = dyld_process_state_libSystem_initialized;
            if ( _allImageInfo->initialImageCount != loadedImagesInfos->size() ) {
                _dyldState = dyld_process_state_program_running;
            }
        }
        if ( _allImageInfo->errorMessage != 0 ) {
            _dyldState = _allImageInfo->terminationFlags ? dyld_process_state_terminated_before_inits : dyld_process_state_dyld_terminated;
        }

#if DYLD_FEATURE_COMPACT_INFO_GENERATION
        static FileManager* glueFileManager = nullptr;
        if (!glueFileManager) {
            // We create a new file manager here to support old style compact info. We don't want to use the one
            // on the hose runtimeState since that is TPRO protected and it would be a lot of effort to wire that
            // all through, which is not worth for the legacy simulator case.
            void* fieManagerBuffer = MemoryManager::defaultAllocator().aligned_alloc(alignof(FileManager), sizeof(FileManager));
            glueFileManager = new (fieManagerBuffer) FileManager(MemoryManager::defaultAllocator());
        }

        Atlas::ProcessSnapshot snapshot(ephemeralAllocator, *glueFileManager, true);
        // This has been busted for ages and we will get rid of it soon
        snapshot.setInitialImageCount(2);
        snapshot.setDyldState(_dyldState);
        snapshot.setPlatform((uint32_t)_runtimeState->config.process.platform.value());

        if (cache) {
            uint64_t sharedCacheLoadAddress = (uint64_t)cache;
            // Technically this wrong, but private caches are mostly broken right now and this is a temporary path until we turn on ATLAS generation
            auto cacheFile = glueFileManager->fileRecordForVolumeDevIDAndObjID(_allImageInfo->sharedCacheFSID, _allImageInfo->sharedCacheFSObjID);
            Atlas::SharedCache atlasCache(ephemeralAllocator, std::move(cacheFile), snapshot.identityMapper(), sharedCacheLoadAddress, false);
            snapshot.addSharedCache(std::move(atlasCache));
        }

        for (auto imageInfo: *loadedImagesInfos) {
            auto fileRecord = glueFileManager->fileRecordForPath(ephemeralAllocator, imageInfo.imageFilePath);
            auto image = Atlas::Image(ephemeralAllocator, std::move(fileRecord),  snapshot.identityMapper(), (uint64_t)imageInfo.imageLoadAddress);
            snapshot.addImage(std::move(image));
        }

        // Add dyld
        auto legacyDyldFileRecord = glueFileManager->fileRecordForPath(ephemeralAllocator, _runtimeState->config.process.dyldPath);
        auto legacyDyldImage = Atlas::Image(ephemeralAllocator, std::move(legacyDyldFileRecord),  snapshot.identityMapper(), (uint64_t)&__dso_handle);
        snapshot.addImage(std::move(legacyDyldImage));

        auto serializedCompactInfo = snapshot.serialize();
        aarEncoder.addFile("process.cinfo", serializedCompactInfo);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION */
#if DYLD_FEATURE_ATLAS_GENERATION
        using UUID          = PropertyList::UUID;
        using Array         = PropertyList::Array;
        using Bitmap        = PropertyList::Bitmap;
        using String        = PropertyList::String;
        using Integer       = PropertyList::Integer;
        using Dictionary    = PropertyList::Dictionary;

        STACK_ALLOCATOR(allocator, 0);
        auto propertyListEncoder    = PropertyList(allocator);
        auto& rootDictionary        = propertyListEncoder.rootDictionary();
        auto& images                = rootDictionary.addObjectForKey<Array>(kDyldAtlasSnapshotImagesArrayKey);
        auto snapshotFlags          = rootDictionary.addObjectForKey<PropertyList::Flags<SnapshotFlags>>(kDyldAtlasSnapshotFlagsKey);
        rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotTimestampKey, _allImageInfo->infoArrayChangeTimestamp);
        rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotPidKey, _runtimeState->config.process.pid);
        rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotState, _dyldState);
        rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotPlatformTypeKey, _runtimeState->config.process.platform.value());
        rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotInitialImageCount, 1);
        snapshotFlags.setFlag(SnapshotFlagsPrivateSharedRegion, true);

        if (_runtimeState->config.pathOverrides.simRootPath()) {
            auto& envDictionary          = rootDictionary.addObjectForKey<Dictionary>(kDyldAtlasSnapshotEnvironmentVarsKey);
            envDictionary.addObjectForKey<String>(kDyldAtlasEnvironmentRootPathKey, _runtimeState->config.pathOverrides.simRootPath());
        }

        std::span<const dyld_cache_image_text_info> textInfos;
        PropertyList::Bitmap* cacheBitMap   = nullptr;
        uint64_t sharedCacheLoadAddress     = (uint64_t)cache;
        uint64_t lastCacheAddress           = 0;

        if (cache) {
            lastCacheAddress           = sharedCacheLoadAddress + cache->header.sharedRegionSize;
            auto& cacheAtlas            = rootDictionary.addObjectForKey<Dictionary>(kDyldAtlasSnapshotSharedCacheKey);
            textInfos = std::span((const dyld_cache_image_text_info*)(sharedCacheLoadAddress+cache->header.imagesTextOffset),(size_t)cache->header.imagesTextCount);
            cacheAtlas.addObjectForKey<Integer>(kDyldAtlasSharedCacheLoadAddressKey, sharedCacheLoadAddress);
            if (!_dyldSimCachePath) {
                fsid_t fsid  = *((fsid_t*)&_allImageInfo->sharedCacheFSID);
                char cachePath[PATH_MAX+1] = {};
                if (fsgetpath(cachePath, PATH_MAX+1, &fsid, _allImageInfo->sharedCacheFSObjID) > 0) {
                    _dyldSimCachePath = MemoryManager::defaultAllocator().strdup(cachePath);
                }
            }
            cacheAtlas.addObjectForKey<String>(kDyldAtlasSharedCacheFilePathKey, _dyldSimCachePath);
            uuid_t cacheUUID;
            cache->getUUID(cacheUUID);
            cacheAtlas.addObjectForKey<UUID>(kDyldAtlasSharedCacheUUIDKey, cacheUUID);
            cacheBitMap = &cacheAtlas.addObjectForKey<Bitmap>(kDyldAtlasSharedCacheBitmapArrayKey, textInfos.size());
        }

        for (auto info : *loadedImagesInfos) {
            if (info.imageLoadAddress->flags & MH_DYLIB_IN_CACHE) {
                for (auto& textInfo : textInfos) {
                    if (textInfo.loadAddress + _runtimeState->config.dyldCache.slide == (uint64_t)info.imageLoadAddress) {
                        uint64_t index = &textInfo - &textInfos[0];
                        cacheBitMap->setBit(index);
                        break;
                    }
                }
                continue;
            }
            auto& image = images.addObject<Dictionary>();
            atlasAddImage(image, (uint64_t)info.imageLoadAddress, info.imageFilePath);
        }

        // Handle dyld
        auto& dyldImage = images.addObject<Dictionary>();
        atlasAddImage(dyldImage, (uint64_t)&__dso_handle, _runtimeState->config.process.dyldPath);

        // Synthesize the dyldState
        if ( _allImageInfo->errorMessage != 0 ) {
            rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotState, _allImageInfo->terminationFlags ? dyld_process_state_terminated_before_inits : dyld_process_state_dyld_terminated);
        }

        ByteStream atlasStream(ephemeralAllocator);
        propertyListEncoder.encode(atlasStream);
        aarEncoder.addFile("process.plist", atlasStream);
#endif /* DYLD_FEATURE_COMPACT_INFO_GENERATION */
        ByteStream newAtlas(ephemeralAllocator);
        aarEncoder.encode(newAtlas);
        activateAtlas(MemoryManager::defaultAllocator(), newAtlas);

        // We need to wrap the data that was passed in in structs suitable for notification and the pass it to triggerNotifications()
        Vector<dyld_image_info> notificationList(ephemeralAllocator);
        notificationList.reserve(imageCount);
        for(auto i = 0; i < imageCount; ++i) {
            struct dyld_image_info info = { loadAddresses[i], imagePaths[i], 0 };
            notificationList.push_back(info);
        }

        triggerNotifications(unloading ? dyld_image_removing : dyld_image_adding, imageCount, &notificationList[0]);
    });
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */
}

void ExternallyViewableState::notifyMonitorOfMainCalled()
{
#if DYLD_FEATURE_SIMULATOR_NOTIFICATIONS
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 17 )
        _syscallHelpers->notifyMonitorOfMainCalled();
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATIONS */
#if DYLD_FEATURE_MACH_PORT_NOTIFICATIONS
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfMainCalled();
#endif /* DYLD_FEATURE_MACH_PORT_NOTIFICATIONS */
}

void ExternallyViewableState::notifyMonitorOfDyldBeforeInitializers()
{
#if DYLD_FEATURE_SIMULATOR_NOTIFICATIONS
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 17 )
        _syscallHelpers->notifyMonitorOfDyldBeforeInitializers();
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATIONS */
#if DYLD_FEATURE_MACH_PORT_NOTIFICATIONS
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfDyldBeforeInitializers();
#endif /* DYLD_FEATURE_MACH_PORT_NOTIFICATIONS */
}

void ExternallyViewableState::setRuntimeState(RuntimeState* state) {
    _runtimeState = state;
    _allImageInfo->platform =  (uint32_t)_runtimeState->config.process.platform.value();
#if DYLD_FEATURE_SIMULATOR_NOTIFICATIONS
    // Normally this is handled by handleDyldInCache, but that does not happen in dyld_sim
    if ( state->config.dyldCache.addr != nullptr ) {
        // update cache info in old all_image_infos
        _allImageInfo->sharedCacheSlide                = (uintptr_t)state->config.dyldCache.slide;
        _allImageInfo->sharedCacheBaseAddress          = (uintptr_t)state->config.dyldCache.unslidLoadAddress;
        _allImageInfo->sharedCacheFSID                 = state->config.dyldCache.mainFileID.fsID();
        _allImageInfo->sharedCacheFSObjID              = state->config.dyldCache.mainFileID.inode();
        _allImageInfo->processDetachedFromSharedRegion = state->config.dyldCache.privateCache;
        if ( const DyldSharedCache* dyldCache = (DyldSharedCache*)state->config.dyldCache.addr) {
            dyldCache->getUUID(_allImageInfo->sharedCacheUUID);
        }
    }
#endif
}

#if SUPPORT_ROSETTA
void ExternallyViewableState::setRosettaSharedCacheInfo(uint64_t aotCacheLoadAddress, const uuid_t aotCacheUUID)
{
    _allImageInfo->aotSharedCacheBaseAddress = aotCacheLoadAddress;
    ::memcpy(_allImageInfo->aotSharedCacheUUID, aotCacheUUID, sizeof(uuid_t));
}

void ExternallyViewableState::addRosettaImages(std::span<const dyld_aot_image_info>& aot_infos, std::span<const dyld_image_info>& image_infos)
{
    // rdar://74693049 (handle if aot_get_runtime_info() returns aot_image_count==0)
    if ( aot_infos.size() != 0 ) {
        // append dyld_aot_image_info to all aot image infos array
        _allImageInfo->aotInfoArray = nullptr;  // set aotInfoArray to NULL to denote it is in-use
            _aotImageInfos->insert(_aotImageInfos->begin(), aot_infos.begin(), aot_infos.end());
            _allImageInfo->aotInfoCount = (uint32_t)_aotImageInfos->size();
            _allImageInfo->aotInfoArrayChangeTimestamp = mach_absolute_time();
        _allImageInfo->aotInfoArray = _aotImageInfos->data();   // set aotInfoArray back to base address of vector (other process can now read)
    }

    if ( image_infos.size() != 0 ) {
        // append dyld_image_info to all image infos array
        _allImageInfo->infoArray = nullptr;  // set infoArray to NULL to denote it is in-use
            _imageInfos->insert(_imageInfos->begin(), image_infos.begin(), image_infos.end());
            _allImageInfo->infoArrayCount = (uint32_t)_imageInfos->size();
            _allImageInfo->infoArrayChangeTimestamp = mach_absolute_time();
        _allImageInfo->infoArray = _imageInfos->data();   // set infoArray back to base address of vector (other process can now read)
    }
}

void ExternallyViewableState::removeRosettaImages(std::span<const mach_header*>& mhs)
{
    // set aotInfoArray to NULL to denote it is in-use
    _allImageInfo->aotInfoArray = nullptr;

    for (const mach_header* mh : mhs) {
        // remove image from aotInfoArray
        for (auto it=_aotImageInfos->begin(); it != _aotImageInfos->end(); ++it) {
            if ( it->aotLoadAddress == (const mach_header*)mh ) {
                _aotImageInfos->erase(it);
                break;
            }
        }
    }
    _allImageInfo->aotInfoCount = (uint32_t)_aotImageInfos->size();
    _allImageInfo->aotInfoArrayChangeTimestamp  = mach_absolute_time();
    // set aotInfoArray back to base address of vector
    _allImageInfo->aotInfoArray = _aotImageInfos->data();

}
#endif // SUPPORT_ROSETTA

#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT
// called from disk based dyld before jumping into dyld in the cache
void ExternallyViewableState::prepareInCacheDyldAllImageInfos(const Header* dyldInCacheMH)
{
    const dyld3::MachOLoaded* dyldInCacheML = (const dyld3::MachOLoaded*)dyldInCacheMH;
    sProcessInfo->dyldImageLoadAddress = (const struct mach_header*)dyldInCacheMH;
    uint64_t newProcessInfoSize = 0;
    struct dyld_all_image_infos* newProcessInfo = (struct dyld_all_image_infos*)dyldInCacheML->findSectionContent("__DATA", "__all_image_info", newProcessInfoSize);
    if (!newProcessInfo) {
        newProcessInfo = (struct dyld_all_image_infos*)dyldInCacheML->findSectionContent("__DATA_DIRTY", "__all_image_info", newProcessInfoSize);
    }

    // Copy all the relevent fields from the on disk dyld to in the cache dyld
    uint64_t currentTimestamp =  sProcessInfo->infoArrayChangeTimestamp;
    newProcessInfo->infoArrayChangeTimestamp        = currentTimestamp;
    newProcessInfo->notifyPorts[0]                  = sProcessInfo->notifyPorts[0];
    newProcessInfo->compact_dyld_image_info_addr    = sProcessInfo->compact_dyld_image_info_addr;
    newProcessInfo->compact_dyld_image_info_size    = sProcessInfo->compact_dyld_image_info_size;
    newProcessInfo->initialImageCount               = sProcessInfo->initialImageCount;
    newProcessInfo->sharedCacheSlide                = sProcessInfo->sharedCacheSlide;
    newProcessInfo->sharedCacheBaseAddress          = sProcessInfo->sharedCacheBaseAddress;
    newProcessInfo->sharedCacheFSID                 = sProcessInfo->sharedCacheFSID;
    newProcessInfo->processDetachedFromSharedRegion = sProcessInfo->processDetachedFromSharedRegion;
    newProcessInfo->uuidArrayCount                  = sProcessInfo->uuidArrayCount;
    newProcessInfo->uuidArray                       = sProcessInfo->uuidArray;
    newProcessInfo->infoArrayCount                  = sProcessInfo->infoArrayCount;
    newProcessInfo->infoArray.store(sProcessInfo->infoArray.load());
    memcpy(newProcessInfo->sharedCacheUUID, sProcessInfo->sharedCacheUUID, sizeof(uuid_t));
    // Hold off on copying anything that requires allocatins, the will copied after the transition

    sProcessInfo->dyldVersion                       = "cache";
    sProcessInfo->dyldImageLoadAddress              = (const struct mach_header*)dyldInCacheMH;
    dyld_image_info info = { (const struct mach_header*)dyldInCacheMH, "/usr/lib/dyld", 0 };
    if ( proc_set_dyld_all_image_info((void*)newProcessInfo, sizeof(dyld_all_image_infos)) == 0 ) {
        sProcessInfo->notification(dyld_image_dyld_moved, 1, &info);
        // FIXME: LLDB/dyld interop issue
        // Breakpoints here are broken. The will usually trigger, but with no image list.
        // It appears the way LLDB is using the existing interfaces has an issue since, kicking the noitifier
        // with no struct change is sufficient to bring breakpoints back online. It is possible it is a defect in
        // the interface and they need more data, so it may be a cross functional fix, but we can live with a one line
        // of code observability gap for now.
        newProcessInfo->notification(dyld_image_adding, sProcessInfo->infoArrayCount, sProcessInfo->infoArray);
        // Breakpoints work again!!
    } else {
        // Moving process info failed, 0 out new process info to signal to in cache dyld its all image info is not the real one
        newProcessInfo->notifyPorts[0] = 0;
        newProcessInfo->compact_dyld_image_info_size = 0;                       // Use a size of 0 to indicate we failed
        __typeof(&lldb_image_notifier) prevNotifyLLDB = sProcessInfo->notification;
        sProcessInfo->notification = newProcessInfo->notification;
        prevNotifyLLDB(dyld_image_dyld_moved, 1, &info);
    }

    // coreSymbolicationShmPage is not used by anything any more, so use it to temporarily smuggle a pointer to the old all image info during transition
    // we will reset it before we do another notification, just in case
    newProcessInfo->coreSymbolicationShmPage = this;
}


//
//// old style all_image_info fields
//lsl::Vector<dyld_image_info>*       _imageInfos     = nullptr;
//lsl::Vector<dyld_uuid_info>*        _imageUUIDs     = nullptr;

// called from dyld in cache to transition all_image_info
bool ExternallyViewableState::completeAllImageInfoTransition(Allocator& allocator, const dyld3::MachOFile* dyldInCacheMF)
{
    bool result = true;
    // Get the stashed pointer to the old process info
    ExternallyViewableState* oldExternalState = ( ExternallyViewableState*)sProcessInfo->coreSymbolicationShmPage;
    // Clear this, just in case
    sProcessInfo->coreSymbolicationShmPage = nullptr;
    if (sProcessInfo->compact_dyld_image_info_size == 0) {
        // If we are in the cache and the size has not been set it means we need to use the on disk all image info
        // Set our pointer to the in on disk process info
        sProcessInfo = oldExternalState->_allImageInfo;
        result = false;
    }
    return result;
}
#endif /* #if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT */

// use at start up to set value in __dyld4 section
void ExternallyViewableState::storeProcessInfoPointer(struct dyld_all_image_infos** loc)
{
    *loc = _allImageInfo;
}

void setExternallyViewableStateToTerminated(const char* message) {
#if !TARGET_OS_EXCLAVEKIT
    // FIXME: Clean this up once we have globally visible memory managers
    if (!sExternallyViewableState) {
        return;
    }
    MemoryManager::withWritableMemory([&] {
        static bool sAlreadyTerminating = false;
        if (!sAlreadyTerminating) {
            sAlreadyTerminating = true;
            // TODO: Stash the message in the atlas. None of the old APIs allow access to it, so let's do it later
            sExternallyViewableState->setDyldState(dyld_process_state_dyld_terminated);
        }
    });
#endif /* !TARGET_OS_EXCLAVEKIT */
}


} // namespace dyld4

#endif // HAS_EXTERNAL_STATE
