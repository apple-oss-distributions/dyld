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


#include <string.h>
#include <stdint.h>
#include <_simple.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>

#include <algorithm>

#include "dlfcn.h"
#include "dyld_priv.h"

#include "AllImages.h"
#include "MachOParser.h"
#include "Loading.h"
#include "Logging.h"
#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "PathOverrides.h"
#include "APIs.h"
#include "StringUtils.h"



extern "C" {
    #include "closuredProtocol.h"
}


namespace dyld {
    extern dyld_all_image_infos dyld_all_image_infos;
}


namespace dyld3 {


uint32_t _dyld_image_count(void)
{
    log_apis("_dyld_image_count()\n");

    return gAllImages.count();
}

const mach_header* _dyld_get_image_header(uint32_t imageIndex)
{
    log_apis("_dyld_get_image_header(%d)\n", imageIndex);

    const mach_header* loadAddress;
    launch_cache::Image image = gAllImages.findByLoadOrder(imageIndex, &loadAddress);
    if ( image.valid() )
        return loadAddress;
    return nullptr;
}

intptr_t _dyld_get_image_slide(const mach_header* mh)
{
    log_apis("_dyld_get_image_slide(%p)\n", mh);

    MachOParser parser(mh);
    return parser.getSlide();
}

intptr_t _dyld_get_image_vmaddr_slide(uint32_t imageIndex)
{
    log_apis("_dyld_get_image_vmaddr_slide(%d)\n", imageIndex);

    const mach_header* mh = _dyld_get_image_header(imageIndex);
    if ( mh != nullptr )
        return dyld3::_dyld_get_image_slide(mh);
    return 0;
}

const char* _dyld_get_image_name(uint32_t imageIndex)
{
    log_apis("_dyld_get_image_name(%d)\n", imageIndex);

    const mach_header* loadAddress;
    launch_cache::Image image = gAllImages.findByLoadOrder(imageIndex, &loadAddress);
    if ( image.valid() )
        return gAllImages.imagePath(image.binaryData());
    return nullptr;
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
    size_t leafNameLen = strlen(leafName);
    size_t libraryNameLen = strlen(libraryName);
    if ( leafNameLen < (libraryNameLen+9) )
        return false;
    if ( strncmp(leafName, "lib", 3) != 0 )
        return false;
    if ( strcmp(&leafName[leafNameLen-6], ".dylib") != 0 )
        return false;
    if ( strncmp(&leafName[3], libraryName, libraryNameLen) != 0 )
        return false;
    return (leafName[libraryNameLen+3] == '.');
}


//
// BETTER, USE: dyld_get_program_sdk_version()
//
// Scans the main executable and returns the version of the specified dylib the program was built against.
//
// The library to find is the leaf name that would have been passed to linker tool
// (e.g. -lfoo or -framework foo would use "foo").
//
// Returns -1 if the main executable did not link against the specified library, or is malformed.
//
int32_t NSVersionOfLinkTimeLibrary(const char* libraryName)
{
    log_apis("NSVersionOfLinkTimeLibrary(\"%s\")\n", libraryName);

    __block int32_t result = -1;
    MachOParser parser(gAllImages.mainExecutable());
    parser.forEachDependentDylib(^(const char* loadPath, bool, bool, bool, uint32_t compatVersion, uint32_t currentVersion, bool& stop) {
        if ( nameMatch(loadPath, libraryName) )
            result = currentVersion;
    });
    log_apis("   NSVersionOfLinkTimeLibrary() => 0x%08X\n", result);
    return result;
}


//
// Searches loaded images for the requested dylib and returns its current version.
//
// The library to find is the leaf name that would have been passed to linker tool
// (e.g. -lfoo or -framework foo would use "foo").
//
// If the specified library is not loaded, -1 is returned.
//
int32_t NSVersionOfRunTimeLibrary(const char* libraryName)
{
    log_apis("NSVersionOfRunTimeLibrary(\"%s\")\n", libraryName);

    uint32_t count = gAllImages.count();
    for (uint32_t i=0; i < count; ++i) {
        const mach_header* loadAddress;
        launch_cache::Image image = gAllImages.findByLoadOrder(i, &loadAddress);
        if ( image.valid() ) {
            MachOParser parser(loadAddress);
            const char* installName;
            uint32_t currentVersion;
            uint32_t compatVersion;
            if ( parser.getDylibInstallName(&installName, &compatVersion, &currentVersion) && nameMatch(installName, libraryName) ) {
                log_apis("   NSVersionOfRunTimeLibrary() => 0x%08X\n", currentVersion);
                return currentVersion;
            }
        }
    }
    log_apis("   NSVersionOfRunTimeLibrary() => -1\n");
    return -1;
}


#if __WATCH_OS_VERSION_MIN_REQUIRED

static uint32_t watchVersToIOSVers(uint32_t vers)
{
    return vers + 0x00070000;
}

uint32_t dyld_get_program_sdk_watch_os_version()
{
    log_apis("dyld_get_program_sdk_watch_os_version()\n");

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    MachOParser parser(gAllImages.mainExecutable());
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        if ( platform == Platform::watchOS )
            return sdk;
    }
    return 0;
}

uint32_t dyld_get_program_min_watch_os_version()
{
    log_apis("dyld_get_program_min_watch_os_version()\n");

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    MachOParser parser(gAllImages.mainExecutable());
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        if ( platform == Platform::watchOS )
            return minOS;  // return raw minOS (not mapped to iOS version)
    }
    return 0;
}
#endif


#if TARGET_OS_BRIDGE

static uint32_t bridgeVersToIOSVers(uint32_t vers)
{
    return vers + 0x00090000;
}

uint32_t dyld_get_program_sdk_bridge_os_version()
{
    log_apis("dyld_get_program_sdk_bridge_os_version()\n");

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    MachOParser parser(gAllImages.mainExecutable());
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        if ( platform == Platform::bridgeOS )
            return sdk;
    }
    return 0;
}

uint32_t dyld_get_program_min_bridge_os_version()
{
    log_apis("dyld_get_program_min_bridge_os_version()\n");

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    MachOParser parser(gAllImages.mainExecutable());
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        if ( platform == Platform::bridgeOS )
            return minOS;  // return raw minOS (not mapped to iOS version)
    }
    return 0;
}

#endif


#if !__WATCH_OS_VERSION_MIN_REQUIRED && !__TV_OS_VERSION_MIN_REQUIRED && !TARGET_OS_BRIDGE

