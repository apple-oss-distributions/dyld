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


#include <TargetConditionals.h>
#if TARGET_OS_EXCLAVEKIT
  #include <liblibc/liblibc.h>
  extern void abort_report_np(const char* format, ...) __attribute__((noreturn,format(printf, 1, 2)));
#else
  #include <libc_private.h>
  #include <sys/errno.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <_simple.h>
#endif

#include <string.h>
#include <stdint.h>


#include <TargetConditionals.h>
#include <mach-o/dyld_priv.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <dlfcn.h>
#include <dlfcn_private.h>

#include <ptrauth.h>
#include <pthread.h>

#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "DyldAPIs.h"
#include "ThreadLocalVariables.h"

namespace dyld4 {
    // dyld finds this section by name and stuffs a pointer to its API object into this section
    // libdyld then uses this gAPIs object to call into dyld
    __attribute__((visibility("hidden"), section("__TPRO_CONST,__dyld_apis")))
    APIs* gAPIs = nullptr;
}

using dyld4::gAPIs;


__attribute__((used,section ("__DATA_CONST,__helper")))
static const dyld4::LibSystemHelpers sHelpers;

// This is called during libSystem.dylib initialization.
// It calls back into dyld and lets it know it can start using libSystem.dylib
// functions which are wrapped in the LibSystemHelpers class.
void _dyld_initializer()
{
#if !TARGET_OS_DRIVERKIT
    // Initialize the memory manager prior to use
    dyld4::MemoryManager::init();
#endif
#if __has_feature(tls)
    // assign pthread_key for per-thread terminators
    dyld::sThreadLocalVariables.initialize();
#endif
    gAPIs->_libdyld_initialize();
}

// FIXME: should not need dyld_all_image_info
extern "C" VIS_HIDDEN const dyld_all_image_infos* getProcessDyldInfo();
const dyld_all_image_infos* getProcessDyldInfo()
{
    return gAPIs->_dyld_all_image_infos_TEMP();
}

#if SUPPPORT_PRE_LC_MAIN
// called by crt before main() by programs linked with 10.4 or earlier crt1.o
extern void _dyld_make_delayed_module_initializer_calls() VIS_HIDDEN;
void _dyld_make_delayed_module_initializer_calls()
{
    gAPIs->_dyld_get_main_func();
    // We don't actually do anything here, we just need the function to exist
    // If we had a very old binary AND a custom entry point we would have to do something, but dyld has not supported that on x86_64 in years.
    // Instead just return an empty function and let initializers run normally
    gAPIs->runAllInitializersForMain();
}
#endif

static ALWAYS_INLINE void checkTPROState()
{
#if DYLD_FEATURE_USE_HW_TPRO
    if ( os_thread_self_restrict_tpro_is_supported() && os_thread_self_restrict_tpro_is_writable() )
        abort_report_np("TPRO regions should not be writable on entry to dyld\n");
#endif // DYLD_FEATURE_USE_HW_TPRO
}


//
// MARK: --- APIs from macOS 10.2 ---
//
uint32_t _dyld_image_count()
{
    checkTPROState();
    return gAPIs->_dyld_image_count();
}

const mach_header* _dyld_get_image_header(uint32_t index)
{
    checkTPROState();
    return gAPIs->_dyld_get_image_header(index);
}

intptr_t _dyld_get_image_vmaddr_slide(uint32_t index)
{
    checkTPROState();
    return gAPIs->_dyld_get_image_vmaddr_slide(index);
}

const char* _dyld_get_image_name(uint32_t index)
{
    checkTPROState();
    return gAPIs->_dyld_get_image_name(index);
}

void _dyld_register_func_for_add_image(void (*func)(const mach_header* mh, intptr_t vmaddr_slide))
{
    checkTPROState();
#if TARGET_OS_DRIVERKIT
    // DriverKit signs the pointer with a diversity different than dyld expects when calling the pointer.
#if __has_feature(ptrauth_calls)
    func = ptrauth_auth_and_resign(func, ptrauth_key_function_pointer, ptrauth_type_discriminator(void (*)(const mach_header*,intptr_t)), ptrauth_key_function_pointer, 0);
#endif // __has_feature(ptrauth_calls)
#endif // !TARGET_OS_DRIVERKIT
    gAPIs->_dyld_register_func_for_add_image(func);
}

