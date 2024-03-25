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

  #include "FileManager.h"
  #include "ProcessAtlas.h"
  #include "RemoteNotificationResponder.h"
#endif // !TARGET_OS_EXCLAVEKIT

#include <mach-o/dyld_images.h>
#include <stdint.h>
#include "dyld_process_info.h"
#include "DyldProcessConfig.h"
#include "Tracing.h"

#include "ExternallyViewableState.h"

#if !TARGET_OS_EXCLAVEKIT
using dyld4::Atlas::SharedCache;
using dyld4::Atlas::Image;
using dyld4::Atlas::ProcessSnapshot;
#endif // !TARGET_OS_EXCLAVEKIT

using lsl::Allocator;
using dyld3::Platform;

// lldb sets a break point on this function
extern "C" void _dyld_debugger_notification(enum dyld_notify_mode mode, unsigned long count, uint64_t machHeaders[]);

// historical function in dyld_all_image_info.notification
extern "C" void lldb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]);

extern struct mach_header __dso_handle;

#define STR(s) # s
#define XSTR(s) STR(s)

#if defined(__cplusplus) && (BUILDING_LIBDYLD || BUILDING_DYLD)
    #define MAYBE_ATOMIC(x) {x}
#else
    #define MAYBE_ATOMIC(x)  x
#endif


#if !TARGET_OS_SIMULATOR
void lldb_image_notifier(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[])
{
#if BUILDING_DYLD
    dyld3::ScopedTimer timer(DBG_DYLD_GDB_IMAGE_NOTIFIER, 0, 0, 0);
    uint64_t machHeaders[infoCount];
    for (uint32_t i=0; i < infoCount; ++i) {
        machHeaders[i] = (uintptr_t)(info[i].imageLoadAddress);
    }
    switch ( mode ) {
         case dyld_image_adding:
            _dyld_debugger_notification(dyld_notify_adding, infoCount, machHeaders);
            break;
         case dyld_image_removing:
            _dyld_debugger_notification(dyld_notify_removing, infoCount, machHeaders);
            break;
         case dyld_image_dyld_moved:
            _dyld_debugger_notification(dyld_notify_dyld_moved, infoCount, machHeaders);
            break;
        default:
            break;
    }
#endif //  BUILDING_DYLD
}


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

#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT
static FileRecord recordFromInfo(lsl::Allocator& ephemeralAllocator, FileManager& fileManager, const ExternallyViewableState::ImageInfo& info)
{
    FileRecord record;
    if ( info.fsID && info.fsObjID ) {
        record = fileManager.fileRecordForVolumeDevIDAndObjID(info.fsID, info.fsObjID);
        if ( !record.volume().empty() )
            return record;
    }
    return fileManager.fileRecordForPath(ephemeralAllocator, info.path);
}
#endif // !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT

#if TARGET_OS_SIMULATOR
void ExternallyViewableState::initSim(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator,
                                      dyld3::Platform platform, dyld_all_image_infos* hostAllImage, const dyld::SyscallHelpers* syscalls)
{
    // make old all_image_infos, using all_image_info from host dyld
    _allImageInfo = hostAllImage;
    _allImageInfo->platform = (uint32_t)platform;
    _imageInfos     = Vector<dyld_image_info>::make(persistentAllocator);
    _imageUUIDs     = Vector<dyld_uuid_info>::make(persistentAllocator);

    // Copy images from host
    for (uint32_t i = 0; i < _allImageInfo->infoArrayCount; i++) {
        _imageInfos->push_back({_allImageInfo->infoArray[i].imageLoadAddress,
            _allImageInfo->infoArray[i].imageFilePath,
            _allImageInfo->infoArray[i].imageFileModDate});
    }
    _allImageInfo->infoArrayCount           = (uint32_t)_imageInfos->size();
    _allImageInfo->infoArrayChangeTimestamp = mach_absolute_time();
    _allImageInfo->infoArray                = _imageInfos->begin();

    // Copy uuids from host
    for (uint32_t i = 0; i < _allImageInfo->uuidArrayCount; i++) {
        dyld_uuid_info uuidInfo;
        uuidInfo.imageLoadAddress = _allImageInfo->uuidArray[i].imageLoadAddress;
        memcpy(uuidInfo.imageUUID, _allImageInfo->uuidArray[i].imageUUID, 16);
        _imageUUIDs->push_back(uuidInfo);
    }
    _allImageInfo->uuidArrayCount = (uint32_t)_imageUUIDs->size();
    _allImageInfo->uuidArray      = _imageUUIDs->begin();

    // We are leaking the host's _imageInfos and _imageUUIDs from ExternallyViewableState::init

    _syscallHelpers = syscalls;
}
#else