#define PACKED_VERSION(major, minor, tiny) ((((major) & 0xffff) << 16) | (((minor) & 0xff) << 8) | ((tiny) & 0xff))

static uint32_t deriveSDKVersFromDylibs(const mach_header* mh)
{
    __block uint32_t foundationVers = 0;
    __block uint32_t libSystemVers = 0;
    MachOParser parser(mh);
    parser.forEachDependentDylib(^(const char* loadPath, bool, bool, bool, uint32_t compatVersion, uint32_t currentVersion, bool& stop) {
        if ( strcmp(loadPath, "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation") == 0 )
            foundationVers = currentVersion;
        else if ( strcmp(loadPath, "/usr/lib/libSystem.B.dylib") == 0 )
            libSystemVers = currentVersion;
    });

    struct DylibToOSMapping {
        uint32_t dylibVersion;
        uint32_t osVersion;
    };
    
  #if __IPHONE_OS_VERSION_MIN_REQUIRED
    static const DylibToOSMapping foundationMapping[] = {
        { PACKED_VERSION(678,24,0), 0x00020000 },
        { PACKED_VERSION(678,26,0), 0x00020100 },
        { PACKED_VERSION(678,29,0), 0x00020200 },
        { PACKED_VERSION(678,47,0), 0x00030000 },
        { PACKED_VERSION(678,51,0), 0x00030100 },
        { PACKED_VERSION(678,60,0), 0x00030200 },
        { PACKED_VERSION(751,32,0), 0x00040000 },
        { PACKED_VERSION(751,37,0), 0x00040100 },
        { PACKED_VERSION(751,49,0), 0x00040200 },
        { PACKED_VERSION(751,58,0), 0x00040300 },
        { PACKED_VERSION(881,0,0),  0x00050000 },
        { PACKED_VERSION(890,1,0),  0x00050100 },
        { PACKED_VERSION(992,0,0),  0x00060000 },
        { PACKED_VERSION(993,0,0),  0x00060100 },
        { PACKED_VERSION(1038,14,0),0x00070000 },
        { PACKED_VERSION(0,0,0),    0x00070000 }
        // We don't need to expand this table because all recent
        // binaries have LC_VERSION_MIN_ load command.
    };

    if ( foundationVers != 0 ) {
        uint32_t lastOsVersion = 0;
        for (const DylibToOSMapping* p=foundationMapping; ; ++p) {
            if ( p->dylibVersion == 0 )
                return p->osVersion;
            if ( foundationVers < p->dylibVersion )
                return lastOsVersion;
            lastOsVersion = p->osVersion;
        }
    }

  #else
    // Note: versions are for the GM release.  The last entry should
    // always be zero.  At the start of the next major version,
    // a new last entry needs to be added and the previous zero
    // updated to the GM dylib version.
    static const DylibToOSMapping libSystemMapping[] = {
        { PACKED_VERSION(88,1,3),   0x000A0400 },
        { PACKED_VERSION(111,0,0),  0x000A0500 },
        { PACKED_VERSION(123,0,0),  0x000A0600 },
        { PACKED_VERSION(159,0,0),  0x000A0700 },
        { PACKED_VERSION(169,3,0),  0x000A0800 },
        { PACKED_VERSION(1197,0,0), 0x000A0900 },
        { PACKED_VERSION(0,0,0),    0x000A0900 }
        // We don't need to expand this table because all recent
        // binaries have LC_VERSION_MIN_ load command.
    };

    if ( libSystemVers != 0 ) {
        uint32_t lastOsVersion = 0;
        for (const DylibToOSMapping* p=libSystemMapping; ; ++p) {
            if ( p->dylibVersion == 0 )
                return p->osVersion;
            if ( libSystemVers < p->dylibVersion )
                return lastOsVersion;
            lastOsVersion = p->osVersion;
        }
    }
  #endif
  return 0;
}
#endif


//
// Returns the sdk version (encode as nibble XXXX.YY.ZZ) that the
// specified binary was built against.
//
// First looks for LC_VERSION_MIN_* in binary and if sdk field is
// not zero, return that value.
// Otherwise, looks for the libSystem.B.dylib the binary linked
// against and uses a table to convert that to an sdk version.
//
uint32_t dyld_get_sdk_version(const mach_header* mh)
{
    log_apis("dyld_get_sdk_version(%p)\n", mh);

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    if ( !MachOParser::wellFormedMachHeaderAndLoadCommands(mh) )
        return 0;
    MachOParser parser(mh);
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        switch (platform) {
#if TARGET_OS_BRIDGE
            case Platform::bridgeOS:
                // new binary. sdk version looks like "2.0" but API wants "11.0"
                return bridgeVersToIOSVers(sdk);
            case Platform::iOS:
                // old binary. sdk matches API semantics so can return directly.
                return sdk;
#elif __WATCH_OS_VERSION_MIN_REQUIRED
            case Platform::watchOS:
                // new binary. sdk version looks like "2.0" but API wants "9.0"
                return watchVersToIOSVers(sdk);
            case Platform::iOS:
                // old binary. sdk matches API semantics so can return directly.
                return sdk;
#elif __TV_OS_VERSION_MIN_REQUIRED
            case Platform::tvOS:
            case Platform::iOS:
                return sdk;
#elif __IPHONE_OS_VERSION_MIN_REQUIRED
            case Platform::iOS:
                if ( sdk != 0 )    // old binaries might not have SDK set
                    return sdk;
                break;
#else
            case Platform::macOS:
                if ( sdk != 0 )    // old binaries might not have SDK set
                    return sdk;
                break;
#endif
            default:
                // wrong binary for this platform
                break;
        }
    }

#if __WATCH_OS_VERSION_MIN_REQUIRED ||__TV_OS_VERSION_MIN_REQUIRED || TARGET_OS_BRIDGE
    // All watchOS and tvOS binaries should have version load command.
    return 0;
#else
    // MacOSX and iOS have old binaries without version load commmand.
    return deriveSDKVersFromDylibs(mh);
#endif
}

uint32_t dyld_get_program_sdk_version()
{
     log_apis("dyld_get_program_sdk_version()\n");

    return dyld3::dyld_get_sdk_version(gAllImages.mainExecutable());
}

