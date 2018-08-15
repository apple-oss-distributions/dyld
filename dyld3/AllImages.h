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


#ifndef __ALL_IMAGES_H__
#define __ALL_IMAGES_H__

#include <pthread.h>
#include <mach-o/loader.h>

#include "dyld_priv.h"

#include "LaunchCache.h"
#include "Loading.h"



#if __MAC_OS_X_VERSION_MIN_REQUIRED
// only in macOS and deprecated 
struct VIS_HIDDEN __NSObjectFileImage
{
    const char*                                         path        = nullptr;
    const void*                                         memSource   = nullptr;
    size_t                                              memLength   = 0;
    const mach_header*                                  loadAddress = nullptr;
    const dyld3::launch_cache::binary_format::Image*    binImage    = nullptr;
};
#endif

namespace dyld3 {

class VIS_HIDDEN AllImages
{
public:
    typedef launch_cache::binary_format::Closure    BinaryClosure;
    typedef launch_cache::binary_format::ImageGroup BinaryImageGroup;
    typedef launch_cache::binary_format::Image      BinaryImage;
    typedef launch_cache::ImageGroupList            ImageGroupList;
    typedef void                                    (*NotifyFunc)(const mach_header* mh, intptr_t slide);

    void                        init(const BinaryClosure* closure, const void* dyldCacheLoadAddress, const char* dyldCachePath,
                                     const dyld3::launch_cache::DynArray<loader::ImageInfo>& initialImages);
    void                        setMainPath(const char* path);
    void                        applyInitialImages();

    void                        addImages(const dyld3::launch_cache::DynArray<loader::ImageInfo>& newImages);
    void                        removeImages(const launch_cache::DynArray<loader::ImageInfo>& unloadImages);
    void                        setNeverUnload(const loader::ImageInfo& existingImage);
    void                        applyInterposingToDyldCache(const launch_cache::binary_format::Closure* closure, const dyld3::launch_cache::DynArray<loader::ImageInfo>& initialImages);
    void                        runInitialzersBottomUp(const mach_header* imageLoadAddress);
    void                        setInitialGroups();

    uint32_t                    count() const;
    const BinaryImageGroup*     cachedDylibsGroup();
    const BinaryImageGroup*     otherDylibsGroup();
    const BinaryImageGroup*     mainClosureGroup();
    const BinaryClosure*        mainClosure() { return _mainClosure; }
    uint32_t                    currentGroupsCount() const;
    void                        copyCurrentGroups(ImageGroupList& groups) const;

    const BinaryImage*          messageClosured(const char* path, const char* apiName, const char* closuredErrorMessages[3], int& closuredErrorMessagesCount);

    launch_cache::Image         findByLoadOrder(uint32_t index, const mach_header** loadAddress) const;
    launch_cache::Image         findByLoadAddress(const mach_header* loadAddress) const;
    launch_cache::Image         findByOwnedAddress(const void* addr, const mach_header** loadAddress, uint8_t* permissions=nullptr) const;
    const mach_header*          findLoadAddressByImage(const BinaryImage*) const;
    bool                        findIndexForLoadAddress(const mach_header* loadAddress, uint32_t& index);
    void                        forEachImage(void (^handler)(uint32_t imageIndex, const mach_header* loadAddress, const launch_cache::Image image, bool& stop)) const;

    const mach_header*          mainExecutable() const;
    launch_cache::Image         mainExecutableImage() const;
    const void*                 cacheLoadAddress() const { return _dyldCacheAddress; }
    const char*                 dyldCachePath() const { return _dyldCachePath; }
    const char*                 imagePath(const BinaryImage*) const;

    const mach_header*          alreadyLoaded(const char* path, bool bumpRefCount);
    const mach_header*          alreadyLoaded(const BinaryImage*, bool bumpRefCount);
    const mach_header*          alreadyLoaded(uint64_t inode, uint64_t mtime, bool bumpRefCount);
    const BinaryImage*          findImageInKnownGroups(const char* path);

    bool                        imageUnloadable(const launch_cache::Image& image, const mach_header* loadAddress) const;
    void                        incRefCount(const mach_header* loadAddress);
    void                        decRefCount(const mach_header* loadAddress);

    void                        addLoadNotifier(NotifyFunc);
    void                        addUnloadNotifier(NotifyFunc);
    void                        setObjCNotifiers(_dyld_objc_notify_mapped, _dyld_objc_notify_init, _dyld_objc_notify_unmapped);
    void                        notifyObjCUnmap(const char* path, const struct mach_header* mh);

    void                        runLibSystemInitializer(const mach_header* imageLoadAddress, const launch_cache::binary_format::Image* binImage);

    void                        setOldAllImageInfo(dyld_all_image_infos* old) { _oldAllImageInfos = old; }
    dyld_all_image_infos*       oldAllImageInfo() const { return _oldAllImageInfos;}

    void                        notifyMonitorMain();
    void                        notifyMonitorLoads(const launch_cache::DynArray<loader::ImageInfo>& newImages);
    void                        notifyMonitorUnloads(const launch_cache::DynArray<loader::ImageInfo>& unloadingImages);

#if __MAC_OS_X_VERSION_MIN_REQUIRED
    __NSObjectFileImage*        addNSObjectFileImage();
    bool                        hasNSObjectFileImage(__NSObjectFileImage*);
    void                        removeNSObjectFileImage(__NSObjectFileImage*);
#endif

    struct ProgramVars
    {
        const void*        mh;
        int*               NXArgcPtr;
        const char***      NXArgvPtr;
        const char***      environPtr;
        const char**       __prognamePtr;
    };
    void                    setProgramVars(ProgramVars* vars);

private:

    typedef void (*Initializer)(int argc, const char* argv[], char* envp[], const char* apple[], const ProgramVars* vars);
    typedef const launch_cache::DynArray<loader::ImageInfo> StartImageArray;
    
    void                        runInitialzersInImage(const mach_header* imageLoadAddress, const launch_cache::binary_format::Image* binImage);
    void                        mirrorToOldAllImageInfos();
    void                        garbageCollectImages();
    void                        vmAccountingSetSuspended(bool suspend);
    void                        copyCurrentGroupsNoLock(ImageGroupList& groups) const;

    const BinaryClosure*                    _mainClosure         = nullptr;
    const void*                             _dyldCacheAddress    = nullptr;
    const char*                             _dyldCachePath       = nullptr;
    uint64_t                                _dyldCacheSlide      = 0;
    StartImageArray*                        _initialImages       = nullptr;
    const char*                             _mainExeOverridePath = nullptr;
    _dyld_objc_notify_mapped                _objcNotifyMapped    = nullptr;
    _dyld_objc_notify_init                  _objcNotifyInit      = nullptr;
    _dyld_objc_notify_unmapped              _objcNotifyUnmapped  = nullptr;
    ProgramVars*                            _programVars         = nullptr;
    dyld_all_image_infos*                   _oldAllImageInfos    = nullptr;
    dyld_image_info*                        _oldAllImageArray    = nullptr;
    uint32_t                                _oldArrayAllocCount  = 0;
    pthread_mutex_t                         _initializerLock     = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    pthread_cond_t                          _initializerCondition= PTHREAD_COND_INITIALIZER;
    int32_t                                 _gcCount             = 0;
};

extern AllImages gAllImages;


} // dyld3


#endif // __ALL_IMAGES_H__
