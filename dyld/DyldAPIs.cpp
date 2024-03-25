/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <mach-o/dyld_priv.h>
#include <dlfcn.h>
#include <dlfcn_private.h>

#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <dirent.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <mach-o/dyld_images.h>
  #include <_simple.h>
  #include <libkern/OSAtomic.h>
  #include <_simple.h>
  #include <sys/errno.h>
  #include <malloc/malloc.h>
  #include <libc_private.h>
  #include <dyld/VersionMap.h>

  #include "dyld_process_info_internal.h"
  #include "OptimizerObjC.h"
#endif

#include "mach-o/dyld.h"
#include "mach-o/dyld_priv.h"
#include "MachOFile.h"
#include "Loader.h"
#include "Tracing.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "OptimizerSwift.h"
#include "PrebuiltObjC.h"
#include "PrebuiltSwift.h"
#include "objc-shared-cache.h"
#include "DyldAPIs.h"
#include "JustInTimeLoader.h"
#include "Utils.h"

#if !TARGET_OS_EXCLAVEKIT
// internal libc.a variable that needs to be reset during fork()
extern mach_port_t mach_task_self_;
#endif

using dyld3::MachOFile;
using dyld3::MachOLoaded;

extern const dyld3::MachOLoaded __dso_handle;

// only in macOS and deprecated
struct VIS_HIDDEN __NSObjectFileImage
{
    const char*               path        = nullptr;
    const void*               memSource   = nullptr;
    size_t                    memLength   = 0;
    const dyld3::MachOLoaded* loadAddress = nullptr;
    void*                     handle      = nullptr;
};

#if TARGET_OS_EXCLAVEKIT
#define unavailable_on_exclavekit() { log("dyld API not available: %s\n", __func__); abort(); };
#endif
namespace dyld4 {


RecursiveAutoLock::RecursiveAutoLock(RuntimeState& state, bool skip)
    : _runtimeLocks(state.locks), _skip(skip)
{
    if ( !_skip )
        _runtimeLocks.takeDlopenLockBeforeFork();
}

RecursiveAutoLock::~RecursiveAutoLock()
{
    if ( !_skip )
        _runtimeLocks.releaseDlopenLockInForkParent();
}

static void* handleFromLoader(const Loader* ldr, bool firstOnly)
{
    uintptr_t dyldStart  = (uintptr_t)&__dso_handle;

    // We need the low bit to store the "firstOnly" flag.  Loaders should be
    // at least 4-byte aligned though, so this is ok
    assert((((uintptr_t)ldr) & 1) == 0);
    uintptr_t flags = (firstOnly ? 1 : 0);
    void* handle = (void*)(((uintptr_t)ldr ^ dyldStart) | flags);

#if __has_feature(ptrauth_calls)
    if ( handle != nullptr )
        handle = ptrauth_sign_unauthenticated(handle, ptrauth_key_process_dependent_data, ptrauth_string_discriminator("dlopen"));
#endif

    return handle;
}

static const Loader* loaderFromHandle(void* h, bool& firstOnly)
{
    uintptr_t dyldStart  = (uintptr_t)&__dso_handle;

#if __has_feature(ptrauth_calls)
    if ( h != nullptr ) {
        // Note we don't use ptrauth_auth_data, as we don't want to crash on bad handles
        void* strippedHandle = ptrauth_strip(h, ptrauth_key_process_dependent_data);
        void* validHandle = ptrauth_sign_unauthenticated(strippedHandle, ptrauth_key_process_dependent_data, ptrauth_string_discriminator("dlopen"));
        if ( h == validHandle )
            h = strippedHandle;
    }
#endif

    firstOnly = (((uintptr_t)h) & 1);
    return (Loader*)((((uintptr_t)h) & ~1) ^ dyldStart);
}

bool APIs::validLoader(const Loader* maybeLoader)
{
    // ideally we'd walk the loaded array and validate this is a currently registered Loader
    // but that would require taking a lock, which may deadlock some apps
    if ( maybeLoader == nullptr )
        return false;
    // verifier loader is within the Allocator pool, or in a PrebuiltLoaderSet
    bool inDynamicPool    = this->persistentAllocator.owned(maybeLoader, sizeof(Loader));
#if TARGET_OS_EXCLAVEKIT
    bool inPrebuiltLoader = false;
#else
    bool inPrebuiltLoader = !inDynamicPool && this->inPrebuiltLoader(maybeLoader, sizeof(Loader));
#endif
    if ( !inDynamicPool && !inPrebuiltLoader )
        return false;
    // pointer into memory we own, so safe to dereference and see if it has magic header
    return maybeLoader->hasMagic();
}

const mach_header* APIs::_dyld_get_dlopen_image_header(void* handle)
{
    if ( config.log.apis )
        log("_dyld_get_dlopen_image_header(%p)\n", handle);
    if ( handle == RTLD_SELF ) {
        void* callerAddress = __builtin_return_address(0);
        if ( const Loader* caller = findImageContaining(callerAddress) )
            return caller->analyzer(*this);
    }
    if ( handle == RTLD_MAIN_ONLY ) {
        return mainExecutableLoader->analyzer(*this);
    }

    bool          firstOnly;
    const Loader* ldr = loaderFromHandle(handle, firstOnly);
    if ( !validLoader(ldr) ) {
        // if an invalid 'handle` passed in, return NULL
        return nullptr;
    }

    return ldr->analyzer(*this);
}

static const void* stripPointer(const void* ptr)
{
#if __has_feature(ptrauth_calls)
    return __builtin_ptrauth_strip(ptr, ptrauth_key_asia);
#else
    return ptr;
#endif
}

void APIs::_libdyld_initialize(const dyld4::LibSystemHelpers* helpers)
{
    // Since this called from libdyld`_dyld_initializer the allocator will be marked read only
    memoryManager.withWritableMemory([&]{
        // libSystem.dylib is being initialized, set helpers pointer
        this->setHelpers(helpers);

        // set up thread-local-variable and dlerror handling
        this->initialize();
    });
}

uint32_t APIs::_dyld_image_count()
{
    // NOTE: we are not taking the LoaderLock here
    // That is becuase count() on a array is a field read which is as
    // thread safe as this API is in general.
    uint32_t result = (uint32_t)loaded.size();
    if ( config.log.apis )
        log("_dyld_image_count() => %d\n", result);
    return result;
}

static uint32_t normalizeImageIndex(const ProcessConfig& config, uint32_t index)
{
#if BUILDING_DYLD && TARGET_OS_OSX && __x86_64__
    // some old macOS apps assume index of zero is always the main executable even when dylibs are inserted, so permute order
    uint32_t insertCount = config.pathOverrides.insertedDylibCount();
    if ( (insertCount != 0) && (config.process.platform == dyld3::Platform::macOS) && (config.process.mainExecutableMinOSVersion < 0x0000C0000) ) {
        // special case index==0 to map to the main executable
        if ( index == 0 )
            return insertCount;
        // shift inserted dylibs
        if ( index <= insertCount )
            return index-1;
    }
#endif
    return index;
}

const mach_header* APIs::_dyld_get_image_header(uint32_t imageIndex)
{
    __block const mach_header* result = 0;
    locks.withLoadersReadLock(^{
        if ( imageIndex < loaded.size() )
            result = loaded[normalizeImageIndex(config, imageIndex)]->loadAddress(*this);
    });
    if ( config.log.apis )
        log("_dyld_get_image_header(%u) => %p\n", imageIndex, result);
    return result;
}

intptr_t APIs::_dyld_get_image_slide(const mach_header* mh)
{
    if ( config.log.apis )
        log("_dyld_get_image_slide(%p)", mh);
#if !TARGET_OS_EXCLAVEKIT
    intptr_t result = 0;
    const MachOLoaded* ml = (MachOLoaded*)mh;
    if ( ml->hasMachOMagic() ) {
        if ( DyldSharedCache::inDyldCache(config.dyldCache.addr, ml) )
            result = config.dyldCache.slide;
        else
            result = ml->getSlide();
    }
    if ( config.log.apis )
        log(" => 0x%lX\n", result);
    return result;
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

intptr_t APIs::_dyld_get_image_vmaddr_slide(uint32_t imageIndex)
{
    __block intptr_t result = 0;
    locks.withLoadersReadLock(^{
        if ( imageIndex < loaded.size() )
            result = loaded[normalizeImageIndex(config, imageIndex)]->loadAddress(*this)->getSlide();
    });
    if ( config.log.apis )
        log("_dyld_get_image_vmaddr_slide(%u) => 0x%lX\n", imageIndex, result);
    return result;
}

const char* APIs::_dyld_get_image_name(uint32_t imageIndex)
{
    __block const char* result = 0;
    locks.withLoadersReadLock(^{
        if ( imageIndex < loaded.size() )
            result = loaded[normalizeImageIndex(config, imageIndex)]->path();
    });
    if ( config.log.apis )
        log("_dyld_get_image_name(%u) => %s\n", imageIndex, result);
    return result;
}

static bool nameMatch(const char* installName, const char* libraryName)
{
    const char* leafName = strrchr(installName, '/');
    if ( leafName == NULL )
        leafName = installName;
    else
        leafName++;

    // -framework case is exact match of leaf name
    if ( strcmp(leafName, libraryName) == 0 )
        return true;

    // -lxxx case: leafName must match "lib" <libraryName> ["." ?] ".dylib"
    size_t leafNameLen    = strlen(leafName);
    size_t libraryNameLen = strlen(libraryName);
    if ( leafNameLen < (libraryNameLen + 9) )
        return false;
    if ( strncmp(leafName, "lib", 3) != 0 )
        return false;
    if ( strcmp(&leafName[leafNameLen - 6], ".dylib") != 0 )
        return false;
    if ( strncmp(&leafName[3], libraryName, libraryNameLen) != 0 )
        return false;
    return (leafName[libraryNameLen + 3] == '.');
}

int32_t APIs::NSVersionOfLinkTimeLibrary(const char* libraryName)
{
    __block int32_t result = -1;
    mainExecutableLoader->loadAddress(*this)->forEachDependentDylib(^(const char* loadPath, bool, bool, bool, uint32_t compatVersion, uint32_t currentVersion, bool& stop) {
        if ( nameMatch(loadPath, libraryName) )
            result = currentVersion;
    });
    if ( config.log.apis )
        log("NSVersionOfLinkTimeLibrary(%s) =>0x%08X\n", libraryName, result);
    return result;
}

int32_t APIs::NSVersionOfRunTimeLibrary(const char* libraryName)
{
    __block int32_t result = -1;
    locks.withLoadersReadLock(^{
        for ( const dyld4::Loader* image : loaded ) {
            const MachOLoaded* ml = image->loadAddress(*this);
            const char*        installName;
            uint32_t           currentVersion;
            uint32_t           compatVersion;
            if ( ml->getDylibInstallName(&installName, &compatVersion, &currentVersion) && nameMatch(installName, libraryName) ) {
                result = currentVersion;
                break;
            }
        }
    });
    if ( config.log.apis )
        log("NSVersionOfRunTimeLibrary(%s) => 0x%08X\n", libraryName, result);
    return result;
}

uint32_t APIs::dyld_get_program_sdk_watch_os_version()
{
    __block uint32_t retval       = 0;
    __block bool     versionFound = false;
    forEachImageVersion(config.process.mainExecutable, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( MachOFile::basePlatform((dyld3::Platform)platform) == dyld3::Platform::watchOS ) {
            versionFound = true;
            retval       = sdk_version;
        }
    });
    if ( config.log.apis )
        log("dyld_get_program_sdk_watch_os_version() => 0x%08X\n", retval);
    return retval;
}

uint32_t APIs::dyld_get_program_min_watch_os_version()
{
    __block uint32_t retval       = 0;
    __block bool     versionFound = false;
    forEachImageVersion(config.process.mainExecutable, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( MachOFile::basePlatform((dyld3::Platform)platform) == dyld3::Platform::watchOS ) {
            versionFound = true;
            retval       = min_version;
        }
    });
    if ( config.log.apis )
        log("dyld_get_program_min_watch_os_version() => 0x%08X\n", retval);
    return retval;
}

uint32_t APIs::dyld_get_program_sdk_bridge_os_version()
{
    __block uint32_t retval       = 0;
    __block bool     versionFound = false;
    forEachImageVersion(config.process.mainExecutable, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( MachOFile::basePlatform((dyld3::Platform)platform) == dyld3::Platform::bridgeOS ) {
            versionFound = true;
            retval       = sdk_version;
        }
    });
    if ( config.log.apis )
        log("dyld_get_program_sdk_bridge_os_version() => 0x%08X\n", retval);
    return retval;
}

uint32_t APIs::dyld_get_program_min_bridge_os_version()
{
    __block uint32_t retval       = 0;
    __block bool     versionFound = false;
    forEachImageVersion(config.process.mainExecutable, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( MachOFile::basePlatform((dyld3::Platform)platform) == dyld3::Platform::bridgeOS ) {
            versionFound = true;
            retval       = min_version;
        }
    });
    if ( config.log.apis )
        log("dyld_get_program_min_bridge_os_version() => 0x%08X\n", retval);
    return retval;
}

//
// Returns the sdk version (encode as nibble XXXX.YY.ZZ) that the
// specified binary was built against.
//
// First looks for LC_VERSION_MIN_* in binary and if sdk field is
// not zero, return that value.
// Otherwise, looks for the libSystem.B.dylib the binary linked
// against and uses a table to convert that to an sdk version.
//
uint32_t APIs::getSdkVersion(const mach_header* mh)
{
    __block bool     versionFound = false;
    __block uint32_t retval       = 0;
    forEachImageVersion(mh, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( platform == (dyld_platform_t)config.process.platform ) {
            versionFound = true;
            switch ( MachOFile::basePlatform((dyld3::Platform)platform)  ) {
                case dyld3::Platform::bridgeOS:
                    retval = sdk_version + 0x00090000;
                    return;
                case dyld3::Platform::watchOS:
                    retval = sdk_version + 0x00070000;
                    return;
                default:
                    retval = sdk_version;
                    return;
            }
        }
        else if ( platform == PLATFORM_IOSSIMULATOR && (dyld_platform_t)config.process.platform == PLATFORM_IOSMAC ) {
            //FIXME bringup hack
            versionFound = true;
            retval       = 0x000C0000;
        }
    });

    return retval;
}

uint32_t APIs::dyld_get_sdk_version(const mach_header* mh)
{
    uint32_t result = getSdkVersion(mh);
    if ( config.log.apis )
        log("dyld_get_sdk_version(%p) => 0x%08X\n", mh, result);
    return result;
}

uint32_t APIs::dyld_get_program_sdk_version()
{
    uint32_t result = getSdkVersion(config.process.mainExecutable);
    if ( config.log.apis )
        log("dyld_get_program_sdk_version() => 0x%08X\n", result);
    return result;
}

uint32_t APIs::dyld_get_min_os_version(const mach_header* mh)
{
    __block bool     versionFound = false;
    __block uint32_t retval       = 0;
    forEachImageVersion(mh, ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if ( versionFound )
            return;

        if ( platform == (dyld_platform_t)config.process.platform ) {
            versionFound = true;
            switch ( MachOFile::basePlatform((dyld3::Platform)platform) ) {
                case dyld3::Platform::bridgeOS:
                    retval = min_version + 0x00090000;
                    return;
                case dyld3::Platform::watchOS:
                    retval = min_version + 0x00070000;
                    return;
                default:
                    retval = min_version;
                    return;
            }
        }
        else if ( platform == PLATFORM_IOSSIMULATOR && (dyld_platform_t)config.process.platform == PLATFORM_IOSMAC ) {
            //FIXME bringup hack
            versionFound = true;
            retval       = 0x000C0000;
        }
    });
    if ( config.log.apis )
        log("dyld_get_min_os_version(%p) => 0x%08X\n", mh, retval);
    return retval;
}

dyld_platform_t APIs::dyld_get_active_platform(void)
{
    dyld_platform_t result = (dyld_platform_t)config.process.platform;
    if ( config.log.apis )
        log("dyld_get_active_platform() => %d\n", result);
    return result;
}

dyld_platform_t APIs::dyld_get_base_platform(dyld_platform_t platform)
{
    dyld_platform_t result = (dyld_platform_t)MachOFile::basePlatform((dyld3::Platform)platform);
    if ( config.log.apis )
        log("dyld_get_base_platform(%d) => %d\n", platform, result);
    return result;
}

bool APIs::dyld_is_simulator_platform(dyld_platform_t platform)
{
    bool result = MachOFile::isSimulatorPlatform((dyld3::Platform)platform);
    if ( config.log.apis )
        log("dyld_is_simulator_platform(%d) => %d\n", platform, result);
    return result;
}

dyld_build_version_t APIs::mapFromVersionSet(dyld_build_version_t versionSet)
{
#if TARGET_OS_EXCLAVEKIT
    return { 0, 0 }; //FIXME
#else
    if ( versionSet.platform != 0xffffffff )
        return versionSet;
    const dyld3::VersionSetEntry* foundEntry = nullptr;
    for (const dyld3::VersionSetEntry& entry : dyld3::sVersionMap) {
        if ( entry.set >= versionSet.version ) {
            foundEntry = &entry;
            break;
        }
    }
    if ( foundEntry == nullptr ) {
        return { .platform = 0, .version = 0 };
    }
    switch ( MachOFile::basePlatform(config.process.platform) ) {
        case dyld3::Platform::macOS:
            return { .platform = PLATFORM_MACOS,    .version = foundEntry->macos };
        case dyld3::Platform::iOS:
            return { .platform = PLATFORM_IOS,      .version = foundEntry->ios };
        case dyld3::Platform::watchOS:
            return { .platform = PLATFORM_WATCHOS,  .version = foundEntry->watchos };
        case dyld3::Platform::tvOS:
            return { .platform = PLATFORM_TVOS,     .version = foundEntry->tvos };
        case dyld3::Platform::bridgeOS:
            return { .platform = PLATFORM_BRIDGEOS, .version = foundEntry->bridgeos };
        default:
            return { .platform = (dyld_platform_t)MachOFile::basePlatform(config.process.platform), .version = 0 };
    }
#endif
}