uint32_t dyld_get_min_os_version(const mach_header* mh)
{
    log_apis("dyld_get_min_os_version(%p)\n", mh);

    Platform platform;
    uint32_t minOS;
    uint32_t sdk;

    if ( !MachOParser::wellFormedMachHeaderAndLoadCommands(mh) )
        return 0;
    MachOParser parser(mh);
    if ( parser.getPlatformAndVersion(&platform, &minOS, &sdk) ) {
        switch (platform) {
#if TARGET_OS_BRIDGE
            case Platform::bridgeOS:
                // new binary. sdk version looks like "2.0" but API wants "11.0"
                return bridgeVersToIOSVers(minOS);
            case Platform::iOS:
                // old binary. sdk matches API semantics so can return directly.
                return minOS;
#elif __WATCH_OS_VERSION_MIN_REQUIRED
            case Platform::watchOS:
                // new binary. OS version looks like "2.0" but API wants "9.0"
                return watchVersToIOSVers(minOS);
            case Platform::iOS:
                // old binary. OS matches API semantics so can return directly.
                return minOS;
#elif __TV_OS_VERSION_MIN_REQUIRED
            case Platform::tvOS:
            case Platform::iOS:
                return minOS;
#elif __IPHONE_OS_VERSION_MIN_REQUIRED
            case Platform::iOS:
                return minOS;
#else
            case Platform::macOS:
                return minOS;
#endif
            default:
                // wrong binary for this platform
                break;
        }
    }
    return 0;
}


uint32_t dyld_get_program_min_os_version()
{
     log_apis("dyld_get_program_min_os_version()\n");

    return dyld3::dyld_get_min_os_version(gAllImages.mainExecutable());
}


bool _dyld_get_image_uuid(const mach_header* mh, uuid_t uuid)
{
     log_apis("_dyld_get_image_uuid(%p, %p)\n", mh, uuid);

    if ( !MachOParser::wellFormedMachHeaderAndLoadCommands(mh) )
        return false;
    MachOParser parser(mh);
    return parser.getUuid(uuid);
}

//
// _NSGetExecutablePath() copies the path of the main executable into the buffer. The bufsize parameter
// should initially be the size of the buffer.  The function returns 0 if the path was successfully copied,
// and *bufsize is left unchanged. It returns -1 if the buffer is not large enough, and *bufsize is set
// to the size required.
//
int _NSGetExecutablePath(char* buf, uint32_t* bufsize)
{
     log_apis("_NSGetExecutablePath(%p, %p)\n", buf, bufsize);

   launch_cache::Image image = gAllImages.mainExecutableImage();
    if ( image.valid() ) {
        const char* path = gAllImages.imagePath(image.binaryData());
        size_t pathSize = strlen(path) + 1;
        if ( *bufsize >= pathSize ) {
            strcpy(buf, path);
            return 0;
        }
        *bufsize = (uint32_t)pathSize;
    }
    return -1;
}

void _dyld_register_func_for_add_image(void (*func)(const mach_header *mh, intptr_t vmaddr_slide))
{
    log_apis("_dyld_register_func_for_add_image(%p)\n", func);

    gAllImages.addLoadNotifier(func);
}

void _dyld_register_func_for_remove_image(void (*func)(const mach_header *mh, intptr_t vmaddr_slide))
{
    log_apis("_dyld_register_func_for_remove_image(%p)\n", func);

    gAllImages.addUnloadNotifier(func);
}

void _dyld_objc_notify_register(_dyld_objc_notify_mapped    mapped,
                                _dyld_objc_notify_init      init,
                                _dyld_objc_notify_unmapped  unmapped)
{
    log_apis("_dyld_objc_notify_register(%p, %p, %p)\n", mapped, init, unmapped);

    gAllImages.setObjCNotifiers(mapped, init, unmapped);
}


const mach_header* dyld_image_header_containing_address(const void* addr)
{
    log_apis("dyld_image_header_containing_address(%p)\n", addr);

    const mach_header* loadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(addr, &loadAddress);
    if ( image.valid() )
        return loadAddress;
    return nullptr;
}


const char* dyld_image_path_containing_address(const void* addr)
{
    log_apis("dyld_image_path_containing_address(%p)\n", addr);

    const mach_header* loadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(addr, &loadAddress);
    if ( image.valid() ) {
        const char* path = gAllImages.imagePath(image.binaryData());
        log_apis("   dyld_image_path_containing_address() => %s\n", path);
        return path;
    }
    log_apis("   dyld_image_path_containing_address() => NULL\n");
    return nullptr;
}

bool _dyld_is_memory_immutable(const void* addr, size_t length)
{
    uintptr_t checkStart = (uintptr_t)addr;
    uintptr_t checkEnd   = checkStart + length;

    // quick check to see if in r/o region of shared cache.  If so return true.
    const DyldSharedCache* cache = (DyldSharedCache*)gAllImages.cacheLoadAddress();
    if ( cache != nullptr ) {
        __block bool firstVMAddr = 0;
        __block bool isReadOnlyInCache = false;
        __block bool isInCache = false;
        cache->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
            if ( firstVMAddr == 0 )
                firstVMAddr = vmAddr;
            uintptr_t regionStart = (uintptr_t)cache + (uintptr_t)(vmAddr - firstVMAddr);
            uintptr_t regionEnd   = regionStart + (uintptr_t)size;
            if ( (regionStart < checkStart) && (checkEnd < regionEnd) ) {
                isInCache = true;
                isReadOnlyInCache = ((permissions & VM_PROT_WRITE) != 0);
            }
        });
        if ( isInCache )
            return isReadOnlyInCache;
    }

    // go slow route of looking at each image's segments
    const mach_header* loadAddress;
    uint8_t permissions;
    launch_cache::Image image = gAllImages.findByOwnedAddress(addr, &loadAddress, &permissions);
    if ( !image.valid() )
        return false;
    if ( (permissions & VM_PROT_WRITE) != 0 )
        return false;
    return !gAllImages.imageUnloadable(image, loadAddress);
}


int dladdr(const void* addr, Dl_info* info)
{
    log_apis("dladdr(%p, %p)\n", addr, info);

    const mach_header* loadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(addr, &loadAddress);
    if ( !image.valid() ) {
        log_apis("   dladdr() => 0\n");
        return 0;
    }
    MachOParser parser(loadAddress);
    info->dli_fname = gAllImages.imagePath(image.binaryData());
    info->dli_fbase = (void*)(loadAddress);
    if ( addr == info->dli_fbase ) {
        // special case lookup of header
        info->dli_sname = "__dso_handle";
        info->dli_saddr = info->dli_fbase;
    }
    else if ( parser.findClosestSymbol(addr, &(info->dli_sname), (const void**)&(info->dli_saddr)) ) {
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
    }
    log_apis("   dladdr() => 1, { \"%s\", %p, \"%s\", %p }\n", info->dli_fname, info->dli_fbase, info->dli_sname, info->dli_saddr);
    return 1;
}