void _dyld_register_func_for_remove_image(void (*func)(const mach_header* mh, intptr_t vmaddr_slide))
{
    checkTPROState();
#if TARGET_OS_DRIVERKIT
    // DriverKit signs the pointer with a diversity different than dyld expects when calling the pointer.
#if __has_feature(ptrauth_calls)
    func = ptrauth_auth_and_resign(func, ptrauth_key_function_pointer, ptrauth_type_discriminator(void (*)(const mach_header*,intptr_t)), ptrauth_key_function_pointer, 0);
#endif // __has_feature(ptrauth_calls)
#endif // !TARGET_OS_DRIVERKIT
    gAPIs->_dyld_register_func_for_remove_image(func);
}

int32_t NSVersionOfLinkTimeLibrary(const char* libraryName)
{
    checkTPROState();
    return gAPIs->NSVersionOfLinkTimeLibrary(libraryName);
}

int32_t NSVersionOfRunTimeLibrary(const char* libraryName)
{
    checkTPROState();
    return gAPIs->NSVersionOfRunTimeLibrary(libraryName);
}

int _NSGetExecutablePath(char* buf, uint32_t* bufsize)
{
    checkTPROState();
    return gAPIs->_NSGetExecutablePath(buf, bufsize);
}

void _dyld_fork_child()
{
    // checkTPROState();

    // FIXME: rdar://135425853 There seems to be a bug where we have TPRO RW here, even if it was RO before fork().
#if DYLD_FEATURE_USE_HW_TPRO
    if ( os_thread_self_restrict_tpro_is_supported() && os_thread_self_restrict_tpro_is_writable() ) {
        os_compiler_barrier();
        os_thread_self_restrict_tpro_to_ro();
        os_compiler_barrier();
    }
    checkTPROState();
#endif // DYLD_FEATURE_USE_HW_TPRO

    gAPIs->_dyld_fork_child();
}

//
// MARK: --- APIs from macOS 10.4 ---
//
int dladdr(const void* addr, Dl_info* result)
{
    checkTPROState();
    return gAPIs->dladdr(addr, result);
}

void* dlsym(void* handle, const char* symbol)
{
    checkTPROState();
    return gAPIs->dlsym(handle, symbol);
}

#if !TARGET_OS_DRIVERKIT
void* dlopen(const char* path, int mode)
{
    checkTPROState();
    return gAPIs->dlopen(path, mode);
}

int dlclose(void* handle)
{
    checkTPROState();
    return gAPIs->dlclose(handle);
}

char* dlerror()
{
    checkTPROState();
    return gAPIs->dlerror();
}

bool dlopen_preflight(const char* path)
{
    checkTPROState();
    return gAPIs->dlopen_preflight(path);
}
#endif

//
// MARK: --- APIs deprecated in macOS 10.5 and not on any other platform ---
//
#if TARGET_OS_OSX
NSObjectFileImageReturnCode NSCreateObjectFileImageFromFile(const char* pathName, NSObjectFileImage* objectFileImage)
{
    checkTPROState();
    return gAPIs->NSCreateObjectFileImageFromFile(pathName, objectFileImage);
}

NSObjectFileImageReturnCode NSCreateObjectFileImageFromMemory(const void* address, size_t size, NSObjectFileImage* objectFileImage)
{
    checkTPROState();
    return gAPIs->NSCreateObjectFileImageFromMemory(address, size, objectFileImage);
}

bool NSDestroyObjectFileImage(NSObjectFileImage objectFileImage)
{
    checkTPROState();
    return gAPIs->NSDestroyObjectFileImage(objectFileImage);
}

uint32_t NSSymbolDefinitionCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
    checkTPROState();
    gAPIs->obsolete();
    return 0;
}

const char* NSSymbolDefinitionNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal)
{
    checkTPROState();
    gAPIs->obsolete();
    return nullptr;
}

uint32_t NSSymbolReferenceCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
    checkTPROState();
    gAPIs->obsolete();
    return 0;
}

const char* NSSymbolReferenceNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal, bool* tentative_definition)
{
    checkTPROState();
    gAPIs->obsolete();
    return nullptr;
}

bool NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage objectFileImage, const char* symbolName)
{
    checkTPROState();
    return gAPIs->NSIsSymbolDefinedInObjectFileImage(objectFileImage, symbolName);
}

void* NSGetSectionDataInObjectFileImage(NSObjectFileImage objectFileImage, const char* segmentName, const char* sectionName, size_t* size)
{
    checkTPROState();
    return gAPIs->NSGetSectionDataInObjectFileImage(objectFileImage, segmentName, sectionName, size);
}

const char* NSNameOfModule(NSModule m)
{
    checkTPROState();
    return gAPIs->NSNameOfModule(m);
}