void ExternallyViewableState::initOld(lsl::Allocator& longTermAllocator, Platform platform)
{
    _allImageInfo = sProcessInfo;
    _allImageInfo->platform = (uint32_t)platform;
    _imageInfos    = lsl::Vector<dyld_image_info>::make(longTermAllocator);
    _imageUUIDs    = lsl::Vector<dyld_uuid_info>::make(longTermAllocator);
}
#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::init(lsl::Allocator& longTermAllocator, lsl::Allocator& ephemeralAllocator, FileManager& fileManager, Platform platform)
{
    // make compact info handler
    _fileManager   = &fileManager;
    _snapshot      = ephemeralAllocator.makeUnique<ProcessSnapshot>(ephemeralAllocator, fileManager, true).release();

    // set initial state in compact info
    _snapshot->setDyldState(dyld_process_state_dyld_initialized);
    _snapshot->setPlatform((uint32_t)platform);

    // make old all_image_infos
    initOld(longTermAllocator, platform);
#if SUPPORT_ROSETTA
    _aotImageInfos = Vector<dyld_aot_image_info>::make(longTermAllocator);
#endif
}

void ExternallyViewableState::addImageInfo(Allocator& ephemeralAllocator, const ImageInfo& imageInfo)
{
    FileRecord imageFile = recordFromInfo(ephemeralAllocator, *_fileManager, imageInfo);
    Image image(ephemeralAllocator, std::move(imageFile), _snapshot->identityMapper(), (const mach_header*)imageInfo.loadAddress);
    _snapshot->addImage(std::move(image));

    // add image to old all_image_infos
    addImageInfoOld(imageInfo, mach_absolute_time(), (uintptr_t)image.file().mtime());

    // if some other process is monitoring this one, notify it
    if ( this->notifyMonitorNeeded() ) {
        const mach_header* mhs[]   = { (mach_header*)imageInfo.loadAddress };
        const char*        paths[] = { imageInfo.path };
        this->notifyMonitorOfImageListChanges(false, 1, &mhs[0], &paths[0]);
    }
}
#endif // !TARGET_OS_EXCLAVEKIT
#endif // TARGET_OS_SIMULATOR

void ExternallyViewableState::addImageInfoOld(const ImageInfo& imageInfo, uint64_t timeStamp, uintptr_t mTime)
{
    _allImageInfo->infoArray = nullptr;   // set infoArray to NULL to denote it is in-use
    _imageInfos->push_back({(mach_header*)(long)imageInfo.loadAddress, imageInfo.path, mTime});
    _allImageInfo->infoArrayCount           = (uint32_t)_imageInfos->size();
    _allImageInfo->infoArrayChangeTimestamp = timeStamp;
    _allImageInfo->infoArray = _imageInfos->begin();

    // add image uuid to list of non-cached images
    this->addImageUUID((MachOFile*)imageInfo.loadAddress);

    // now if lldb is attached, let it know the image list changed
    _allImageInfo->notification(dyld_image_adding, 1, &_imageInfos->back());
}


void ExternallyViewableState::setDyldOld(const ImageInfo& dyldInfo)
{
    _allImageInfo->dyldPath = dyldInfo.path;
    const dyld3::MachOFile* dyldMF = (const dyld3::MachOFile*)dyldInfo.loadAddress;
    if ( !dyldMF->inDyldCache() )
        this->addImageUUID(dyldMF);
}

#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::setDyld(Allocator& ephemeralAllocator, const ImageInfo& dyldInfo)
{
#if !TARGET_OS_SIMULATOR
    const MachOFile* dyldMF = (const MachOFile*)dyldInfo.loadAddress;
    this->ensureSnapshot(ephemeralAllocator);
    if ( dyldMF->inDyldCache() && _snapshot->sharedCache() ) {
        _snapshot->addSharedCacheImage(dyldMF);
    }
    else {
        FileRecord dyldFile = recordFromInfo(ephemeralAllocator, *_fileManager, dyldInfo);
        Image dyldImage(ephemeralAllocator, std::move(dyldFile), _snapshot->identityMapper(), (const mach_header*)dyldInfo.loadAddress);
        _snapshot->addImage(std::move(dyldImage));
    }
#endif /* !TARGET_OS_SIMULATOR */
    // update dyld info in old all_image_infos
    setDyldOld(dyldInfo);
}
#endif // !TARGET_OS_EXCLAVEKIT


void ExternallyViewableState::setLibSystemInitializedOld()
{
    _allImageInfo->libSystemInitialized = true;
}
#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::setLibSystemInitialized()
{
    if ( _snapshot != nullptr )
        _snapshot->setDyldState(dyld_process_state_libSystem_initialized);

    // set old all_image_info
    setLibSystemInitializedOld();
}
#endif // !TARGET_OS_EXCLAVEKIT