bool APIs::dyld_sdk_at_least(const mach_header* mh, dyld_build_version_t atLeast)
{
    dyld_build_version_t concreteAtLeast = mapFromVersionSet(atLeast);
    __block bool retval = false;
    forEachImageVersion(mh, ^(dyld_platform_t imagePlatform, uint32_t imageSDK, uint32_t imageOS) {
        if ( MachOFile::basePlatform((dyld3::Platform)imagePlatform) == MachOFile::basePlatform((dyld3::Platform)concreteAtLeast.platform) ) {
            if ( MachOFile::basePlatform((dyld3::Platform)imagePlatform) == dyld3::Platform::unknown )
                return;
            if ( imageSDK >= concreteAtLeast.version )
                retval = true;
        }
    });
    if ( config.log.apis )
        log("dyld_sdk_at_least(%p, <%d,0x%08X>) => %d\n", mh, atLeast.platform, atLeast.version, retval);
    return retval;
}

bool APIs::dyld_minos_at_least(const mach_header* mh, dyld_build_version_t atLeast)
{
    dyld_build_version_t concreteAtLeast = mapFromVersionSet(atLeast);
    __block bool         retval          = false;
    forEachImageVersion(mh, ^(dyld_platform_t imagePlatform, uint32_t imageSDK, uint32_t imageMinOS) {
        if ( MachOFile::basePlatform((dyld3::Platform)imagePlatform) == MachOFile::basePlatform((dyld3::Platform)concreteAtLeast.platform) ) {
            if ( MachOFile::basePlatform((dyld3::Platform)imagePlatform) == dyld3::Platform::unknown )
                return;
            if ( imageMinOS >= concreteAtLeast.version )
                retval = true;
        }
    });
    if ( config.log.apis )
        log("dyld_minos_at_least(%p, <%d,0x%08X>) => %d\n", mh, atLeast.platform, atLeast.version, retval);
    return retval;
}

__attribute__((aligned(64)))
bool APIs::dyld_program_minos_at_least (dyld_build_version_t version) {
//    contract(config.process.mainExecutableMinOSVersionSet != 0);
//    contract(config.process.mainExecutableMinOSVersion != 0);
//    contract((dyld_platform_t)config.process.basePlatform != 0);

    uint32_t currentVersion = 0;
    bool defaultResult = true;
    if ( config.process.basePlatform == dyld3::Platform::unknown ) {
        defaultResult = false;
    }
    if (version.platform == 0xffffffff) {
        currentVersion = config.process.mainExecutableMinOSVersionSet;
    } else if (version.platform == (dyld_platform_t)config.process.basePlatform) {
        currentVersion = config.process.mainExecutableMinOSVersion;
    } else if (version.platform == (dyld_platform_t)config.process.platform) {
        currentVersion = config.process.mainExecutableMinOSVersion;
    } else {
        // Hack
        // If it is not the specific platform or a version set, we should return false.
        // If we explicitly return false here the compiler will emit a branch, so instead we change a value
        // so that through a series of conditional selects we always return false.
        defaultResult = false;
    }
    return ( currentVersion >= version.version ) ? defaultResult : false;
}

__attribute__((aligned(64)))
bool APIs::dyld_program_sdk_at_least (dyld_build_version_t version) {
//    contract(config.process.mainExecutableSDKVersionSet != 0);
//    contract(config.process.mainExecutableSDKVersion != 0);
//    contract((dyld_platform_t)config.process.basePlatform != 0);

    uint32_t currentVersion = 0;
    bool defaultResult = true;
    if ( config.process.basePlatform == dyld3::Platform::unknown ) {
        defaultResult = false;
    }
    if (version.platform == 0xffffffff) {
        currentVersion = config.process.mainExecutableSDKVersionSet;
    } else if (version.platform == (dyld_platform_t)config.process.basePlatform) {
        currentVersion = config.process.mainExecutableSDKVersion;
    } else if (version.platform == (dyld_platform_t)config.process.platform) {
        currentVersion = config.process.mainExecutableSDKVersion;
    } else {
        // Hack
        // If it is not the specific platform or a version set, we should return false.
        // If we explicitly return false here the compiler will emit a branch, so instead we change a value
        // so that through a series of conditional selects we always return false.
        defaultResult = false;
    }
    return ( currentVersion >= version.version ) ? defaultResult : false;
}

uint32_t APIs::linkedDylibVersion(const MachOFile* mf, const char* installname)
{
    __block uint32_t retval = 0;
    mf->forEachDependentDylib(^(const char* loadPath, bool, bool, bool, uint32_t compatVersion, uint32_t currentVersion, bool& stop) {
        if ( strcmp(loadPath, installname) == 0 ) {
            retval = currentVersion;
            stop   = true;
        }
    });
    return retval;
}

#define PACKED_VERSION(major, minor, tiny) ((((major)&0xffff) << 16) | (((minor)&0xff) << 8) | ((tiny)&0xff))

uint32_t APIs::deriveVersionFromDylibs(const MachOFile* mf)
{
    // This is a binary without a version load command, we need to infer things
    struct DylibToOSMapping
    {
        uint32_t dylibVersion;
        uint32_t osVersion;
    };
    uint32_t linkedVersion = 0;
#if TARGET_OS_OSX
    linkedVersion                                  = linkedDylibVersion(mf, "/usr/lib/libSystem.B.dylib");
    static const DylibToOSMapping versionMapping[] = {
        { PACKED_VERSION(88, 1, 3), 0x000A0400 },
        { PACKED_VERSION(111, 0, 0), 0x000A0500 },
        { PACKED_VERSION(123, 0, 0), 0x000A0600 },
        { PACKED_VERSION(159, 0, 0), 0x000A0700 },
        { PACKED_VERSION(169, 3, 0), 0x000A0800 },
        { PACKED_VERSION(1197, 0, 0), 0x000A0900 },
        { PACKED_VERSION(0, 0, 0), 0x000A0900 }
        // We don't need to expand this table because all recent
        // binaries have LC_VERSION_MIN_ load command.
    };
#elif TARGET_OS_IOS
    linkedVersion                                  = linkedDylibVersion(mf, "/System/Library/Frameworks/Foundation.framework/Foundation");
    static const DylibToOSMapping versionMapping[] = {
        { PACKED_VERSION(678, 24, 0), 0x00020000 },
        { PACKED_VERSION(678, 26, 0), 0x00020100 },
        { PACKED_VERSION(678, 29, 0), 0x00020200 },
        { PACKED_VERSION(678, 47, 0), 0x00030000 },
        { PACKED_VERSION(678, 51, 0), 0x00030100 },
        { PACKED_VERSION(678, 60, 0), 0x00030200 },
        { PACKED_VERSION(751, 32, 0), 0x00040000 },
        { PACKED_VERSION(751, 37, 0), 0x00040100 },
        { PACKED_VERSION(751, 49, 0), 0x00040200 },
        { PACKED_VERSION(751, 58, 0), 0x00040300 },
        { PACKED_VERSION(881, 0, 0), 0x00050000 },
        { PACKED_VERSION(890, 1, 0), 0x00050100 },
        { PACKED_VERSION(992, 0, 0), 0x00060000 },
        { PACKED_VERSION(993, 0, 0), 0x00060100 },
        { PACKED_VERSION(1038, 14, 0), 0x00070000 },
        { PACKED_VERSION(0, 0, 0), 0x00070000 }
        // We don't need to expand this table because all recent
        // binaries have LC_VERSION_MIN_ load command.
    };
#else
    static const DylibToOSMapping versionMapping[] = {};
#endif
    if ( linkedVersion != 0 ) {
        uint32_t lastOsVersion = 0;
        for ( const DylibToOSMapping* p = versionMapping;; ++p ) {
            if ( p->dylibVersion == 0 ) {
                return p->osVersion;
            }
            if ( linkedVersion < p->dylibVersion ) {
                return lastOsVersion;
            }
            lastOsVersion = p->osVersion;
        }
    }
    return 0;
}

// assumes mh has already been validated
void APIs::forEachPlatform(const MachOFile* mf, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version))
{
    __block bool lcFound = false;
    mf->forEachSupportedPlatform(^(dyld3::Platform platform, uint32_t minOS, uint32_t sdk) {
        lcFound = true;
        // If SDK field is empty then derive the value from library linkages
        if ( sdk == 0 ) {
            sdk = deriveVersionFromDylibs(mf);
        }
        callback((const dyld_platform_t)platform, sdk, minOS);
    });

    // No load command was found, so again, fallback to deriving it from library linkages
    if ( !lcFound ) {
#if TARGET_OS_IOS
    #if __x86_64__ || __x86__
        dyld_platform_t platform = PLATFORM_IOSSIMULATOR;
    #else
        dyld_platform_t platform = PLATFORM_IOS;
    #endif
#elif TARGET_OS_OSX
        dyld_platform_t platform = PLATFORM_MACOS;
#else
        dyld_platform_t platform = 0;
#endif
        uint32_t derivedVersion = deriveVersionFromDylibs(mf);
        if ( platform != 0 && derivedVersion != 0 ) {
            callback(platform, derivedVersion, 0);
        }
    }
}

void APIs::dyld_get_image_versions(const mach_header* mh, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version))
{
    if ( config.log.apis )
        log("dyld_get_image_versions(%p, %p)\n", mh, callback);
    forEachImageVersion(mh, callback);
}

void APIs::forEachImageVersion(const mach_header* mh, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version))
{
#if !TARGET_OS_EXCLAVEKIT
    Diagnostics      diag;
    const MachOFile* mf = (MachOFile*)mh;

    if ( mh == config.process.mainExecutable ) {
        // Special case main executable, that info is store in ProcessConfig
        callback((dyld_platform_t)config.process.platform, config.process.mainExecutableSDKVersion, config.process.mainExecutableMinOSVersion);
    }
    else if ( DyldSharedCache::inDyldCache(config.dyldCache.addr, mf) ) {
        // If the image is in the shared cache, then all versions OS and SDK versions are the same
       callback((dyld_platform_t)config.dyldCache.platform, config.dyldCache.osVersion, config.dyldCache.osVersion);
    }
    else if ( mf->isMachO(diag, mh->sizeofcmds + sizeof(mach_header_64)) ) {
        // look for LC_BUILD_VERSION or derive from dylib info
        this->forEachPlatform(mf, callback);
    }
#else
    abort();
#endif // !TARGET_OS_EXCLAVEKIT
}

uint32_t APIs::dyld_get_program_min_os_version()
{
    return dyld_get_min_os_version(config.process.mainExecutable);
}

bool APIs::_dyld_get_image_uuid(const mach_header* mh, uuid_t uuid)
{
    if ( config.log.apis )
        log("_dyld_get_image_uuid(%p, %p)\n", mh, uuid);
    const MachOFile* mf = (MachOFile*)mh;
    return (mf->hasMachOMagic() && mf->getUuid(uuid));
}

int APIs::_NSGetExecutablePath(char* buf, uint32_t* bufsize)
{
    if ( config.log.apis )
        log("_NSGetExecutablePath(%p, %p)\n", buf, bufsize);
    const char* path     = config.process.mainExecutablePath;
    if ( config.process.platform == dyld3::Platform::macOS )
        path = config.process.mainUnrealPath; // Note: this is not real-path. It may be a symlink rdar://74451681
    size_t      pathSize = strlen(path) + 1;
    if ( *bufsize >= pathSize ) {
        strlcpy(buf, path, *bufsize);
        return 0;
    }
    *bufsize = (uint32_t)pathSize;
    return -1;
}

void APIs::_dyld_register_func_for_add_image(void (*func)(const mach_header* mh, intptr_t slide))
{
    if ( config.log.apis )
        log("_dyld_register_func_for_add_image(%p)\n", func);
#if !TARGET_OS_EXCLAVEKIT
    // callback about already loaded images
    locks.withLoadersReadLock(^{
        // rdar://102114011 make copy of mach_headers and slides in case 'func' calls dlopen/dlclose
        unsigned           count = (unsigned)loaded.size();
        const mach_header* mhs[count];
        intptr_t           slides[count];
        for ( unsigned i = 0; i < count; ++i ) {
            const MachOLoaded* ml = loaded[i]->loadAddress(*this);
            mhs[i]    = ml;
            slides[i] = loaded[i]->dylibInDyldCache ? config.dyldCache.slide : ml->getSlide();
        }
        for ( unsigned i = 0; i < count; ++i ) {
            if ( config.log.notifications )
                log("add notifier %p called with mh=%p\n", func, mhs[i]);
            func(mhs[i], slides[i]);
        }
    });

    // add to list of functions to call about future loads
    const Loader* callbackLoader = this->findImageContaining((void*)func);
    locks.withNotifiersWriteLock(memoryManager, ^() {
        addNotifyAddFunc(callbackLoader, func);
    });
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_register_func_for_remove_image(void (*func)(const mach_header* mh, intptr_t slide))
{
    if ( config.log.apis )
        log("_dyld_register_func_for_remove_image(%p)\n", func);
#if !TARGET_OS_EXCLAVEKIT
    // add to list of functions to call about future unloads
    const Loader* callbackLoader = this->findImageContaining((void*)func);
    locks.withNotifiersWriteLock(memoryManager, ^() {
        addNotifyRemoveFunc(callbackLoader, func);
    });
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

// FIXME: Remove this once libobjc moves to _dyld_objc_register_callbacks()
void APIs::_dyld_objc_notify_register(_dyld_objc_notify_mapped   mapped,
                                      _dyld_objc_notify_init     init,
                                      _dyld_objc_notify_unmapped unmapped)
{
    halt("_dyld_objc_notify_register is unsupported");
}

void APIs::_dyld_objc_register_callbacks(const _dyld_objc_callbacks* callbacks)
{
    if ( config.log.apis ) {
        void** p = (void**)callbacks;
        log("_dyld_objc_register_callbacks(%lu, %p, %p, %p, %p)\n", callbacks->version, p[1], p[2], p[31], p[4]);
    }

    if ( callbacks->version == 1 ) {
        halt("_dyld_objc_register_callbacks v1 is no longer supported");
    }
    else if ( callbacks->version == 2 ) {
        const _dyld_objc_callbacks_v2* v2 = (const _dyld_objc_callbacks_v2*)callbacks;
        setObjCNotifiers(v2->unmapped, v2->patches, v2->mapped, v2->init, nullptr);
    }
    else if ( callbacks->version == 3 ) {
        const _dyld_objc_callbacks_v3* v3 = (const _dyld_objc_callbacks_v3*)callbacks;
        setObjCNotifiers(v3->unmapped, v3->patches, nullptr, v3->init, v3->mapped);
    }
    else {
        halt("_dyld_objc_register_callbacks unknown version");
    }

#if SUPPORT_PREBUILTLOADERS
    // If we have prebuilt loaders, then the objc optimisations may hide duplicate classes from libobjc.
    // We need to print the same warnings libobjc would have.
    if ( const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet() )
        mainSet->logDuplicateObjCClasses(*this);
#endif
}

bool APIs::findImageMappedAt(const void* addr, const MachOLoaded** ml, bool* neverUnloads, const char** path, const void** segAddr, uint64_t* segSize, uint8_t* segPerms, const Loader** loader)
{
    __block bool result = false;

    bool inSharedCache = false;
#if !TARGET_OS_EXCLAVEKIT
    // if address is in cache, do fast search of TEXT segments in cache
    const DyldSharedCache* dyldCache = config.dyldCache.addr;
    if ( (dyldCache != nullptr) && (addr > dyldCache) ) {
        if ( addr < (void*)((uint8_t*)dyldCache + dyldCache->mappedSize()) ) {
            inSharedCache = true;

            uint64_t cacheSlide       = (uint64_t)dyldCache - dyldCache->unslidLoadAddress();
            uint64_t unslidTargetAddr = (uint64_t)addr - cacheSlide;

            // Find where we are in the cache.  The permissions can be used to then do a faster check later
            __block uint32_t sharedCacheRegionProt = 0;
            dyldCache->forEachRange(^(const char *mappingName, uint64_t unslidVMAddr, uint64_t vmSize,
                                      uint32_t cacheFileIndex, uint64_t fileOffset, uint32_t initProt, uint32_t maxProt, bool& stopRange) {
                if ( (unslidVMAddr <= unslidTargetAddr) && (unslidTargetAddr < (unslidVMAddr + vmSize)) ) {
                    sharedCacheRegionProt = initProt;
                    stopRange = true;
                }
            });

#if !TARGET_OS_SIMULATOR
            // rdar://76406035 (simulator cache paths need prefix)
            if ( sharedCacheRegionProt == (VM_PROT_READ | VM_PROT_EXECUTE) ) {
                dyldCache->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const unsigned char* dylibUUID, const char* installName, bool& stop) {
                    if ( (loadAddressUnslid <= unslidTargetAddr) && (unslidTargetAddr < loadAddressUnslid + textSegmentSize) ) {
                        if ( ml != nullptr )
                            *ml = (MachOLoaded*)(loadAddressUnslid + cacheSlide);
                        if ( neverUnloads != nullptr )
                            *neverUnloads = true;
                        if ( path != nullptr )
                            *path = installName;
                        if ( segAddr != nullptr )
                            *segAddr = (void*)(loadAddressUnslid + cacheSlide);
                        if ( segSize != nullptr )
                            *segSize = textSegmentSize;
                        if ( segPerms != nullptr )
                            *segPerms = VM_PROT_READ | VM_PROT_EXECUTE;
                        if ( loader )
                          *loader = nullptr;
                        stop   = true;
                        result = true;
                    }
                });
                if ( result )
                    return result;
            }
#endif // TARGET_OS_SIMULATOR
        }
    }

    // next check if address is in a permanent range
    const Loader*   ldr;
    uint8_t         perms;
    if ( this->inPermanentRange((uintptr_t)addr, (uintptr_t)addr + 1, &perms, &ldr) ) {
        if ( ml != nullptr )
            *ml = ldr->loadAddress(*this);
        if ( neverUnloads != nullptr )
            *neverUnloads = true;
        if ( path != nullptr )
            *path = ldr->path();
        if ( (segAddr != nullptr) || (segSize != nullptr) ) {
            // only needed by _dyld_images_for_addresses()
            const void* ldrSegAddr;
            uint64_t    ldrSegSize;
            uint8_t     ldrPerms;
            if ( ldr->contains(*this, addr, &ldrSegAddr, &ldrSegSize, &ldrPerms) ) {
                if ( segAddr != nullptr )
                    *segAddr = ldrSegAddr;
                if ( segSize != nullptr )
                    *segSize = ldrSegSize;
            }
        }
        if ( segPerms != nullptr )
            *segPerms = perms;
        if ( loader )
          *loader = ldr;
        return true;
    }
#endif // !TARGET_OS_EXCLAVEKIT

    // slow path - search image list
    locks.withLoadersReadLock(^{
        // If we found a cache range for this address, then we know we only need to look in loaders for the cache
        for ( const Loader* image : loaded ) {
            if ( image->dylibInDyldCache != inSharedCache )
                continue;
            const void* sgAddr;
            uint64_t    sgSize;
            uint8_t     sgPerm;
            if ( image->contains(*this, addr, &sgAddr, &sgSize, &sgPerm) ) {
                if ( ml != nullptr )
                    *ml = image->loadAddress(*this);
                if ( neverUnloads != nullptr )
                    *neverUnloads = image->neverUnload;
                if ( path != nullptr )
                    *path = image->path();
                if ( segAddr != nullptr )
                    *segAddr = sgAddr;
                if ( segSize != nullptr )
                    *segSize = sgSize;
                if ( segPerms != nullptr )
                    *segPerms = sgPerm;
                if ( loader )
                    *loader = image;
                result = true;
                return;
            }
        }
    });

    // [NSBundle bundleForClass] will call dyld_image_path_containing_address(cls) with the shared
    // cache version of the class, not the one in the root.  We need to return the path to the root
    // so that resources can be found relative to the bundle
    if ( !result && !this->patchedObjCClasses.empty() ) {
        for ( const InterposeTupleAll& tuple : this->patchedObjCClasses ) {
            if ( tuple.replacement == (uintptr_t)addr )
                return this->findImageMappedAt((void*)tuple.replacee, ml, neverUnloads, path, segAddr, segSize, segPerms, loader);
        }
    }

    return result;
}