const char* NSLibraryNameForModule(NSModule m)
{
    checkTPROState();
    return gAPIs->NSLibraryNameForModule(m);
}

NSModule NSLinkModule(NSObjectFileImage objectFileImage, const char* moduleName, uint32_t options)
{
    checkTPROState();
    return gAPIs->NSLinkModule(objectFileImage, moduleName, options);
}

bool NSUnLinkModule(NSModule module, uint32_t options)
{
    checkTPROState();
    return gAPIs->NSUnLinkModule(module, options);
}

bool NSIsSymbolNameDefined(const char* symbolName)
{
    checkTPROState();
    return gAPIs->NSIsSymbolNameDefined(symbolName);
}

bool NSIsSymbolNameDefinedWithHint(const char* symbolName, const char* libraryNameHint)
{
    checkTPROState();
    return gAPIs->NSIsSymbolNameDefinedWithHint(symbolName, libraryNameHint);
}

bool NSIsSymbolNameDefinedInImage(const mach_header* image, const char* symbolName)
{
    checkTPROState();
    return gAPIs->NSIsSymbolNameDefinedInImage(image, symbolName);
}

NSSymbol NSLookupAndBindSymbol(const char* symbolName)
{
    checkTPROState();
    return gAPIs->NSLookupAndBindSymbol(symbolName);
}

NSSymbol NSLookupAndBindSymbolWithHint(const char* symbolName, const char* libraryNameHint)
{
    checkTPROState();
    return gAPIs->NSLookupAndBindSymbolWithHint(symbolName, libraryNameHint);
}

NSSymbol NSLookupSymbolInModule(NSModule module, const char* symbolName)
{
    checkTPROState();
    return gAPIs->NSLookupSymbolInModule(module, symbolName);
}

NSSymbol NSLookupSymbolInImage(const mach_header* image, const char* symbolName, uint32_t options)
{
    checkTPROState();
    return gAPIs->NSLookupSymbolInImage(image, symbolName, options);
}

const char* NSNameOfSymbol(NSSymbol symbol)
{
    checkTPROState();
    gAPIs->obsolete();
    return nullptr;
}

void* NSAddressOfSymbol(NSSymbol symbol)
{
    checkTPROState();
    return gAPIs->NSAddressOfSymbol(symbol);
}

NSModule NSModuleForSymbol(NSSymbol symbol)
{
    checkTPROState();
    return gAPIs->NSModuleForSymbol(symbol);
}

void NSLinkEditError(NSLinkEditErrors* c, int* errorNumber, const char** fileName, const char** errorString)
{
    checkTPROState();
    gAPIs->NSLinkEditError(c, errorNumber, fileName, errorString);
}

void NSInstallLinkEditErrorHandlers(const NSLinkEditErrorHandlers* handlers)
{
    checkTPROState();
    gAPIs->obsolete();
}

bool NSAddLibrary(const char* pathName)
{
    checkTPROState();
    return gAPIs->NSAddLibrary(pathName);
}

bool NSAddLibraryWithSearching(const char* pathName)
{
    checkTPROState();
    return gAPIs->NSAddLibraryWithSearching(pathName);
}

const mach_header* NSAddImage(const char* image_name, uint32_t options)
{
    checkTPROState();
    return gAPIs->NSAddImage(image_name, options);
}

bool _dyld_present()
{
    checkTPROState();
    return true;
}

bool _dyld_launched_prebound()
{
    checkTPROState();
    gAPIs->obsolete();
    return false;
}

bool _dyld_all_twolevel_modules_prebound()
{
    checkTPROState();
    gAPIs->obsolete();
    return false;
}

bool _dyld_bind_fully_image_containing_address(const void* address)
{
    checkTPROState();
    // in dyld4, everything is always fully bound
    return true;
}

bool _dyld_image_containing_address(const void* address)
{
    checkTPROState();
    return gAPIs->_dyld_image_containing_address(address);
}

void _dyld_lookup_and_bind(const char* symbol_name, void** address, NSModule* module)
{
    checkTPROState();
    gAPIs->_dyld_lookup_and_bind(symbol_name, address, module);
}

void _dyld_lookup_and_bind_with_hint(const char* symbol_name, const char* library_name_hint, void** address, NSModule* module)
{
    checkTPROState();
    gAPIs->_dyld_lookup_and_bind_with_hint(symbol_name, library_name_hint, address, module);
}