void ExternallyViewableState::setInitialImageCountOld(uint32_t count)
{
    _allImageInfo->initialImageCount = count;
    _imageInfos->resize(count);
}
#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::setInitialImageCount(uint32_t count)
{
#if !TARGET_OS_SIMULATOR
    _snapshot->setInitialImageCount(count);
#endif /* !TARGET_OS_SIMULATOR */

    // update old all_image_infos with how many images are loaded before initializers run
    setInitialImageCountOld(count);
}
#endif // !TARGET_OS_EXCLAVEKIT

void ExternallyViewableState::addImageUUID(const dyld3::MachOFile* mf)
{
    dyld_uuid_info uuidAndAddr;
    uuidAndAddr.imageLoadAddress = mf;
    mf->getUuid(uuidAndAddr.imageUUID);
    _allImageInfo->uuidArray = nullptr;  // set uuidArray to NULL to denote it is in-use
        _imageUUIDs->push_back(uuidAndAddr);
        _allImageInfo->uuidArrayCount = (uintptr_t)_imageUUIDs->size();
    _allImageInfo->uuidArray = _imageUUIDs->begin();
}

void ExternallyViewableState::addImagesOld(lsl::Vector<dyld_image_info>& oldStyleAdditions, uint64_t timeStamp)
{
    // append old style additions to all image infos array
    _allImageInfo->infoArray = nullptr;   // set infoArray to NULL to denote it is in-use
    _imageInfos->insert(_imageInfos->begin(), oldStyleAdditions.begin(), oldStyleAdditions.end());
    _allImageInfo->infoArrayCount           = (uint32_t)_imageInfos->size();
    _allImageInfo->infoArrayChangeTimestamp = timeStamp;
    _allImageInfo->infoArray = _imageInfos->begin();

    // now if lldb is attached, let it know the image list changed
    _allImageInfo->notification(dyld_image_adding, (uint32_t)oldStyleAdditions.size(), &oldStyleAdditions[0]);
}

void ExternallyViewableState::addImagesOld(lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& imageInfos)
{
    lsl::Vector<dyld_image_info> oldStyleAdditions(ephemeralAllocator);
    for (const ImageInfo& imageInfo : imageInfos) {
        //const dyld3::MachOFile* mf = (const dyld3::MachOFile*)imageInfo.loadAddress;
        //fprintf(stderr, "ExternallyViewableState::addImages(): mh=%p, path=%s\n", mf, imageInfo.path);

        oldStyleAdditions.push_back({(mach_header*)imageInfo.loadAddress, imageInfo.path, 0});
        if ( !imageInfo.inSharedCache ) {
            this->addImageUUID((dyld3::MachOFile*)imageInfo.loadAddress);
        }
    }

    // append old style additions to all image infos array
    addImagesOld(oldStyleAdditions, 0);
}

#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::addImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& imageInfos)
{
    lsl::Vector<dyld_image_info> oldStyleAdditions(ephemeralAllocator);
#if !TARGET_OS_SIMULATOR
    os_unfair_lock_lock(&_processSnapshotLock);

    // append each new image to the current snapshot and build an ephemeral vector of old style additions
    this->ensureSnapshot(ephemeralAllocator);
#endif // !TARGET_OS_SIMULATOR
    for (const ImageInfo& imageInfo : imageInfos) {
        const MachOFile* mf = (const MachOFile*)imageInfo.loadAddress;
        //fprintf(stderr, "ExternallyViewableState::addImages(): mh=%p, path=%s\n", mf, imageInfo.path);
        if ( imageInfo.inSharedCache ) {
            oldStyleAdditions.push_back({(mach_header*)mf, imageInfo.path, 0});
#if !TARGET_OS_SIMULATOR
            if ( _snapshot->sharedCache() ) {
                _snapshot->addSharedCacheImage(mf);
            }
#endif // !TARGET_OS_SIMULATOR
        }
        else {
            oldStyleAdditions.push_back({(mach_header*)mf, imageInfo.path, 0});
            this->addImageUUID(mf);
#if !TARGET_OS_SIMULATOR
            FileRecord file = recordFromInfo(ephemeralAllocator, *_fileManager, imageInfo);
            Image anImage(ephemeralAllocator, std::move(file), _snapshot->identityMapper(), mf);
            _snapshot->addImage(std::move(anImage));
#endif // !TARGET_OS_SIMULATOR
        }
    }
#if !TARGET_OS_SIMULATOR
    this->commit(persistentAllocator, ephemeralAllocator);
#endif // !TARGET_OS_SIMULATOR

    addImagesOld(oldStyleAdditions, mach_absolute_time());

    // if some other process is monitoring this one, notify it
    if ( this->notifyMonitorNeeded() ) {
        // notify any other processing inspecting this one
        // notify any processes tracking loads in this process
        STACK_ALLOC_ARRAY(const char*, pathsBuffer, imageInfos.size());
        STACK_ALLOC_ARRAY(const mach_header*, mhBuffer, imageInfos.size());
        for ( const ImageInfo& info :  imageInfos ) {
            pathsBuffer.push_back(info.path);
            mhBuffer.push_back((mach_header*)info.loadAddress);
        }
        this->notifyMonitorOfImageListChanges(false, (unsigned int)imageInfos.size(), &mhBuffer[0], &pathsBuffer[0]);
    }
#if !TARGET_OS_SIMULATOR
    os_unfair_lock_unlock(&_processSnapshotLock);
#endif // !TARGET_OS_SIMULATOR
}
#endif // !TARGET_OS_EXCLAVEKIT

