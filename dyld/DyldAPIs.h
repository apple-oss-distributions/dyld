/*
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#ifndef DYLD_APIS_H
#define DYLD_APIS_H

#include <os/availability.h>
#include "DyldRuntimeState.h"
#include "OptimizerSwift.h"

#if !BUILDING_DYLD && !BUILDING_LIBDYLD
typedef struct dyld_process_s*              dyld_process_t;
typedef struct dyld_process_snapshot_s*     dyld_process_snapshot_t;
typedef struct dyld_shared_cache_s*         dyld_shared_cache_t;
typedef struct dyld_image_s*                dyld_image_t;
#endif

namespace dyld4 {

struct RuntimeLocks;

#if BUILDING_DYLD
#define RUNTIME_STATE_INHERITANCE public
#else
#define RUNTIME_STATE_INHERITANCE private
#endif

class VIS_HIDDEN APIs : RUNTIME_STATE_INHERITANCE RuntimeState
{
public:
                                            APIs(const ProcessConfig& c, RuntimeLocks& locks, Allocator& alloc) : RuntimeState(c, locks, alloc) { }

    //
    // private call from libdyld.dylib into dyld to tell that libSystem.dylib is initialized
    //
    virtual     void                        _libdyld_initialize();


    //
    // APIs from macOS 10.2
    //
    virtual     uint32_t                    _dyld_image_count();
    virtual     const mach_header*          _dyld_get_image_header(uint32_t index);
    virtual     intptr_t                    _dyld_get_image_vmaddr_slide(uint32_t index);
    virtual     const char*                 _dyld_get_image_name(uint32_t index);
    virtual     void                        _dyld_register_func_for_add_image(NotifyFunc func);
    virtual     void                        _dyld_register_func_for_remove_image(NotifyFunc func);
    virtual     int32_t                     NSVersionOfLinkTimeLibrary(const char* libraryName);
    virtual     int32_t                     NSVersionOfRunTimeLibrary(const char* libraryName);
    virtual     int                         _NSGetExecutablePath(char* buf, uint32_t* bufsize);
    virtual     void                        _dyld_fork_child();


    //
    // APIs from macOS 10.4
    //
    virtual     int                         dladdr(const void*, Dl_info* result);
    virtual     void*                       dlopen(const char* path, int mode) API_UNAVAILABLE(driverkit);
    virtual     int                         dlclose(void* handle) API_UNAVAILABLE(driverkit);
    virtual     char*                       dlerror() API_UNAVAILABLE(driverkit);
    virtual     void*                       dlsym(void* handle, const char* symbol);
    virtual     bool                        dlopen_preflight(const char* path) API_UNAVAILABLE(driverkit);


    //
    // APIs deprecated in macOS 10.5 and not on any other platform
    //
    virtual     NSObjectFileImageReturnCode NSCreateObjectFileImageFromFile(const char* pathName, NSObjectFileImage* objectFileImage);
    virtual     NSObjectFileImageReturnCode NSCreateObjectFileImageFromMemory(const void* address, size_t size, NSObjectFileImage* objectFileImage);
    virtual     bool                        NSDestroyObjectFileImage(NSObjectFileImage objectFileImage);
    virtual     bool                        NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage objectFileImage, const char* symbolName);
    virtual     void*                       NSGetSectionDataInObjectFileImage(NSObjectFileImage objectFileImage, const char* segmentName, const char* sectionName, size_t* size);
    virtual     const char*                 NSNameOfModule(NSModule m);
    virtual     const char*                 NSLibraryNameForModule(NSModule m);
    virtual     NSModule                    NSLinkModule(NSObjectFileImage objectFileImage, const char* moduleName, uint32_t options);
    virtual     bool                        NSUnLinkModule(NSModule module, uint32_t options);
    virtual     bool                        NSIsSymbolNameDefined(const char* symbolName);
    virtual     bool                        NSIsSymbolNameDefinedWithHint(const char* symbolName, const char* libraryNameHint);
    virtual     bool                        NSIsSymbolNameDefinedInImage(const mach_header* image, const char* symbolName);
    virtual     NSSymbol                    NSLookupAndBindSymbol(const char* symbolName);
    virtual     NSSymbol                    NSLookupAndBindSymbolWithHint(const char* symbolName, const char* libraryNameHint);
    virtual     NSSymbol                    NSLookupSymbolInModule(NSModule module, const char* symbolName);
    virtual     NSSymbol                    NSLookupSymbolInImage(const mach_header* image, const char* symbolName, uint32_t options);
    virtual     void*                       NSAddressOfSymbol(NSSymbol symbol);
    virtual     NSModule                    NSModuleForSymbol(NSSymbol symbol);
    virtual     void                        NSLinkEditError(NSLinkEditErrors* c, int* errorNumber, const char** fileName, const char** errorString);
    virtual     bool                        NSAddLibrary(const char* pathName);
    virtual     bool                        NSAddLibraryWithSearching(const char* pathName);
    virtual     const mach_header*          NSAddImage(const char* image_name, uint32_t options);
    virtual     bool                        _dyld_image_containing_address(const void* address);
    virtual     void                        _dyld_lookup_and_bind(const char* symbol_name, void** address, NSModule* module);
    virtual     void                        _dyld_lookup_and_bind_with_hint(const char* symbol_name, const char* library_name_hint, void** address, NSModule* module);
    virtual     void                        _dyld_lookup_and_bind_fully(const char* symbol_name, void** address, NSModule* module);


    //
    // Added macOS 10.6
    //
    virtual     intptr_t                    _dyld_get_image_slide(const mach_header* mh);
    virtual     const char*                 dyld_image_path_containing_address(const void* addr);
#if !__USING_SJLJ_EXCEPTIONS__
    virtual     bool                        _dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info);
#endif

    //
    // Added iOS 6, macOS 10.8
    //
    virtual     uint32_t                    dyld_get_sdk_version(const mach_header* mh);
    virtual     uint32_t                    dyld_get_min_os_version(const mach_header* mh);
    virtual     uint32_t                    dyld_get_program_sdk_version();
    virtual     uint32_t                    dyld_get_program_min_os_version();


    //
    // Added iOS 7, macOS 10.9
    //
    virtual     bool                        dyld_process_is_restricted();


    //
    // Added iOS 8, macOS 10.10
    //
    virtual     bool                        dyld_shared_cache_some_image_overridden();

    
    //
    // Added iOS 9, macOS 10.11, watchOS 2.0
    //
    virtual     int                         dyld_shared_cache_iterate_text(const uuid_t cacheUuid, IterateCacheTextFunc callback);
    virtual     const mach_header*          dyld_image_header_containing_address(const void* addr);
    virtual     const char*                 dyld_shared_cache_file_path();
    virtual     uint32_t                    dyld_get_program_sdk_watch_os_version();
    virtual     uint32_t                    dyld_get_program_min_watch_os_version();


    //
    // Added iOS 10, macOS 10.12, watchOS 3.0
    //
    virtual     void                        _dyld_objc_notify_register(ReadOnlyCallback<_dyld_objc_notify_mapped>,
                                                                       ReadOnlyCallback<_dyld_objc_notify_init>,
                                                                       ReadOnlyCallback<_dyld_objc_notify_unmapped>);
    virtual     bool                        _dyld_get_image_uuid(const mach_header* mh, uuid_t uuid);
    virtual     bool                        _dyld_get_shared_cache_uuid(uuid_t uuid);
    virtual     bool                        _dyld_is_memory_immutable(const void* addr, size_t length);
    virtual     int                         dyld_shared_cache_find_iterate_text(const uuid_t cacheUuid, const char* extraSearchDirs[], 
                                                                                IterateCacheTextFunc callback);


    //
    // Added iOS 11, macOS 10.13, bridgeOS 2.0
    //
    virtual     const void*                 _dyld_get_shared_cache_range(size_t* length);
    virtual     void                        obsolete_dyld_get_program_sdk_bridge_os_version() __attribute__((__noreturn__));
    virtual     void                        obsolete_dyld_get_program_min_bridge_os_version() __attribute__((__noreturn__));


    //
    // Added iOS 12, macOS 10.14
    //
    virtual     dyld_platform_t             dyld_get_active_platform();
    virtual     dyld_platform_t             dyld_get_base_platform(dyld_platform_t platform);
    virtual     bool                        dyld_is_simulator_platform(dyld_platform_t platform);
    virtual     bool                        dyld_sdk_at_least(const mach_header* mh, dyld_build_version_t version);
    virtual     bool                        dyld_minos_at_least(const mach_header* mh, dyld_build_version_t version);
    virtual     bool                        dyld_program_sdk_at_least(dyld_build_version_t version);
    virtual     bool                        dyld_program_minos_at_least(dyld_build_version_t version);
    virtual     void                        dyld_get_image_versions(const mach_header* mh, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version));
    virtual     void                        _dyld_images_for_addresses(unsigned count, const void* addresses[], dyld_image_uuid_offset infos[]);
    virtual     void                        _dyld_register_for_image_loads(LoadNotifyFunc func);


    //
    // Added iOS 13, macOS 10.15
    //
    virtual     void                        _dyld_atfork_prepare();
    virtual     void                        _dyld_atfork_parent();
    virtual     bool                        dyld_need_closure(const char* execPath, const char* dataContainerRootDir);
    virtual     bool                        dyld_has_inserted_or_interposing_libraries(void);
    virtual     bool                        _dyld_shared_cache_optimized(void);
    virtual     bool                        _dyld_shared_cache_is_locally_built(void);
    virtual     void                        _dyld_register_for_bulk_image_loads(BulkLoadNotifier func);
    virtual     void                        _dyld_register_driverkit_main(void (*mainFunc)(void));
    virtual     const char*                 _dyld_get_objc_selector(const char* selName);
    virtual     void                        _dyld_for_each_objc_class(const char* className, ObjCClassFunc func);
    virtual     void                        _dyld_for_each_objc_protocol(const char* protocolName, ObjCProtocolFunc func);


    //
    // Added iOS 14, macOS 11
    //
    virtual     uint32_t                    _dyld_launch_mode();
    virtual     bool                        _dyld_is_objc_constant(DyldObjCConstantKind kind, const void* addr);
    virtual     bool                        _dyld_has_fix_for_radar(const char* rdar);
    virtual     const char*                 _dyld_shared_cache_real_path(const char* path);
    virtual     bool                        _dyld_shared_cache_contains_path(const char* path);
    virtual     void*                       dlopen_from(const char* path, int mode, void* addressInCaller);
    virtual     void *                      dlopen_audited(const char * path, int mode);
    virtual     const struct mach_header*   _dyld_get_prog_image_header();


    //
    // Added iOS 15, macOS 12
    //
    virtual     void                                obsolete() __attribute__((__noreturn__));
    virtual     void                                _dyld_visit_objc_classes(ObjCVisitClassesFunc func);
    virtual     uint32_t                            _dyld_objc_class_count(void);
    virtual     bool                                _dyld_objc_uses_large_shared_cache(void);
    virtual     _dyld_protocol_conformance_result   _dyld_find_protocol_conformance(const void *protocolDescriptor,
                                                                                    const void *metadataType,
                                                                                    const void *typeDescriptor) const;
    virtual     _dyld_protocol_conformance_result   _dyld_find_foreign_type_protocol_conformance(const void *protocol,
                                                                                                 const char *foreignTypeIdentityStart,
                                                                                                 size_t foreignTypeIdentityLength) const;
    virtual     uint32_t                            _dyld_swift_optimizations_version() const;
    virtual     void                                runAllInitializersForMain();

    //
    // Added iOS 15.x, macOS 12.x
    //
    // Note these 3 methods were technically internally defined in iOS 15/macOS 12, but are now exported from
    // libdyld.dylib as of iOS 15.x/macOS 12.x.
    // Note the names of these virtual methods doesn't match the API names.  We can't change them due
    // to pointer signing
    virtual     void                                _dyld_before_fork_dlopen();
    virtual     void                                _dyld_after_fork_dlopen_parent();
    virtual     void                                _dyld_after_fork_dlopen_child();

    //
    // Added iOS 16, macOS 13
    //
    virtual     const struct mach_header*           _dyld_get_dlopen_image_header(void* handle);
    virtual     void                                _dyld_objc_register_callbacks(const ObjCCallbacks* callbacks);
    virtual     bool                                _dyld_has_preoptimized_swift_protocol_conformances(const struct mach_header* mh);
    virtual     _dyld_protocol_conformance_result   _dyld_find_protocol_conformance_on_disk(const void *protocolDescriptor,
                                                                                            const void *metadataType,
                                                                                            const void *typeDescriptor,
                                                                                            uint32_t flags);
    virtual     _dyld_protocol_conformance_result   _dyld_find_foreign_type_protocol_conformance_on_disk(const void *protocol,
                                                                                                         const char *foreignTypeIdentityStart,
                                                                                                         size_t foreignTypeIdentityLength,
                                                                                                         uint32_t flags);

    //
    // Added iOS 17, macOS 14
    //
    // The pseudo-dylib functions are new in iOS 17 / macOS 14.
    virtual     _dyld_section_info_result           _dyld_lookup_section_info(const struct mach_header* mh,
                                                                              _dyld_section_location_info_t locationHandle,
                                                                              _dyld_section_location_kind kind);

    virtual     _dyld_pseudodylib_callbacks_handle  _dyld_pseudodylib_register_callbacks(const struct PseudoDylibRegisterCallbacks* callbacks);
    virtual     void                                _dyld_pseudodylib_deregister_callbacks(_dyld_pseudodylib_callbacks_handle callbacks);
    virtual     _dyld_pseudodylib_handle            _dyld_pseudodylib_register(void* addr, size_t size, _dyld_pseudodylib_callbacks_handle callbacks_handle, void* context);
    virtual     void                                _dyld_pseudodylib_deregister(_dyld_pseudodylib_handle pd_handle);
    virtual     bool                                _dyld_is_preoptimized_objc_image_loaded(uint16_t imageID);
    virtual     void*                               _dyld_for_objc_header_opt_rw();
    virtual     const void*                         _dyld_for_objc_header_opt_ro();


    //
    // Added iOS 18, macOS 15
    //
    virtual bool                                    _dyld_dlsym_blocked();
    virtual void                                    _dyld_register_dlsym_notifier(DlsymNotify func);
    virtual const void*                             _dyld_get_swift_prespecialized_data();
    virtual const void*                             _dyld_find_pointer_hash_table_entry(const void *table, const void *key1,
                                                                                        size_t restKeysCount, const void **restKeys);
    virtual uint64_t                                dyld_get_program_sdk_version_token();
    virtual uint64_t                                dyld_get_program_minos_version_token();
    virtual dyld_platform_t                         dyld_version_token_get_platform(uint64_t token);
    virtual bool                                    dyld_version_token_at_least(uint64_t token, dyld_build_version_t version);
    virtual bool                                    _dyld_is_pseudodylib(void* handle);

    //
    // Added iOS 18.4, macOS 15.4
    //
    virtual void                                    _dyld_for_each_prewarming_range(PrewarmingDataFunc callback);

    //
    // Added iOS 18.4, macOS 15.4
    //
    virtual struct dyld_all_image_infos*            _dyld_all_image_infos_TEMP();
#if SUPPPORT_PRE_LC_MAIN
    virtual MainFunc                                _dyld_get_main_func();
#endif
#if !TARGET_OS_EXCLAVEKIT
    virtual DyldCommPage                            _dyld_commpage();
#endif
    virtual void                                    _dyld_stack_range(const void** stack_bottom, const void** stack_top);

private:

#if SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS
    typedef SwiftTypeProtocolConformanceDiskLocationKey          TypeKey;
    typedef SwiftMetadataProtocolConformanceDiskLocationKey      MetadataKey;
    typedef SwiftForeignTypeProtocolConformanceDiskLocationKey   ForeignKey;
#endif // SUPPORT_PREBUILTLOADERS || BUILDING_UNIT_TESTS

    // internal helpers
    uint32_t                    getSdkVersion(const mach_header* mh);
    dyld_build_version_t        mapFromVersionSet(dyld_build_version_t version, mach_o::Platform platform);
    mach_o::Version32           linkedDylibVersion(const mach_o::Header* header, const char* installname);
    mach_o::Version32           deriveVersionFromDylibs(const mach_o::Header* header);
    mach_o::PlatformAndVersions getPlatformAndVersions(const mach_o::Header* header);
    bool                        findImageMappedAt(const void* addr, const MachOLoaded** ml, bool* neverUnloads = nullptr, const char** path = nullptr, const void** segAddr = nullptr, uint64_t* segSize = nullptr, uint8_t* segPerms = nullptr, const Loader** loader = nullptr);
    void                        clearErrorString() API_UNAVAILABLE(driverkit);
    void                        setErrorString(const char* format, ...) __attribute__((format(printf, 2, 3))) API_UNAVAILABLE(driverkit);
    const Loader*               findImageContaining(const void* addr);
    bool                        flatFindSymbol(const char* symbolName, void** symbolAddress, const mach_header** foundInImageAtLoadAddress);
    bool                        validLoader(const Loader* maybeLoader);
    mach_o::PlatformAndVersions getImagePlatformAndVersions(const mach_o::Header* hdr);
    bool                        addressLookupsDisabled(const char* symbolName=nullptr) const;
};

} // namespace dyld4

#endif