void _dyld_lookup_and_bind_fully(const char* symbol_name, void** address, NSModule* module)
{
    checkTPROState();
    gAPIs->_dyld_lookup_and_bind_fully(symbol_name, address, module);
}

const mach_header* _dyld_get_image_header_containing_address(const void* address)
{
    checkTPROState();
    return gAPIs->dyld_image_header_containing_address(address);
}
#endif // TARGET_OS_OSX


//
// MARK: --- APIs Added macOS 10.6 ---
//
intptr_t  _dyld_get_image_slide(const mach_header* mh)
{
    checkTPROState();
    return gAPIs->_dyld_get_image_slide(mh);
}

const char* dyld_image_path_containing_address(const void* addr)
{
    checkTPROState();
    return gAPIs->dyld_image_path_containing_address(addr);
}

#if !__USING_SJLJ_EXCEPTIONS__
bool _dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info)
{
    checkTPROState();
    return gAPIs->_dyld_find_unwind_sections(addr, info);
}
#endif


//
// MARK: --- APIs added iOS 6, macOS 10.8 ---
//
uint32_t dyld_get_sdk_version(const mach_header* mh)
{
    checkTPROState();
    return gAPIs->dyld_get_sdk_version(mh);
}

uint32_t dyld_get_min_os_version(const mach_header* mh)
{
    checkTPROState();
    return gAPIs->dyld_get_min_os_version(mh);
}

uint32_t dyld_get_program_sdk_version()
{
    checkTPROState();
    return gAPIs->dyld_get_program_sdk_version();
}

uint32_t dyld_get_program_min_os_version()
{
    checkTPROState();
    return gAPIs->dyld_get_program_min_os_version();
}



//
// MARK: --- APIs added iOS 7, macOS 10.9 ---
//
bool dyld_process_is_restricted()
{
    checkTPROState();
    return gAPIs->dyld_process_is_restricted();
}



//
// MARK: --- APIs added iOS 8, macOS 10.10 ---
//
bool dyld_shared_cache_some_image_overridden()
{
    checkTPROState();
    return gAPIs->dyld_shared_cache_some_image_overridden();
}

void dyld_dynamic_interpose(const mach_header* mh, const dyld_interpose_tuple array[], size_t count)
{
    checkTPROState();
    // <rdar://74287303> (Star 21A185 REG: Adobe Photoshop 2021 crash on launch)
    return;
}

// call by C++ codegen to register a function to call when a thread goes away
void _tlv_atexit(void (*termFunc)(void* objAddr), void* objAddr)
{
    checkTPROState();
#if __has_feature(tls)
   dyld::sThreadLocalVariables.addTermFunc(termFunc, objAddr);
#endif
}

// called by exit() in libc to call all tlv_atexit handlers
void _tlv_exit()
{
    checkTPROState();
#if __has_feature(tls)
    dyld::sThreadLocalVariables.exit();
#endif
}

// for catching uses of thread_locals before they are set up
extern "C" void _tlv_bootstrap_error();
VIS_HIDDEN void _tlv_bootstrap_error()
{
    checkTPROState();
#if __has_feature(tls) && !TARGET_OS_EXCLAVEKIT
    abort_report_np("thread locals not initialized");
#endif
}



//
// MARK: --- APIs added iOS 9, macOS 10.11, watchOS 2.0 ---
//
int dyld_shared_cache_iterate_text(const uuid_t cacheUuid, void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    checkTPROState();
    return gAPIs->dyld_shared_cache_iterate_text(cacheUuid, callback);
}

const mach_header* dyld_image_header_containing_address(const void* addr)
{
    checkTPROState();
    return gAPIs->dyld_image_header_containing_address(addr);
}

const char* dyld_shared_cache_file_path()
{
    checkTPROState();
    return gAPIs->dyld_shared_cache_file_path();
}

#if TARGET_OS_WATCH
uint32_t  dyld_get_program_sdk_watch_os_version()
{
    checkTPROState();
    return gAPIs->dyld_get_program_sdk_watch_os_version();
}

uint32_t  dyld_get_program_min_watch_os_version()
{
    checkTPROState();
    return gAPIs->dyld_get_program_min_watch_os_version();
}
#endif // TARGET_OS_WATCH



//
// MARK: --- APIs added iOS 10, macOS 10.12, watchOS 3.0 ---
//
void _dyld_objc_notify_register(_dyld_objc_notify_mapped m, _dyld_objc_notify_init i, _dyld_objc_notify_unmapped u)
{
    checkTPROState();
    gAPIs->_dyld_objc_notify_register(m, i, u);
}