void ExternallyViewableState::removeImagesOld(dyld3::Array<const char*> &pathsBuffer, dyld3::Array<const mach_header*>& unloadedMHs, std::span<const mach_header*>& mhs, uint64_t timeStamp)
{
    _allImageInfo->infoArray = nullptr;  // set infoArray to NULL to denote it is in-use
    _allImageInfo->uuidArray = nullptr;  // set uuidArray to NULL to denote it is in-use
    for (const mach_header* mh : mhs) {
        dyld_image_info goingAway;

        // remove image from infoArray
        for (auto it=_imageInfos->begin(); it != _imageInfos->end(); ++it) {
            if ( it->imageLoadAddress == mh ) {
                pathsBuffer.push_back(it->imageFilePath);
                unloadedMHs.push_back(mh);
                goingAway = *it;
                _imageInfos->erase(it);
                break;
            }
        }

        // remove image from uuidArray
        for (auto it=_imageUUIDs->begin(); it != _imageUUIDs->end(); ++it) {
            if ( it->imageLoadAddress == mh ) {
                _imageUUIDs->erase(it);
                break;
            }
        }

        // now if lldb is attached, let it know this image has gone away
        _allImageInfo->notification(dyld_image_removing, 1, &goingAway);
    }
    _allImageInfo->infoArrayCount = (uint32_t)_imageInfos->size();
    _allImageInfo->infoArray      = _imageInfos->begin();  // set infoArray back to base address of vector
    _allImageInfo->infoArrayChangeTimestamp = timeStamp;
    _allImageInfo->uuidArrayCount = (uintptr_t)_imageUUIDs->size();
    _allImageInfo->uuidArray      = _imageUUIDs->begin();  // set uuidArray back to base address of vector
}

void ExternallyViewableState::removeImagesOld(std::span<const mach_header*>& mhs)
{
    // remove from old style list
    STACK_ALLOC_ARRAY(const char*, pathsBuffer, mhs.size());
    STACK_ALLOC_ARRAY(const mach_header*, unloadedMHs, mhs.size());
    removeImagesOld(pathsBuffer, unloadedMHs, mhs, 0);
}

#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::removeImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, std::span<const mach_header*>& mhs)
{
#if !TARGET_OS_SIMULATOR
    os_unfair_lock_lock(&_processSnapshotLock);
    this->ensureSnapshot(ephemeralAllocator);

    // remove images from snapshot
    for (const mach_header* mh : mhs) {
        //fprintf(stderr, "ExternallyViewableState::removeImages(): mh=%p\n", mh);
        _snapshot->removeImageAtAddress((unsigned long)mh);
    }
    this->commit(persistentAllocator, ephemeralAllocator);
#endif // !TARGET_OS_SIMULATOR

    // remove from old style list
    STACK_ALLOC_ARRAY(const char*, pathsBuffer, mhs.size());
    STACK_ALLOC_ARRAY(const mach_header*, unloadedMHs, mhs.size());
    removeImagesOld(pathsBuffer, unloadedMHs, mhs, mach_absolute_time());

    // if there are any changes and some other process is monitoring this one, notify it
    if ( this->notifyMonitorNeeded() && !unloadedMHs.empty()) {
        this->notifyMonitorOfImageListChanges(true, (unsigned int)mhs.size(), &unloadedMHs[0], &pathsBuffer[0]);
    }
#if !TARGET_OS_SIMULATOR
    os_unfair_lock_unlock(&_processSnapshotLock);
#endif // !TARGET_OS_SIMULATOR
}
#endif // !TARGET_OS_EXCLAVEKIT

