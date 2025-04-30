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

#ifndef _DYLD_PROCESS_INFO_INTERNAL_H_
#define _DYLD_PROCESS_INFO_INTERNAL_H_

#define VIS_HIDDEN __attribute__((visibility("hidden")))

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <uuid/uuid.h>
#include <TargetConditionals.h>
#include "Defines.h"
#if !TARGET_OS_EXCLAVEKIT
  #include <mach/task.h>
#endif


struct dyld_all_image_infos_32 {
    uint32_t                        version;
    uint32_t                        infoArrayCount;
    uint32_t                        infoArray;
    uint32_t                        notification;
    bool                            processDetachedFromSharedRegion;
    bool                            libSystemInitialized;
    uint32_t                        dyldImageLoadAddress;
    uint32_t                        jitInfo;
    uint32_t                        dyldVersion;
    uint32_t                        errorMessage;
    uint32_t                        terminationFlags;
    uint32_t                        coreSymbolicationShmPage;
    uint32_t                        systemOrderFlag;
    uint32_t                        uuidArrayCount;
    uint32_t                        uuidArray;
    uint32_t                        dyldAllImageInfosAddress;
    uint32_t                        initialImageCount;
    uint32_t                        errorKind;
    uint32_t                        errorClientOfDylibPath;
    uint32_t                        errorTargetDylibPath;
    uint32_t                        errorSymbol;
    uint32_t                        sharedCacheSlide;
    uint8_t                         sharedCacheUUID[16];
    uint32_t                        sharedCacheBaseAddress;
    uint64_t                        infoArrayChangeTimestamp;
    uint32_t                        dyldPath;
    uint32_t                        notifyMachPorts[8];
    uint32_t                        reserved;
    uint64_t                        sharedCacheFSID;
    uint64_t                        sharedCacheFSObjID;
    uint32_t                        compact_dyld_image_info_addr;
    uint32_t                        compact_dyld_image_info_size;
    uint32_t                        platform;
    // the aot fields below will not be set in the 32 bit case
    uint32_t                        aotInfoCount;
    uint64_t                        aotInfoArray;
    uint64_t                        aotInfoArrayChangeTimestamp;
    uint64_t                        aotSharedCacheBaseAddress;
    uint8_t                         aotSharedCacheUUID[16];
};

struct dyld_all_image_infos_64 {
    uint32_t                version;
    uint32_t                infoArrayCount;
    uint64_t                infoArray;
    uint64_t                notification;
    bool                    processDetachedFromSharedRegion;
    bool                    libSystemInitialized;
    uint32_t                paddingToMakeTheSizeCorrectOn32bitAndDoesntAffect64b; // NOT PART OF DYLD_ALL_IMAGE_INFOS!
    uint64_t                dyldImageLoadAddress;
    uint64_t                jitInfo;
    uint64_t                dyldVersion;
    uint64_t                errorMessage;
    uint64_t                terminationFlags;
    uint64_t                coreSymbolicationShmPage;
    uint64_t                systemOrderFlag;
    uint64_t                uuidArrayCount;
    uint64_t                uuidArray;
    uint64_t                dyldAllImageInfosAddress;
    uint64_t                initialImageCount;
    uint64_t                errorKind;
    uint64_t                errorClientOfDylibPath;
    uint64_t                errorTargetDylibPath;
    uint64_t                errorSymbol;
    uint64_t                sharedCacheSlide;
    uint8_t                 sharedCacheUUID[16];
    uint64_t                sharedCacheBaseAddress;
    uint64_t                infoArrayChangeTimestamp;
    uint64_t                dyldPath;
    uint32_t                notifyMachPorts[8];
    uint64_t                reserved[7];
    uint64_t                sharedCacheFSID;
    uint64_t                sharedCacheFSObjID;
    uint64_t                compact_dyld_image_info_addr;
    uint64_t                compact_dyld_image_info_size;
    uint32_t                platform;
    uint32_t                aotInfoCount;
    uint64_t                aotInfoArray;
    uint64_t                aotInfoArrayChangeTimestamp;
    uint64_t                aotSharedCacheBaseAddress;
    uint8_t                 aotSharedCacheUUID[16];
};