bool _dyld_get_image_uuid(const mach_header* mh, uuid_t uuid)
{
    checkTPROState();
    return gAPIs->_dyld_get_image_uuid(mh, uuid);
}

bool _dyld_get_shared_cache_uuid(uuid_t uuid)
{
    checkTPROState();
    return gAPIs->_dyld_get_shared_cache_uuid(uuid);
}

bool _dyld_is_memory_immutable(const void* addr, size_t length)
{
    checkTPROState();
    return gAPIs->_dyld_is_memory_immutable(addr, length);
}

int  dyld_shared_cache_find_iterate_text(const uuid_t cacheUuid, const char* extraSearchDirs[], void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    checkTPROState();
    return gAPIs->dyld_shared_cache_find_iterate_text(cacheUuid, extraSearchDirs, callback);
}



//
// MARK: --- APIs iOS 11, macOS 10.13, bridgeOS 2.0 ---
//
const void* _dyld_get_shared_cache_range(size_t* length)
{
    checkTPROState();
    return gAPIs->_dyld_get_shared_cache_range(length);
}

//
// MARK: --- APIs iOS 12, macOS 10.14 ---
//
dyld_platform_t dyld_get_active_platform()
{
    checkTPROState();
    return gAPIs->dyld_get_active_platform();
}

dyld_platform_t dyld_get_base_platform(dyld_platform_t platform)
{
    checkTPROState();
    return gAPIs->dyld_get_base_platform(platform);
}

bool dyld_is_simulator_platform(dyld_platform_t platform)
{
    checkTPROState();
    return gAPIs->dyld_is_simulator_platform(platform);
}

bool dyld_sdk_at_least(const mach_header* mh, dyld_build_version_t version)
{
    checkTPROState();
    return gAPIs->dyld_sdk_at_least(mh, version);
}

bool dyld_minos_at_least(const mach_header* mh, dyld_build_version_t version)
{
    checkTPROState();
    return gAPIs->dyld_minos_at_least(mh, version);
}

bool dyld_program_sdk_at_least(dyld_build_version_t version)
{
    checkTPROState();
    return gAPIs->dyld_program_sdk_at_least(version);
}

bool dyld_program_minos_at_least(dyld_build_version_t version)
{
    checkTPROState();
    return gAPIs->dyld_program_minos_at_least(version);
}

void dyld_get_image_versions(const mach_header* mh, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version))
{
    checkTPROState();
    gAPIs->dyld_get_image_versions(mh, callback);
}

void _dyld_images_for_addresses(unsigned count, const void* addresses[], dyld_image_uuid_offset infos[])
{
    checkTPROState();
    gAPIs->_dyld_images_for_addresses(count, addresses, infos);
}

void _dyld_register_for_image_loads(void (*func)(const mach_header* mh, const char* path, bool unloadable))
{
    checkTPROState();
    gAPIs->_dyld_register_for_image_loads(func);
}

//
// MARK: --- APIs added iOS 13, macOS 10.15 ---
//
void _dyld_atfork_prepare()
{
    checkTPROState();
    gAPIs->_dyld_atfork_prepare();
}

void _dyld_atfork_parent()
{
    checkTPROState();
    gAPIs->_dyld_atfork_parent();
}

bool dyld_need_closure(const char* execPath, const char* dataContainerRootDir)
{
    checkTPROState();
    return gAPIs->dyld_need_closure(execPath, dataContainerRootDir);
}

bool dyld_has_inserted_or_interposing_libraries()
{
    checkTPROState();
    return gAPIs->dyld_has_inserted_or_interposing_libraries();
}

bool _dyld_shared_cache_optimized()
{
    checkTPROState();
    return gAPIs->_dyld_shared_cache_optimized();
}

bool _dyld_shared_cache_is_locally_built()
{
    checkTPROState();
    return gAPIs->_dyld_shared_cache_is_locally_built();
}

void _dyld_register_for_bulk_image_loads(void (*func)(unsigned imageCount, const mach_header* mhs[], const char* paths[]))
{
    checkTPROState();
    gAPIs->_dyld_register_for_bulk_image_loads(func);
}

void _dyld_register_driverkit_main(void (*mainFunc)(void))
{
    checkTPROState();
    gAPIs->_dyld_register_driverkit_main(mainFunc);
}

const char* _dyld_get_objc_selector(const char* selName)
{
    checkTPROState();
    return gAPIs->_dyld_get_objc_selector(selName);
}