#if !TARGET_OS_EXCLAVEKIT
void ExternallyViewableState::setSharedCacheInfo(Allocator& ephemeralAllocator, uint64_t cacheSlide, const ImageInfo& cacheInfo, bool privateCache)
{
#if !TARGET_OS_SIMULATOR
    FileRecord cacheFileRecord = recordFromInfo(ephemeralAllocator, *_fileManager, cacheInfo);
    if ( cacheFileRecord.exists() ) {
        SharedCache sharedCache(ephemeralAllocator, std::move(cacheFileRecord), _snapshot->identityMapper(), (uint64_t)cacheInfo.loadAddress, privateCache);
        _snapshot->addSharedCache(std::move(sharedCache));
    }
#endif /* !TARGET_OS_SIMULATOR */
    // update cache info in old all_image_infos
    _allImageInfo->sharedCacheSlide                = (uintptr_t)cacheSlide;
    _allImageInfo->sharedCacheBaseAddress          = (uintptr_t)cacheInfo.loadAddress;
    _allImageInfo->sharedCacheFSID                 = cacheInfo.fsID;
    _allImageInfo->sharedCacheFSObjID              = cacheInfo.fsObjID;
    _allImageInfo->processDetachedFromSharedRegion = privateCache;
    if ( const DyldSharedCache* dyldCache = (DyldSharedCache*)cacheInfo.loadAddress )
        dyldCache->getUUID(_allImageInfo->sharedCacheUUID);
}


// used by host dyld before calling into dyld_sim
void ExternallyViewableState::detachFromSharedRegion()
{
    _allImageInfo->processDetachedFromSharedRegion  = true;
    _allImageInfo->sharedCacheSlide                 = 0;
    _allImageInfo->sharedCacheBaseAddress           = 0;
    ::bzero(_allImageInfo->sharedCacheUUID,sizeof(uuid_t));
}

void ExternallyViewableState::release(Allocator& ephemeralAllocator)
{
#if BUILDING_DYLD && !__i386__
    // release ProcessSnapshot object
    if (_snapshot) {
        _snapshot->~ProcessSnapshot();
        ephemeralAllocator.free((void*)_snapshot);
        _snapshot = nullptr;
    }
#endif
}


void ExternallyViewableState::commit(Atlas::ProcessSnapshot* processSnapshot, Allocator& persistentAllocator, Allocator& ephemeralAllocator)
{
#if BUILDING_DYLD && !TARGET_OS_SIMULATOR && !__i386__
    if ( !processSnapshot )
        return;
    // serialize compact info info a chunk in the persistentAllocator
    Vector<std::byte> compactInfoData = processSnapshot->serialize();
    std::byte*        compactInfo     = (std::byte*)persistentAllocator.malloc(compactInfoData.size());
    std::copy(compactInfoData.begin(), compactInfoData.end(), &compactInfo[0]);

    // atomically update compactInfo addr/size in all_image_infos
    struct CompactInfoDescriptor {
        uintptr_t   addr;
        size_t      size;
    } __attribute__((aligned(16)));
    CompactInfoDescriptor newDescriptor;
    newDescriptor.addr = (uintptr_t)compactInfo;
    newDescriptor.size = (size_t)compactInfoData.size();
    uintptr_t oldCompactInfo = _allImageInfo->compact_dyld_image_info_addr;
#if __arm__ || !__LP64__
    // armv32 archs are missing the atomic primitive, but we only need to be guaraantee the write does not sheer, as the only thing
    // accessing this outside of a lock is the kernel or a remote process
    uint64_t* currentDescriptor = (uint64_t*)&_allImageInfo->compact_dyld_image_info_addr;
    *currentDescriptor = *((uint64_t*)&newDescriptor);
#else
    // We do not need a compare and swap since we are under a lock, but we do need the updates to be atomic to out of process observers
    std::atomic<CompactInfoDescriptor>* currentDescriptor = (std::atomic<CompactInfoDescriptor>*)&_allImageInfo->compact_dyld_image_info_addr;
    currentDescriptor->store(newDescriptor, std::memory_order_relaxed);
#endif
    if ( oldCompactInfo != 0 ) {
        // This might be info setup by the dyld runtime state, and if so we don't know the tpro state.
        // If the oldCompactInfo is not owned by the persistentAllocator then purposefully leak it.
        auto allocationMetadata = lsl::AllocationMetadata::getForPointer((void*)oldCompactInfo);
        auto oldAllocator = &allocationMetadata->allocator();
        if (oldAllocator == &persistentAllocator) {
            persistentAllocator.free((void*)oldCompactInfo);
        }
    }

    {
        // TODO: Move timer events into the responder class
        // Use a scope to prevent compiler reordering timer
        dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
        RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
        responder.blockOnSynchronousEvent(DYLD_REMOTE_EVENT_ATLAS_CHANGED);
    }
#endif
}