const mach_header* APIs::dyld_image_header_containing_address(const void* addr)
{
    const MachOLoaded* ml = nullptr;
    this->findImageMappedAt(stripPointer(addr), &ml);
    if ( config.log.apis )
        log("dyld_image_header_containing_address(%p) =>%p\n", addr, ml);
    return ml;
}

const char* APIs::dyld_image_path_containing_address(const void* addr)
{
    const MachOLoaded* ml;
    bool               neverUnloads;
    const char*        path = nullptr;
    this->findImageMappedAt(stripPointer(addr), &ml, &neverUnloads, &path);
    if ( config.log.apis )
        log("dyld_image_path_containing_address(%p) => '%s'\n", addr, path);
    return path;
}

bool APIs::_dyld_is_memory_immutable(const void* addr, size_t length)
{
    // NOTE: this is all done without the dyld lock
    // because this SPI is called from many threads in frameworks that
    // could deadlock if the dyld lock was held here

    // if address is in cache, only TEXT is immutable
    __block bool result = false;
#if !TARGET_OS_EXCLAVEKIT
    const DyldSharedCache* dyldCache = config.dyldCache.addr;
    if ( (dyldCache != nullptr) && (addr > dyldCache) ) {
        if ( addr < (void*)((uint8_t*)dyldCache + dyldCache->mappedSize()) ) {
            dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
                cache->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size,
                                            uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
                    if ( (addr > content) && (((uint8_t*)addr + length) < ((uint8_t*)content + size)) ) {
                        // Note: in cache __DATA_CONST has initProt=1 and initProt=3
                        // we don't want __DATA_CONST to be considered immutable, so we check maxProt
                        bool writable = (maxProt & VM_PROT_WRITE);
                        if ( !writable )
                            result = true;
                    }
                });
            });
        }
    }

    if ( !result ) {
        // check if address is in a permanently loaded image
        const Loader*   ldr;
        uint8_t         perms;
        if ( this->inPermanentRange((uintptr_t)addr, (uintptr_t)addr + length, &perms, &ldr) ) {
            bool writable = (perms & VM_PROT_WRITE);
            result = !writable;
        }
    }
#endif // !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_is_memory_immutable(%p, %lu) => %d\n", addr, length, result);
    return result;
}

int APIs::dladdr(const void* addr, Dl_info* info)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLADDR, (uint64_t)addr, 0, 0);
    if ( config.log.apis )
        log("dladdr(%p, %p)\n", addr, info);
    // <rdar://problem/42171466> calling dladdr(xx,NULL) crashes
    if ( info == nullptr )
        return 0; // failure

    addr = stripPointer(addr);

    int                result = 0;
    bool               neverUnloads;
    const MachOLoaded* ml;
    const Loader*      loader;
    const char*        path;
    const void*        segAddr;
    uint64_t           segSize;
    uint8_t            segPerms;
    if ( findImageMappedAt(addr, &ml, &neverUnloads, &path, &segAddr, &segSize, &segPerms, &loader) ) {
        info->dli_fname = path;
        info->dli_fbase = (void*)ml;

        uint64_t symbolAddr;
        if ( addr == info->dli_fbase ) {
            // special case lookup of header
            info->dli_sname = "__dso_handle";
            info->dli_saddr = info->dli_fbase;
        }
        else if ( ml->findClosestSymbol((long)addr, &(info->dli_sname), &symbolAddr) ) {
            info->dli_saddr = (void*)(long)symbolAddr;
            // never return the mach_header symbol
            if ( info->dli_saddr == info->dli_fbase ) {
                info->dli_sname = nullptr;
                info->dli_saddr = nullptr;
            }
            // strip off leading underscore
            else if ( (info->dli_sname != nullptr) && (info->dli_sname[0] == '_') ) {
                info->dli_sname = info->dli_sname + 1;
            }
        }
        else {
            info->dli_sname = nullptr;
            info->dli_saddr = nullptr;
            if ( loader != nullptr ) {
                if (const JustInTimeLoader* jitLoader = loader->isJustInTimeLoader() ) {
                    if ( const PseudoDylib* pd = jitLoader->pseudoDylib() )
                        pd->lookupAddress(addr, info);
                }
            }
        }
        result = 1;
    }
    else {
        // check if pointer is into dyld
        uintptr_t dyldStart  = (uintptr_t)&__dso_handle;
        uint64_t  targetAddr = (uint64_t)addr;
        if ( (dyldStart <= targetAddr) && (targetAddr < dyldStart + 0x200000) ) {
            uint64_t     slide  = (uintptr_t)&__dso_handle; // dyld is always zero based
            __block bool inDyld = false;
            __dso_handle.forEachSegment(^(const MachOAnalyzer::SegmentInfo& segInfo, bool& stop) {
                if ( ((segInfo.vmAddr + slide) <= targetAddr) && (targetAddr < (segInfo.vmAddr + slide + segInfo.vmSize)) ) {
                    inDyld = true;
                    stop   = true;
                }
            });
            if ( inDyld ) {
                info->dli_fname = "/usr/lib/dyld";
                info->dli_fbase = (void*)&__dso_handle;
                uint64_t symbolAddr;
                if ( __dso_handle.findClosestSymbol(targetAddr, &(info->dli_sname), &symbolAddr) ) {
                    info->dli_saddr = (void*)(long)symbolAddr;
                    // never return the mach_header symbol
                    if ( info->dli_saddr == info->dli_fbase ) {
                        info->dli_sname = nullptr;
                        info->dli_saddr = nullptr;
                    }
                    // strip off leading underscore
                    else if ( (info->dli_sname != nullptr) && (info->dli_sname[0] == '_') ) {
                        info->dli_sname = info->dli_sname + 1;
                    }
                    if ( strcmp(info->dli_sname, "_ZN5dyld45startEPKNS_10KernelArgsE") == 0 ) {
                        // start (which calls main()) is now in dyld, so be nice and allow dladdr() to return that
                        info->dli_sname = "start";
                        info->dli_saddr = (void*)addr;
                    }
                }
            }
        }
    }
    timer.setData4(result);
    timer.setData5(info->dli_fbase);
    timer.setData6(info->dli_saddr);
    return result;
}

struct PerThreadErrorMessage
{
    size_t sizeAllocated;
    bool   valid;
    char   message[1];
};

void APIs::clearErrorString()
{
    if ( (dlerrorPthreadKey() == -1) || !libSystemInitialized() )
        return;
    PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)this->libSystemHelpers->pthread_getspecific(dlerrorPthreadKey());
    if ( errorBuffer != nullptr )
        errorBuffer->valid = false;
}

void APIs::setErrorString(const char* format, ...)
{
    // if dlopen/dlsym called before libSystem initialized, dlerrorPthreadKey() won't be set, and malloc won't be available
    if ( (dlerrorPthreadKey() == -1) || !libSystemInitialized() )
        return;

#if !TARGET_OS_EXCLAVEKIT
    _SIMPLE_STRING buf = _simple_salloc();
    if ( buf == nullptr )
        return;
    va_list list;
    va_start(list, format);
    _simple_vsprintf(buf, format, list);
    va_end(list);
    size_t                 strLen      = strlen(_simple_string(buf)) + 1;
#else
    char buf[1024];
    buf[sizeof(buf)-1]='\0';
    va_list list;
    va_start(list, format);
    vsnprintf(buf, sizeof(buf), format, list);
    va_end(list);
    size_t                 strLen      = strlen(buf) + 1;
#endif // !TARGET_OS_EXCLAVEKIT

    size_t                 sizeNeeded  = sizeof(PerThreadErrorMessage) + strLen;
    PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)this->libSystemHelpers->pthread_getspecific(dlerrorPthreadKey());
    if ( errorBuffer != nullptr ) {
        if ( errorBuffer->sizeAllocated < sizeNeeded ) {
            this->libSystemHelpers->free(errorBuffer);
            errorBuffer = nullptr;
        }
    }
    if ( errorBuffer == nullptr ) {
        size_t allocSize                 = std::max(sizeNeeded, (size_t)256);
        // dlerrorPthreadKey is set up to call libSystem's free() on thread destruction, so this has to use libSystem's malloc()
        PerThreadErrorMessage* p         = (PerThreadErrorMessage*)this->libSystemHelpers->malloc(allocSize);
        p->sizeAllocated                 = allocSize;
        p->valid                         = false;
        this->libSystemHelpers->pthread_setspecific(dlerrorPthreadKey(), p);
        errorBuffer = p;
    }
#if !TARGET_OS_EXCLAVEKIT
    strcpy(errorBuffer->message, _simple_string(buf));
    errorBuffer->valid = true;
    _simple_sfree(buf);
#else
    strlcpy(errorBuffer->message, buf, errorBuffer->sizeAllocated);
    errorBuffer->valid = true;
#endif // !TARGET_OS_EXCLAVEKIT
}

char* APIs::dlerror()
{
    if ( config.log.apis )
        log("dlerror()");

    if ( (dlerrorPthreadKey() == -1) || !libSystemInitialized() )
        return nullptr; // if dlopen/dlsym called before libSystem initialized, dlerrorPthreadKey() won't be set
    PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)this->libSystemHelpers->pthread_getspecific(dlerrorPthreadKey());
    if ( errorBuffer != nullptr ) {
        if ( errorBuffer->valid ) {
            // you can only call dlerror() once, then the message is cleared
            errorBuffer->valid = false;
            if ( config.log.apis )
                log(" => '%s'\n", errorBuffer->message);
            return errorBuffer->message;
        }
    }
    if ( config.log.apis )
        log(" => NULL\n");

    return nullptr;
}

const Loader* APIs::findImageContaining(void* addr)
{
    addr                         = (void*)stripPointer(addr);
    __block const Loader* result = nullptr;
    locks.withLoadersReadLock(^{
        for ( const dyld4::Loader* image : loaded ) {
            const void* sgAddr;
            uint64_t    sgSize;
            uint8_t     sgPerm;
            if ( image->contains(*this, addr, &sgAddr, &sgSize, &sgPerm) ) {
                result = image;
                break;
            }
        }
    });
    return result;
}

void* APIs::dlopen(const char* path, int mode)
{
    void* callerAddress = __builtin_return_address(0);



    return dlopen_from(path, mode, callerAddress);
}

void* APIs::dlopen_from(const char* path, int mode, void* addressInCaller)
{
#if SUPPPORT_PRE_LC_MAIN
    if (!libSystemInitialized()) {
        // Usually libSystem will already be initialized, but some legacy binaries can call dlopen() first. If they do then
        // we need to force initialization of libSystem at that time. The reason is any library will link to libSystem and
        // trigger its initializers anyway, but until libSystem is up unfair locks don't work. If we let that happen we will
        // skip taking the api lock on entry, but will try to unlock it on release triggering a lock assertion
        const_cast<Loader*>(this->libSystemLoader)->beginInitializers(*this);
        this->libSystemLoader->runInitializers(*this);
    }
#endif
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLOPEN, path, mode, 0);
    if ( config.log.apis )
        log("dlopen(\"%s\", 0x%08X)\n", path, mode);

    clearErrorString();

    const bool firstOnly = (mode & RTLD_FIRST);

    // passing NULL for path means return magic object
    if ( path == nullptr ) {
        // RTLD_FIRST means any dlsym() calls on the handle should only search that handle and not subsequent images
        if ( firstOnly )
            return RTLD_MAIN_ONLY;
        else
            return RTLD_DEFAULT;
    }

#if SUPPORT_PREBUILTLOADERS
    // Fast path.  If we are dlopening a shared cache path, and its already initialized, then we can just return it
    if ( const PrebuiltLoaderSet* cachePBLS = cachedDylibsPrebuiltLoaderSet() ) {
        uint32_t dylibInCacheIndex = 0;
        if ( this->config.dyldCache.indexOfPath(path, dylibInCacheIndex) ) {
            const PrebuiltLoader* ldr = cachePBLS->atIndex(dylibInCacheIndex);
            if ( ldr->isInitialized(*this) ) {
                // make handle
                void* result = handleFromLoader(ldr, firstOnly);
                if ( config.log.apis ) {
                    log("      dlopen(%s) => %p\n", Loader::leafName(path), result);
                }
                timer.setData4(result);
                return result;
            }
        }
    }