void _dyld_for_each_objc_class(const char* className, void (^callback)(void* classPtr, bool isLoaded, bool* stop))
{
    checkTPROState();
    gAPIs->_dyld_for_each_objc_class(className, callback);
}

void _dyld_for_each_objc_protocol(const char* protocolName, void (^callback)(void* protocolPtr, bool isLoaded, bool* stop))
{
    checkTPROState();
    gAPIs->_dyld_for_each_objc_protocol(protocolName, callback);
}


//
// MARK: --- APIs added iOS 14, macOS 11 ---
//
uint32_t _dyld_launch_mode()
{
    checkTPROState();
    return gAPIs->_dyld_launch_mode();
}

bool _dyld_is_objc_constant(DyldObjCConstantKind kind, const void* addr)
{
    checkTPROState();
    return gAPIs->_dyld_is_objc_constant(kind, addr);
}

bool _dyld_has_fix_for_radar(const char* rdar)
{
    checkTPROState();
    return gAPIs->_dyld_has_fix_for_radar(rdar);
}

const char* _dyld_shared_cache_real_path(const char* path)
{
    checkTPROState();
    return gAPIs->_dyld_shared_cache_real_path(path);
}

#if !TARGET_OS_DRIVERKIT
bool _dyld_shared_cache_contains_path(const char* path)
{
    checkTPROState();
    return gAPIs->_dyld_shared_cache_contains_path(path);
}

void* dlopen_from(const char* path, int mode, void* addressInCaller)
{
    checkTPROState();
    return gAPIs->dlopen_from(path, mode, addressInCaller);
}

void* dlopen_audited(const char* path, int mode)
{
    checkTPROState();
    return gAPIs->dlopen_audited(path, mode);
}
#endif // !TARGET_OS_DRIVERKIT

const struct mach_header* _dyld_get_prog_image_header()
{
    checkTPROState();
    return gAPIs->_dyld_get_prog_image_header();
}



//
// MARK: --- APIs added iOS 15, macOS 12 ---
//
void _dyld_visit_objc_classes(void (^callback)(const void* classPtr))
{
    checkTPROState();
    gAPIs->_dyld_visit_objc_classes(callback);
}

uint32_t _dyld_objc_class_count(void)
{
    checkTPROState();
    return gAPIs->_dyld_objc_class_count();
}

bool _dyld_objc_uses_large_shared_cache(void)
{
    checkTPROState();
    return gAPIs->_dyld_objc_uses_large_shared_cache();
}

struct _dyld_protocol_conformance_result _dyld_find_protocol_conformance(const void *protocolDescriptor,
                                                                         const void *metadataType,
                                                                         const void *typeDescriptor)
{
    checkTPROState();
    return gAPIs->_dyld_find_protocol_conformance(protocolDescriptor, metadataType, typeDescriptor);
}

struct _dyld_protocol_conformance_result _dyld_find_foreign_type_protocol_conformance(const void *protocol,
                                                                                      const char *foreignTypeIdentityStart,
                                                                                      size_t foreignTypeIdentityLength)
{
    checkTPROState();
    return gAPIs->_dyld_find_foreign_type_protocol_conformance(protocol, foreignTypeIdentityStart, foreignTypeIdentityLength);
}

uint32_t _dyld_swift_optimizations_version()
{
    checkTPROState();
    return gAPIs->_dyld_swift_optimizations_version();
}



//
// MARK: --- APIs added iOS 16, macOS 13 ---
//
const struct mach_header* _dyld_get_dlopen_image_header(void* handle)
{
    checkTPROState();
    return gAPIs->_dyld_get_dlopen_image_header(handle);
}

void _dyld_objc_register_callbacks(const _dyld_objc_callbacks* callbacks)
{
    checkTPROState();
    // Convert from the callbacks we are passed in to those which wrap the function
    // pointers to make them safe in dyld
    if ( callbacks->version == 4 ) {
        const _dyld_objc_callbacks_v4* v4 = (const _dyld_objc_callbacks_v4*)callbacks;

        dyld4::ObjCCallbacksV4 newCallbacks;
        newCallbacks.version = callbacks->version;
        newCallbacks.mapped = v4->mapped;
        newCallbacks.init = v4->init;
        newCallbacks.unmapped = v4->unmapped;
        newCallbacks.patches = v4->patches;
        gAPIs->_dyld_objc_register_callbacks(&newCallbacks);
    }
    else {
        dyld4::ObjCCallbacks newCallbacks;
        newCallbacks.version = callbacks->version;
        gAPIs->_dyld_objc_register_callbacks(&newCallbacks);
    }
}

