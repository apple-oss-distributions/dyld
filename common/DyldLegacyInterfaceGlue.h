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

#ifndef DyldLegacyInterfaceGlue_h
#define DyldLegacyInterfaceGlue_h

#include "Defines.h"

#if BUILDING_DYLD_FRAMEWORK
// We build all these functions in Dyld.framework, but the existing SPIs live libdyld
// In the long run we want to completely remove them, but no matter what we absolutely
// do not want anyone linking to them directly out of Dyld.framework. `libdyld.dylib
// accesses them via a pseudo-vtable, so the symbols do not need to be exported. We
// mark the visibility as hidden to prevent that.
#define GLUE_VISIBILITY VIS_HIDDEN
#else
// If this header is incidentally included in any other linked image we do not want
// it to image visibility, so do not alter it.
#define GLUE_VISIBILITY
#endif

typedef struct dyld_process_state_info dyld_process_state_info;
typedef struct dyld_process_cache_info dyld_process_cache_info;
typedef struct dyld_process_aot_cache_info dyld_process_aot_cache_info;
typedef const struct dyld_process_info_base* dyld_process_info;
typedef struct dyld_process_s*              dyld_process_t;
typedef struct dyld_process_snapshot_s*     dyld_process_snapshot_t;
typedef struct dyld_shared_cache_s*         dyld_shared_cache_t;
typedef struct dyld_image_s*                dyld_image_t;
typedef const struct dyld_process_info_notify_base* dyld_process_info_notify;

extern "C" GLUE_VISIBILITY dyld_process_t dyld_process_create_for_task(task_t task, kern_return_t *kr);
extern "C" GLUE_VISIBILITY dyld_process_t dyld_process_create_for_current_task();
extern "C" GLUE_VISIBILITY void dyld_process_dispose(dyld_process_t process);
extern "C" GLUE_VISIBILITY uint32_t dyld_process_register_for_image_notifications(dyld_process_t process, kern_return_t *kr, dispatch_queue_t queue, void (^block)(dyld_image_t image, bool load));
extern "C" GLUE_VISIBILITY uint32_t dyld_process_register_for_event_notification(dyld_process_t process, kern_return_t *kr, uint32_t event, dispatch_queue_t queue, void (^block)());
extern "C" GLUE_VISIBILITY void dyld_process_unregister_for_notification(dyld_process_t process, uint32_t handle);
extern "C" GLUE_VISIBILITY dyld_process_snapshot_t dyld_process_snapshot_create_for_process(dyld_process_t process, kern_return_t *kr);
extern "C" GLUE_VISIBILITY dyld_process_snapshot_t dyld_process_snapshot_create_from_data(void* buffer, size_t size, void* reserved1, size_t reserved2);
extern "C" GLUE_VISIBILITY void dyld_process_snapshot_dispose(dyld_process_snapshot_t snapshot);
extern "C" GLUE_VISIBILITY void dyld_process_snapshot_for_each_image(dyld_process_snapshot_t snapshot, void (^block)(dyld_image_t image));
extern "C" GLUE_VISIBILITY dyld_shared_cache_t dyld_process_snapshot_get_shared_cache(dyld_process_snapshot_t snapshot);
extern "C" GLUE_VISIBILITY bool dyld_shared_cache_pin_mapping(dyld_shared_cache_t cache);
extern "C" GLUE_VISIBILITY void dyld_shared_cache_unpin_mapping(dyld_shared_cache_t cache);
extern "C" GLUE_VISIBILITY uint64_t dyld_shared_cache_get_base_address(dyld_shared_cache_t cache);
extern "C" GLUE_VISIBILITY uint64_t dyld_shared_cache_get_mapped_size(dyld_shared_cache_t cache);
extern "C" GLUE_VISIBILITY bool dyld_shared_cache_is_mapped_private(dyld_shared_cache_t cache);
extern "C" GLUE_VISIBILITY void dyld_shared_cache_copy_uuid(dyld_shared_cache_t cache, uuid_t *uuid);
extern "C" GLUE_VISIBILITY void dyld_shared_cache_for_each_file(dyld_shared_cache_t cache, void (^block)(const char* file_path));
extern "C" GLUE_VISIBILITY void dyld_shared_cache_for_each_image(dyld_shared_cache_t cache, void (^block)(dyld_image_t image));
extern "C" GLUE_VISIBILITY void dyld_for_each_installed_shared_cache_with_system_path(const char* root_path, void (^block)(dyld_shared_cache_t atlas));
extern "C" GLUE_VISIBILITY void dyld_for_each_installed_shared_cache(void (^block)(dyld_shared_cache_t cache));
extern "C" GLUE_VISIBILITY bool dyld_shared_cache_for_file(const char* filePath, void (^block)(dyld_shared_cache_t cache));
extern "C" GLUE_VISIBILITY bool dyld_image_content_for_segment(dyld_image_t image, const char* segmentName, void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize));
extern "C" GLUE_VISIBILITY bool dyld_image_content_for_section(dyld_image_t image, const char* segmentName, const char* sectionName, void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize));
extern "C" GLUE_VISIBILITY bool dyld_image_copy_uuid(dyld_image_t image, uuid_t* uuid);
extern "C" GLUE_VISIBILITY bool dyld_image_for_each_segment_info(dyld_image_t image, void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm));
extern "C" GLUE_VISIBILITY bool dyld_image_for_each_section_info(dyld_image_t image, void (^block)(const char* segmentName, const char* sectionName, uint64_t vmAddr, uint64_t vmSize));
extern "C" GLUE_VISIBILITY const char* dyld_image_get_installname(dyld_image_t image);
extern "C" GLUE_VISIBILITY const char* dyld_image_get_file_path(dyld_image_t image);
extern "C" GLUE_VISIBILITY bool dyld_image_local_nlist_content_4Symbolication(dyld_image_t image, void (^contentReader)(const void* nListStart, uint64_t nListCount, const char* stringTable));