#endif // SUPPORT_PREBUILTLOADERS

    // don't take the lock until after the check for path==NULL
    // don't take the lock in RTLD_NOLOAD mode, since that will never change the set of loaded images
    bool skipApiLock = (mode & RTLD_NOLOAD);
    RecursiveAutoLock  apiLock(*this, skipApiLock);

    // some aspect of dlopen depend on who called it
    const Loader* caller = findImageContaining(addressInCaller);

    void*           result    = nullptr;
    const Loader*   topLoader = nullptr;
    STACK_ALLOC_VECTOR(const Loader*, loadersToNotify, 32);
    STACK_ALLOC_VECTOR(const Loader*, loadersUnDelayed, 32);
    locks.withLoadersWriteLock(memoryManager, [&] {
        // since we have the dyld lock, any appends to state.loaded will be from this dlopen
        // so record the length now, and cut it back to that point if dlopen fails
        const uint64_t startLoaderCount = loaded.size();
        const uint64_t startPatchedObjCClassesCount = this->patchedObjCClasses.size();
        const uint64_t startPatchedSingletonsCount = this->patchedSingletons.size();
        Diagnostics     diag;

        // try to load specified dylib
        Loader::LoadChain   loadChainMain { nullptr, mainExecutableLoader };
        Loader::LoadChain   loadChainCaller { &loadChainMain, caller };
        Loader::LoadOptions options;
        options.staticLinkage    = false;
        options.launching        = false;
        options.canBeMissing     = false;
        options.rtldLocal        = (mode & RTLD_LOCAL);
        options.rtldNoDelete     = (mode & RTLD_NODELETE);
        options.rtldNoLoad       = (mode & RTLD_NOLOAD);
        options.insertedDylib    = false;
        options.canBeDylib       = true;
        options.canBeBundle      = true;
        // only allow dlopen() of main executables on macOS (eventually ban there too)
#if TARGET_OS_SIMULATOR
        options.canBeExecutable  = (strncmp(config.process.progname, "IBDesignablesAgent", 18) == 0);
#else
        options.canBeExecutable  = (config.process.platform == dyld3::Platform::macOS);
#endif
        options.forceUnloadable  = (mode & RTLD_UNLOADABLE);
        options.requestorNeedsFallbacks = caller ? caller->pre2022Binary : false;
        options.rpathStack       = (caller ? &loadChainCaller : &loadChainMain);
        options.finder           = nullptr;
        topLoader = Loader::getLoader(diag, *this, path, options);
        if ( topLoader == nullptr ) {
            setErrorString("dlopen(%s, 0x%04X): %s", path, mode, diag.errorMessageCStr());
            return;
        }

        // if RTLD_LOCAL was *not* used, and image was already loaded hidden, then unhide it
        if ( ((mode & RTLD_LOCAL) == 0) && topLoader->hiddenFromFlat() )
            topLoader->hiddenFromFlat(true);

        // RTLD_NOLOAD means don't load if not already loaded
        if ( mode & RTLD_NOLOAD ) {
#if SUPPORT_IMAGE_UNLOADING
            incDlRefCount(topLoader);
#endif // SUPPORT_IMAGE_UNLOADING
            result = handleFromLoader(topLoader, firstOnly);
            return;
        }

        // if RTLD_NODELETE is used on any dlopen, it sets the leavedMapped bit
        if ( mode & RTLD_NODELETE ) {
            // dylibs in cache, or dylibs statically link will always remain, so RTLD_NODELETE is already in effect
            if ( !topLoader->dylibInDyldCache && !topLoader->neverUnload && !topLoader->leaveMapped ) {
                // PrebuiltLoaders are never used for things that can be unloaded, so ignore
                if ( !topLoader->isPrebuilt ) {
                    JustInTimeLoader* jitLoader = (JustInTimeLoader*)topLoader;
                    jitLoader->setLateLeaveMapped();
                }
            }
        }

        // load all dependents
        Loader::LoadChain   loadChain { options.rpathStack, topLoader };
        Loader::LoadOptions depOptions;
        depOptions.staticLinkage   = true;
        depOptions.rtldLocal       = false; // RTLD_LOCAL only effects top level dylib
        depOptions.rtldNoDelete    = (mode & RTLD_NODELETE);
        depOptions.canBeDylib      = true;
        depOptions.requestorNeedsFallbacks = topLoader->pre2022Binary;
        depOptions.rpathStack      = &loadChain;
        ((Loader*)topLoader)->loadDependents(diag, *this, depOptions);
        // only do fixups and notifications if new dylibs are loaded (could be dlopen that just bumps the ref count)
        STACK_ALLOC_VECTOR(const Loader*, newLoaders, loaded.size() - startLoaderCount);
        for (uint64_t i = startLoaderCount; i != loaded.size(); ++i)
            newLoaders.push_back(loaded[i]);

        DyldCacheDataConstLazyScopedWriter cacheDataConst(*this);
        if ( diag.noError() && !newLoaders.empty() ) {
            // tell debugger about newly loaded images in case there is a crash during fixups
            notifyDebuggerLoad(newLoaders);

            // proactive weakDefMap means we update the weakDefMap with everything just loaded before doing any binding
            if ( config.process.proactivelyUseWeakDefMap ) {
                Loader::addWeakDefsToMap(*this, newLoaders);
            }

            // do fixups
            {
                dyld3::ScopedTimer fixupsTimer(DBG_DYLD_TIMING_APPLY_FIXUPS, 0, 0, 0);

                for ( const Loader* ldr : newLoaders ) {
                    bool allowLazyBinds = ((mode & RTLD_NOW) == 0);
                    ldr->applyFixups(diag, *this, cacheDataConst, allowLazyBinds);
                    if ( diag.hasError() )
                        break;
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
                    // Roots need to patch the uniqued GOTs in the cache
                    //FIXME: Is the right place to conditionalize this?
                    ldr->applyCachePatches(*this, cacheDataConst);
#endif // BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
                }
            }

            if ( diag.noError() ) {
                // add to permanent ranges
                STACK_ALLOC_ARRAY(const Loader*, nonCacheNeverUnloadLoaders, newLoaders.size());
                for (const Loader* ldr : newLoaders) {
                    if ( !ldr->dylibInDyldCache && ldr->neverUnload )
                        nonCacheNeverUnloadLoaders.push_back(ldr);
                }
                if ( !nonCacheNeverUnloadLoaders.empty() )
                    this->addPermanentRanges(nonCacheNeverUnloadLoaders);

#if !TARGET_OS_EXCLAVEKIT
                // notify kernel about new static user probes
                notifyDtrace(newLoaders);
#endif // !TARGET_OS_EXCLAVEKIT

                // If any previous images had missing flat lazy symbols, try bind them again now
                rebindMissingFlatLazySymbols(newLoaders);

                // if image has thread locals, set them up
                for ( const Loader* ldr : newLoaders ) {
                    const MachOAnalyzer* ma = ldr->analyzer(*this);
                    if ( ma->hasThreadLocalVariables() )
                        setUpTLVs(ma);
                }

                // Store loaders to be notified later
                loadersToNotify.reserve(newLoaders.size());
                for (const Loader* ldr : newLoaders)
                    loadersToNotify.push_back(ldr);
            }
        }

#if SUPPORT_IMAGE_UNLOADING
        // increment ref count before notifiers are called and before initializers are run,
        // because either of those could call dlclose() and cause a garbage collection.
        if ( diag.noError() )
            incDlRefCount(topLoader);
#endif // SUPPORT_IMAGE_UNLOADING

        // If there was an error while loading or doing fixups, then unload everything added in this dlopen.
        // This has to be done while we still have the LoadersLock
        if ( diag.hasError() ) {
            setErrorString("dlopen(%s, 0x%04X): %s", path, mode, diag.errorMessageCStr());

            // Remove missing lazy symbols for the new loaders.  These were recorded eagerly during symbol binding
            removeMissingFlatLazySymbols(newLoaders);

            // remove any entries these temp dylibs may have map in the weak-def map
            if ( this->weakDefMap != nullptr ) {
                for ( const Loader* incompleteLoader : newLoaders )
                    this->removeDynamicDependencies(incompleteLoader);
            }

            // Remove the loaders from the image lists
            notifyDebuggerUnload(newLoaders);

#if SUPPORT_IMAGE_UNLOADING
            // unmap everthing just loaded (note: unmap() does not unmap stuff in shared cache)
            for ( const Loader* ldr : newLoaders )
                ldr->unmap(*this, true);
#endif
            
            // remove new loaders from runtime list
            while ( loaded.size() > startLoaderCount ) {
                //const Loader* removeeLdr = loaded.back();
                //log("removing %p from state.loaded (%s)\n", removeeLdr, removeeLdr->path());
                loaded.pop_back();
                // FIXME: free malloced JITLoaders
            }
            result    = nullptr;
            topLoader = nullptr;

            // Clear any potential objc patching entries from the lists.  We aren't going to do patching
            // on these binaries as the dlopen failed
            this->objcReplacementClasses.clear();
            while ( this->patchedObjCClasses.size() > startPatchedObjCClassesCount ) {
                this->patchedObjCClasses.pop_back();
            }
            while ( this->patchedSingletons.size() > startPatchedSingletonsCount ) {
                this->patchedSingletons.pop_back();
            }
        }

        // on success, run objc notifiers.  This has to be done while still in the write lock as
        // the notifier mutates the list of objc classes
        if ( (topLoader != nullptr) && ((mode & RTLD_NOLOAD) == 0) && diag.noError() ) {
            const Loader* rootLoaders[1] = { topLoader };
            std::span<const Loader*> rootLoadersSpan(rootLoaders, 1);
            partitionDelayLoads(newLoaders, rootLoadersSpan, loadersUnDelayed);
            doSingletonPatching(cacheDataConst);
            notifyObjCPatching();
        }
    });

    // on success, run initializers
    if ( (topLoader != nullptr) && ((mode & RTLD_NOLOAD) == 0) ) {
        // Note: we have released the withLoadersWriteLock while running the notifiers/initializers
        // This is intentional to avoid deadlocks with other framework locks, that might call dyld
        // inquiry functions now (such as walking loaded images).
        // It is safe, because we still have the API-lock, so no other thread can call dlclose() and remove
        // the images that are having their notifiers/initializers run.  A initializer may call dlopen() again and
        // add more images, but that will be on the same thread as this, so the ivar in Loaders about if
        // its initializer has been run does not need to be thread safe.

        // first notify about any delay-init dylibs that just got moved to being needed
        if ( !loadersUnDelayed.empty() ) {
            std::span<const Loader*> ldrs(&loadersUnDelayed[0], (size_t)loadersUnDelayed.size());
            notifyLoad(ldrs);
        }

        // notify everyone else about all loaded images (do this late, so we don't have to undo incase of error).
        if ( !loadersToNotify.empty() ) {
            std::span<const Loader*> ldrs(&loadersToNotify[0], (size_t)loadersToNotify.size());
            notifyLoad(ldrs);
        }

        // run initializers
        topLoader->runInitializersBottomUpPlusUpwardLinks(*this);

        // make handle
        result = handleFromLoader(topLoader, firstOnly);
    }

    if ( config.log.apis ) {
        PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)this->libSystemHelpers->pthread_getspecific(dlerrorPthreadKey());
        if ( (errorBuffer != nullptr) && errorBuffer->valid )
            log("      dlopen(%s) => NULL, '%s'\n", Loader::leafName(path), errorBuffer->message);
        else
            log("      dlopen(%s) => %p\n", Loader::leafName(path), result);
    }
    timer.setData4(result);
    return result;
}

int APIs::dlclose(void* handle)
{
    RecursiveAutoLock apiLock(*this);
    if ( config.log.apis )
        log("dlclose(%p)\n", handle);
#if !TARGET_OS_EXCLAVEKIT

    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLCLOSE, (uint64_t)handle, 0, 0);

    // silently accept magic handles for main executable
    if ( handle == RTLD_MAIN_ONLY )
        return 0;
    if ( handle == RTLD_DEFAULT )
        return 0;

    bool          firstOnly;
    const Loader* ldr = loaderFromHandle(handle, firstOnly);
    if ( !validLoader(ldr) ) {
        setErrorString("dlclose(%p): invalid handle", handle);
        return -1;
    }

    // unloads if reference count goes to zero
    decDlRefCount(ldr);
#endif // !TARGET_OS_EXCLAVEKIT

    clearErrorString();
    return 0;
}

bool APIs::dlopen_preflight(const char* path)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLOPEN_PREFLIGHT, path, 0, 0);

    if ( config.log.apis )
        log("dlopen_preflight(%s)\n", path);
#if !TARGET_OS_EXCLAVEKIT
    // check if path is in dyld shared cache
    uint32_t               imageIndex;
    const DyldSharedCache* dyldCache = config.dyldCache.addr;
    if ( dyldCache && dyldCache->hasImagePath(path, imageIndex) ) {
        timer.setData4(true);
        return true;
    }

    // may be symlink to something in dyld cache
    char realerPath[PATH_MAX];
    if ( config.syscall.realpath(path, realerPath) ) {
        if ( strcmp(path, realerPath) != 0 ) {
            if ( dyldCache && dyldCache->hasImagePath(realerPath, imageIndex) ) {
                timer.setData4(true);
                return true;
            }
        }
    }

    // check if file is loadable (note: this handles DYLD_*_PATH variables and simulator prefix, but not @ paths)
    bool                topStop = false;
    __block bool        result  = false;
    __block Diagnostics diag;
    config.pathOverrides.forEachPathVariant(path, config.process.platform, false, true, topStop, ^(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& stop) {
        __block Diagnostics possiblePathDiag;
        config.syscall.withReadOnlyMappedFile(possiblePathDiag, possiblePath, true, ^(const void* mapping, size_t mappedSize, bool isOSBinary, const FileID&, const char*) {
            if ( MachOFile::compatibleSlice(possiblePathDiag, mapping, mappedSize, path, config.process.platform, isOSBinary, *config.process.archs, config.security.internalInstall) != nullptr ) {
                result = true;
                stop   = true;
            }
        });
        if ( !result && possiblePathDiag.hasError() ) {
            if ( diag.noError() )
                diag.error("tried: '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());
            else
                diag.appendError(", '%s' (%s)", possiblePath, possiblePathDiag.errorMessageCStr());
        }
    });
    if ( !result && diag.hasError() )
        setErrorString("dlopen_preflight(%s) => false, %s", path, diag.errorMessageCStr());

    if ( config.log.apis )
        log("      dlopen_preflight(%s) => %d\n", Loader::leafName(path), result);

    timer.setData4(result);
    return result;
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

#if !__i386__
void* APIs::dlopen_audited(const char* path, int mode)
{
    return dlopen(path, mode);
}
#endif

void* APIs::dlsym(void* handle, const char* symbolName)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLSYM, (uint64_t)handle, symbolName, 0);

    if ( config.log.apis )
        log("dlsym(%p, \"%s\")\n", handle, symbolName);

    clearErrorString();


#if !TARGET_OS_EXCLAVEKIT
    // allow apps to disable dlsym()
    if ( config.security.dlsymBlocked ) {
        // either abort
        if ( config.security.dlsymAbort ) {
#if BUILDING_DYLD
            halt("dlsym() called");
#elif BUILDING_UNIT_TESTS
            abort();
#endif
        }
        // or silently return NULL
        if ( config.log.apis )
            log("     dlsym(\"%s\") => NULL (blocked)\n", symbolName);
        return nullptr;
    }