struct PerThreadErrorMessage
{
    size_t      sizeAllocated;
    bool        valid;
    char        message[1];
};

static pthread_key_t dlerror_perThreadKey()
{
    static dispatch_once_t  onceToken;
    static pthread_key_t    dlerrorPThreadKey;
    dispatch_once(&onceToken, ^{
        pthread_key_create(&dlerrorPThreadKey, &free);
    });
    return dlerrorPThreadKey;
}

static void clearErrorString()
{
    PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)pthread_getspecific(dlerror_perThreadKey());
    if ( errorBuffer != nullptr )
        errorBuffer->valid = false;
}

__attribute__((format(printf, 1, 2)))
static void setErrorString(const char* format, ...)
{
    _SIMPLE_STRING buf = _simple_salloc();
    if ( buf != nullptr ) {
        va_list    list;
        va_start(list, format);
        _simple_vsprintf(buf, format, list);
        va_end(list);
        size_t strLen = strlen(_simple_string(buf)) + 1;
        size_t sizeNeeded = sizeof(PerThreadErrorMessage) + strLen;
        PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)pthread_getspecific(dlerror_perThreadKey());
        if ( errorBuffer != nullptr ) {
            if ( errorBuffer->sizeAllocated < sizeNeeded ) {
                free(errorBuffer);
                errorBuffer = nullptr;
            }
        }
        if ( errorBuffer == nullptr ) {
            size_t allocSize = std::max(sizeNeeded, (size_t)256);
            PerThreadErrorMessage* p = (PerThreadErrorMessage*)malloc(allocSize);
            p->sizeAllocated = allocSize;
            p->valid = false;
            pthread_setspecific(dlerror_perThreadKey(), p);
            errorBuffer = p;
        }
        strcpy(errorBuffer->message, _simple_string(buf));
        errorBuffer->valid = true;
        _simple_sfree(buf);
    }
}

char* dlerror()
{
    log_apis("dlerror()\n");

    PerThreadErrorMessage* errorBuffer = (PerThreadErrorMessage*)pthread_getspecific(dlerror_perThreadKey());
    if ( errorBuffer != nullptr ) {
        if ( errorBuffer->valid ) {
            // you can only call dlerror() once, then the message is cleared
            errorBuffer->valid = false;
            return errorBuffer->message;
        }
    }
    return nullptr;
}

#if __arm64__
    #define CURRENT_CPU_TYPE CPU_TYPE_ARM64
#elif __arm__
    #define CURRENT_CPU_TYPE CPU_TYPE_ARM
#endif


class VIS_HIDDEN RecursiveAutoLock
{
public:
    RecursiveAutoLock() {
        pthread_mutex_lock(&_sMutex);
    }
    ~RecursiveAutoLock() {
        pthread_mutex_unlock(&_sMutex);
    }
private:
    static pthread_mutex_t _sMutex;
};

pthread_mutex_t RecursiveAutoLock::_sMutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static void* makeDlHandle(const mach_header* mh, bool dontContinue)
{
    uintptr_t flags = (dontContinue ? 1 : 0);
    return (void*)((((uintptr_t)mh) >> 5) | flags);
}

VIS_HIDDEN
void parseDlHandle(void* h, const mach_header** mh, bool* dontContinue)
{
    *dontContinue = (((uintptr_t)h) & 1);
    *mh           = (const mach_header*)((((uintptr_t)h) & (-2)) << 5);
}

int dlclose(void* handle)
{
    log_apis("dlclose(%p)\n", handle);

    // silently accept magic handles for main executable
    if ( handle == RTLD_MAIN_ONLY )
        return 0;
    if ( handle == RTLD_DEFAULT )
        return 0;
    
   // from here on, serialize all dlopen()s
    RecursiveAutoLock dlopenSerializer;

    const mach_header*  mh;
    bool                dontContinue;
    parseDlHandle(handle, &mh, &dontContinue);
    launch_cache::Image image = gAllImages.findByLoadAddress(mh);
    if ( image.valid() ) {
        // removes image if reference count went to zero
        if ( !image.neverUnload() )
            gAllImages.decRefCount(mh);
        clearErrorString();
        return 0;
    }
    else {
        setErrorString("invalid handle passed to dlclose()");
        return -1;
    }
}