void ExternallyViewableState::commit(Allocator& persistentAllocator, Allocator& ephemeralAllocator)
{
    this->commit(_snapshot, persistentAllocator, ephemeralAllocator);
    this->release(ephemeralAllocator);
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

void ExternallyViewableState::ensureSnapshot(lsl::Allocator& ephemeralAllocator)
{
    if ( _snapshot != nullptr )
        return;

    // if we are here, then we are in a dlopen/dlclose after the launch _snapshot was deleted
    // recreate the snapshot from the all_image_info bytes
    const std::span<std::byte> comactInfoBytes((std::byte*)_allImageInfo->compact_dyld_image_info_addr, _allImageInfo->compact_dyld_image_info_size);
    _snapshot = ephemeralAllocator.makeUnique<ProcessSnapshot>(ephemeralAllocator, *_fileManager, true, comactInfoBytes).release();
    _snapshot->setDyldState(dyld_process_state_program_running);
}

void ExternallyViewableState::fork_child()
{
    // If dyld is sending load/unload notices to CoreSymbolication, the shared memory
    // page is not copied on fork. <rdar://problem/6797342>
    _allImageInfo->coreSymbolicationShmPage = nullptr;
    // for safety, make sure child starts with clean systemOrderFlag
    _allImageInfo->systemOrderFlag = 0;
}

mach_port_t ExternallyViewableState::notifyPortValue()
{
#if TARGET_OS_SIMULATOR
    // simulator does not support quick check, so it calls through to host with image list changes and host checks for monitor
    return 0;
#else
    return _allImageInfo->notifyPorts[0];
#endif
}

uint64_t ExternallyViewableState::lastImageListUpdateTime()
{
    return _allImageInfo->infoArrayChangeTimestamp;
}

/* Since legacy simulators call notifyMonitorOfImageListChangesSim we can use it as a choke point to have the host dyld generate compact info
   for them. Since we don't have access to the runtime state here and we can't change the call signature need to just materialize
   everythign it needs here */
void ExternallyViewableState::notifyMonitorOfImageListChangesSim(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[])
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    // in practice, notifyMonitorOfImageListChangesSim is never called from the simulator
    if ( _syscallHelpers->version >= 11 )
        _syscallHelpers->notifyMonitorOfImageListChanges(unloading, imageCount, loadAddresses, imagePaths);
  #elif TARGET_OS_OSX
    static Allocator* glueAllocator = nullptr;
    if (!glueAllocator) {
        glueAllocator = &Allocator::persistentAllocator();
    }
    EphemeralAllocator ephemeralAllocator;
    SyscallDelegate syscall;
    FileManager fileManager(ephemeralAllocator, &syscall);
    auto* gProcessInfo = ExternallyViewableState::getProcessInfo();
    UniquePtr<ProcessSnapshot> processSnapshot;
    if (gProcessInfo->compact_dyld_image_info_addr) {
        const std::span<std::byte> data((std::byte*)gProcessInfo->compact_dyld_image_info_addr, gProcessInfo->compact_dyld_image_info_size);
        processSnapshot = ephemeralAllocator.makeUnique<ProcessSnapshot>(ephemeralAllocator, fileManager, true, data);
    } else {
        processSnapshot = ephemeralAllocator.makeUnique<ProcessSnapshot>(ephemeralAllocator, fileManager, true);
    }
    if ( processSnapshot) {
        if (unloading) {
            for (auto i = 0; i < imageCount; ++i) {
                processSnapshot->removeImageAtAddress((uint64_t)loadAddresses[i]);
            }
        } else {
            for (auto i = 0; i < imageCount; ++i) {
                const struct mach_header* mh =  loadAddresses[i];
                if (mh->flags & MH_DYLIB_IN_CACHE) {
                    if (!processSnapshot->sharedCache()) {
                        // Process snapshot has no shared cache, but this is
                        // `MH_DYLIB_IN_CACHE` so shared cache should have been loaded.
                        if (gProcessInfo->sharedCacheFSID && gProcessInfo->sharedCacheFSObjID) {
                            auto sharedCache = Atlas::SharedCache(
                                                                  ephemeralAllocator,
                                                                  fileManager.fileRecordForVolumeDevIDAndObjID(
                                                                                                               gProcessInfo->sharedCacheFSID,
                                                                                                               gProcessInfo->sharedCacheFSObjID),
                                                                  processSnapshot->identityMapper(), gProcessInfo->sharedCacheBaseAddress, true);
                            processSnapshot->addSharedCache(std::move(sharedCache));
                        } else {
                            halt("notifyMonitorOfImageListChangesSim() tried to add a "
                                 "shared cache image, but there's no information about "
                                 "the shared cache location");
                        }
                    }
                    processSnapshot->addSharedCacheImage(mh);
                } else {
                    uuid_t rawUUID;
                    lsl::UUID machoUUID;
                    if (((MachOFile*)mh)->getUuid(rawUUID)) {
                        machoUUID = rawUUID;
                    }
                    auto file = fileManager.fileRecordForPath(ephemeralAllocator, imagePaths[i]);
                    auto image = dyld4::Atlas::Image(ephemeralAllocator, std::move(file), processSnapshot->identityMapper(), mh);
                    processSnapshot->addImage(std::move(image));
                }
            }
        }
        processSnapshot.withUnsafe([&](ProcessSnapshot* snapshot) {
            this->commit(snapshot, *glueAllocator, ephemeralAllocator);
        });
    }
    this->notifyMonitorOfImageListChanges(unloading, imageCount, loadAddresses, imagePaths);
  #endif // TARGET_OS_SIMULATOR
#endif // BUILDING_DYLD
}