struct dyld_image_info_32 {
    uint32_t                    imageLoadAddress;
    uint32_t                    imageFilePath;
    uint32_t                    imageFileModDate;
};
struct dyld_image_info_64 {
    uint64_t                    imageLoadAddress;
    uint64_t                    imageFilePath;
    uint64_t                    imageFileModDate;
};

#define DYLD_AOT_IMAGE_KEY_SIZE 32
struct dyld_aot_image_info_64 {
    uint64_t                    x86LoadAddress;
    uint64_t                    aotLoadAddress;
    uint64_t                    aotImageSize;
    uint8_t                     aotImageKey[DYLD_AOT_IMAGE_KEY_SIZE];
};

//TODO: When we factor out libdyld_introspection we should define these in a private header shared between that and dyld.
#define DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE	(32*1024)
#define DYLD_PROCESS_INFO_NOTIFY_LOAD_ID			0x1000
#define DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID			0x2000
#define DYLD_PROCESS_INFO_NOTIFY_MAIN_ID			0x3000
#ifndef DYLD_PROCESS_EVENT_ID_BASE
#define DYLD_PROCESS_EVENT_ID_BASE                  0x4000
#endif
#ifndef DYLD_REMOTE_EVENT_MAIN
#define DYLD_REMOTE_EVENT_MAIN                      (1)
#endif
#ifndef DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED
#define DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED       (2)
#endif
#ifndef DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS
#define DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS  DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED
#endif

struct dyld_process_info_image_entry {
    uuid_t						uuid;
    uint64_t                    loadAddress;
    uint32_t                    pathStringOffset;
    uint32_t                    pathLength;
};

struct dyld_process_info_notify_header {
	mach_msg_header_t			header;
    uint32_t                    version;
    uint32_t                    imageCount;
    uint32_t                    imagesOffset;
    uint32_t                    stringsOffset;
    uint64_t                    timestamp;
};

#if __cplusplus
#include <tuple>
//FIXME: Refactor this out into a seperate file
struct VIS_HIDDEN RemoteBuffer {
    RemoteBuffer();
    RemoteBuffer(task_t task, mach_vm_address_t remote_address, size_t remote_size, bool allow_truncation);
    RemoteBuffer& operator=(RemoteBuffer&& other);
    ~RemoteBuffer();
    void *getLocalAddress() const;
    kern_return_t getKernelReturn() const;
    size_t getSize() const;
private:
    static std::pair<mach_vm_address_t, kern_return_t> map( task_t task, mach_vm_address_t remote_address, vm_size_t _size);
    static std::tuple<mach_vm_address_t,vm_size_t,kern_return_t>create(    task_t task,
                                                                                mach_vm_address_t remote_address,
                                                                                size_t remote_size,
                                                                                bool allow_truncation);
    RemoteBuffer(std::tuple<mach_vm_address_t,vm_size_t,kern_return_t> T);

    mach_vm_address_t _localAddress;
    vm_size_t _size;
    kern_return_t _kr;
};

// only called during libdyld set up
void setNotifyMonitoringDyldMain(void (*func)()) VIS_HIDDEN;
void setNotifyMonitoringDyld(void (*func)(bool unloading, unsigned imageCount,
                                          const struct mach_header* loadAddresses[],
                                          const char* imagePaths[])) VIS_HIDDEN;

void withRemoteBuffer(task_t task, mach_vm_address_t remote_address, size_t remote_size, bool allow_truncation, kern_return_t *kr, void (^block)(void *buffer, size_t size)) __attribute__((visibility("hidden")));

template<typename T>
VIS_HIDDEN void withRemoteObject(task_t task, mach_vm_address_t remote_address, kern_return_t *kr, void (^block)(T t))
{
    withRemoteBuffer(task, remote_address, sizeof(T), false, kr, ^(void *buffer, size_t size) {
        block(*reinterpret_cast<T *>(buffer));
    });
}
#endif /* __cplusplus */

#endif // _DYLD_PROCESS_INFO_INTERNAL_H_