VIS_HIDDEN
const mach_header* loadImageAndDependents(Diagnostics& diag, const launch_cache::binary_format::Image* imageToLoad, bool bumpDlopenCount)
{
    launch_cache::Image topImage(imageToLoad);
    uint32_t maxLoad = topImage.maxLoadCount();
    // first construct array of all BinImage* objects that dlopen'ed image depends on
    const dyld3::launch_cache::binary_format::Image*    fullImageList[maxLoad];
    dyld3::launch_cache::SlowLoadSet imageSet(&fullImageList[0], &fullImageList[maxLoad]);
    imageSet.add(imageToLoad);
    STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, gAllImages.currentGroupsCount(), currentGroupsList);
    gAllImages.copyCurrentGroups(currentGroupsList);
    if ( !topImage.recurseAllDependentImages(currentGroupsList, imageSet, nullptr) ) {
        diag.error("unexpected > %d images loaded", maxLoad);
        return nullptr;
    }

    // build array of BinImage* that are not already loaded
    const dyld3::launch_cache::binary_format::Image*    toLoadImageList[maxLoad];
    const dyld3::launch_cache::binary_format::Image**   toLoadImageArray = toLoadImageList;
    __block int needToLoadCount = 0;
    imageSet.forEach(^(const dyld3::launch_cache::binary_format::Image* aBinImage) {
        if ( gAllImages.findLoadAddressByImage(aBinImage) == nullptr )
            toLoadImageArray[needToLoadCount++] = aBinImage;
    });
    assert(needToLoadCount > 0);

    // build one array of all existing and to-be-loaded images
    uint32_t alreadyLoadImageCount = gAllImages.count();
    STACK_ALLOC_DYNARRAY(loader::ImageInfo, alreadyLoadImageCount + needToLoadCount, allImages);
    loader::ImageInfo* allImagesArray = &allImages[0];
    gAllImages.forEachImage(^(uint32_t imageIndex, const mach_header* loadAddress, const launch_cache::Image image, bool& stop) {
        launch_cache::ImageGroup grp = image.group();
        loader::ImageInfo&       info= allImagesArray[imageIndex];
        info.imageData               = image.binaryData();
        info.loadAddress             = loadAddress;
        info.groupNum                = grp.groupNum();
        info.indexInGroup            = grp.indexInGroup(info.imageData);
        info.previouslyFixedUp       = true;
        info.justMapped              = false;
        info.justUsedFromDyldCache   = false;
        info.neverUnload             = false;
    });
    for (int i=0; i < needToLoadCount; ++i) {
        launch_cache::Image      img(toLoadImageArray[i]);
        launch_cache::ImageGroup grp = img.group();
        loader::ImageInfo&       info= allImages[alreadyLoadImageCount+i];
        info.imageData               = toLoadImageArray[i];
        info.loadAddress             = nullptr;
        info.groupNum                = grp.groupNum();
        info.indexInGroup            = grp.indexInGroup(img.binaryData());
        info.previouslyFixedUp       = false;
        info.justMapped              = false;
        info.justUsedFromDyldCache   = false;
        info.neverUnload             = false;
    }

    // map new images and apply all fixups
    mapAndFixupImages(diag, allImages, (const uint8_t*)gAllImages.cacheLoadAddress(), &dyld3::log_loads, &dyld3::log_segments, &dyld3::log_fixups, &dyld3::log_dofs);
    if ( diag.hasError() )
         return nullptr;
    const mach_header* topLoadAddress = allImages[alreadyLoadImageCount].loadAddress;

    // bump dlopen refcount of image directly loaded
    if ( bumpDlopenCount )
        gAllImages.incRefCount(topLoadAddress);

    // tell gAllImages about new images
    dyld3::launch_cache::DynArray<loader::ImageInfo> newImages(needToLoadCount, &allImages[alreadyLoadImageCount]);
    gAllImages.addImages(newImages);

    // tell gAllImages about any old images which now have never unload set
    for (int i=0; i < alreadyLoadImageCount; ++i) {
        if (allImages[i].neverUnload && !allImages[i].imageData->neverUnload)
            gAllImages.setNeverUnload(allImages[i]);
    }

    // run initializers
    gAllImages.runInitialzersBottomUp(topLoadAddress);

    return topLoadAddress;
}


void* dlopen(const char* path, int mode)
{    
    log_apis("dlopen(\"%s\", 0x%08X)\n", ((path==NULL) ? "NULL" : path), mode);

    clearErrorString();

    // passing NULL for path means return magic object
    if ( path == NULL ) {
        // RTLD_FIRST means any dlsym() calls on the handle should only search that handle and not subsequent images
        if ( (mode & RTLD_FIRST) != 0 )
            return RTLD_MAIN_ONLY;
        else
            return RTLD_DEFAULT;
    }

    // from here on, serialize all dlopen()s
    RecursiveAutoLock dlopenSerializer;

    const char* leafName = strrchr(path, '/');
    if ( leafName != nullptr )
        ++leafName;
    else
        leafName = path;

    // RTLD_FIRST means when dlsym() is called with handle, only search the image and not those loaded after it
    bool dontContinue = (mode & RTLD_FIRST);
    bool bumpRefCount = true;

    // check if dylib with same inode/mtime is already loaded
    __block const mach_header* alreadyLoadMH = nullptr;
    struct stat statBuf;
    if ( stat(path, &statBuf) == 0 ) {
        alreadyLoadMH = gAllImages.alreadyLoaded(statBuf.st_ino, statBuf.st_mtime, bumpRefCount);
        if ( alreadyLoadMH != nullptr) {
            log_apis("   dlopen: path inode/mtime matches already loaded image\n");
            void* result = makeDlHandle(alreadyLoadMH, dontContinue);
            log_apis("   dlopen(%s) => %p\n", leafName, result);
            return result;
        }
    }

    // check if already loaded, and if so, just bump ref-count
    gPathOverrides.forEachPathVariant(path, ^(const char* possiblePath, bool& stop) {
        alreadyLoadMH = gAllImages.alreadyLoaded(possiblePath, bumpRefCount);
        if ( alreadyLoadMH != nullptr ) {
            log_apis("   dlopen: matches already loaded image %s\n", possiblePath);
            stop = true;
        }
    });
    if ( alreadyLoadMH != nullptr) {
        void* result = makeDlHandle(alreadyLoadMH, dontContinue);
        log_apis("   dlopen(%s) => %p\n", leafName, result);
        return result;
    }

    // it may be that the path supplied is a symlink to something already loaded
    char resolvedPath[PATH_MAX];
    const char* realPathResult = realpath(path, resolvedPath);
    // If realpath() resolves to a path which does not exist on disk, errno is set to ENOENT
    bool checkRealPathToo = ((realPathResult != nullptr) || (errno == ENOENT)) && (strcmp(path, resolvedPath) != 0);
    if ( checkRealPathToo ) {
        alreadyLoadMH = gAllImages.alreadyLoaded(resolvedPath, bumpRefCount);
        log_apis("   dlopen: real path=%s\n", resolvedPath);
        if ( alreadyLoadMH != nullptr) {
            void* result = makeDlHandle(alreadyLoadMH, dontContinue);
            log_apis("   dlopen(%s) => %p\n", leafName, result);
            return result;
        }
    }

    // check if image is in a known ImageGroup
    __block const launch_cache::binary_format::Image* imageToLoad = nullptr;
    gPathOverrides.forEachPathVariant(path, ^(const char* possiblePath, bool& stop) {
        log_apis("   dlopen: checking for pre-built closure for path: %s\n", possiblePath);
        imageToLoad = gAllImages.findImageInKnownGroups(possiblePath);
        if ( imageToLoad != nullptr )
            stop = true;
    });
    if ( (imageToLoad == nullptr) && checkRealPathToo ) {
        gPathOverrides.forEachPathVariant(resolvedPath, ^(const char* possiblePath, bool& stop) {
            log_apis("   dlopen: checking for pre-built closure for real path: %s\n", possiblePath);
            imageToLoad = gAllImages.findImageInKnownGroups(possiblePath);
            if ( imageToLoad != nullptr )
                stop = true;
        });
    }

    // check if image from a known ImageGroup is already loaded (via a different path)
    if ( imageToLoad != nullptr ) {
        alreadyLoadMH = gAllImages.alreadyLoaded(imageToLoad, bumpRefCount);
        if ( alreadyLoadMH != nullptr) {
            void* result = makeDlHandle(alreadyLoadMH, dontContinue);
            log_apis("   dlopen(%s) => %p\n", leafName, result);
            return result;
        }
    }

    // RTLD_NOLOAD means do nothing if image not already loaded
    if ( mode & RTLD_NOLOAD ) {
        log_apis("   dlopen(%s) => NULL\n", leafName);
        return nullptr;
    }

    // if we have a closure, optimistically use it.  If out of date, it will fail
    if ( imageToLoad != nullptr ) {
        log_apis("   dlopen: trying existing closure image=%p\n", imageToLoad);
        Diagnostics diag;
        const mach_header* topLoadAddress = loadImageAndDependents(diag, imageToLoad, true);
        if ( diag.noError() ) {
            void* result = makeDlHandle(topLoadAddress, dontContinue);
            log_apis("   dlopen(%s) => %p\n", leafName, result);
            return result;
        }
        // image is no longer valid, will need to build one
        imageToLoad = nullptr;
        log_apis("   dlopen: existing closure no longer valid\n");
    }

    // if no existing closure, RPC to closured to create one
    const char* closuredErrorMessages[3];
    int closuredErrorMessagesCount = 0;
    if ( imageToLoad == nullptr ) {
        imageToLoad = gAllImages.messageClosured(path, "dlopen", closuredErrorMessages, closuredErrorMessagesCount);
    }

    // load images using new closure
    if ( imageToLoad != nullptr ) {
        log_apis("   dlopen: using closured built image=%p\n", imageToLoad);
        Diagnostics diag;
        const mach_header* topLoadAddress = loadImageAndDependents(diag, imageToLoad, true);
        if ( diag.noError() ) {
            void* result = makeDlHandle(topLoadAddress, dontContinue);
            log_apis("   dlopen(%s) => %p\n", leafName, result);
            return result;
        }
        if ( closuredErrorMessagesCount < 3 ) {
            closuredErrorMessages[closuredErrorMessagesCount++] = strdup(diag.errorMessage());
        }
    }

    // otherwise, closured failed to build needed load info
    switch ( closuredErrorMessagesCount ) {
        case 0:
            setErrorString("dlopen(%s, 0x%04X): closured error", path, mode);
            log_apis("   dlopen: closured error\n");
            break;
        case 1:
            setErrorString("dlopen(%s, 0x%04X): %s", path, mode, closuredErrorMessages[0]);
            log_apis("   dlopen: closured error: %s\n", closuredErrorMessages[0]);
            break;
        case 2:
            setErrorString("dlopen(%s, 0x%04X): %s %s", path, mode, closuredErrorMessages[0], closuredErrorMessages[1]);
            log_apis("   dlopen: closured error: %s %s\n", closuredErrorMessages[0], closuredErrorMessages[1]);
            break;
        case 3:
            setErrorString("dlopen(%s, 0x%04X): %s %s %s", path, mode, closuredErrorMessages[0], closuredErrorMessages[1], closuredErrorMessages[2]);
            log_apis("   dlopen: closured error: %s %s %s\n", closuredErrorMessages[0], closuredErrorMessages[1], closuredErrorMessages[2]);
            break;
    }
    for (int i=0; i < closuredErrorMessagesCount;++i)
        free((void*)closuredErrorMessages[i]);

    log_apis("   dlopen(%s) => NULL\n", leafName);

    return nullptr;
}