void ExternallyViewableState::coresymbolication_load_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh)
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 4 )
        _syscallHelpers->coresymbolication_load_notifier(connection, timestamp, path, mh);
  #else
    const mach_header* loadAddress[] = { mh };
    const char*        loadPath[]    = { path };
    this->notifyMonitorOfImageListChangesSim(false, 1, loadAddress, loadPath);
  #endif // TARGET_OS_SIMULATOR
#endif // BUILDING_DYLD
}

void ExternallyViewableState::coresymbolication_unload_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh)
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 4 )
        _syscallHelpers->coresymbolication_unload_notifier(connection, timestamp, path, mh);
  #else
    const mach_header* loadAddress = { mh };
    const char*        loadPath    = { path };
    this->notifyMonitorOfImageListChangesSim(true, 1, &loadAddress, &loadPath);
  #endif // TARGET_OS_SIMULATOR
#endif // BUILDING_DYLD
}

bool ExternallyViewableState::notifyMonitorNeeded()
{
#if TARGET_OS_SIMULATOR
    // dyld_sim cannot tell if being monitored, so always call through to host dyld
    return true;
#else
    return (_allImageInfo->notifyPorts[0] == RemoteNotificationResponder::DYLD_PROCESS_INFO_NOTIFY_MAGIC);
#endif
}

void ExternallyViewableState::notifyMonitorOfImageListChanges(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[])
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 11 )
        _syscallHelpers->notifyMonitorOfImageListChanges(unloading, imageCount, loadAddresses, imagePaths);
  #else
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfImageListChanges(unloading, imageCount, loadAddresses, imagePaths, this->lastImageListUpdateTime());
  #endif // TARGET_OS_SIMULATOR
#endif // BUILDING_DYLD
}

void ExternallyViewableState::notifyMonitorOfMainCalled()
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 17 )
        _syscallHelpers->notifyMonitorOfMainCalled();
  #else
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfMainCalled();
  #endif /* TARGET_OS_SIMULATOR */
#endif /* BUILDING_DYLD */
}

void ExternallyViewableState::notifyMonitorOfDyldBeforeInitializers()
{
#if BUILDING_DYLD
  #if TARGET_OS_SIMULATOR
    // notifications are tied to macOS kernel, so dyld_sim cannot send them, it must route through host dyld
    if ( _syscallHelpers->version >= 17 )
        _syscallHelpers->notifyMonitorOfDyldBeforeInitializers();
  #else
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
    RemoteNotificationResponder responder(_allImageInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfDyldBeforeInitializers();
  #endif /* TARGET_OS_SIMULATOR */
#endif /* BUILDING_DYLD */
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
        _allImageInfo->aotInfoArray = _aotImageInfos->begin();   // set aotInfoArray back to base address of vector (other process can now read)
    }

    if ( image_infos.size() != 0 ) {
        // append dyld_image_info to all image infos array
        _allImageInfo->infoArray = nullptr;  // set infoArray to NULL to denote it is in-use
            _imageInfos->insert(_imageInfos->begin(), image_infos.begin(), image_infos.end());
            _allImageInfo->infoArrayCount = (uint32_t)_imageInfos->size();
            _allImageInfo->infoArrayChangeTimestamp = mach_absolute_time();
        _allImageInfo->infoArray = _imageInfos->begin();   // set infoArray back to base address of vector (other process can now read)
    }
}