extern "C" GLUE_VISIBILITY dyld_process_info _dyld_process_info_create(task_t task, uint64_t timestamp, kern_return_t* kernelError);
extern "C" GLUE_VISIBILITY void _dyld_process_info_get_state(dyld_process_info info, dyld_process_state_info* stateInfo);
extern "C" GLUE_VISIBILITY void _dyld_process_info_get_cache(dyld_process_info info, dyld_process_cache_info* cacheInfo);
extern "C" GLUE_VISIBILITY void _dyld_process_info_get_aot_cache(dyld_process_info info, dyld_process_aot_cache_info* aotCacheInfo);
extern "C" GLUE_VISIBILITY void _dyld_process_info_retain(dyld_process_info object);
extern "C" GLUE_VISIBILITY dyld_platform_t _dyld_process_info_get_platform(dyld_process_info object);
extern "C" GLUE_VISIBILITY void _dyld_process_info_release(dyld_process_info object);
extern "C" GLUE_VISIBILITY void _dyld_process_info_for_each_image(dyld_process_info info, void (^callback)(uint64_t machHeaderAddress, const uuid_t uuid, const char* path));
#if TARGET_OS_OSX
extern "C" GLUE_VISIBILITY void _dyld_process_info_for_each_aot_image(dyld_process_info info, bool (^callback)(uint64_t x86Address, uint64_t aotAddress, uint64_t aotSize, uint8_t* aotImageKey, size_t aotImageKeySize));
#endif
extern "C" GLUE_VISIBILITY void _dyld_process_info_for_each_segment(dyld_process_info info, uint64_t machHeaderAddress, void (^callback)(uint64_t segmentAddress, uint64_t segmentSize, const char* segmentName));

extern "C" GLUE_VISIBILITY dyld_process_info_notify _dyld_process_info_notify(task_t task, dispatch_queue_t queue,
                                                          void (^notify)(bool unload, uint64_t timestamp, uint64_t machHeader, const uuid_t uuid, const char* path),
                                                          void (^notifyExit)(void),
                                                          kern_return_t* kernelError);
extern "C" GLUE_VISIBILITY void  _dyld_process_info_notify_main(dyld_process_info_notify objc, void (^notifyMain)(void));
extern "C" GLUE_VISIBILITY void  _dyld_process_info_notify_release(dyld_process_info_notify object);
extern "C" GLUE_VISIBILITY void  _dyld_process_info_notify_retain(dyld_process_info_notify object);


#define DYLD_INTROSPECTION_VTABLE_ENTRY(x) __typeof__ (x) *x
struct IntrospectionVtable
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
    uintptr_t        version;
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_create_for_task);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_create_for_current_task);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_dispose);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_snapshot_create_for_process);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_snapshot_create_from_data);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_snapshot_dispose);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_snapshot_for_each_image);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_pin_mapping);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_unpin_mapping);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_get_base_address);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_get_mapped_size);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_snapshot_get_shared_cache);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_is_mapped_private);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_copy_uuid);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_for_each_file);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_for_each_image);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_for_each_installed_shared_cache_with_system_path);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_for_each_installed_shared_cache);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_shared_cache_for_file);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_content_for_segment);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_content_for_section);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_copy_uuid);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_for_each_segment_info);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_for_each_section_info);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_get_installname);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_get_file_path);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_image_local_nlist_content_4Symbolication);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_register_for_image_notifications);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_register_for_event_notification);
    DYLD_INTROSPECTION_VTABLE_ENTRY(dyld_process_unregister_for_notification);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_create);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_get_state);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_get_cache);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_get_aot_cache);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_retain);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_get_platform);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_release);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_for_each_image);
#if TARGET_OS_OSX
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_for_each_aot_image);
#endif
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_for_each_segment);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_notify);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_notify_main);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_notify_retain);
    DYLD_INTROSPECTION_VTABLE_ENTRY(_dyld_process_info_notify_release);
#pragma clang diagnostic pop
};

extern "C" struct IntrospectionVtable _dyld_legacy_introspection_vtable;

VIS_HIDDEN IntrospectionVtable* dyldFrameworkIntrospectionVtable();

#endif /* DyldLegacyInterfaceGlue_h */