bool dlopen_preflight(const char* path)
{
    log_apis("dlopen_preflight(%s)\n", path);

    if ( gAllImages.alreadyLoaded(path, false) != nullptr )
        return true;

    if ( gAllImages.findImageInKnownGroups(path) != nullptr )
        return true;

    // map whole file
    struct stat statBuf;
    if ( ::stat(path, &statBuf) != 0 )
        return false;
    int fd = ::open(path, O_RDONLY);
    if ( fd < 0 )
        return false;
    const void* fileBuffer = ::mmap(NULL, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if ( fileBuffer == MAP_FAILED )
        return false;
    size_t mappedSize = (size_t)statBuf.st_size;

    // check if it is current arch mach-o or fat with slice for current arch
    __block bool result = false;
    __block Diagnostics diag;
    if ( MachOParser::isMachO(diag, fileBuffer, mappedSize) ) {
        result = true;
    }
    else {
        if ( FatUtil::isFatFile(fileBuffer) ) {
            FatUtil::forEachSlice(diag, fileBuffer, mappedSize, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, size_t sliceSz, bool& stop) {
                if ( MachOParser::isMachO(diag, sliceStart, sliceSz) ) {
                    result = true;
                    stop = true;
                }
            });
        }
    }
    ::munmap((void*)fileBuffer, mappedSize);

    // FIXME: may be symlink to something in dyld cache

    // FIXME: maybe ask closured

    return result;
}

static void* dlsym_search(const char* symName, const mach_header* startImageLoadAddress, const launch_cache::Image& startImage, bool searchStartImage, MachOParser::DependentFinder reExportFollower)
{
    // construct array of all BinImage* objects that dlopen'ed image depends on
    uint32_t maxLoad = startImage.maxLoadCount();
    const dyld3::launch_cache::binary_format::Image*    fullImageList[maxLoad];
    dyld3::launch_cache::SlowLoadSet imageSet(&fullImageList[0], &fullImageList[maxLoad]);
    imageSet.add(startImage.binaryData());
    STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, gAllImages.currentGroupsCount(), currentGroupsList);
    gAllImages.copyCurrentGroups(currentGroupsList);

    __block void* result = nullptr;
    auto handler = ^(const dyld3::launch_cache::binary_format::Image* aBinImage, bool& stop) {
        const mach_header* loadAddress = gAllImages.findLoadAddressByImage(aBinImage);
        if ( !searchStartImage && (loadAddress == startImageLoadAddress) )
            return;
        if ( loadAddress != nullptr ) {
            MachOParser parser(loadAddress);
            if ( parser.hasExportedSymbol(symName, reExportFollower, &result) ) {
                stop = true;
            }
        }
    };

    bool stop = false;
    handler(startImage.binaryData(), stop);
    if (stop)
        return result;

    // check each dependent image for symbol
    if ( !startImage.recurseAllDependentImages(currentGroupsList, imageSet, handler) ) {
        setErrorString("unexpected > %d images loaded", maxLoad);
        return nullptr;
    }
    return result;
}