void ExternallyViewableState::removeRosettaImages(std::span<const mach_header*>& mhs)
{
    // set aotInfoArray to NULL to denote it is in-use
    _allImageInfo->aotInfoArray = nullptr;

    for (const mach_header* mh : mhs) {
        // remove image from aotInfoArray
        for (auto it=_aotImageInfos->begin(); it != _aotImageInfos->end(); ++it) {
            if ( it->aotLoadAddress == mh ) {
                _aotImageInfos->erase(it);
                break;
            }
        }
    }
    _allImageInfo->aotInfoCount = (uint32_t)_aotImageInfos->size();
    _allImageInfo->aotInfoArrayChangeTimestamp  = mach_absolute_time();
    // set aotInfoArray back to base address of vector
    _allImageInfo->aotInfoArray = _aotImageInfos->begin();

}
#endif // SUPPORT_ROSETTA
#endif // !TARGET_OS_EXCLAVEKIT

#if !TARGET_OS_SIMULATOR
#if !TARGET_OS_EXCLAVEKIT
// Use the older notifiers to tell Instruments that we load or unloaded dyld
void ExternallyViewableState::notifyMonitoringDyld(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[])
{
#if BUILDING_DYLD
    dyld3::ScopedTimer timer(DBG_DYLD_REMOTE_IMAGE_NOTIFIER, 0, 0, 0);
    auto* gProcessInfo = ExternallyViewableState::getProcessInfo();
    RemoteNotificationResponder responder(gProcessInfo->notifyPorts[0]);
    if ( responder.active() )
        responder.notifyMonitorOfImageListChanges(unloading, imageCount, loadAddresses, imagePaths, gProcessInfo->infoArrayChangeTimestamp);
#endif // BUILDING_DYLD
}

// called from disk based dyld before jumping into dyld in the cache
void ExternallyViewableState::switchDyldLoadAddress(const dyld3::MachOFile* dyldInCacheMF)
{
    sProcessInfo->dyldImageLoadAddress = dyldInCacheMF;
}

// called from
bool ExternallyViewableState::switchToDyldInDyldCache(const dyld3::MachOFile* dyldInCacheMF)
{
    // in case dyld moved, switch the process info that kernel is using
    bool                      result = false;
    task_dyld_info_data_t     task_dyld_info;
    mach_msg_type_number_t    count           = TASK_DYLD_INFO_COUNT;
    if ( ::task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count) == KERN_SUCCESS) {
        struct dyld_all_image_infos* oldProcessInfo = (struct dyld_all_image_infos*)(long)(task_dyld_info.all_image_info_addr);
        if ( proc_set_dyld_all_image_info(&dyld_all_image_infos, sizeof(dyld_all_image_infos)) == 0 ) {
            // if the process is being monitored transfer over the port
            if ( oldProcessInfo->notifyPorts[0] != 0 )
                dyld_all_image_infos.notifyPorts[0] = oldProcessInfo->notifyPorts[0];
            // in case being debugged, tell lldb that dyld moved
            __typeof(&lldb_image_notifier) prevNotifyLLDB = oldProcessInfo->notification;
            dyld_image_info info = { dyldInCacheMF, "/usr/lib/dyld", 0 };
            prevNotifyLLDB(dyld_image_dyld_moved, 1, &info);
            // we've switch the dyld_all_image_infos that the kernel knows about to the one in the dyld cache
            result = true;
        }
        else {
            // we were unable to switch dyld_all_image_infos, so set sProcessInfo so we use the one from dyld-on-disk
            sProcessInfo = oldProcessInfo;
            __typeof(&lldb_image_notifier) prevNotifyLLDB = oldProcessInfo->notification;
            // switch struct in disk-dyld to point to dyld in cache
            oldProcessInfo->dyldImageLoadAddress = dyldInCacheMF;
            oldProcessInfo->dyldPath             = "/usr/lib/dyld";
            oldProcessInfo->notification         = &lldb_image_notifier;
            oldProcessInfo->dyldVersion          = "cache";
            // in case being debugged, tell lldb that dyld moved
            dyld_image_info info = { dyldInCacheMF, "/usr/lib/dyld", 0 };
            prevNotifyLLDB(dyld_image_dyld_moved, 1, &info);
        }
    }
    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT

// used in macOS host dyld to support old dyld_sim that need access to host dyld_all_image_info
struct dyld_all_image_infos* ExternallyViewableState::getProcessInfo()
{
    return sProcessInfo;
}
#endif // !TARGET_OS_SIMULATOR

// use at start up to set value in __dyld4 section
void ExternallyViewableState::storeProcessInfoPointer(struct dyld_all_image_infos** loc)
{
    *loc = _allImageInfo;
}


} // namespace dyld4

#endif // HAS_EXTERNAL_STATE