bool _dyld_has_preoptimized_swift_protocol_conformances(const struct mach_header* mh)
{
    checkTPROState();
    return gAPIs->_dyld_has_preoptimized_swift_protocol_conformances(mh);
}

struct _dyld_protocol_conformance_result _dyld_find_protocol_conformance_on_disk(const void *protocolDescriptor,
                                                                                 const void *metadataType,
                                                                                 const void *typeDescriptor,
                                                                                 uint32_t flags)
{
    checkTPROState();
    return gAPIs->_dyld_find_protocol_conformance_on_disk(protocolDescriptor, metadataType, typeDescriptor, flags);
}

struct _dyld_protocol_conformance_result _dyld_find_foreign_type_protocol_conformance_on_disk(const void *protocol,
                                                                                              const char *foreignTypeIdentityStart,
                                                                                              size_t foreignTypeIdentityLength,
                                                                                              uint32_t flags)
{
    checkTPROState();
    return gAPIs->_dyld_find_foreign_type_protocol_conformance_on_disk(protocol, foreignTypeIdentityStart, foreignTypeIdentityLength, flags);
}

//
// MARK: --- APIs added iOS 15.x, macOS 12.x ---
//
void _dyld_dlopen_atfork_prepare()
{
    checkTPROState();
    gAPIs->_dyld_before_fork_dlopen();
}

void _dyld_dlopen_atfork_parent()
{
    checkTPROState();
    gAPIs->_dyld_after_fork_dlopen_parent();
}

void _dyld_dlopen_atfork_child()
{
    checkTPROState();
    gAPIs->_dyld_after_fork_dlopen_child();
}

//
// MARK: --- APIs added iOS 17.x, macOS 14.x ---
//
struct _dyld_section_info_result _dyld_lookup_section_info(const struct mach_header* mh,
                                                           _dyld_section_location_info_t locationHandle,
                                                           _dyld_section_location_kind kind)
{
    checkTPROState();
    return gAPIs->_dyld_lookup_section_info(mh, locationHandle, kind);
}


_dyld_pseudodylib_callbacks_handle _dyld_pseudodylib_register_callbacks(const struct _dyld_pseudodylib_callbacks* callbacks)
{
    checkTPROState();
    // Convert from the callbacks we are passed in to those which wrap the function
    // pointers to make them safe in dyld
    if ( callbacks->version == 1 ) {
        const auto* callbacks_v1 = (const _dyld_pseudodylib_callbacks_v1*)callbacks;

        dyld4::PseudoDylibRegisterCallbacksV1 newCallbacks;
        newCallbacks.version = callbacks->version;
        newCallbacks.dispose_error_message  = callbacks_v1->dispose_error_message;
        newCallbacks.initialize             = callbacks_v1->initialize;
        newCallbacks.deinitialize           = callbacks_v1->deinitialize;
        newCallbacks.lookup_symbols         = callbacks_v1->lookup_symbols;
        newCallbacks.lookup_address         = callbacks_v1->lookup_address;
        newCallbacks.find_unwind_sections   = callbacks_v1->find_unwind_sections;
        return gAPIs->_dyld_pseudodylib_register_callbacks(&newCallbacks);
    } else if ( callbacks->version == 2 ) {
        const auto* callbacks_v2 = (const _dyld_pseudodylib_callbacks_v2*)callbacks;

        dyld4::PseudoDylibRegisterCallbacksV2 newCallbacks;
        newCallbacks.version = callbacks->version;
        newCallbacks.dispose_string         = callbacks_v2->dispose_string;
        newCallbacks.initialize             = callbacks_v2->initialize;
        newCallbacks.deinitialize           = callbacks_v2->deinitialize;
        newCallbacks.lookup_symbols         = callbacks_v2->lookup_symbols;
        newCallbacks.lookup_address         = callbacks_v2->lookup_address;
        newCallbacks.find_unwind_sections   = callbacks_v2->find_unwind_sections;
        newCallbacks.loadable_at_path       = callbacks_v2->loadable_at_path;
        return gAPIs->_dyld_pseudodylib_register_callbacks(&newCallbacks);
    } else if ( callbacks->version == 3 ) {
        const auto* callbacks_v3 = (const _dyld_pseudodylib_callbacks_v3*)callbacks;

        dyld4::PseudoDylibRegisterCallbacksV3 newCallbacks;
        newCallbacks.version = callbacks->version;
        newCallbacks.dispose_string             = callbacks_v3->dispose_string;
        newCallbacks.initialize                 = callbacks_v3->initialize;
        newCallbacks.deinitialize               = callbacks_v3->deinitialize;
        newCallbacks.lookup_symbols             = callbacks_v3->lookup_symbols;
        newCallbacks.lookup_address             = callbacks_v3->lookup_address;
        newCallbacks.find_unwind_sections       = callbacks_v3->find_unwind_sections;
        newCallbacks.loadable_at_path           = callbacks_v3->loadable_at_path;
        newCallbacks.finalize_requested_symbols = callbacks_v3->finalize_requested_symbols;
        return gAPIs->_dyld_pseudodylib_register_callbacks(&newCallbacks);
    } else {
        dyld4::PseudoDylibRegisterCallbacks newCallbacks;
        newCallbacks.version = callbacks->version;
        return gAPIs->_dyld_pseudodylib_register_callbacks(&newCallbacks);
    }
}