void* dlsym(void* handle, const char* symbolName)
{
    log_apis("dlsym(%p, \"%s\")\n", handle, symbolName);

    clearErrorString();

    // dlsym() assumes symbolName passed in is same as in C source code
    // dyld assumes all symbol names have an underscore prefix
    char underscoredName[strlen(symbolName)+2];
    underscoredName[0] = '_';
    strcpy(&underscoredName[1], symbolName);

    // this block is only used if hasExportedSymbol() needs to trace re-exported dylibs to find a symbol
    MachOParser::DependentFinder reExportFollower = ^(uint32_t targetDepIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        if ( (strncmp(depLoadPath, "@rpath/", 7) == 0) && (extra != nullptr) ) {
            const mach_header* parentMH = (mach_header*)extra;
            launch_cache::Image parentImage = gAllImages.findByLoadAddress(parentMH);
            if ( parentImage.valid() ) {
                STACK_ALLOC_DYNARRAY(const launch_cache::BinaryImageGroupData*, gAllImages.currentGroupsCount(), currentGroupsList);
                gAllImages.copyCurrentGroups(currentGroupsList);
                parentImage.forEachDependentImage(currentGroupsList, ^(uint32_t parentDepIndex, dyld3::launch_cache::Image parentDepImage, dyld3::launch_cache::Image::LinkKind kind, bool &stop) {
                    if ( parentDepIndex != targetDepIndex )
                        return;
                    const mach_header* parentDepMH = gAllImages.findLoadAddressByImage(parentDepImage.binaryData());
                    if ( parentDepMH != nullptr ) {
                        *foundMH = parentDepMH;
                        stop = true;
                    }
                });
            }
        }
        else {
            *foundMH = gAllImages.alreadyLoaded(depLoadPath, false);
        }
        return (*foundMH != nullptr);
    };

    if ( handle == RTLD_DEFAULT ) {
        // magic "search all in load order" handle
        for (uint32_t index=0; index < gAllImages.count(); ++index) {
            const mach_header* loadAddress;
            launch_cache::Image image = gAllImages.findByLoadOrder(index, &loadAddress);
            if ( image.valid() ) {
                MachOParser parser(loadAddress);
                void* result;
                //log_apis("   dlsym(): index=%d, loadAddress=%p\n", index, loadAddress);
                if ( parser.hasExportedSymbol(underscoredName, reExportFollower, &result) ) {
                    log_apis("   dlsym() => %p\n", result);
                    return result;
                }
            }
        }
        setErrorString("dlsym(RTLD_DEFAULT, %s): symbol not found", symbolName);
        log_apis("   dlsym() => NULL\n");
        return nullptr;
    }
    else if ( handle == RTLD_MAIN_ONLY ) {
        // magic "search only main executable" handle
        MachOParser parser(gAllImages.mainExecutable());
        //log_apis("   dlsym(): index=%d, loadAddress=%p\n", index, loadAddress);
        void* result;
        if ( parser.hasExportedSymbol(underscoredName, reExportFollower, &result) ) {
            log_apis("   dlsym() => %p\n", result);
            return result;
        }
        setErrorString("dlsym(RTLD_MAIN_ONLY, %s): symbol not found", symbolName);
        log_apis("   dlsym() => NULL\n");
        return nullptr;
    }

    // rest of cases search in dependency order
    const mach_header* startImageLoadAddress;
    launch_cache::Image startImage(nullptr);
    void* result = nullptr;
    if ( handle == RTLD_NEXT ) {
        // magic "search what I would see" handle
        void* callerAddress = __builtin_return_address(0);
        startImage = gAllImages.findByOwnedAddress(callerAddress, &startImageLoadAddress);
        if ( ! startImage.valid() ) {
            setErrorString("dlsym(RTLD_NEXT, %s): called by unknown image (caller=%p)", symbolName, callerAddress);
            return nullptr;
        }
        result = dlsym_search(underscoredName, startImageLoadAddress, startImage, false, reExportFollower);
    }
    else if ( handle == RTLD_SELF ) {
        // magic "search me, then what I would see" handle
        void* callerAddress = __builtin_return_address(0);
        startImage = gAllImages.findByOwnedAddress(callerAddress, &startImageLoadAddress);
        if ( ! startImage.valid() ) {
            setErrorString("dlsym(RTLD_SELF, %s): called by unknown image (caller=%p)", symbolName, callerAddress);
            return nullptr;
        }
        result = dlsym_search(underscoredName, startImageLoadAddress, startImage, true, reExportFollower);
    }
    else {
        // handle value was something returned by dlopen()
        bool dontContinue;
        parseDlHandle(handle, &startImageLoadAddress, &dontContinue);
        startImage = gAllImages.findByLoadAddress(startImageLoadAddress);
        if ( !startImage.valid() ) {
            setErrorString("dlsym(%p, %s): invalid handle", handle, symbolName);
            log_apis("   dlsym() => NULL\n");
            return nullptr;
        }
        if ( dontContinue ) {
            // RTLD_FIRST only searches one place
            MachOParser parser(startImageLoadAddress);
           parser.hasExportedSymbol(underscoredName, reExportFollower, &result);
        }
        else {
            result = dlsym_search(underscoredName, startImageLoadAddress, startImage, true, reExportFollower);
        }
    }

    if ( result != nullptr ) {
        log_apis("   dlsym() => %p\n", result);
        return result;
    }

    setErrorString("dlsym(%p, %s): symbol not found", handle, symbolName);
    log_apis("   dlsym() => NULL\n");
    return nullptr;
}


const struct dyld_all_image_infos* _dyld_get_all_image_infos()
{
    return gAllImages.oldAllImageInfo();
}

bool dyld_shared_cache_some_image_overridden()
{
    log_apis("dyld_shared_cache_some_image_overridden()\n");

    assert(0 && "not implemented yet");
}

bool _dyld_get_shared_cache_uuid(uuid_t uuid)
{
    log_apis("_dyld_get_shared_cache_uuid()\n");

    if ( gAllImages.oldAllImageInfo() != nullptr ) {
        memcpy(uuid, gAllImages.oldAllImageInfo()->sharedCacheUUID, sizeof(uuid_t));
        return true;
    }
    return false;
}

const void* _dyld_get_shared_cache_range(size_t* mappedSize)
{
    log_apis("_dyld_get_shared_cache_range()\n");

    const DyldSharedCache* sharedCache = (DyldSharedCache*)gAllImages.cacheLoadAddress();
    if ( sharedCache != nullptr ) {
        *mappedSize = (size_t)sharedCache->mappedSize();
        return sharedCache;
    }
    *mappedSize = 0;
    return NULL;
}