#endif
    // dlsym() assumes symbolName passed in is same as in C source code
    // dyld assumes all symbol names have an underscore prefix
    size_t symLen = strlen(symbolName);
    BLOCK_ACCCESSIBLE_ARRAY(char, underscoredName, symLen + 2);
    underscoredName[0] = '_';
    strlcpy(&underscoredName[1], symbolName, symLen+1);

    __block Diagnostics diag;
    __block Loader::ResolvedSymbol result;
    if ( handle == RTLD_DEFAULT ) {
        // magic "search all in load order" handle
        __block bool found = false;
        locks.withLoadersReadLock(^{
            for ( const dyld4::Loader* image : loaded ) {

                if ( !image->hiddenFromFlat() && image->hasExportedSymbol(diag, *this, underscoredName, Loader::shallow, Loader::runResolver, &result) ) {
                    found = true;
                    break;
                }
            }
        });
        if ( !found ) {
            setErrorString("dlsym(RTLD_DEFAULT, %s): symbol not found", symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
    }
    else if ( handle == RTLD_MAIN_ONLY ) {
        // magic "search only main executable" handle
        if ( !mainExecutableLoader->hasExportedSymbol(diag, *this, underscoredName, Loader::staticLink, Loader::skipResolver, &result) ) {
            setErrorString("dlsym(RTLD_MAIN_ONLY, %s): symbol not found", symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
    }
    else if ( handle == RTLD_NEXT ) {
        // magic "search what I would see" handle
        void*         callerAddress = __builtin_return_address(0);
        const Loader* callerImage   = findImageContaining(callerAddress);
        if ( callerImage == nullptr ) {
            setErrorString("dlsym(RTLD_NEXT, %s): called by unknown image (caller=%p)", symbolName, callerAddress);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
        STACK_ALLOC_ARRAY(const Loader*, alreadySearched, loaded.size());
        if ( !callerImage->hasExportedSymbol(diag, *this, underscoredName, Loader::dlsymNext, Loader::runResolver, &result, &alreadySearched) ) {
            setErrorString("dlsym(RTLD_NEXT, %s): symbol not found", symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
    }
    else if ( handle == RTLD_SELF ) {
        // magic "search me, then what I would see" handle
        void*         callerAddress = __builtin_return_address(0);
        const Loader* callerImage   = findImageContaining(callerAddress);
        if ( callerImage == nullptr ) {
            setErrorString("dlsym(RTLD_SELF, %s): called by unknown image (caller=%p)", symbolName, callerAddress);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
        STACK_ALLOC_ARRAY(const Loader*, alreadySearched, loaded.size());
        if ( !callerImage->hasExportedSymbol(diag, *this, underscoredName, Loader::dlsymSelf, Loader::runResolver, &result, &alreadySearched) ) {
            setErrorString("dlsym(RTLD_SELF, %s): symbol not found", symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
    }
    else {
        // handle value was something returned by dlopen()
        bool          firstOnly;
        const Loader* image = loaderFromHandle(handle, firstOnly);
#if TARGET_OS_OSX
        // FIXME: temp work around for syspolicyd <rdar://73731400>
        if ( (MachOAnalyzer*)handle == config.process.mainExecutable ) {
            setErrorString("dlsym(%p, %s): invalid handle", handle, symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
             return nullptr;
        }
#endif
        // verify is a valid loader
        if ( !validLoader(image) ) {
            setErrorString("dlsym(%p, %s): invalid handle", handle, symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
        // RTLD_FIRST only searches one place
        STACK_ALLOC_ARRAY(const Loader*, alreadySearched, loaded.size());
        Loader::ExportedSymbolMode mode = (firstOnly ? Loader::staticLink : Loader::dlsymSelf);
        if ( !image->hasExportedSymbol(diag, *this, underscoredName, mode, Loader::runResolver, &result, &alreadySearched) ) {
            setErrorString("dlsym(%p, %s): symbol not found", handle, symbolName);
            if ( config.log.apis )
                log("     dlsym(\"%s\") => NULL\n", symbolName);
            return nullptr;
        }
    }

    if ( result.targetLoader != nullptr ) {
        void* ptr = (void*)Loader::resolvedAddress(*this, result);
        ptr       = (void*)Loader::interpose(*this, (uintptr_t)ptr);
#if __has_feature(ptrauth_calls)
        if ( result.isCode )
            ptr = __builtin_ptrauth_sign_unauthenticated(ptr, ptrauth_key_asia, 0);
#endif
        if ( config.log.apis )
            log("     dlsym(\"%s\") => %p\n", symbolName, ptr);
        timer.setData4((uint64_t)(stripPointer(ptr)));
        return ptr;
    }
    if ( config.log.apis )
        log("     dlsym(\"%s\") => NULL\n", symbolName);
    return nullptr;

}

bool APIs::dyld_shared_cache_some_image_overridden()
{
    bool result = hasOverriddenCachedDylib();
    if ( config.log.apis )
        log("dyld_shared_cache_some_image_overridden() => %d\n", result);
    return result;
}

bool APIs::_dyld_get_shared_cache_uuid(uuid_t uuid)
{
    if ( config.log.apis )
        log("_dyld_get_shared_cache_uuid(%p)\n", uuid);
#if !TARGET_OS_EXCLAVEKIT
    const DyldSharedCache* sharedCache = config.dyldCache.addr;
    if ( sharedCache != nullptr ) {
        sharedCache->getUUID(uuid);
        return true;
    }
    return false;
#else
    return false;
#endif // !TARGET_OS_EXCLAVEKIT
}

const void* APIs::_dyld_get_shared_cache_range(size_t* mappedSize)
{
    if ( config.log.apis )
        log("_dyld_get_shared_cache_range(%p)", mappedSize);
    const void* result = nullptr;
    *mappedSize = 0;
#if !TARGET_OS_EXCLAVEKIT
    if ( const DyldSharedCache* sharedCache = config.dyldCache.addr ) {
        *mappedSize = (size_t)sharedCache->mappedSize();
        result = sharedCache;
    }
#endif // !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log(" => %p,0x%lX\n", result, *mappedSize);
    return result;
}

bool APIs::_dyld_shared_cache_optimized()
{
    bool result = false;
    if ( const DyldSharedCache* sharedCache = config.dyldCache.addr ) {
        result = (sharedCache->header.cacheType == kDyldSharedCacheTypeProduction);
    }
    if ( config.log.apis )
        log("_dyld_shared_cache_optimized() => %d\n", result);
    return result;
}

void APIs::_dyld_images_for_addresses(unsigned count, const void* addresses[], dyld_image_uuid_offset infos[])
{
    if ( config.log.apis )
        log("_dyld_images_for_addresses(%d, %p, %p)\n", count, addresses, infos);
    // in stack crawls, common for contiguous frames to be in same image, so cache
    // last lookup and check if next addresss in in there before doing full search
    const MachOLoaded* ml = nullptr;
    bool               neverUnloads;
    const char*        path;
    const void*        segAddr;
    uint64_t           segSize;
    const void*        end = (void*)ml;
    for ( unsigned i = 0; i < count; ++i ) {
        const void* addr = stripPointer(addresses[i]);
        bzero(&infos[i], sizeof(dyld_image_uuid_offset));
        if ( (ml == nullptr) || (addr < (void*)ml) || (addr > end) ) {
            if ( findImageMappedAt(addr, &ml, &neverUnloads, &path, &segAddr, &segSize) ) {
                end = (void*)((uint8_t*)ml + segSize);
            }
            else {
                ml = nullptr;
            }
        }
        if ( ml != nullptr ) {
            infos[i].image         = ml;
            infos[i].offsetInImage = (uintptr_t)addr - (uintptr_t)ml;
            ml->getUuid(infos[i].uuid);
        }
    }
}

void APIs::_dyld_register_for_image_loads(LoadNotifyFunc func)
{
    if ( config.log.apis )
        log("_dyld_register_for_image_loads(%p)\n", func);
#if !TARGET_OS_EXCLAVEKIT
    // callback about already loaded images
    locks.withLoadersReadLock(^{
        for ( const dyld4::Loader* image : loaded ) {
            const MachOLoaded* ml = image->loadAddress(*this);
            if ( config.log.notifications )
                log("add notifier %p called with mh=%p\n", func, ml);
            func(ml, image->path(), !image->neverUnload);
        }
    });

    // add to list of functions to call about future loads
    const Loader* callbackLoader = this->findImageContaining((void*)func);
    locks.withNotifiersWriteLock(memoryManager, ^() {
        addNotifyLoadImage(callbackLoader, func);
    });
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_register_for_bulk_image_loads(void (*func)(unsigned imageCount, const mach_header* mhs[], const char* paths[]))
{
    if ( config.log.apis )
        log("_dyld_register_for_bulk_image_loads(%p)\n", func);
#if !TARGET_OS_EXCLAVEKIT

    // callback about already loaded images
    locks.withLoadersReadLock(^{
        unsigned           count = (unsigned)loaded.size();
        const mach_header* mhs[count];
        const char*        paths[count];
        for ( unsigned i = 0; i < count; ++i ) {
            mhs[i]   = loaded[i]->loadAddress(*this);
            paths[i] = loaded[i]->path();
        }
        //dyld3::ScopedTimer timer(DBG_DYLD_TIMING_FUNC_FOR_ADD_IMAGE, (uint64_t)mhs[0], (uint64_t)func, 0);
        if ( config.log.notifications )
            log("add bulk notifier %p called with %d images\n", func, count);
        func(count, mhs, paths);
    });

    // add to list of functions to call about future loads
    const Loader* callbackLoader = this->findImageContaining((void*)func);
    locks.withNotifiersWriteLock(memoryManager, ^() {
        addNotifyBulkLoadImage(callbackLoader, func);
    });
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

#if !__USING_SJLJ_EXCEPTIONS__
bool APIs::_dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info)
{
    if ( config.log.apis )
        log("_dyld_find_unwind_sections(%p, %p)\n", addr, info);
    const MachOLoaded* ml;
    const Loader *ldr = nullptr;
    if ( findImageMappedAt(stripPointer(addr), &ml, /*neverUnloads = */ nullptr, /* path = */ nullptr,
                           /* segAddr = */ nullptr, /* segSize = */ nullptr, /* segPerms = */ nullptr,
                           &ldr) ) {
        info->mh                            = ml;
        info->dwarf_section                 = nullptr;
        info->dwarf_section_length          = 0;
        info->compact_unwind_section        = nullptr;
        info->compact_unwind_section_length = 0;

        if ( ldr ) {
            if (const JustInTimeLoader* jitLoader = ldr->isJustInTimeLoader() ) {
                if ( const PseudoDylib* pd = jitLoader->pseudoDylib() ) {
                    bool found = false;
                    if ( char* errMsg = pd->findUnwindSections(addr, &found, info) ) {
                        if ( config.log.apis )
                            log("_dyld_pseudodylib_find_unwind_sections(%p, %p) returned error: %s",
                                addr, info, errMsg);
                        pd->disposeString(errMsg);
                    }
                    if ( found )
                        return true;
                }
            }
        }

        uint64_t size;
        if ( const void* content = ml->findSectionContent("__TEXT", "__eh_frame", size) ) {
            info->dwarf_section        = content;
            info->dwarf_section_length = (uintptr_t)size;
        }
        if ( const void* content = ml->findSectionContent("__TEXT", "__unwind_info", size) ) {
            info->compact_unwind_section        = content;
            info->compact_unwind_section_length = (uintptr_t)size;
        }
        return true;
    }

    return false;
}
#endif

bool APIs::dyld_process_is_restricted()
{
    if ( config.log.apis )
        log("dyld_process_is_restricted()");
#if !TARGET_OS_EXCLAVEKIT
    bool result = !config.security.allowEnvVarsPath;
    if ( config.log.apis )
        log(" => %d\n", result);
    return result;
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

const char* APIs::dyld_shared_cache_file_path()
{
    const char* result = config.dyldCache.path;
    if ( config.log.apis )
        log("dyld_shared_cache_file_path() => %s\n", result);
    return result;
}

bool APIs::dyld_has_inserted_or_interposing_libraries()
{
     bool result = (!interposingTuplesAll.empty() || config.pathOverrides.hasInsertedDylibs());
     if ( config.log.apis )
        log("dyld_has_inserted_or_interposing_libraries() => %d\n", result);
    return result;
}

#if !TARGET_OS_EXCLAVEKIT
static void* mapStartOfCache(const char* path, size_t& length)
{
    struct stat statbuf;
    if ( ::stat(path, &statbuf) == -1 )
        return NULL;

    if ( statbuf.st_size < length )
        length = (size_t)statbuf.st_size;

    int cache_fd = dyld3::open(path, O_RDONLY, 0);
    if ( cache_fd < 0 )
        return NULL;

    void* result = ::mmap(NULL, length, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    close(cache_fd);

    if ( result == MAP_FAILED )
        return NULL;

    return result;
}

static const DyldSharedCache* findCacheInDirAndMap(RuntimeState& state, const uuid_t cacheUuid, const char* dirPath, size_t& sizeMapped)
{
    __block const DyldSharedCache* result = nullptr;
    state.config.syscall.forEachInDirectory(dirPath, false, ^(const char* pathInDir, const char* leafName) {
        if ( DyldSharedCache::isSubCachePath(leafName) )
            return;
        // FIXME: This needs to be at least large enough to read the path for any shared cache image.  We need to do something better
        // than a hard coded value here.
        size_t mapSize =  0x100000;
        if ( result == nullptr ) {
            result = (DyldSharedCache*)mapStartOfCache(pathInDir, mapSize);
            if ( result != nullptr ) {
                uuid_t foundUuid;
                result->getUUID(foundUuid);
                if ( ::memcmp(foundUuid, cacheUuid, 16) != 0 ) {
                    // wrong uuid, unmap and keep looking
                    ::munmap((void*)result, mapSize);
                    result = nullptr;
                }
                else {
                    // found cache
                    sizeMapped = mapSize;
                }
            }
        }
    });
    return result;
}
#endif // !TARGET_OS_EXCLAVEKIT

int APIs::dyld_shared_cache_find_iterate_text(const uuid_t cacheUuid, const char* extraSearchDirs[], void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    if ( config.log.apis )
        log("dyld_shared_cache_find_iterate_text()\n");
#if !TARGET_OS_EXCLAVEKIT
    // see if requested cache is the active one in this process
    size_t                 sizeMapped  = 0;
    const DyldSharedCache* sharedCache = config.dyldCache.addr;
    if ( sharedCache != nullptr ) {
        uuid_t runningUuid;
        sharedCache->getUUID(runningUuid);
        if ( ::memcmp(runningUuid, cacheUuid, 16) != 0 )
            sharedCache = nullptr;
    }
    if ( sharedCache == nullptr ) {
        // look first is default location for cache files
#if TARGET_OS_IPHONE
        sharedCache = findCacheInDirAndMap(*this, cacheUuid, IPHONE_DYLD_SHARED_CACHE_DIR, sizeMapped);
        // if not there, look in cryptex locations
        if ( sharedCache == nullptr ) {
            for (int i = 0; i < sizeof(cryptexPrefixes)/sizeof(char*); i++) {
                const char* prefix = cryptexPrefixes[i];
                char cacheDir[PATH_MAX];
                cacheDir[0] = 0;
                if ( Utils::concatenatePaths(cacheDir, prefix, PATH_MAX) >= PATH_MAX )
                    continue;
                if ( Utils::concatenatePaths(cacheDir, IPHONE_DYLD_SHARED_CACHE_DIR, PATH_MAX) >= PATH_MAX )
                    continue;
                sharedCache = findCacheInDirAndMap(*this, cacheUuid, cacheDir, sizeMapped);
                if ( sharedCache != nullptr )
                    break;
            }
        }
#else
        // on macOS look first in new system location, then old location
        sharedCache = findCacheInDirAndMap(*this, cacheUuid, MACOSX_MRM_DYLD_SHARED_CACHE_DIR, sizeMapped);
        // if not there, look in cryptex locations
        if ( sharedCache == nullptr ) {
            for (int i = 0; i < sizeof(cryptexPrefixes)/sizeof(char*); i++) {
                const char* prefix = cryptexPrefixes[i];
                char cacheDir[PATH_MAX];
                cacheDir[0] = 0;
            if ( Utils::concatenatePaths(cacheDir, prefix, PATH_MAX) >= PATH_MAX )
                    continue;
                if ( Utils::concatenatePaths(cacheDir, MACOSX_MRM_DYLD_SHARED_CACHE_DIR, PATH_MAX) >= PATH_MAX )
                    continue;
                sharedCache = findCacheInDirAndMap(*this, cacheUuid, cacheDir, sizeMapped);
                if ( sharedCache != nullptr )
                    break;
            }
        }
#endif //  TARGET_OS_IPHONE
        if ( sharedCache == nullptr ) {
            // look in DriverKit location
            sharedCache = findCacheInDirAndMap(*this, cacheUuid, DRIVERKIT_DYLD_SHARED_CACHE_DIR, sizeMapped);
            // if not there, look in cryptex Driverkit locations
            if ( sharedCache == nullptr ) {
                for (int i = 0; i < sizeof(cryptexPrefixes)/sizeof(char*); i++) {
                    const char* prefix = cryptexPrefixes[i];
                    char cacheDir[PATH_MAX];
                    cacheDir[0] = 0;
                    if ( Utils::concatenatePaths(cacheDir, prefix, PATH_MAX) >= PATH_MAX )
                        continue;
                    if ( Utils::concatenatePaths(cacheDir, DRIVERKIT_DYLD_SHARED_CACHE_DIR, PATH_MAX) >= PATH_MAX )
                        continue;
                    sharedCache = findCacheInDirAndMap(*this, cacheUuid, cacheDir, sizeMapped);
                    if ( sharedCache != nullptr )
                        break;
                }
            }
            // if not there, look in extra search locations
            if ( sharedCache == nullptr ) {
                for ( const char** p = extraSearchDirs; *p != nullptr; ++p ) {
                    sharedCache = findCacheInDirAndMap(*this, cacheUuid, *p, sizeMapped);
                    if ( sharedCache != nullptr )
                        break;
                }
            }
        }
    }
    if ( sharedCache == nullptr )
        return -1;

    // get base address of cache
    __block uint64_t cacheUnslidBaseAddress = 0;
    sharedCache->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
        if ( cacheUnslidBaseAddress == 0 )
            cacheUnslidBaseAddress = vmAddr;
    });

    // iterate all images
    sharedCache->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName, bool& stop) {
        dyld_shared_cache_dylib_text_info dylibTextInfo;
        dylibTextInfo.version           = 2;
        dylibTextInfo.loadAddressUnslid = loadAddressUnslid;
        dylibTextInfo.textSegmentSize   = textSegmentSize;
        dylibTextInfo.path              = installName;
        ::memcpy(dylibTextInfo.dylibUuid, dylibUUID, 16);
        dylibTextInfo.textSegmentOffset = loadAddressUnslid - cacheUnslidBaseAddress;
        callback(&dylibTextInfo);
    });

    if ( sizeMapped != 0 )
        ::munmap((void*)sharedCache, sizeMapped);

    return 0;
#else
    return -1;
#endif // !TARGET_OS_EXCLAVEKIT
}

int APIs::dyld_shared_cache_iterate_text(const uuid_t cacheUuid, void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    if ( config.log.apis )
        log("dyld_shared_cache_iterate_text()\n");
    const char* extraSearchDirs[] = { NULL };
    return dyld_shared_cache_find_iterate_text(cacheUuid, extraSearchDirs, callback);
}


void APIs::_dyld_fork_child()
{
#if !TARGET_OS_EXCLAVEKIT
    // this is new process, so reset task port
    mach_task_self_ = task_self_trap();

#if HAS_EXTERNAL_STATE
    this->externallyViewable.fork_child();
#endif // !HAS_EXTERNAL_STATE
    locks.resetLockInForkChild();
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_atfork_prepare()
{
#if !TARGET_OS_EXCLAVEKIT
    locks.takeLockBeforeFork();
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_atfork_parent()
{
#if !TARGET_OS_EXCLAVEKIT
    locks.releaseLockInForkParent();
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_before_fork_dlopen()
{
#if !TARGET_OS_EXCLAVEKIT
    locks.takeDlopenLockBeforeFork();
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_after_fork_dlopen_parent()
{
#if !TARGET_OS_EXCLAVEKIT
    locks.releaseDlopenLockInForkParent();
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_after_fork_dlopen_child()
{
    locks.resetDlopenLockInForkChild();
}

const char* APIs::_dyld_get_objc_selector(const char* selName)
{
#if !TARGET_OS_EXCLAVEKIT

    // The selector table meaning changed from version 15 -> version 16.
    // Version 15 is the legacy table with cache offsets
    // We don't support that old version here, as dyld is always using a new enough cache
    if ( const objc::SelectorHashTable* selectorHashTable = config.dyldCache.objcSelectorHashTable ) {
        if ( const char* uniqueName = selectorHashTable->get(selName) ) {
            if ( config.log.apis )
                log("_dyld_get_objc_selector(%s) => %s\n", selName, uniqueName);
            return uniqueName;
        }
    }

#if SUPPORT_PREBUILTLOADERS
    // If main program has PrebuiltLoader, check selector table in that
    if ( const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet() ) {
        if ( const ObjCSelectorOpt* selectorHashTable = mainSet->objcSelectorOpt() ) {
            const char* uniqueName = selectorHashTable->getString(selName, *this);
            if ( config.log.apis )
                log("_dyld_get_objc_selector(%s) => %s\n", selName, uniqueName);
            return uniqueName;
        }
    }
#endif // SUPPORT_PREBUILTLOADERS
    if ( config.log.apis )
        log("_dyld_get_objc_selector(%s) => nullptr\n", selName);
    return nullptr;
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_for_each_objc_class(const char* className,
                                     void (^callback)(void* classPtr, bool isLoaded, bool* stop))
{
    if ( config.log.apis )
        log("_dyld_get_objc_class(%s)\n", className);
#if !TARGET_OS_EXCLAVEKIT
#if SUPPORT_PREBUILTLOADERS
    // If main program has PrebuiltLoader, check classes table in that
    if ( const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet() ) {
        if ( const ObjCDataStructOpt* classesHashTable = mainSet->objcClassOpt() ) {
            bool stop = classesHashTable->forEachDataStruct(className, *this, callback);
            if ( stop ) {
                // If we found the class here, then stop.  Otherwise fall through to looking in the shared cache
                return;
            }
        }
    }
#endif

    // Also check the table in the shared cache
    // The cache class table meaning changed from version 15 -> version 16.
    // Version 15 is the legacy table with cache offsets
    // We don't support that old version here, as dyld is always using a new enough cache
    if ( const objc::ClassHashTable* classHashTable = config.dyldCache.objcClassHashTable ) {
        classHashTable->forEachClass(className,
                                     ^(uint64_t objectCacheOffset, uint16_t dylibObjCIndex, bool &stopObjects) {
            const auto* headerInfoRW = (objc::objc_headeropt_rw_t<uintptr_t>*)config.dyldCache.objcHeaderInfoRW;
            if ( headerInfoRW->isLoaded(dylibObjCIndex) ) {
                // Dylib is loaded, so tell objc about it
                bool callbackStop = false;
                callback((uint8_t*)config.dyldCache.addr + objectCacheOffset, true, &callbackStop);
                if ( callbackStop )
                    stopObjects = true;
            }
        });
    }
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_for_each_objc_protocol(const char* protocolName,
                                        void (^callback)(void* protocolPtr, bool isLoaded, bool* stop))
{
    if ( config.log.apis )
        log("_dyld_get_objc_protocol(%s)\n", protocolName);
#if !TARGET_OS_EXCLAVEKIT
#if SUPPORT_PREBUILTLOADERS
    // If main program has PrebuiltLoader, check protocols table in that
    if ( const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet() ) {
        if ( const ObjCDataStructOpt* protocolsHashTable = mainSet->objcProtocolOpt() ) {
            bool stop = protocolsHashTable->forEachDataStruct(protocolName, *this, callback);
            if ( stop ) {
                // If we found the class here, then stop.  Otherwise fall through to looking in the shared cache
                return;
            }
        }
    }
#endif

    // Also check the table in the shared cache
    // The cache protocol table meaning changed from version 15 -> version 16.
    // Version 15 is the legacy table with cache offsets
    // We don't support that old version here, as dyld is always using a new enough cache
    if ( const objc::ProtocolHashTable* protocolHashTable = config.dyldCache.objcProtocolHashTable ) {
        protocolHashTable->forEachProtocol(protocolName,
                                           ^(uint64_t objectCacheOffset, uint16_t dylibObjCIndex, bool &stopObjects) {
            const auto* headerInfoRW = (objc::objc_headeropt_rw_t<uintptr_t>*)config.dyldCache.objcHeaderInfoRW;
            if ( headerInfoRW->isLoaded(dylibObjCIndex) ) {
                // Dylib is loaded, so tell objc about it
                bool callbackStop = false;
                callback((uint8_t*)config.dyldCache.addr + objectCacheOffset, true, &callbackStop);
                if ( callbackStop )
                    stopObjects = true;
            }
        });
    }
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

void APIs::_dyld_visit_objc_classes(void (^callback)(const void* classPtr))
{
    if ( config.log.apis )
        log("_dyld_visit_objc_classes()\n");
#if !TARGET_OS_EXCLAVEKIT
    // The cache class table meaning changed from version 15 -> version 16.
    // Version 15 is the legacy table with cache offsets
    // We don't support that old version here, as dyld is always using a new enough cache
    if ( const objc::ClassHashTable* classOpt = config.dyldCache.objcClassHashTable ) {
        typedef objc::ClassHashTable::ObjectAndDylibIndex ObjectAndDylibIndex;
        classOpt->forEachClass(^(uint32_t bucketIndex, const char *className,
                                 const dyld3::Array<ObjectAndDylibIndex>& implCacheInfos) {
            for (const ObjectAndDylibIndex& implCacheInfo : implCacheInfos)
                callback((const void*)((uintptr_t)config.dyldCache.addr + implCacheInfo.first));
        });
    }
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

uint32_t APIs::_dyld_objc_class_count(void)
{
    if ( config.log.apis )
        log("_dyld_objc_class_count()\n");
#if !TARGET_OS_EXCLAVEKIT
    // The cache class table meaning changed from version 15 -> version 16.
    // Version 15 is the legacy table with cache offsets
    // We don't support that old version here, as dyld is always using a new enough cache
    if ( const objc::ClassHashTable* classOpt = config.dyldCache.objcClassHashTable ) {
        return classOpt->classCount();
    }
    return 0;
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

bool APIs::_dyld_is_preoptimized_objc_image_loaded(uint16_t imageID)
{
    bool isLoaded = false;

#if !TARGET_OS_EXCLAVEKIT
    if ( !config.dyldCache.addr ) {
        if ( config.log.apis )
            log("_dyld_is_preoptimized_objc_image_loaded(%d) : no dyld shared cache\n", imageID);
        return isLoaded;
    }
    const auto* headerInfoRW = (objc::objc_headeropt_rw_t<uintptr_t>*)config.dyldCache.objcHeaderInfoRW;
    if ( !headerInfoRW ) {
        if ( config.log.apis )
            log("_dyld_is_preoptimized_objc_image_loaded(%d) : no objC RW header\n", imageID);
        return isLoaded;
    }

    if ( imageID >= headerInfoRW->getCount() ) {
        if ( config.log.apis )
            log("_dyld_is_preoptimized_objc_image_loaded(%d) : imageID is invalid\n", imageID);
        return false;
    }
    isLoaded = headerInfoRW->isLoaded(imageID);
#endif // !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_is_preoptimized_objc_image_loaded(%d) => %d\n", imageID, isLoaded);
    return isLoaded;
}


void* APIs::_dyld_for_objc_header_opt_rw()
{
    if ( !config.dyldCache.addr ) {
        if ( config.log.apis )
            log("_dyld_for_objc_header_opt_rw(): no dyld shared cache\n");
        return nullptr;
    }
    void* headerInfoRW = (void*)config.dyldCache.objcHeaderInfoRW;
    if ( !headerInfoRW ) {
        if ( config.log.apis )
            log("_dyld_for_objc_header_opt_rw(): no objC RW header\n");
        return nullptr;
    }
    if ( config.log.apis )
        log("_dyld_for_objc_header_opt_rw() => 0x%llx\n", (uint64_t)headerInfoRW);

    return headerInfoRW;
}

const void* APIs::_dyld_for_objc_header_opt_ro()
{
    if ( !config.dyldCache.addr ) {
        if ( config.log.apis )
            log("_dyld_for_objc_header_opt_ro(): no dyld shared cache\n");
        return nullptr;
    }
    void* headerInfoRO = (void*)config.dyldCache.objcHeaderInfoRO;
    if ( !headerInfoRO ) {
        if ( config.log.apis )
            log("_dyld_for_objc_header_opt_ro(): no objC RO header\n");
        return nullptr;
    }
    if ( config.log.apis )
        log("_dyld_for_objc_header_opt_ro() => 0x%llx\n", (uint64_t)headerInfoRO);

    return headerInfoRO;
}

bool APIs::_dyld_objc_uses_large_shared_cache(void)
{
    // This is always true, as every cache is on a new enough platform to have Large Shared Caches
    return true;
}

struct header_info_rw {

    bool getLoaded() const {
        return isLoaded;
    }

private:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-private-field"
#ifdef __LP64__
    uintptr_t isLoaded              : 1;
    uintptr_t allClassesRealized    : 1;
    uintptr_t next                  : 62;
#else
    uintptr_t isLoaded              : 1;
    uintptr_t allClassesRealized    : 1;
    uintptr_t next                  : 30;
#endif
#pragma clang diagnostic pop
};

struct objc_headeropt_rw_t {
    uint32_t count;
    uint32_t entsize;
    header_info_rw headers[0];  // sorted by mhdr address

    void* get(uint32_t i) const {
        assert(i < count);
        return (void*)((uint8_t *)&headers + (i * entsize));
    }

    bool isLoaded(uint32_t i) const {
        return ((header_info_rw*)get(i))->getLoaded();
    }
};

struct _dyld_protocol_conformance_result
APIs::_dyld_find_protocol_conformance(const void *protocolDescriptor,
                                      const void *metadataType,
                                      const void *typeDescriptor) const
{
    if ( config.log.apis )
        log("_dyld_find_protocol_conformance(%p, %p, %p)\n", protocolDescriptor, metadataType, typeDescriptor);
#if !TARGET_OS_EXCLAVEKIT
    const objc_headeropt_rw_t* objcHeaderInfoRW = nullptr;
    if ( const objc::HeaderInfoRW* optRW = config.dyldCache.objcHeaderInfoRW )
        objcHeaderInfoRW = (objc_headeropt_rw_t*)optRW;

    const SwiftOptimizationHeader* swiftOptHeader = config.dyldCache.swiftCacheInfo;

    // We need objc, swift, and of the correct versions.  If anything isn't right, just bail out
    if ( !objcHeaderInfoRW || !swiftOptHeader || (swiftOptHeader->version != 1))
        return { _dyld_protocol_conformance_result_kind_not_found, nullptr };

    if ( (typeDescriptor != nullptr) && (swiftOptHeader->typeConformanceHashTableCacheOffset != 0) ) {
        const SwiftHashTable* typeHashTable = (const SwiftHashTable*)((uint8_t*)config.dyldCache.addr + swiftOptHeader->typeConformanceHashTableCacheOffset);

        SwiftTypeProtocolConformanceLocationKey protocolKey;
        protocolKey.typeDescriptorCacheOffset  = (uint64_t)typeDescriptor - (uint64_t)config.dyldCache.addr;
        protocolKey.protocolCacheOffset        = (uint64_t)protocolDescriptor - (uint64_t)config.dyldCache.addr;
        const auto* protocolTarget = typeHashTable->getValue<SwiftTypeProtocolConformanceLocation>(protocolKey, nullptr);
        if ( protocolTarget != nullptr ) {
            if ( !protocolTarget->nextIsDuplicate ) {
                // No duplicates, so return this conformance if its from a loaded image.
                if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                    const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                    return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                }
            } else {
                // One of the duplicates might be loaded.  We'll return the first loaded one if we find one
                while ( true ) {
                    if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                        const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                        return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                    }
                    if ( !protocolTarget->nextIsDuplicate )
                        break;
                    protocolTarget = ++protocolTarget;
                }
                // TODO: Should we error here?  Somehow the user has pointers to data which should have been loaded.
            }
        }
    }

    if ( (metadataType != nullptr) && (swiftOptHeader->metadataConformanceHashTableCacheOffset != 0) ) {
        const SwiftHashTable* metadataHashTable = (const SwiftHashTable*)((uint8_t*)config.dyldCache.addr + swiftOptHeader->metadataConformanceHashTableCacheOffset);

        SwiftMetadataProtocolConformanceLocationKey protocolKey;
        protocolKey.metadataCacheOffset  = (uint64_t)metadataType - (uint64_t)config.dyldCache.addr;
        protocolKey.protocolCacheOffset  = (uint64_t)protocolDescriptor - (uint64_t)config.dyldCache.addr;
        const auto* protocolTarget = metadataHashTable->getValue<SwiftMetadataProtocolConformanceLocation>(protocolKey, nullptr);
        if (protocolTarget != nullptr) {
            if ( !protocolTarget->nextIsDuplicate ) {
                // No duplicates, so return this conformance if its from a loaded image.
                if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                    const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                    return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                }
            } else {
                // One of the duplicates should be loaded.  We'll return the first loaded one
                while ( true ) {
                    if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                        const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                        return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                    }
                    if ( !protocolTarget->nextIsDuplicate )
                        break;
                    protocolTarget = ++protocolTarget;
                }
                // TODO: Should we error here?  Somehow the user has pointers to data which should have been loaded.
            }
        }
    }
    return { _dyld_protocol_conformance_result_kind_not_found, nullptr };
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

struct _dyld_protocol_conformance_result
APIs::_dyld_find_foreign_type_protocol_conformance(const void *protocol,
                                                   const char *foreignTypeIdentityStart,
                                                   size_t foreignTypeIdentityLength) const
{
    if ( config.log.apis )
        log("_dyld_find_protocol_conformance(%p, %s)\n", protocol, foreignTypeIdentityStart);
#if !TARGET_OS_EXCLAVEKIT
    const objc_headeropt_rw_t* objcHeaderInfoRW = nullptr;
    if ( const objc::HeaderInfoRW* optRW = config.dyldCache.objcHeaderInfoRW )
        objcHeaderInfoRW = (objc_headeropt_rw_t*)optRW;

    const SwiftOptimizationHeader* swiftOptHeader = config.dyldCache.swiftCacheInfo;

    // We need objc, swift, and of the correct versions.  If anything isn't right, just bail out
    if ( !objcHeaderInfoRW || !swiftOptHeader || (swiftOptHeader->version != 1))
        return { _dyld_protocol_conformance_result_kind_not_found, nullptr };

    if ( swiftOptHeader->foreignTypeConformanceHashTableCacheOffset != 0 ) {
        const SwiftHashTable* typeHashTable = (const SwiftHashTable*)((uint8_t*)config.dyldCache.addr + swiftOptHeader->foreignTypeConformanceHashTableCacheOffset);

        SwiftForeignTypeProtocolConformanceLookupKey protocolKey;
        protocolKey.foreignDescriptorName = std::string_view(foreignTypeIdentityStart, foreignTypeIdentityLength);
        protocolKey.protocolCacheOffset     = (uint64_t)protocol - (uint64_t)config.dyldCache.addr;
        const auto* protocolTarget = typeHashTable->getValue<SwiftForeignTypeProtocolConformanceLookupKey, SwiftForeignTypeProtocolConformanceLocation>(protocolKey, (const uint8_t*)config.dyldCache.addr);
        if ( protocolTarget != nullptr ) {
            if ( !protocolTarget->nextIsDuplicate ) {
                // No duplicates, so return this conformance if its from a loaded image.
                if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                    const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                    return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                }
            } else {
                // One of the duplicates might be loaded.  We'll return the first loaded one if we find one
                while ( true ) {
                    if ( objcHeaderInfoRW->isLoaded(protocolTarget->dylibObjCIndex) ) {
                        const uint8_t* conformanceDescriptor = (const uint8_t*)config.dyldCache.addr + protocolTarget->protocolConformanceCacheOffset;
                        return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
                    }
                    if ( !protocolTarget->nextIsDuplicate )
                        break;
                    protocolTarget = ++protocolTarget;
                }
                // TODO: Should we error here?  Somehow the user has pointers to data which should have been loaded.
            }
        }
    }

    return { _dyld_protocol_conformance_result_kind_not_found, nullptr };
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

uint32_t APIs::_dyld_swift_optimizations_version() const
{
    return 1;
}


bool APIs::_dyld_has_preoptimized_swift_protocol_conformances(const struct mach_header* mh)
{
#if SUPPORT_PREBUILTLOADERS
    const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)mh;
    // return early if is not a swift binary
    if ( !ma->hasSwift() )
        return false;

    if ( const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet() ) {
        // return early if there is no prebuilt swift protocols in the closure
        if ( !mainSet->hasOptimizedSwift() )
            return false;

        size_t ldrCount = mainSet->loaderCount();
        for (size_t i = 0; i < ldrCount; i++) {
            const PrebuiltLoader* ldr = mainSet->atIndex(i);
            const dyld3::MachOAnalyzer* maLoader = ldr->analyzer(*this);
            if ( maLoader ==  ma )
                return true;
        }
    }
#endif // SUPPORT_PREBUILTLOADERS
    return false;

}

struct _dyld_protocol_conformance_result
APIs::_dyld_find_protocol_conformance_on_disk(const void *protocolDescriptor,
                                              const void *metadataType,
                                              const void *typeDescriptor,
                                              uint32_t flags)
{
    if ( config.log.apis )
        log("_dyld_find_protocol_conformance_on_disk(%p, %p, %p)\n", protocolDescriptor, metadataType, typeDescriptor);
#if SUPPORT_PREBUILTLOADERS
    const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet();
    if ( mainSet == nullptr || !mainSet->hasOptimizedSwift()) {
        return { _dyld_protocol_conformance_result_kind_not_found, nullptr };
    }

    const uint64_t* typeProtocolTable = mainSet->swiftTypeProtocolTable();
    if ( typeDescriptor != nullptr && typeProtocolTable != nullptr && this->typeProtocolMap != nullptr) {

        APIs::TypeKey protocolKey = {
            PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)typeDescriptor),
            PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)protocolDescriptor)
        };

        auto* protocolTargetIt = typeProtocolMap->find(protocolKey);
        if ( protocolTargetIt != typeProtocolMap->end() ) {
            bool foundTypeConformance = false;
            while ( true ) {
                if ( EqualTypeConformanceLookupKey::equal(protocolTargetIt->key, (uint64_t)typeDescriptor, (uint64_t)protocolDescriptor, this) ) {
                    foundTypeConformance = true;
                    break;
                }
                if ( !protocolTargetIt->next.hasMoreDuplicates() )
                    break;
                protocolTargetIt = typeProtocolMap->nextDuplicate(protocolTargetIt);
            }

            if ( foundTypeConformance ) {
                const auto& conformanceTarget = protocolTargetIt->value;
                uint16_t idx = conformanceTarget.protocolConformance.loaderRef().index;
                uint8_t* loaderAddress = (uint8_t*)mainSet->atIndex(idx)->loadAddress(*this);
                const uint8_t* conformanceDescriptor = loaderAddress + conformanceTarget.protocolConformance.offset();
                return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
            }
        }
    }

    const uint64_t* metadataProtocolTable = mainSet->swiftMetadataProtocolTable();
    if ( metadataType != nullptr && metadataProtocolTable != nullptr && this->metadataProtocolMap != nullptr) {

        APIs::MetadataKey protocolKey = {
            PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)metadataType),
            PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)protocolDescriptor)
        };
        auto* protocolTargetIt = metadataProtocolMap->find(protocolKey);
        if ( protocolTargetIt != metadataProtocolMap->end() ) {
            bool foundMetadataConformance = false;
            while ( true ) {
                if ( EqualMetadataConformanceLookupKey::equal(protocolTargetIt->key, (uint64_t)metadataType, (uint64_t)protocolDescriptor, this) ) {
                    foundMetadataConformance = true;
                    break;
                }
                if ( !protocolTargetIt->next.hasMoreDuplicates() )
                    break;
                protocolTargetIt = metadataProtocolMap->nextDuplicate(protocolTargetIt);
            }

            if ( foundMetadataConformance ) {
                const auto& conformanceTarget = protocolTargetIt->value;
                uint16_t idx = conformanceTarget.protocolConformance.loaderRef().index;
                uint8_t* loaderAddress = (uint8_t*)mainSet->atIndex(idx)->loadAddress(*this);
                const uint8_t* conformanceDescriptor = loaderAddress + conformanceTarget.protocolConformance.offset();
                return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
            }
        }
    }
#endif // SUPPORT_PREBUILTLOADERS
    return { _dyld_protocol_conformance_result_kind_not_found, nullptr };

}

struct _dyld_protocol_conformance_result
APIs::_dyld_find_foreign_type_protocol_conformance_on_disk(const void *protocol,
                                                           const char *foreignTypeIdentityStart,
                                                           size_t foreignTypeIdentityLength,
                                                           uint32_t flags)
{
#if SUPPORT_PREBUILTLOADERS
    const PrebuiltLoaderSet* mainSet = this->processPrebuiltLoaderSet();
    if ( mainSet == nullptr || !mainSet->hasOptimizedSwift())
        return { _dyld_protocol_conformance_result_kind_not_found, nullptr };

    const uint64_t* foreignTable = mainSet->swiftForeignTypeProtocolTable();
    if ( foreignTable == nullptr || this->foreignProtocolMap == nullptr)
        return { _dyld_protocol_conformance_result_kind_not_found, nullptr };

    APIs::ForeignKey protocolKey = {
        0,
        PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)foreignTypeIdentityStart),
        foreignTypeIdentityLength,
        PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)protocol)
    };

    auto* protocolTargetIt = foreignProtocolMap->find(protocolKey);
    if ( protocolTargetIt != foreignProtocolMap->end() ) {

        bool foundForeignTypeConformance = false;
        while ( true ) {
            if ( EqualForeignConformanceLookupKey::equal(protocolTargetIt->key, foreignTypeIdentityStart, foreignTypeIdentityLength, (uint64_t)protocol, this) ) {
                foundForeignTypeConformance = true;
                break;
            }
            if ( !protocolTargetIt->next.hasMoreDuplicates() )
                break;
            protocolTargetIt = foreignProtocolMap->nextDuplicate(protocolTargetIt);
        }

        if ( foundForeignTypeConformance ) {
            const auto conformanceTarget = protocolTargetIt->value;
            uint16_t idx = conformanceTarget.protocolConformance.loaderRef().index;
            uint8_t* loaderAddress = (uint8_t*)mainSet->atIndex(idx)->loadAddress(*this);
            const uint8_t* conformanceDescriptor = loaderAddress + conformanceTarget.protocolConformance.offset();
            return { _dyld_protocol_conformance_result_kind_found_descriptor, conformanceDescriptor };
        }
    }
#endif // SUPPORT_PREBUILTLOADERS
    return { _dyld_protocol_conformance_result_kind_not_found, nullptr };
}

static _dyld_section_info_result lookupObjCInfo(_dyld_section_location_kind kind, const dyld3::MachOFile* mf,
                                                const SectionLocations* metadata)
{
    const uint64_t* sectionOffsets = metadata->offsets;
    const uint64_t* sectionSizes = metadata->sizes;

    uint64_t sectionOffset = sectionOffsets[kind];
    uint64_t sectionSize = sectionSizes[kind];
    if ( sectionOffset != 0 )
        return { (uint8_t*)mf + sectionOffset, (size_t)sectionSize };

    return { nullptr, 0 };
}

_dyld_section_info_result APIs::_dyld_lookup_section_info(const struct mach_header* mh,
                                                          _dyld_section_location_info_t sectionLocations,
                                                          _dyld_section_location_kind kind)
{
    // Clients might have a newer header than the dyld in use, so make sure they don't
    // call with an out of bounds entry
    if ( kind >= _dyld_section_location_count )
        return { nullptr, (size_t)-1 };

    const dyld3::MachOFile* mf = (const dyld3::MachOFile*)mh;
    if ( sectionLocations == nullptr ) {
        SectionLocations metadata;
        JustInTimeLoader::parseSectionLocations(mf, metadata);
        return lookupObjCInfo(kind, mf, &metadata);
    }

#if !TARGET_OS_EXCLAVEKIT
    // The section location handle is actually a Loader*, but for the shared cache that is going
    // to point to a Loader* which we may not use.  Make sure shared cache loaders are in use
    if ( const DyldSharedCache* sharedCache = config.dyldCache.addr ) {
        uint64_t mappedSize = (uint64_t)sharedCache->mappedSize();
        if ( ((uintptr_t)sectionLocations >= (uintptr_t)sharedCache) && ((uintptr_t)sectionLocations > ((uintptr_t)sharedCache + mappedSize)) ) {
            if ( this->cachedDylibsPrebuiltLoaderSet() == nullptr )
                return this->_dyld_lookup_section_info(mh, nullptr, kind);
        }
    }
#endif // !TARGET_OS_EXCLAVEKIT

    // We have metadata, but it might be the wrong version, ie, dyld root running with shared cache
    // metadata
    const Loader* ldr = (const Loader*)sectionLocations;
    if ( ldr->getSectionLocations()->version != 1 )
        return this->_dyld_lookup_section_info(mh, nullptr, kind);

    return lookupObjCInfo(kind, mf, ldr->getSectionLocations());
}

static PseudoDylibCallbacks *createPseudoDylibCallbacks(Allocator &allocator,
                                                        _dyld_pseudodylib_dispose_string dispose_string,
                                                        _dyld_pseudodylib_initialize initialize,
                                                        _dyld_pseudodylib_deinitialize deinitialize,
                                                        _dyld_pseudodylib_lookup_symbols lookup_symbols,
                                                        _dyld_pseudodylib_lookup_address lookup_address,
                                                        _dyld_pseudodylib_find_unwind_sections find_unwind_sections,
                                                        _dyld_pseudodylib_loadable_at_path loadable_at_path) {
    PseudoDylibCallbacks* pd_cb =
         (PseudoDylibCallbacks*)allocator.aligned_alloc(alignof(PseudoDylibCallbacks),
                                                        sizeof(PseudoDylibCallbacks));
    pd_cb->dispose_string = dispose_string;
    pd_cb->initialize = initialize;
    pd_cb->deinitialize = deinitialize;
    pd_cb->lookupSymbols = lookup_symbols;
    pd_cb->lookupAddress = lookup_address;
    pd_cb->findUnwindSections = find_unwind_sections;
    pd_cb->loadableAtPath = loadable_at_path;
    return pd_cb;
}

_dyld_pseudodylib_callbacks_handle APIs::_dyld_pseudodylib_register_callbacks(const struct _dyld_pseudodylib_callbacks* callbacks) {
    PseudoDylibCallbacks* pd_cb = nullptr;
    locks.withLoadersWriteLock(memoryManager, [&] {
      if (callbacks->version == 1) {
          const auto* callbacks_v1 = (const _dyld_pseudodylib_callbacks_v1*)callbacks;
          pd_cb = createPseudoDylibCallbacks(persistentAllocator, callbacks_v1->dispose_error_message,
                                             callbacks_v1->initialize, callbacks_v1->deinitialize,
                                             callbacks_v1->lookup_symbols, callbacks_v1->lookup_address,
                                             callbacks_v1->find_unwind_sections, nullptr);
      } else if (callbacks->version == 2) {
          const auto* callbacks_v2 = (const _dyld_pseudodylib_callbacks_v2*)callbacks;
          pd_cb = createPseudoDylibCallbacks(persistentAllocator, callbacks_v2->dispose_string,
                                             callbacks_v2->initialize, callbacks_v2->deinitialize,
                                             callbacks_v2->lookup_symbols, callbacks_v2->lookup_address,
                                             callbacks_v2->find_unwind_sections,
                                             callbacks_v2->loadable_at_path);
      }
    });

    if (!pd_cb && config.log.apis ) {
        log("_dyld_pseudodylib_register_callbacks(%p): callbacks struct version not recognized",
            callbacks);
    }

    return (_dyld_pseudodylib_callbacks_handle)pd_cb;
}

void APIs::_dyld_pseudodylib_deregister_callbacks(_dyld_pseudodylib_callbacks_handle callbacks_handle) {
    if (!callbacks_handle)
        return;
    locks.withLoadersWriteLock(memoryManager, [&] {
        persistentAllocator.free((PseudoDylibCallbacks*)callbacks_handle);
    });
}

_dyld_pseudodylib_handle APIs::_dyld_pseudodylib_register(
        void* addr, size_t size, _dyld_pseudodylib_callbacks_handle callbacks_handle, void* context) {
    const MachOFile* pseudoDylibMF = (const MachOFile*)addr;
    const char* path = pseudoDylibMF->installName();

    if (!path) {
        if ( config.log.apis ) {
          log("_dyld_register_pseudodylib(%p, %lx, %p, %p): registered range does not contain an install name",
              addr, size, (void*)callbacks_handle, context);
        }
        return nullptr;
    }

    if ( config.log.apis )
        log("_dyld_register_pseudodylib(%p, %lx, %p, %p): [%p, %p) \"%s\"\n",
            addr, size, callbacks_handle, context, addr, (const char*)addr + size, path);

    _dyld_pseudodylib_handle result = nullptr;
    PseudoDylib* existingPD = nullptr;
    locks.withLoadersWriteLock(memoryManager, [&] {
        for (auto &pd : pseudoDylibs)
            if (strcmp(pd->getIdentifier(), path) == 0) {
                existingPD = pd;
                break;
            }
        if (!existingPD) {
            PseudoDylib* newPD = PseudoDylib::create(persistentAllocator, path, addr, size, (PseudoDylibCallbacks*)callbacks_handle, context);
            pseudoDylibs.push_back(newPD);
            result = (_dyld_pseudodylib_handle)newPD;
        }
    });

    if (existingPD) {
        if ( config.log.apis ) {
          log("_dyld_register_pseudodylib(\"%s\", %p, %lx): identifier conflicts with existing registration covering [%p, %p)",
              path, addr, size, existingPD->getAddress(), (const char *)existingPD->getAddress() + existingPD->getSize());
        }
        assert(result == nullptr && "Existing pseudo-dylib, but result set anyway?");
    }

    return result;
}

void APIs::_dyld_pseudodylib_deregister(_dyld_pseudodylib_handle pd_handle) {
    PseudoDylib* pd = (PseudoDylib*)pd_handle;

    if ( config.log.apis )
        log("_dyld_deregister_pseudodylib(<handle for \"%s\">)\n", pd->getIdentifier());

    bool found = false;
    locks.withLoadersWriteLock(memoryManager, [&] {
        for (auto it = pseudoDylibs.begin(); it != pseudoDylibs.end(); ++it)
            if (*it == pd) {
                found = true;
                pseudoDylibs.erase(it);
                persistentAllocator.free(pd);
                break;
            }
    });

    if (!found && config.log.apis ) {
        log("_dyld_deregister_pseudodylib(<handle for \"%s\">): no registered pseudo-dylib for handle",
            pd->getIdentifier());
    }
}

const mach_header* APIs::_dyld_get_prog_image_header()
{
    const mach_header* result = config.process.mainExecutable;
    if ( config.log.apis )
        log("_dyld_get_prog_image_header() => %p\n", result);
    return result;
}

bool APIs::_dyld_has_fix_for_radar(const char* radar)
{
    if ( config.log.apis )
        log("_dyld_has_fix_for_radar(%s)\n", radar);
    // FIXME
    return false;
}

bool APIs::_dyld_is_objc_constant(DyldObjCConstantKind kind, const void* addr)
{
    if ( config.log.apis )
        log("_dyld_is_objc_constant(%d, %p)\n", kind, addr);
    // FIXME
    return false;
}

uint32_t APIs::_dyld_launch_mode()
{
    uint32_t result = 0;

    // map "dyld3-using-closure" to main Loader is a PrebuiltLoader
    if ( mainExecutableLoader->isPrebuilt )
        result |= DYLD_LAUNCH_MODE_USING_CLOSURE;

    // set if a closure file was written
    if ( this->saveAppClosureFile() && this->didSavePrebuiltLoaderSet() )
        result |= DYLD_LAUNCH_MODE_CLOSURE_SAVED_TO_FILE;

    // hack to see if main PrebuiltLoader is in dyld cache
    if ( mainExecutableLoader->isPrebuilt && (config.dyldCache.addr != nullptr) && ((uint8_t*)mainExecutableLoader > (uint8_t*)config.dyldCache.addr) )
        result |= DYLD_LAUNCH_MODE_CLOSURE_FROM_OS;

    // set if interposing is being used
    if ( !this->interposingTuplesAll.empty() )
        result |= DYLD_LAUNCH_MODE_HAS_INTERPOSING;

    // set if customer dyld cache is in use
    const DyldSharedCache* cache         = this->config.dyldCache.addr;
    const bool             customerCache = (cache != nullptr) && (cache->header.cacheType == kDyldSharedCacheTypeProduction);
    if ( customerCache )
        result |= DYLD_LAUNCH_MODE_OPTIMIZED_DYLD_CACHE;

    if ( config.log.apis )
        log("_dyld_launch_mode() => 0x%08X\n", result);
    return result;
}

void APIs::_dyld_register_driverkit_main(void (*mainFunc)())
{
    if ( config.log.apis )
        log("_dyld_register_driverkit_main(%p)\n", mainFunc);

    if ( config.process.platform == dyld3::Platform::driverKit ) {
#if BUILDING_DYLD
        if ( this->mainFunc() != nullptr )
            halt("_dyld_register_driverkit_main() may only be called once");
#endif
        setMainFunc((MainFunc)mainFunc);
    }
    else {
        log("_dyld_register_driverkit_main() can only be called in DriverKit processes\n");
    }
}

bool APIs::_dyld_shared_cache_contains_path(const char* path)
{
    bool result = (config.canonicalDylibPathInCache(path) != nullptr);
    if ( config.log.apis )
        log("_dyld_shared_cache_contains_path(%s) => %d\n", path, result);
    return result;
}

const char* APIs::_dyld_shared_cache_real_path(const char* path)
{
    const char* result = config.canonicalDylibPathInCache(path);
    if ( config.log.apis )
        log("_dyld_shared_cache_real_path(%s) => '%s'\n", path, result);
    return result;
}

bool APIs::_dyld_shared_cache_is_locally_built()
{
    bool result = false;
    if ( const DyldSharedCache* cache = config.dyldCache.addr ) {
        result = (cache->header.locallyBuiltCache == 1);
    }
    if ( config.log.apis )
        log("_dyld_shared_cache_is_locally_built() => %d\n", result);
    return result;
}

bool APIs::dyld_need_closure(const char* execPath, const char* dataContainerRootDir)
{
    if ( config.log.apis )
        log("dyld_need_closure()\n");
    // FIXME
    return false;
}

void APIs::_dyld_missing_symbol_abort()
{
#if BUILDING_DYLD
    halt("missing symbol called");
#endif
}

void APIs::_tlv_atexit(void (*termFunc)(void* objAddr), void* objAddr)
{
#if !TARGET_OS_EXCLAVEKIT
  #if __has_feature(tls)
    addTLVTerminationFunc(termFunc, objAddr);
  #endif
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

// called by exit() before it calls cxa_finalize() so that thread_local
// objects are destroyed before global objects.
void APIs::_tlv_exit()
{
#if !TARGET_OS_EXCLAVEKIT
  #if __has_feature(tls)
    exitTLV();
  #endif
#else
    unavailable_on_exclavekit();
#endif // !TARGET_OS_EXCLAVEKIT
}

#if __has_feature(tls)
// linked images with TLV have references to this symbol, but it is never used at runtime
void APIs::_tlv_bootstrap()
{
#if BUILDING_DYLD
    halt("_tlv_bootstrap called");
#endif
}
#endif

void APIs::obsolete()
{
#if BUILDING_DYLD
    halt("obsolete dyld API called");
#else
    abort();
#endif
}

//
// macOS needs to support an old API that only works with fileype==MH_BUNDLE.
// In this deprecated API (unlike dlopen), loading and linking are separate steps.
// NSCreateObjectFileImageFrom*() just maps in the bundle mach-o file.
// NSLinkModule() does the load of dependent modules and rebasing/binding.
// To unload one of these, you must call NSUnLinkModule() and NSDestroyObjectFileImage() in any order!
//

NSObjectFileImageReturnCode APIs::NSCreateObjectFileImageFromFile(const char* path, NSObjectFileImage* ofi)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSCreateObjectFileImageFromFile(%s)\n", path);

    // verify path exists
    if ( !config.syscall.fileExists(path) )
        return NSObjectFileImageFailure;

    // create ofi that just contains path. NSLinkModule does all the work (can't use operator new in dyld)
    void*                storage = this->libSystemHelpers->malloc(sizeof(__NSObjectFileImage));
    __NSObjectFileImage* result  = new (storage) __NSObjectFileImage();
    result->path                 = (char*)this->libSystemHelpers->malloc(strlen(path)+1);
    strcpy((char*)(result->path), path);
    *ofi                        = result;

    return NSObjectFileImageSuccess;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSObjectFileImageReturnCode APIs::NSCreateObjectFileImageFromMemory(const void* memImage, size_t memImageSize, NSObjectFileImage* ofi)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSCreateObjectFileImageFromMemory(%p, 0x%08lX)\n", memImage, memImageSize);
    // sanity check the buffer is a mach-o file
    __block Diagnostics diag;

    // check if it is current arch mach-o or fat with slice for current arch
    bool             usable = false;
    const MachOFile* mf     = (MachOFile*)memImage;
    if ( mf->hasMachOMagic() && mf->isMachO(diag, memImageSize) ) {
        usable = (config.process.archs->grade(mf->cputype, mf->cpusubtype, false) != 0);
    }
    else if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(memImage) ) {
        uint64_t sliceOffset;
        uint64_t sliceLen;
        bool     missingSlice;
        if ( ff->isFatFileWithSlice(diag, memImageSize, *config.process.archs, false, sliceOffset, sliceLen, missingSlice) ) {
            mf = (MachOFile*)((long)memImage + sliceOffset);
            if ( mf->isMachO(diag, sliceLen) ) {
                usable = true;
            }
        }
    }
    if ( usable ) {
        if ( !mf->loadableIntoProcess(config.process.platform, "OFI", config.security.isInternalOS) )
            usable = false;
    }
    if ( !usable ) {
        return NSObjectFileImageFailure;
    }

    // this API can only be used with bundles
    if ( !mf->isBundle() ) {
        return NSObjectFileImageInappropriateFile;
    }

    // some apps deallocate the buffer right after calling NSCreateObjectFileImageFromMemory(), so we need to copy the buffer
    vm_address_t newAddr = 0;
    kern_return_t r = this->libSystemHelpers->vm_allocate(mach_task_self(), &newAddr, memImageSize, VM_FLAGS_ANYWHERE);
    if ( r == KERN_SUCCESS ) {
        ::memcpy((void*)newAddr, memImage, memImageSize);
        if ( config.log.apis )
            log("NSCreateObjectFileImageFromMemory() copy %p to %p\n", memImage, (void*)newAddr);
        memImage = (void*)newAddr;
    }

    // allocate ofi that just lists the memory range
    void*                storage = this->libSystemHelpers->malloc(sizeof(__NSObjectFileImage));
    __NSObjectFileImage* result  = new (storage) __NSObjectFileImage();
    result->memSource            = memImage;
    result->memLength            = memImageSize;
    *ofi                         = result;

    return NSObjectFileImageSuccess;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSModule APIs::NSLinkModule(NSObjectFileImage ofi, const char* moduleName, uint32_t options)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSLinkModule(%p, %s)\n", ofi, moduleName);
    // if this is memory based image, write to temp file, then use file based loading
    int openMode = 0;
    if ( ofi->memSource != nullptr ) {
        // make temp file with content of memory buffer
        ofi->path = nullptr;
        char        tempFileName[PATH_MAX];
        const char* tmpDir = this->libSystemHelpers->getenv("TMPDIR");
        if ( (tmpDir != nullptr) && (strlen(tmpDir) > 2) ) {
            strlcpy(tempFileName, tmpDir, PATH_MAX);
            if ( tmpDir[strlen(tmpDir) - 1] != '/' )
                strlcat(tempFileName, "/", PATH_MAX);
        }
        else
            strlcpy(tempFileName, "/tmp/", PATH_MAX);
        strlcat(tempFileName, "NSCreateObjectFileImageFromMemory-XXXXXXXX", PATH_MAX);
        int fd = this->libSystemHelpers->mkstemp(tempFileName);
        if ( fd != -1 ) {
            ssize_t writtenSize = ::pwrite(fd, ofi->memSource, ofi->memLength, 0);
            if ( writtenSize == ofi->memLength ) {
                ofi->path = (char*)this->libSystemHelpers->malloc(strlen(tempFileName)+1);
                ::strcpy((char*)(ofi->path), tempFileName);
            }
            else {
                //log_apis("NSLinkModule() => NULL (could not save memory image to temp file)\n");
            }
            ::close(fd);
        }
        // <rdar://74913193> support old licenseware plugins
        openMode = RTLD_UNLOADABLE | RTLD_NODELETE;
    }

    if ( ofi->path == nullptr )
        return nullptr;

    // dlopen the binary outside of the read lock as we don't want to risk deadlock
    void* callerAddress = __builtin_return_address(0);
    ofi->handle = dlopen_from(ofi->path, openMode, callerAddress);
    if ( ofi->handle == nullptr ) {
        if ( config.log.apis )
            log("NSLinkModule(%p, %s) => NULL (%s)\n", ofi, moduleName, dlerror());
        return nullptr;
    }

    bool          firstOnly;
    const Loader* ldr = loaderFromHandle(ofi->handle, firstOnly);
    ofi->loadAddress  = ldr->loadAddress(*this);

    // if memory based load, delete temp file
    if ( ofi->memSource != nullptr ) {
        ::unlink(ofi->path);
    }

    if ( config.log.apis )
        log("NSLinkModule(%p, %s) => %p\n", ofi, moduleName, ofi->handle);
    return (NSModule)ofi->handle;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

// NSUnLinkModule unmaps the image, but does not release the NSObjectFileImage
bool APIs::NSUnLinkModule(NSModule module, uint32_t options)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSUnLinkModule(%p)\n", module);

    int closeResult = dlclose(module);
    return (closeResult == 0);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

// NSDestroyObjectFileImage releases the NSObjectFileImage, but the mapped image may remain in use
bool APIs::NSDestroyObjectFileImage(NSObjectFileImage ofi)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSDestroyObjectFileImage(%p)\n", ofi);

    if ( ofi->memSource != nullptr ) {
        // if object was created from a memory, release that memory
        // NOTE: this is the way dyld has always done this. NSCreateObjectFileImageFromMemory() hands ownership of the memory to dyld
        // we don't know if memory came from malloc or vm_allocate, so ask malloc
        if ( this->libSystemHelpers->malloc_size(ofi->memSource) != 0 )
            this->libSystemHelpers->free((void*)(ofi->memSource));
        else
            this->libSystemHelpers->vm_deallocate(mach_task_self(), (vm_address_t)ofi->memSource, ofi->memLength);
    }

    // ofi always owns the path
    if ( ofi->path != nullptr )
        this->libSystemHelpers->free((void*)(ofi->path));

    // free object
    this->libSystemHelpers->free((void*)ofi);

    return true;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
static const Loader* getLoader(NSObjectFileImage ofi)
{
    if ( ofi == nullptr )
        return nullptr;
    if ( ofi->handle == nullptr )
        return nullptr;
    bool firstOnly;
    return loaderFromHandle(ofi->handle, firstOnly);
}
#endif

bool APIs::NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage ofi, const char* symbolName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSIsSymbolDefinedInObjectFileImage(%p, %s)\n", ofi, symbolName);

    const Loader* ldr = getLoader(ofi);
    if ( ldr == nullptr )
        return false;
    void* addr;
    bool  resultPointsToInstructions = false;
    return ldr->loadAddress(*this)->hasExportedSymbol(symbolName, nullptr, &addr, &resultPointsToInstructions);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void* APIs::NSGetSectionDataInObjectFileImage(NSObjectFileImage ofi, const char* segmentName, const char* sectionName, size_t* size)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSGetSectionDataInObjectFileImage(%p, %s, %s)\n", ofi, segmentName, sectionName);

    const Loader* ldr = getLoader(ofi);
    if ( ldr == nullptr )
        return nullptr;

    uint64_t    sz;
    const void* result = ldr->loadAddress(*this)->findSectionContent(segmentName, sectionName, sz);
    *size              = (size_t)sz;

    return (void*)result;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

const char* APIs::NSNameOfModule(NSModule m)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSNameOfModule(%p)\n", m);
    bool firstOnly;
    if ( const Loader* ldr = loaderFromHandle(m, firstOnly) )
        return ldr->path();
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

const char* APIs::NSLibraryNameForModule(NSModule m)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSLibraryNameForModule(%p)\n", m);
    bool firstOnly;
    if ( const Loader* ldr = loaderFromHandle(m, firstOnly) )
        return ldr->path();
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::flatFindSymbol(const char* symbolName, void** symbolAddress, const mach_header** foundInImageAtLoadAddress)
{
    __block bool result = false;
    locks.withLoadersReadLock(^{
        for ( const Loader* ldr : loaded ) {
            Diagnostics diag;
            Loader::ResolvedSymbol symInfo;
            if ( ldr->hasExportedSymbol(diag, *this, symbolName, Loader::shallow, Loader::skipResolver, &symInfo) ) {
                const MachOLoaded* ml = symInfo.targetLoader->loadAddress(*this);
                *symbolAddress             = (void*)((uintptr_t)ml + symInfo.targetRuntimeOffset);
                *foundInImageAtLoadAddress = ml;
                result                     = true;
                return;
            }
        }
    });
    return result;
}

bool APIs::NSIsSymbolNameDefined(const char* symbolName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    const mach_header* foundInImageAtLoadAddress;
    void*              address;
    return flatFindSymbol(symbolName, &address, &foundInImageAtLoadAddress);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::NSIsSymbolNameDefinedWithHint(const char* symbolName, const char* libraryNameHint)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    const mach_header* foundInImageAtLoadAddress;
    void*              address;
    return flatFindSymbol(symbolName, &address, &foundInImageAtLoadAddress);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::NSIsSymbolNameDefinedInImage(const struct mach_header* mh, const char* symbolName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    void* addr;
    bool  resultPointsToInstructions = false;
    return ((MachOLoaded*)mh)->hasExportedSymbol(symbolName, nullptr, &addr, &resultPointsToInstructions);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSSymbol APIs::NSLookupAndBindSymbol(const char* symbolName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    const mach_header* foundInImageAtLoadAddress;
    void*              symbolAddress;
    if ( flatFindSymbol(symbolName, &symbolAddress, &foundInImageAtLoadAddress) ) {
        return (NSSymbol)symbolAddress;
    }
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSSymbol APIs::NSLookupAndBindSymbolWithHint(const char* symbolName, const char* libraryNameHint)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    const mach_header* foundInImageAtLoadAddress;
    void*              symbolAddress;
    if ( flatFindSymbol(symbolName, &symbolAddress, &foundInImageAtLoadAddress) ) {
        return (NSSymbol)symbolAddress;
    }
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSSymbol APIs::NSLookupSymbolInModule(NSModule module, const char* symbolName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSLookupSymbolInModule(%p, %s)\n", module, symbolName);
    bool firstOnly;
    if ( const Loader* ldr = loaderFromHandle(module, firstOnly) ) {
        if ( validLoader(ldr) ) {
            const MachOLoaded* ml = ldr->loadAddress(*this);
            void*              addr;
            bool               resultPointsToInstructions = false;
            if ( ml->hasExportedSymbol(symbolName, nullptr, &addr, &resultPointsToInstructions) ) {
                if ( config.log.apis )
                    log("NSLookupSymbolInModule(%p, %s) => %p\n", module, symbolName, addr);
               return (NSSymbol)addr;
            }
        }
        else {
            // for bincompat some apps pass in mach_header as 'module'
            for ( const Loader* aLdr : this->loaded ) {
                const MachOLoaded* ml = aLdr->loadAddress(*this);
                if ( ml == (MachOLoaded*)module) {
                    void* addr;
                    bool  resultPointsToInstructions;
                    if ( ml->hasExportedSymbol(symbolName, nullptr, &addr, &resultPointsToInstructions) ) {
                        if ( config.log.apis )
                            log("NSLookupSymbolInModule(%p, %s) => %p\n", module, symbolName, addr);
                        return (NSSymbol)addr;
                    }
                    break;
                }
            }
        }
    }
    if ( config.log.apis )
        log("NSLookupSymbolInModule(%p, %s) => NULL\n", module, symbolName);
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSSymbol APIs::NSLookupSymbolInImage(const mach_header* mh, const char* symbolName, uint32_t options)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    void* addr;
    bool  resultPointsToInstructions = false;
    if ( ((MachOLoaded*)mh)->hasExportedSymbol(symbolName, nullptr, &addr, &resultPointsToInstructions) ) {
        return (NSSymbol)addr;
    }
    if ( options & NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR ) {
        return nullptr;
    }
    // FIXME: abort();
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void* APIs::NSAddressOfSymbol(NSSymbol symbol)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // special case NULL
    if ( symbol == nullptr )
        return nullptr;

    // in dyld 1.0, NSSymbol was a pointer to the nlist entry in the symbol table
    void* result = (void*)symbol;

    #if __has_feature(ptrauth_calls)
    const MachOLoaded* ml;
    if ( findImageMappedAt(result, &ml) ) {
        int64_t      slide                      = ml->getSlide();
        __block bool resultPointsToInstructions = false;
        ml->forEachSection(^(const MachOAnalyzer::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
            uint64_t sectStartAddr = sectInfo.sectAddr + slide;
            uint64_t sectEndAddr   = sectStartAddr + sectInfo.sectSize;
            if ( ((uint64_t)result >= sectStartAddr) && ((uint64_t)result < sectEndAddr) ) {
                resultPointsToInstructions = (sectInfo.sectFlags & S_ATTR_PURE_INSTRUCTIONS) || (sectInfo.sectFlags & S_ATTR_SOME_INSTRUCTIONS);
                stop                       = true;
            }
        });

        if ( resultPointsToInstructions )
            result = __builtin_ptrauth_sign_unauthenticated(result, ptrauth_key_asia, 0);
    }
    #endif
    return result;

#else
    obsolete();
#endif // TARGET_OS_OSX
}

NSModule APIs::NSModuleForSymbol(NSSymbol symbol)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    __block void* handle = nullptr;
    locks.withLoadersReadLock(^{
        for ( const Loader* ldr : loaded ) {
            const void* sgAddr;
            uint64_t    sgSize;
            uint8_t     sgPerm;
            if ( ldr->contains(*this, symbol, &sgAddr, &sgSize, &sgPerm) ) {
                handle = handleFromLoader(ldr, false);
                break;
            }
        }
    });

    return (NSModule)handle;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void APIs::NSLinkEditError(NSLinkEditErrors* c, int* errorNumber, const char** fileName, const char** errorString)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    *c           = NSLinkEditOtherError;
    *errorNumber = 0;
    *fileName    = NULL;
    *errorString = NULL;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::NSAddLibrary(const char* pathName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSAddLibrary(%s)\n", pathName);
    void* callerAddress = __builtin_return_address(0);
    return (dlopen_from(pathName, 0, callerAddress) != nullptr);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::NSAddLibraryWithSearching(const char* pathName)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSAddLibraryWithSearching(%s)\n", pathName);
    void* callerAddress = __builtin_return_address(0);
    return (dlopen_from(pathName, 0, callerAddress) != nullptr);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

const mach_header* APIs::NSAddImage(const char* imageName, uint32_t options)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("NSAddImage(%s)\n", imageName);
    // Note: this is a quick and dirty implementation that just uses dlopen() and ignores some option flags
    uint32_t dloptions = 0;
    if ( (options & NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED) != 0 )
        dloptions |= RTLD_NOLOAD;

    void* callerAddress = __builtin_return_address(0);
    void* h = dlopen_from(imageName, dloptions, callerAddress);
    if ( h != nullptr ) {
        bool               firstOnly;
        const Loader*      ldr = loaderFromHandle(h, firstOnly);
        const MachOLoaded* mh  = ldr->loadAddress(*this);
        return mh;
    }

    if ( (options & (NSADDIMAGE_OPTION_RETURN_ON_ERROR | NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED)) == 0 ) {
        abort_report_np("NSAddImage() image not found");
    }
    return nullptr;
#else
    obsolete();
#endif // TARGET_OS_OSX
}

bool APIs::_dyld_image_containing_address(const void* address)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_image_containing_address(%p)\n", address);
    return (dyld_image_header_containing_address(address) != nullptr);
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void APIs::_dyld_lookup_and_bind(const char* symbolName, void** address, NSModule* module)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_lookup_and_bind(%s)\n", symbolName);
    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        if ( module ) {
            *module = (NSModule)foundInImageAtLoadAddress;
        }
        if ( config.log.apis )
            log("  _dyld_lookup_and_bind(%s) => %p\n", symbolName, *address);
        return;
    }

    if ( config.log.apis )
        log("  _dyld_lookup_and_bind(%s) => NULL\n", symbolName);
    if ( address ) {
        *address = 0;
    }
    if ( module ) {
        *module  = 0;
    }
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void APIs::_dyld_lookup_and_bind_with_hint(const char* symbolName, const char* libraryNameHint, void** address, NSModule* module)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_lookup_and_bind_with_hint(%s)\n", symbolName);
    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        if ( module ) {
            *module = (NSModule)foundInImageAtLoadAddress;
        }
        return;
    }

    if ( address ) {
        *address = 0;
    }
    if ( module ) {
        *module  = 0;
    }
#else
    obsolete();
#endif // TARGET_OS_OSX
}

void APIs::_dyld_lookup_and_bind_fully(const char* symbolName, void** address, NSModule* module)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( config.log.apis )
        log("_dyld_lookup_and_bind_fully(%s, %p, %p)\n", symbolName, address, module);
    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        if ( module ) {
            *module = (NSModule)foundInImageAtLoadAddress;
        }
        return;
    }

    if ( address ) {
        *address = 0;
    }
    if ( module ) {
        *module  = 0;
    }
#else
    obsolete();
#endif // TARGET_OS_OSX
}

// This is factored out of dyldMain.cpp to support old macOS apps that use crt1.o 
void APIs::runAllInitializersForMain()
{
#if !TARGET_OS_EXCLAVEKIT
    // disable page-in linking, not used for dlopen() loaded images
    if ( !config.security.internalInstall || (config.process.pageInLinkingMode != 3) )
        config.syscall.disablePageInLinking();
#endif
    
    // run libSystem's initializer first
    if (!libSystemInitialized()) {
        const_cast<Loader*>(this->libSystemLoader)->beginInitializers(*this);
        this->libSystemLoader->runInitializers(*this);
    }

#if HAS_EXTERNAL_STATE
#if TARGET_OS_EXCLAVEKIT
    this->externallyViewable.setLibSystemInitializedOld();
#else
    this->externallyViewable.setLibSystemInitialized();
#endif // TARGET_OS_EXCLAVEKIT
#endif /* HAS_EXTERNAL_STATE */

    // after running libSystem's initializer, tell objc to run any +load methods on libSystem sub-dylibs
    this->notifyObjCInit(this->libSystemLoader);
    // <rdar://problem/32209809> call 'init' function on all images already init'ed (below libSystem)
    // Iterate using indices so that the array doesn't grow underneath us if a +load dloopen's
    for ( uint32_t i = 0; i != this->loaded.size(); ++i ) {
        const Loader* ldr = this->loaded[i];
        if ( (ldr->dylibInDyldCache || ldr->analyzer(*this)->isDylib()) && (strncmp(ldr->analyzer(*this)->installName(), "/usr/lib/system/lib", 19) == 0) ) {
            // check install name instead of path, to handle DYLD_LIBRARY_PATH overrides of libsystem sub-dylibs
            const_cast<Loader*>(ldr)->beginInitializers(*this);
            this->notifyObjCInit(ldr);
            const_cast<Loader*>(ldr)->runInitializers(*this);
        }
    }

#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // If we're PID 1, scan for roots
    if ( (this->config.process.pid == 1) && (this->libSystemHelpers->version() >= 5) ) {
        this->libSystemHelpers->run_async(&ProcessConfig::scanForRoots, (void*)&this->config);
    }
#endif // TARGET_OS_OSX

    // run all other initializers bottom-up, running inserted dylib initializers first
    // Iterate using indices so that the array doesn't grow underneath us if an initializer dloopen's
    for ( uint32_t i = 0; i != this->loaded.size(); ++i ) {
        const Loader* ldr = this->loaded[i];
        ldr->runInitializersBottomUpPlusUpwardLinks(*this);
        // stop as soon as we did main executable
        // normally this is first image, but if there are inserted N dylibs, it is Nth in the list
        if ( ldr->analyzer(*this)->isMainExecutable() )
            break;
    }
}


} // namespace