void _dyld_pseudodylib_deregister_callbacks(_dyld_pseudodylib_callbacks_handle callbacks_handle)
{
    checkTPROState();
    gAPIs->_dyld_pseudodylib_deregister_callbacks(callbacks_handle);
}

_dyld_pseudodylib_handle _dyld_pseudodylib_register(
        void* addr, size_t size, _dyld_pseudodylib_callbacks_handle callbacks_handle, void* context)
{
    checkTPROState();
    return gAPIs->_dyld_pseudodylib_register(addr, size, callbacks_handle, context);
}

void _dyld_pseudodylib_deregister(_dyld_pseudodylib_handle pd_handle)
{
    checkTPROState();
    gAPIs->_dyld_pseudodylib_deregister(pd_handle);
}

bool _dyld_is_preoptimized_objc_image_loaded(uint16_t imageID)
{
    checkTPROState();
    return gAPIs->_dyld_is_preoptimized_objc_image_loaded(imageID);
}

void* _dyld_for_objc_header_opt_rw()
{
    checkTPROState();
    return gAPIs->_dyld_for_objc_header_opt_rw();
}

const void* _dyld_for_objc_header_opt_ro()
{
    checkTPROState();
    return gAPIs->_dyld_for_objc_header_opt_ro();
}

//
// MARK: --- APIs added iOS 18.x, macOS 15.x ---
//
bool _dyld_dlsym_blocked()
{
    checkTPROState();
    return gAPIs->_dyld_dlsym_blocked();
}

void _dyld_register_dlsym_notifier(void (*callback)(const char* symbolName))
{
    checkTPROState();
     gAPIs->_dyld_register_dlsym_notifier(callback);
}

const void* _dyld_get_swift_prespecialized_data()
{
    checkTPROState();
    return gAPIs->_dyld_get_swift_prespecialized_data();
}

bool _dyld_is_pseudodylib(void* handle)
{
    checkTPROState();
    return gAPIs->_dyld_is_pseudodylib(handle);
}

const void *_dyld_find_pointer_hash_table_entry(const void *table,
                                               const void *key1,
                                               size_t restKeysCount,
                                               const void **restKeys)
{
    checkTPROState();
    return gAPIs->_dyld_find_pointer_hash_table_entry(table, key1, restKeysCount, restKeys);
}

uint64_t dyld_get_program_sdk_version_token(void)
{
    checkTPROState();
    return gAPIs->dyld_get_program_sdk_version_token();
}

uint64_t dyld_get_program_minos_version_token(void) {
    checkTPROState();
    return gAPIs->dyld_get_program_minos_version_token();
}

dyld_platform_t dyld_version_token_get_platform(uint64_t token) {
    checkTPROState();
    return gAPIs->dyld_version_token_get_platform(token);
}

bool dyld_version_token_at_least(uint64_t token, dyld_build_version_t version)
{
    checkTPROState();
    return gAPIs->dyld_version_token_at_least(token, version);
}

void _dyld_stack_range(const void** stack_bottom, const void** stack_top)
{
    checkTPROState();
    gAPIs->_dyld_stack_range(stack_bottom, stack_top);
}

void _dyld_for_each_prewarming_range(void (*callback)(const void* base, size_t size))
{
    checkTPROState();
    gAPIs->_dyld_for_each_prewarming_range(callback);
}


//
// MARK: --- crt data symbols ---
//
int          NXArgc     = 0;
const char** NXArgv     = nullptr;
      char** environ    = nullptr;
const char*  __progname = nullptr;

//
// MARK: --- dyld stack ---
//
const void* _dyld_stack_top = nullptr;
const void* _dyld_stack_bottom = nullptr;