bool _dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info)
{
    log_apis("_dyld_find_unwind_sections(%p, %p)\n", addr, info);

    const mach_header* mh = dyld_image_header_containing_address(addr);
    if ( mh == nullptr )
        return false;

    info->mh                            = mh;
    info->dwarf_section                 = nullptr;
    info->dwarf_section_length          = 0;
    info->compact_unwind_section        = nullptr;
    info->compact_unwind_section_length = 0;

    MachOParser parser(mh);
    parser.forEachSection(^(const char* segName, const char* sectName, uint32_t flags, const void* content, size_t sectSize, bool illegalSectionSize, bool& stop) {
        if ( strcmp(segName, "__TEXT") == 0 ) {
            if ( strcmp(sectName, "__eh_frame") == 0 ) {
                info->dwarf_section         = content;
                info->dwarf_section_length  = sectSize;
            }
            else if ( strcmp(sectName, "__unwind_info") == 0 ) {
                info->compact_unwind_section         = content;
                info->compact_unwind_section_length  = sectSize;
            }
        }
    });

    return true;
}


bool dyld_process_is_restricted()
{
    log_apis("dyld_process_is_restricted()\n");

    launch_cache::Closure closure(gAllImages.mainClosure());
    return closure.isRestricted();
}


const char* dyld_shared_cache_file_path()
{
    log_apis("dyld_shared_cache_file_path()\n");

    return gAllImages.dyldCachePath();
}


void dyld_dynamic_interpose(const mach_header* mh, const dyld_interpose_tuple array[], size_t count)
{
    log_apis("dyld_dynamic_interpose(%p, %p, %lu)\n", mh, array, count);
    // FIXME
}


static void* mapStartOfCache(const char* path, size_t length)
{
    struct stat statbuf;
    if ( ::stat(path, &statbuf) == -1 )
        return NULL;

    if ( statbuf.st_size < length )
        return NULL;

    int cache_fd = ::open(path, O_RDONLY);
    if ( cache_fd < 0 )
        return NULL;

    void* result = ::mmap(NULL, length, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    close(cache_fd);

    if ( result == MAP_FAILED )
        return NULL;

    return result;
}

static const DyldSharedCache* findCacheInDirAndMap(const uuid_t cacheUuid, const char* dirPath, size_t& sizeMapped)
{
    DIR* dirp = ::opendir(dirPath);
    if ( dirp != NULL) {
        dirent entry;
        dirent* entp = NULL;
        char cachePath[PATH_MAX];
        while ( ::readdir_r(dirp, &entry, &entp) == 0 ) {
            if ( entp == NULL )
                break;
            if ( entp->d_type != DT_REG ) 
                continue;
            if ( strlcpy(cachePath, dirPath, PATH_MAX) >= PATH_MAX )
                continue;
            if ( strlcat(cachePath, "/", PATH_MAX) >= PATH_MAX )
                continue;
            if ( strlcat(cachePath, entp->d_name, PATH_MAX) >= PATH_MAX )
                continue;
            if ( const DyldSharedCache* cache = (DyldSharedCache*)mapStartOfCache(cachePath, 0x00100000) ) {
                uuid_t foundUuid;
                cache->getUUID(foundUuid);
                if ( ::memcmp(foundUuid, cacheUuid, 16) != 0 ) {
                    // wrong uuid, unmap and keep looking
                    ::munmap((void*)cache, 0x00100000);
                }
                else {
                    // found cache
                    closedir(dirp);
                    sizeMapped = 0x00100000;
                    return cache;
                }
            }
        }
        closedir(dirp);
    }
    return nullptr;
}

int dyld_shared_cache_find_iterate_text(const uuid_t cacheUuid, const char* extraSearchDirs[], void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    log_apis("dyld_shared_cache_find_iterate_text()\n");

    // see if requested cache is the active one in this process
    size_t sizeMapped = 0;
    const DyldSharedCache* sharedCache = (DyldSharedCache*)gAllImages.cacheLoadAddress();
    if ( sharedCache != nullptr ) {
        uuid_t runningUuid;
        sharedCache->getUUID(runningUuid);
        if ( ::memcmp(runningUuid, cacheUuid, 16) != 0 )
            sharedCache = nullptr;
    }
    if ( sharedCache == nullptr ) {
         // if not, look in default location for cache files
    #if    __IPHONE_OS_VERSION_MIN_REQUIRED
        const char* defaultSearchDir = IPHONE_DYLD_SHARED_CACHE_DIR;
    #else
        const char* defaultSearchDir = MACOSX_DYLD_SHARED_CACHE_DIR;
    #endif
        sharedCache = findCacheInDirAndMap(cacheUuid, defaultSearchDir, sizeMapped);
        // if not there, look in extra search locations
        if ( sharedCache == nullptr ) {
            for (const char** p = extraSearchDirs; *p != nullptr; ++p) {
                sharedCache = findCacheInDirAndMap(cacheUuid, *p, sizeMapped);
                if ( sharedCache != nullptr )
                    break;
            }
        }
    }
    if ( sharedCache == nullptr )
        return -1;

    // get base address of cache
    __block uint64_t cacheUnslidBaseAddress = 0;
    sharedCache->forEachRegion(^(const void *content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
        if ( cacheUnslidBaseAddress == 0 )
            cacheUnslidBaseAddress = vmAddr;
    });

    // iterate all images
    sharedCache->forEachImageTextSegment(^(uint64_t loadAddressUnslid, uint64_t textSegmentSize, const uuid_t dylibUUID, const char* installName) {
        dyld_shared_cache_dylib_text_info dylibTextInfo;
        dylibTextInfo.version              = 2;
        dylibTextInfo.loadAddressUnslid    = loadAddressUnslid;
        dylibTextInfo.textSegmentSize      = textSegmentSize;
        dylibTextInfo.path                 = installName;
        ::memcpy(dylibTextInfo.dylibUuid, dylibUUID, 16);
        dylibTextInfo.textSegmentOffset    = loadAddressUnslid - cacheUnslidBaseAddress;
        callback(&dylibTextInfo);
    });

    if ( sizeMapped != 0 )
        ::munmap((void*)sharedCache, sizeMapped);

    return 0;
}

int dyld_shared_cache_iterate_text(const uuid_t cacheUuid, void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
    log_apis("dyld_shared_cache_iterate_text()\n");

    const char* extraSearchDirs[] = { NULL };
    return dyld3::dyld_shared_cache_find_iterate_text(cacheUuid, extraSearchDirs, callback);
}



} // namespace dyld3

