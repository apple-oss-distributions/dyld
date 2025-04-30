/*
 * Copyright (c) 2019-2020 Apple Inc. All rights reserved.
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
  #include <liblibc/plat/dyld/exclaves_dyld.h>
#else
  #include <_simple.h>
  #include <stdint.h>
  #include <dyld/VersionMap.h>
  #include <mach/mach_time.h> // mach_absolute_time()
  #include <mach-o/dyld_priv.h>
  #include <sys/syscall.h>
  #if BUILDING_DYLD
    #include <sys/types.h>
    #include <sys/sysctl.h>
    #include <sys/socket.h>
    #include <sys/syslog.h>
    #include <sys/uio.h>
    #include <sys/un.h>
    #include <sys/mman.h>
    #include <System/sys/csr.h>
    #include <System/sys/reason.h>
    #include <kern/kcdata.h>
    //FIXME: Hack to avoid <sys/commpage.h> being included by <System/machine/cpu_capabilities.h>
    #include <System/sys/commpage.h>
    #include <System/machine/cpu_capabilities.h>
    #if __arm64__ && !TARGET_OS_EXCLAVEKIT
        #include <arm/cpu_capabilities_public.h>
    #endif
    #if !TARGET_OS_DRIVERKIT
        #include <vproc_priv.h>
    #endif
  // no libc header for send() syscall interface
  extern "C" ssize_t __sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
  #endif // BUILDING_DYLD
#endif


#include <string_view>

#if !TARGET_OS_EXCLAVEKIT
#if  BUILDING_DYLD && !TARGET_OS_SIMULATOR
    #include <libamfi.h>
#else
enum
{
    AMFI_DYLD_INPUT_PROC_IN_SIMULATOR = (1 << 0),
};
enum amfi_dyld_policy_output_flag_set
{
    AMFI_DYLD_OUTPUT_ALLOW_AT_PATH                  = (1 << 0),
    AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS                = (1 << 1),
    AMFI_DYLD_OUTPUT_ALLOW_CUSTOM_SHARED_CACHE      = (1 << 2),
    AMFI_DYLD_OUTPUT_ALLOW_FALLBACK_PATHS           = (1 << 3),
    AMFI_DYLD_OUTPUT_ALLOW_PRINT_VARS               = (1 << 4),
    AMFI_DYLD_OUTPUT_ALLOW_FAILED_LIBRARY_INSERTION = (1 << 5),
    AMFI_DYLD_OUTPUT_ALLOW_LIBRARY_INTERPOSING      = (1 << 6),
    AMFI_DYLD_OUTPUT_ALLOW_EMBEDDED_VARS            = (1 << 7),
    AMFI_DYLD_OUTPUT_ALLOW_DEVELOPMENT_VARS         = (1 << 8),
    AMFI_DYLD_OUTPUT_ALLOW_LIBSYSTEM_OVERRIDE       = (1 << 9),
};
extern "C" int amfi_check_dyld_policy_self(uint64_t input_flags, uint64_t* output_flags);
    #include "dyldSyscallInterface.h"
#endif //  BUILDING_DYLD && !TARGET_OS_SIMULATOR
#endif // !TARGET_OS_EXCLAVEKIT

// common
#include "Defines.h"
#include "MachOLoaded.h"
#include "MachOAnalyzer.h"
#include "DyldSharedCache.h"

// dyld
#include "DyldProcessConfig.h"
#include "SharedCacheRuntime.h"
#include "Loader.h"
#include "Utilities.h"

// mach_o
#include "Header.h"
#include "Image.h"

#if BUILDING_DYLD && SUPPORT_IGNITION
    #include <ignition/ignite.h>
#endif


using dyld3::MachOFile;

// mach_o
using mach_o::FunctionVariants;
using mach_o::FunctionVariantsRuntimeTable;
using mach_o::Header;
using mach_o::Image;
using mach_o::Platform;
using mach_o::Version32;


extern const Header __dso_handle;

#if !TARGET_OS_EXCLAVEKIT
static bool hexCharToByte(const char hexByte, uint8_t& value)
{
    if ( hexByte >= '0' && hexByte <= '9' ) {
        value = hexByte - '0';
        return true;
    }
    else if ( hexByte >= 'A' && hexByte <= 'F' ) {
        value = hexByte - 'A' + 10;
        return true;
    }
    else if ( hexByte >= 'a' && hexByte <= 'f' ) {
        value = hexByte - 'a' + 10;
        return true;
    }

    return false;
}

static uint64_t hexToUInt64(const char* startHexByte, const char** endHexByte)
{
    const char* scratch;
    if ( endHexByte == nullptr ) {
        endHexByte = &scratch;
    }
    if ( startHexByte == nullptr )
        return 0;
    uint64_t retval = 0;
    if ( (startHexByte[0] == '0') && (startHexByte[1] == 'x') ) {
        startHexByte += 2;
    }
    *endHexByte = startHexByte + 16;

    //FIXME overrun?
    for ( uint32_t i = 0; i < 16; ++i ) {
        uint8_t value;
        if ( !hexCharToByte(startHexByte[i], value) ) {
            *endHexByte = &startHexByte[i];
            break;
        }
        retval = (retval << 4) + value;
    }
    return retval;
}

#endif // !TARGET_OS_EXCLAVEKIT


namespace dyld4 {

//
// MARK: --- KernelArgs methods ---
//
#if !BUILDING_DYLD
KernelArgs::KernelArgs(const MachOFile* mh, const std::vector<const char*>& argv, const std::vector<const char*>& envp, const std::vector<const char*>& apple)
    : mainExecutable((const MachOAnalyzer*)mh)
    , argc(argv.size())
{
    assert( argv.size() + envp.size() + apple.size() < MAX_KERNEL_ARGS);

    // build the info passed to dyld on startup the same way the kernel does on the stack
    size_t index = 0;
    for ( const char* arg : argv )
        args[index++] = arg;
    args[index++] = nullptr;

    for ( const char* arg : envp )
        args[index++] = arg;
    args[index++] = nullptr;

    for ( const char* arg : apple )
        args[index++] = arg;
    args[index++] = nullptr;
}
#endif


#if !TARGET_OS_EXCLAVEKIT
const char** KernelArgs::findArgv() const
{
    return (const char**)&args[0];
}

const char** KernelArgs::findEnvp() const
{
    // argv array has nullptr at end, so envp starts at argc+1
    return (const char**)&args[argc + 1];
}

const char** KernelArgs::findApple() const
{
    // envp array has nullptr at end, apple starts after that
    const char** p = findEnvp();
    while ( *p != nullptr )
        ++p;
    ++p;
    return p;
}
#endif // !TARGET_OS_EXCLAVEKIT


//
// MARK: --- ProcessConfig methods ---
//
ProcessConfig::ProcessConfig(const KernelArgs* kernArgs, SyscallDelegate& syscallDelegate, Allocator& allocator)
  : syscall(syscallDelegate),
    process(kernArgs, syscallDelegate, allocator),
    security(process, syscallDelegate),
    log(process, security, syscallDelegate),
    dyldCache(process, security, log, syscallDelegate, allocator, *this),
    pathOverrides(process, security, log, dyldCache, syscallDelegate, allocator)
{
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // hack to allow macOS 13 dyld to run chrooted on older kernels
    if ( (this->dyldCache.addr == nullptr) || (this->dyldCache.addr->header.mappingOffset <= offsetof(dyld_cache_header, cacheSubType)) )
        this->process.pageInLinkingMode = 0;
#endif // TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT

    // env vars maybe override the "roots state" for the shared cache.  Do that now if we have env vars that matter
    if ( pathOverrides.dontUsePrebuiltForApp() )
        dyldCache.adjustRootsSupportForEnvVars();

    // now that rest of config object is initialized, set up per-process function-variant flags
    process.evaluateFunctionVariantFlags(*this);
#if BUILDING_DYLD
    // if dyld itself has function-variants, select those
    if ( process.dyldHdr->hasFunctionVariantFixups() ) {
        assert(process.dyldHdr->preferredLoadAddress() == 0);
        int64_t dyldSlide = process.dyldHdr->getSlide();
        Image dyldImage((mach_header*)process.dyldHdr);
        const FunctionVariants& fvs = dyldImage.functionVariants();
        dyldImage.functionVariantFixups().forEachFixup(^(mach_o::FunctionVariantFixups::InternalFixup fixupInfo) {
            assert(fixupInfo.variantIndex < fvs.count());
            uint64_t   bestImplOffset = process.selectFromFunctionVariants(fvs, fixupInfo.variantIndex);
            uintptr_t  bestImplAddr   = (uintptr_t)process.dyldHdr + (uintptr_t)bestImplOffset;
            uint64_t   address        = dyldImage.header()->segmentVmAddr(fixupInfo.segIndex) + fixupInfo.segOffset + dyldSlide;
            uintptr_t* loc            = (uintptr_t*)address;
#if __has_feature(ptrauth_calls)
            if ( fixupInfo.pacAuth )
                bestImplAddr = signPointer(bestImplAddr, loc, fixupInfo.pacAddress, fixupInfo.pacDiversity, (ptrauth_key)fixupInfo.pacKey);
#endif
            *loc = bestImplAddr;
        });
    }
#endif // BUILDING_DYLD
}

uint64_t ProcessConfig::Process::selectFromFunctionVariants(const FunctionVariants& fvs, uint32_t fvTableIndex) const
{
    __block uint64_t                    result        = 0;
    __block bool                        resultIsTable = false;
    const FunctionVariantsRuntimeTable* fvTable       = fvs.entry(fvTableIndex);
    fvTable->forEachVariant(^(FunctionVariantsRuntimeTable::Kind kind, uint32_t implOffset, bool implIsTable, std::span<const uint8_t> flagIndexes, bool& stop) {
#if BUILDING_CACHE_BUILDER
        // in dyld cache, always bind to "default" impl
        if ( flagIndexes.empty() ) {
            result        = implOffset;
            resultIsTable = implIsTable;
        }
#else
        FunctionVariantFlags actualFlags = 0;
        switch ( kind ) {
            case FunctionVariantsRuntimeTable::Kind::perProcess:
                actualFlags = perProcessFunctionVariantFlags;
                break;
            case FunctionVariantsRuntimeTable::Kind::systemWide:
                actualFlags = systemWideFunctionVariantFlags;
                break;
    #if __arm64__
            case FunctionVariantsRuntimeTable::Kind::arm64:
                actualFlags = processorFunctionVariantFlags;
                break;
    #endif
    #if __x86_64__
            case FunctionVariantsRuntimeTable::Kind::x86_64:
                actualFlags = processorFunctionVariantFlags;
                break;
    #endif
            default:
                result = 0; // function-variant keys for different hardware
                stop = true;
                break;
        }
        bool allMatch = true;
        for ( uint8_t flagIndex : flagIndexes ) {
            FunctionVariantFlags testFlag = ((FunctionVariantFlags)1 << flagIndex);
            if ( (actualFlags & testFlag) == 0 )
                allMatch = false;
        }
        if ( allMatch ) {
            result        = implOffset;
            resultIsTable = implIsTable;
            stop   = true;
        }
#endif
    });
    if ( (result != 0) && resultIsTable ) {
        // multi-level function variant table. process next level
        return selectFromFunctionVariants(fvs, (uint32_t)result);
    }
    return result;
}


#if !BUILDING_DYLD
void ProcessConfig::reset(const MachOFile* mainExe, const char* mainPath, const DyldSharedCache* cache)
{
    process.mainExecutablePath    = mainPath;
    process.mainUnrealPath        = mainPath;
#if BUILDING_CACHE_BUILDER
    process.mainExecutableMF      = mainExe;
#else
    process.mainExecutableMF      = (const MachOAnalyzer*)mainExe;
#endif
    process.mainExecutableHdr     = (const Header*)mainExe;
    dyldCache.addr                = cache;
#if SUPPORT_VM_LAYOUT
    dyldCache.slide               = (cache != nullptr) ? cache->slide() : 0;
#endif
}
#endif

void ProcessConfig::scanForRoots() const
{
#if BUILDING_DYLD && TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    if ( this->dyldCache.addr == nullptr )
        return;

    DyldCommPage commPage = this->process.commPage;

    __block bool foundRoot = false;
    this->dyldCache.addr->forEachImage(^(const Header *hdr, const char *installName) {
        if ( foundRoot )
            return;

        // Skip sim dylibs. They are handled above
        if ( !strcmp(installName, "/usr/lib/system/libsystem_kernel.dylib") ) {
            if ( !commPage.libKernelRoot )
                return;
        }
        if ( !strcmp(installName, "/usr/lib/system/libsystem_platform.dylib") ) {
            if ( !commPage.libPlatformRoot )
                return;
        }
        if ( !strcmp(installName, "/usr/lib/system/libsystem_pthread.dylib") ) {
            if ( !commPage.libPthreadRoot )
                return;
        }

        // Skip dyld.  It knows how to work if its a root
        if ( !strcmp(installName, "/usr/lib/dyld") )
            return;

        bool stop = false;
        this->pathOverrides.forEachPathVariant(installName, Platform::macCatalyst, false, true, stop,
                                               ^(const char* possiblePath, ProcessConfig::PathOverrides::Type type, bool& innerStop) {
            // look only at variants that might override the original path
            if ( type > ProcessConfig::PathOverrides::Type::rawPath ) {
                innerStop = true;
                return;
            }

            // dyld4::console("dyld: checking for root at %s\n", possiblePath);
            if ( this->syscall.fileExists(possiblePath) ) {
                if ( commPage.logRoots )
                    dyld4::console("dyld: found root at %s\n", possiblePath);
                foundRoot = true;
                innerStop = true;
                return;
            }
        });
    });

    commPage.foundRoot = foundRoot;

    this->syscall.setDyldCommPageFlags(commPage);

#endif // #if BUILDING_DYLD && TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
}

void* ProcessConfig::scanForRoots(void* context)
{
    const ProcessConfig* config = (const ProcessConfig*)context;
    config->scanForRoots();
    return nullptr;
}


//
// MARK: --- Process methods ---
//

bool ProcessConfig::Process::defaultDataConst() const
{
#if !TARGET_OS_EXCLAVEKIT
    if ( this->commPage.forceRWDataConst )
        return false;
    else if ( this->commPage.forceRWDataConst )
        return true;
    else  // __DATA_CONST is enabled by default, as the above boot-args didn't override it
#endif // !TARGET_OS_EXCLAVEKIT
        return true;
}

// This returns true if the process is using TPRO for DATA_CONST.
// This is independent of whether the process is using TPRO for the allocator (ie TPRO_CONST segment)
bool ProcessConfig::Process::defaultTproDataConst() const
{
#if TARGET_OS_EXCLAVEKIT || TARGET_OS_SIMULATOR
    return false;
#else
    return (this->appleParam("dyld_hw_tpro_pagers") != nullptr);
#endif
}

// This returns true if the process is using TPRO for the stack.
// This is independent of whether the process is using TPRO for the allocator (ie TPRO_CONST segment)
bool ProcessConfig::Process::defaultTproStack() const
{
    if ( !defaultTproHW() )
        return false;

    mach_o::PlatformAndVersions pvs = mainExecutableHdr->platformAndVersions();
    mach_o::Policy policy(mach_o::Architecture(), pvs, 0);
    return policy.useProtectedStack();
}

// This returns true if the HW supports TPRO.  If this is true, then the allocator's TPRO_CONST segment
// will use TPRO not mprotect.
// This is independent of whether the process is using TPRO for DATA_CONST, see defaultTproDataConst() above.
bool ProcessConfig::Process::defaultTproHW() const
{
#if TARGET_OS_EXCLAVEKIT || TARGET_OS_SIMULATOR
    return false;
#else
    return (this->appleParam("dyld_hw_tpro") != nullptr);
#endif
}



#if TARGET_OS_EXCLAVEKIT
static dyld3::OverflowSafeArray<PreMappedFileEntry> parseExclaveMappingDescriptor(const char* data)
{
    dyld3::OverflowSafeArray<PreMappedFileEntry> result;
    while (1) {
        uintptr_t address;
        memcpy(&address, data, sizeof(address));
        data += sizeof(address);
        if (address == 0)
            break;

        uintptr_t size;
        memcpy(&size, data, sizeof(size));
        data += sizeof(size);

        const char *path = data;
        size_t path_len = strlen(data);
        data += path_len + 1;

        result.push_back({ .loadAddress = (mach_header*)address, .mappedSize = size, .path = path });
    }

    return result;
}
#endif // TARGET_OS_EXCLAVEKIT

ProcessConfig::Process::Process(const KernelArgs* kernArgs, SyscallDelegate& syscall, Allocator& allocator)
{
#if TARGET_OS_EXCLAVEKIT
    this->entry_vec              = kernArgs->entry_vec;
    uint32_t* startupPtr         = (uint32_t*)kernArgs->mappingDescriptor;
    this->startupContractVersion = *startupPtr;
    assert(this->startupContractVersion == 1);
    startupPtr++;
    this->sharedCacheFileEnabled = kernArgs->dyldSharedCacheEnabled;
    this->preMappedFiles      = parseExclaveMappingDescriptor((const char*)startupPtr);
    this->mainExecutableMF      = (MachOAnalyzer*)this->preMappedFiles[0].loadAddress;
    this->mainExecutableHdr      = (const Header*)this->preMappedFiles[0].loadAddress;
    this->mainExecutablePath  = this->preMappedFiles[0].path;
    if ( this->sharedCacheFileEnabled ) {
        this->preMappedCache      = (void*)this->preMappedFiles[this->preMappedFiles.count()-1].loadAddress;
        this->preMappedCacheSize  = this->preMappedFiles[this->preMappedFiles.count()-1].mappedSize;
        this->preMappedCachePath  = this->preMappedFiles[this->preMappedFiles.count()-1].path;
    }
    this->progname            = PathOverrides::getLibraryLeafName(this->mainExecutablePath);
    //TODO: EXCLAVES Update/Remove argc, argv, etc.
    this->argc                = 1;
    this->argv                = NULL;
    this->envp                = NULL;
    this->apple               = NULL;
#else
    this->mainExecutableMF                                          = kernArgs->mainExecutable;
    this->mainExecutableHdr                                         = (const Header*)kernArgs->mainExecutable;
    this->argc                                                      = (int)kernArgs->argc;
    this->argv                                                      = kernArgs->findArgv();
    this->envp                                                      = kernArgs->findEnvp();
    this->apple                                                     = kernArgs->findApple();
    this->pid                                                       = syscall.getpid();
    this->commPage                                                  = syscall.dyldCommPageFlags();
    this->isTranslated                                              = syscall.isTranslated();
    std::tie(this->mainExecutableFSID, this->mainExecutableObjID)   = this->getMainFileID();
    std::tie(this->dyldFSID, this->dyldObjID)                       = this->getDyldFileID();
    this->mainUnrealPath                                            = this->getMainUnrealPath(syscall, allocator);
    this->mainExecutablePath                                        = this->getMainPath(syscall, allocator);
    this->progname                                                  = PathOverrides::getLibraryLeafName(this->mainUnrealPath);
    this->dyldPath                                                  = this->getDyldPath(syscall, allocator);
    if ( this->pid == 1 ) {
        // The comm page flags are effectively a namespace.  PID 1 should mask out the bits it owns
        *((uint64_t*)&this->commPage) &= DyldCommPage::bootArgsMask;
#if TARGET_OS_OSX
        // Only macOS uses the "foundRoot" variable.  But its only set later in scanForRoots().
        // Until that runs, assume we have roots, as we don't know for sure that we don't
        this->commPage.foundRoot = true;
#endif // TARGET_OS_OSX
    }
#endif // TARGET_OS_EXCLAVEKIT
    this->dyldHdr                                                   = &__dso_handle;
    this->platform                                                  = this->getMainPlatform();
    this->catalystRuntime                                           = this->usesCatalyst();
    this->archs                                                     = this->getMainArchs(syscall);
    this->enableDataConst                                           = this->defaultDataConst();
    this->enableTproHeap                                            = this->defaultTproHW();
    this->enableTproDataConst                                       = this->defaultTproDataConst();
    this->enableProtectedStack                                      = this->defaultTproStack();
#if TARGET_OS_SIMULATOR
    std::tie(this->dyldSimFSID, this->dyldSimObjID)                 = this->getDyldSimFileID(syscall);
#endif
#if TARGET_OS_EXCLAVEKIT
    // check if exclaves supports page-in linking
    fixup_register_status_tag xrt_result = xrt_dyld_register_fixup_cb(Loader::exclaveKitPageInFixups);
    if ( xrt_result & XRT_DYLD_DYNAMIC_FIXUPS_FOR_DYLIBS )
        this->pageInLinkingMode  = 2;
    this->sharedCachePageInLinking = ((xrt_result & XRT_DYLD_DYNAMIC_FIXUPS_FOR_SHAREDCACHE) != 0);

#endif // TARGET_OS_EXCLAVEKIT
#if TARGET_OS_OSX
    this->proactivelyUseWeakDefMap = (strncmp(progname, "MATLAB",6) == 0); // rdar://81498849
#else
    this->proactivelyUseWeakDefMap = false;
#endif // TARGET_OS_OSX

#if !TARGET_OS_EXCLAVEKIT
    this->pageInLinkingMode  = 2;
    if ( syscall.internalInstall() ) {
        if ( this->commPage.disablePageInLinking ) {
            this->pageInLinkingMode = 0;    // no page-in-linking
        } else if ( const char* mode = environ("DYLD_PAGEIN_LINKING") ) {
            if (strcmp(mode, "0") == 0 )
                this->pageInLinkingMode = 0;    // no page-in-linking
            else if (strcmp(mode, "1") == 0 )
                this->pageInLinkingMode = 1;    // page-in-linking data structures set up, by applied in-process by dyld
            else if (strcmp(mode, "2") == 0 )
                this->pageInLinkingMode = 2;    // page-in-linking done by kernel
            else if (strcmp(mode, "3") == 0 )
                this->pageInLinkingMode = 3;    // page-in-linking done by kernel, not disabled
        }
    }
    if ( (this->pageInLinkingMode >= 2) && syscall.sandboxBlockedPageInLinking() ) {
        //console("sandboxing has disabled page-in linking\n");
        this->pageInLinkingMode = 0;
    }
#if __has_feature(ptrauth_calls)
        // FIXME: don't use page-in linking for these process that use B keys
    if ( strcmp(this->mainExecutablePath, "/usr/libexec/adid") == 0 )
        this->pageInLinkingMode = 0;
    if ( strcmp(this->mainExecutablePath, "/usr/libexec/fairplaydeviceidentityd") == 0 )
        this->pageInLinkingMode = 0;
    if ( strcmp(this->mainExecutablePath, "/System/Library/PrivateFrameworks/CoreADI.framework/Versions/A/adid") == 0 )
        this->pageInLinkingMode = 0;
#if TARGET_OS_OSX
    if ( strcmp(this->mainExecutablePath, "/System/Library/PrivateFrameworks/CoreFP.framework/Versions/A/fairplayd") == 0 )
        this->pageInLinkingMode = 0;
#else
    if ( strncmp(this->mainExecutablePath, "/usr/sbin/fairplayd", 19) == 0 )
        this->pageInLinkingMode = 0;
#endif // TARGET_OS_OSX
#endif // __has_feature(ptrauth_calls)

#if TARGET_OS_TV && !TARGET_OS_SIMULATOR && BUILDING_DYLD
#endif // TARGET_OS_TV && !TARGET_OS_SIMULATOR
#if TARGET_OS_OSX
    // don't use page-in-linking when running under rosetta
    if ( this->isTranslated )
        this->pageInLinkingMode = 0;
#endif // TARGET_OS_OSX
#endif // !TARGET_OS_EXCLAVEKIT
    

}

#if !TARGET_OS_EXCLAVEKIT
const char* ProcessConfig::Process::appleParam(const char* key) const
{
    return _simple_getenv((const char**)apple, key);
}

const char* ProcessConfig::Process::environ(const char* key) const
{
    return _simple_getenv((const char**)envp, key);
}

std::pair<uint64_t, uint64_t> ProcessConfig::Process::fileIDFromFileHexStrings(const char* encodedFileInfo)
{
    // kernel passes fsID and objID encoded as two hex values (e.g. 0x123,0x456)
    const char* endPtr  = nullptr;
    uint64_t    fsID    = hexToUInt64(encodedFileInfo, &endPtr);
    if ( endPtr == nullptr ) { return {0, 0}; }
    uint64_t objID = hexToUInt64(endPtr+1, &endPtr);
    if ( endPtr == nullptr ) { return {0, 0}; }

    // something wrong with "executable_file=" or "dyld_file=" encoding
    return { fsID, objID };
}

const char* ProcessConfig::Process::pathFromFileHexStrings(SyscallDelegate& sys, Allocator& allocator, const char* encodedFileInfo)
{
    auto [fsID, objID] = fileIDFromFileHexStrings(encodedFileInfo);
    if (fsID && objID) {
        char pathFromIDs[PATH_MAX];
        if ( sys.fsgetpath(pathFromIDs, PATH_MAX, fsID, objID) != -1 ) {
            // return read-only copy of absolute path
            return allocator.strdup(pathFromIDs);
        }
    }
    // something wrong with "executable_file=" or "dyld_file=" encoding
    return nullptr;
}

std::pair<uint64_t, uint64_t> ProcessConfig::Process::getDyldFileID() {
    // kernel passes fsID and objID of dyld encoded as two hex values (e.g. 0x123,0x456)
    if ( const char* dyldFsIdAndObjId = this->appleParam("dyld_file") ) {
        return fileIDFromFileHexStrings(dyldFsIdAndObjId);
    }
    return {0,0};
}

#if TARGET_OS_SIMULATOR
std::pair<uint64_t, uint64_t> ProcessConfig::Process::getDyldSimFileID(SyscallDelegate& sys) {
    const char* rootPath = this->environ("DYLD_ROOT_PATH");
    char simDyldPath[PATH_MAX];
    strlcpy(simDyldPath, rootPath, PATH_MAX);
    strlcat(simDyldPath, "/usr/lib/dyld_sim", PATH_MAX);
    struct stat stat_buf;
    if (sys.stat(simDyldPath, &stat_buf) == 0) {
        return {stat_buf.st_dev, stat_buf.st_ino};
    }
    return {0,0};
}
#endif

const char* ProcessConfig::Process::getDyldPath(SyscallDelegate& sys, Allocator& allocator)
{
#if !TARGET_OS_EXCLAVEKIT
    if (dyldFSID && dyldObjID) {
        char pathFromIDs[PATH_MAX];
        if ( sys.fsgetpath(pathFromIDs, PATH_MAX, dyldFSID, dyldObjID) != -1 ) {
            return allocator.strdup(pathFromIDs);
        }
    }
#endif
    
    // something wrong with "dyld_file=", fallback to default
    return "/usr/lib/dyld";
}

std::pair<uint64_t, uint64_t> ProcessConfig::Process::getMainFileID() {
    // kernel passes fsID and objID of dyld encoded as two hex values (e.g. 0x123,0x456)
    if ( const char* dyldFsIdAndObjId = this->appleParam("executable_file") ) {
        return fileIDFromFileHexStrings(dyldFsIdAndObjId);
    }
    return {0,0};
}

const char* ProcessConfig::Process::getMainPath(SyscallDelegate& sys, Allocator& allocator)
{
    if (mainExecutableFSID && mainExecutableObjID) {
        char pathFromIDs[PATH_MAX];
        if ( sys.fsgetpath(pathFromIDs, PATH_MAX, mainExecutableFSID, mainExecutableObjID) != -1 ) {
            return allocator.strdup(pathFromIDs);
        }
    }

    // something wrong with "executable_file=", fallback to (un)realpath
    char resolvedPath[PATH_MAX];
    if ( sys.realpath(this->mainUnrealPath, resolvedPath) ) {
        return allocator.strdup(resolvedPath);
    }
    return this->mainUnrealPath;
}

const char* ProcessConfig::Process::getMainUnrealPath(SyscallDelegate& sys, Allocator& allocator)
{
    // if above failed, kernel also passes path to main executable in apple param
    const char* mainPath = this->appleParam("executable_path");

    // if kernel arg is missing, fallback to argv[0]
    if ( mainPath == nullptr )
        mainPath = argv[0];

    // if path is not a full path, use cwd to transform it to a full path
    if ( mainPath[0] != '/' ) {
        // normalize someone running ./foo from the command line
        if ( (mainPath[0] == '.') && (mainPath[1] == '/') ) {
            mainPath += 2;
        }
        // have relative path, use cwd to make absolute
        char buff[PATH_MAX];
        if ( sys.getCWD(buff) ) {
            strlcat(buff, "/", PATH_MAX);
            strlcat(buff, mainPath, PATH_MAX);
            mainPath = allocator.strdup(buff);
        }
    }

    return mainPath;
}

uint32_t ProcessConfig::Process::findVersionSetEquivalent(Platform versionPlatform, uint32_t version) const {
    uint32_t candidateVersion = 0;
    uint32_t candidateVersionEquivalent = 0;
    uint32_t newVersionSetVersion = 0;
    for (const auto& i : dyld3::sVersionMap) {
        versionPlatform = versionPlatform.basePlatform();
        if ( versionPlatform == Platform::macOS )
            newVersionSetVersion = i.macos;
        else if ( versionPlatform == Platform::iOS )
            newVersionSetVersion = i.ios;
        else if ( versionPlatform == Platform::watchOS )
            newVersionSetVersion = i.watchos;
        else if ( versionPlatform == Platform::tvOS )
            newVersionSetVersion = i.tvos;
        else if ( versionPlatform == Platform::bridgeOS )
            newVersionSetVersion = i.bridgeos;
#if BUILDING_DYLD || BUILDING_UNIT_TESTS // cache builder builds with old SDK that does not support visionOS
        else if ( versionPlatform == Platform::visionOS )
            newVersionSetVersion = i.visionos;
#endif
        else
            newVersionSetVersion = 0xffffffff; // If we do not know about the platform it is newer than everything

        if (newVersionSetVersion > version) { break; }
        candidateVersion = newVersionSetVersion;
        candidateVersionEquivalent = i.set;
    }

    if (newVersionSetVersion == 0xffffffff && candidateVersion == 0) {
        candidateVersionEquivalent = newVersionSetVersion;
    }

    return candidateVersionEquivalent;
};
#endif // !TARGET_OS_EXCLAVEKIT

bool ProcessConfig::Process::usesCatalyst() const
{
#if BUILDING_DYLD
    #if TARGET_OS_OSX
        #if __arm64__
            // on Apple Silicon macs, iOS apps and Catalyst apps use catalyst runtime
            return ( (this->platform == Platform::macCatalyst) || (this->platform == Platform::iOS) );
        #else
            return (this->platform == Platform::macCatalyst);
        #endif
    #else
        return false;
    #endif // TARGET_OS_OSX
#else
    // FIXME: may need a way to fake iOS-apps-on-Mac for unit tests
    return ( this->platform == Platform::macCatalyst );
#endif// BUILDING_DYLD
}

Platform ProcessConfig::Process::getMainPlatform()
{
    // extract platform from main executable
    mach_o::PlatformAndVersions pvs = mainExecutableHdr->platformAndVersions();
    this->mainExecutableSDKVersion   = pvs.sdk.value();
    this->mainExecutableMinOSVersion = pvs.minOS.value();
    Platform result = pvs.platform;

#if !TARGET_OS_EXCLAVEKIT
    // platform overrides only applicable on macOS, and can only force to 6 or 2
    if ( result == Platform::macOS ) {
        if ( const char* forcedPlatform = this->environ("DYLD_FORCE_PLATFORM") ) {
            if ( mainExecutableHdr->allowsAlternatePlatform() ) {
                if ( strncmp(forcedPlatform, "6", 1) == 0 ) {
                    result = Platform::macCatalyst;
                }
                else if ( (strncmp(forcedPlatform, "2", 1) == 0) && (strcmp(mainExecutableMF->archName(), "arm64") == 0) ) {
                    result = Platform::iOS;
                }

                for (const dyld3::VersionSetEntry& entry : dyld3::sVersionMap) {
                    if ( entry.macos == this->mainExecutableSDKVersion ) {
                        this->mainExecutableSDKVersion = entry.ios;
                        break;
                    }
                }
                for (const dyld3::VersionSetEntry& entry : dyld3::sVersionMap) {
                    if ( entry.macos == this->mainExecutableMinOSVersion ) {
                        this->mainExecutableMinOSVersion = entry.ios;
                        break;
                    }
                }
            }
        }
    }

    this->basePlatform = result.basePlatform();
    this->mainExecutableSDKVersionSet = findVersionSetEquivalent(this->basePlatform, this->mainExecutableSDKVersion);
    this->mainExecutableMinOSVersionSet = findVersionSetEquivalent(this->basePlatform, this->mainExecutableMinOSVersion);
#endif // !TARGET_OS_EXCLAVEKIT

    return result;
}


const GradedArchs* ProcessConfig::Process::getMainArchs(SyscallDelegate& sys)
{
#if TARGET_OS_EXCLAVEKIT
    return &GradedArchs::arm64e;
#else
    bool keysOff        = false;
    bool osBinariesOnly = false;
#if BUILDING_CLOSURE_UTIL
    // In closure util, just assume we want to allow arm64 binaries to get closures built
    // against arm64e shared caches
    if ( strcmp(mainExecutableMF->archName(), "arm64e") == 0 )
        keysOff = true;
#elif BUILDING_DYLD
  #if __has_feature(ptrauth_calls)
    if ( strcmp(mainExecutableMF->archName(), "arm64") == 0 ) {
        // keys are always off for arm64 apps
        keysOff = true;
    }
    else if ( const char* disableStr = this->appleParam("ptrauth_disabled") ) {
        // Check and see if kernel disabled JOP pointer signing for some other reason
        if ( strcmp(disableStr, "1") == 0 )
            keysOff = true;
    }
  #endif
#else
    if ( const char* disableStr = this->appleParam("ptrauth_disabled") ) {
        // Check and see if kernel disabled JOP pointer signing for some other reason
        if ( strcmp(disableStr, "1") == 0 )
            keysOff = true;
    }
#endif
    return &sys.getGradedArchs(mainExecutableMF->archName(), keysOff, osBinariesOnly);
#endif
}

bool ProcessConfig::Process::isInternalSimulator(SyscallDelegate& sys) const
{
#if TARGET_OS_SIMULATOR
    if ( const char* simulator_root = environ("SIMULATOR_ROOT") ) {
        char buf[PATH_MAX];
        strlcpy(buf, simulator_root, sizeof(buf));
        strlcat(buf, "/AppleInternal", sizeof(buf));
        if ( sys.dirExists(buf) )
           return true;
     }
#endif
     return false;
}

// run at end of ProcessConfig constuctor once rest of fields have been set
void ProcessConfig::Process::evaluateFunctionVariantFlags(const ProcessConfig& config)
{
    // set all pre-process variant flags
    this->perProcessFunctionVariantFlags = ProcessConfig::evaluatePerProcessVariantFlags(config);

#if !TARGET_OS_EXCLAVEKIT
    // if there is a dyld cache with variant flags, use that, otherwise evaluate locally
    if ( config.dyldCache.addr != nullptr && (config.dyldCache.addr->dynamicRegion() != nullptr) && (config.dyldCache.addr->dynamicRegion()->version() >= 1) ) {
        this->systemWideFunctionVariantFlags = config.dyldCache.addr->dynamicRegion()->getSystemWideFunctionVariantFlags();
        this->processorFunctionVariantFlags  = config.dyldCache.addr->dynamicRegion()->getProcessorFunctionVariantFlags();
    }
    else 
#endif
    {
        // no dyld cache, so evaluate in-process
        this->systemWideFunctionVariantFlags = ProcessConfig::evaluateSystemWideFunctionVariantFlags(config);
        this->processorFunctionVariantFlags  = ProcessConfig::evaluateProcessorSpecificFunctionVariantFlags(config);
    }
}

//
// MARK: --- Security methods ---
//

ProcessConfig::Security::Security(Process& process, SyscallDelegate& syscall)
{
#if TARGET_OS_EXCLAVEKIT
    this->internalInstall           = false; // FIXME
#else
    // TODO: audit usage of internalInstall and replace usage with isInternalOS which covers both device and simulator.
    this->internalInstall           = syscall.internalInstall(); // Note: internalInstall must be set before calling getAMFI()
    this->isInternalOS              = this->internalInstall || process.isInternalSimulator(syscall);
    this->skipMain                  = this->internalInstall && process.environ("DYLD_SKIP_MAIN");
    this->justBuildClosure          = process.environ("DYLD_JUST_BUILD_CLOSURE");
    this->lockdownMode              = syscall.inLockdownMode();

    // just on internal installs in launchd, dyld_flags= will alter the CommPage
    if ( (process.pid == 1) && this->internalInstall  ) {
        if ( const char* bootFlags = process.appleParam("dyld_flags") ) {
            *((uint32_t*)&process.commPage) = (uint32_t)hexToUInt64(bootFlags, nullptr);
        }
    }

    const uint64_t amfiFlags = getAMFI(process, syscall);
    this->allowAtPaths              = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_AT_PATH);
    this->allowEnvVarsPrint         = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_PRINT_VARS);
    this->allowEnvVarsPath          = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS);
    this->allowEnvVarsSharedCache   = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_CUSTOM_SHARED_CACHE);
    this->allowClassicFallbackPaths = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_FALLBACK_PATHS);
    this->allowInsertFailures       = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_FAILED_LIBRARY_INSERTION);
    this->allowInterposing          = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_LIBRARY_INTERPOSING);
    this->allowEmbeddedVars         = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_EMBEDDED_VARS);
    this->allowDevelopmentVars      = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_DEVELOPMENT_VARS);
    this->allowLibSystemOverrides   = (amfiFlags & AMFI_DYLD_OUTPUT_ALLOW_LIBSYSTEM_OVERRIDE);
#if TARGET_OS_SIMULATOR
    this->allowInsertFailures       = true; // FIXME: amfi is returning the wrong value for simulators <rdar://74025454>
#endif

    // DYLD_DLSYM_RESULT can be set by any main executable
    this->dlsymBlocked   = false;
    this->dlsymAbort     = false;
    this->dlsymAllowList = nullptr;
    process.mainExecutableHdr->forDyldEnv(^(const char* keyEqualValue, bool& stop) {
        if ( strncmp(keyEqualValue, "DYLD_DLSYM_RESULT=", 18) == 0 ) {
            if ( strcmp(&keyEqualValue[18], "null") == 0 ) {
                this->dlsymBlocked = true;
                this->dlsymAbort   = false;
            }
            else if ( strcmp(&keyEqualValue[18], "abort") == 0 ) {
                this->dlsymBlocked = true;
                this->dlsymAbort   = true;
            }
            else if ( strncmp(&keyEqualValue[18], "null-allow:", 11) == 0 ) {
                this->dlsymBlocked   = true;
                this->dlsymAbort     = false;
                this->dlsymAllowList = &keyEqualValue[29];
            }
            else if ( strncmp(&keyEqualValue[18], "abort-allow:", 12) == 0 ) {
                this->dlsymBlocked   = true;
                this->dlsymAbort     = true;
                this->dlsymAllowList = &keyEqualValue[30];
            }
            else if ( strncmp(&keyEqualValue[18], "allow:", 6) == 0 ) {
                // for logging of non-allowed symbols, yet nothing blocked
                this->dlsymBlocked   = false;
                this->dlsymAbort     = false;
                this->dlsymAllowList = &keyEqualValue[24];
            }
        }
    });

    // env vars are only pruned on macOS
    switch ( process.platform.value() ) {
        case PLATFORM_MACOS:
        case PLATFORM_IOSMAC:
        case PLATFORM_DRIVERKIT:
            break;
        default:
            return;
    }

    // env vars are only pruned when process is restricted
    if ( this->allowEnvVarsPrint || this->allowEnvVarsPath || this->allowEnvVarsSharedCache )
        return;

    this->pruneEnvVars(process);
#endif // !TARGET_OS_EXCLAVEKIT
}

#if !TARGET_OS_EXCLAVEKIT
uint64_t ProcessConfig::Security::getAMFI(const Process& proc, SyscallDelegate& sys)
{
    uint32_t fpTextOffset;
    uint32_t fpSize;
    uint64_t amfiFlags = sys.amfiFlags(proc.mainExecutableHdr->isRestricted(), proc.mainExecutableHdr->isFairPlayEncrypted(fpTextOffset, fpSize));

    // let DYLD_AMFI_FAKE override actual AMFI flags, but only on internalInstalls with boot-arg set
    bool testMode = proc.commPage.testMode;
    if ( const char* amfiFake = proc.environ("DYLD_AMFI_FAKE") ) {
        //console("env DYLD_AMFI_FAKE set, boot-args dyld_flags=%s\n", proc.appleParam("dyld_flags"));
        if ( !testMode ) {
            //console("env DYLD_AMFI_FAKE ignored because boot-args dyld_flags=2 is missing (%s)\n", proc.appleParam("dyld_flags"));
        }
        else if ( !this->internalInstall ) {
            //console("env DYLD_AMFI_FAKE ignored because not running on an Internal install\n");
        }
        else {
            amfiFlags = hexToUInt64(amfiFake, nullptr);
            //console("env DYLD_AMFI_FAKE parsed as 0x%08llX\n", amfiFlags);
       }
    }
    return amfiFlags;
}

void ProcessConfig::Security::pruneEnvVars(Process& proc)
{
    //
    // For security, setuid programs ignore DYLD_* environment variables.
    // Additionally, the DYLD_* enviroment variables are removed
    // from the environment, so that any child processes doesn't see them.
    //
    // delete all DYLD_* environment variables
    int          removedCount = 0;
    const char** d            = (const char**)proc.envp;
    for ( const char* const* s = proc.envp; *s != NULL; s++ ) {
        if ( strncmp(*s, "DYLD_", 5) != 0 ) {
            *d++ = *s;
        }
        else {
            ++removedCount;
        }
    }
    *d++ = NULL;
    // slide apple parameters
    if ( removedCount > 0 ) {
        proc.apple = d;
        do {
            *d = d[removedCount];
        } while ( *d++ != NULL );
        for ( int i = 0; i < removedCount; ++i )
            *d++ = NULL;
    }
}
#endif // !TARGET_OS_EXCLAVEKIT


//
// MARK: --- Logging methods ---
//

ProcessConfig::Logging::Logging(const Process& process, const Security& security, SyscallDelegate& syscall)
{
#if !TARGET_OS_EXCLAVEKIT
    this->segments       = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_SEGMENTS");
    this->libraries      = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_LIBRARIES");
    this->fixups         = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_BINDINGS");
    this->initializers   = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_INITIALIZERS");
    this->apis           = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_APIS");
    this->notifications  = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_NOTIFICATIONS");
    this->interposing    = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_INTERPOSING");
    this->loaders        = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_LOADERS");
    this->searching      = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_SEARCHING");
    this->env            = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_ENV");
    this->useStderr      = security.allowEnvVarsPrint && process.environ("DYLD_PRINT_TO_STDERR");
    this->descriptor     = STDERR_FILENO;
    this->useFile        = false;
    if ( security.allowEnvVarsPrint && security.allowEnvVarsSharedCache ) {
        if ( const char* path = process.environ("DYLD_PRINT_TO_FILE") ) {
            int fd = syscall.openLogFile(path);
            if ( fd != -1 ) {
                this->useFile    = true;
                this->descriptor = fd;
            }
        }
    }
    if ( security.allowEnvVarsPrint ) {
        if (const char* str = process.environ("DYLD_PRINT_LINKS_WITH") )
            this->linksWith = str;
    }
#else
    this->segments       = true;
    this->libraries      = true;
    this->fixups         = false;
    this->initializers   = true;
    this->apis           = true;
    this->notifications  = true;
    this->interposing    = true;
    this->loaders        = true;
    this->searching      = true;
    this->env            = true;
#endif
}



//
// MARK: --- DyldCache methods ---
//

#if !TARGET_OS_EXCLAVEKIT
static const char* getSystemCacheDir(Platform platform)
{
    if ( platform == Platform::driverKit )
        return DRIVERKIT_DYLD_SHARED_CACHE_DIR;

    // This is gross, but using defines is easier than trying to work out what to do when running
    // iOS apps on macOS
#if TARGET_OS_OSX
    return MACOSX_MRM_DYLD_SHARED_CACHE_DIR;
#else
    return IPHONE_DYLD_SHARED_CACHE_DIR;
#endif
}
// Shared caches may be found in the system cache dir, or an override
// via env vars, or a cryptex from libignition.  This works out which one
struct CacheFinder
{
    CacheFinder(const ProcessConfig::Process& process,
                const ProcessConfig::Logging& log,
                SyscallDelegate& syscall);
    ~CacheFinder();

    int cacheDirFD = -1;
    SyscallDelegate& syscall;

#if BUILDING_DYLD && SUPPORT_IGNITION
    ignition_payload_t ignitionPayload = IGNITION_PAYLOAD_INIT;
    bool usesIgnition = false;
    int ignitionRootFD = -1;
#endif
};

CacheFinder::CacheFinder(const ProcessConfig::Process& process,
                         const ProcessConfig::Logging& log,
                         SyscallDelegate& syscall)
    : syscall(syscall)
{
#if BUILDING_DYLD
    if ( const char* overrideDir = process.environ("DYLD_SHARED_CACHE_DIR") ) {
        this->cacheDirFD = this->syscall.open(overrideDir, O_RDONLY, 0);

        // Early return on invalid shared cache dir.
        if ( this->cacheDirFD == -1 ) {
            return;
        }
    }

    // Check libignition
#if SUPPORT_IGNITION
    if ( !process.commPage.skipIgnition ) {
#if IGNITION_PARAMETERS_STRUCT_VERSION >= 1
        ignition_parameters_t params = {
            IGNITION_PARAMETERS_STRUCT_VERSION,
            process.argc,
            (const char**)process.argv,
            (const char**)process.envp,
            (const char**)process.apple,
            -1,
            process.platform.value(),
        };
#else
        ignition_parameters_t params = {
            IGNITION_PARAMETERS_STRUCT_VERSION,
            process.argc,
            (const char**)process.argv,
            (const char**)process.envp,
            (const char**)process.apple,
            -1
        };
#endif // IGNITION_PARAMETERS_STRUCT_VERSION

        errno_t result = ignite(&params, &ignitionPayload);
        if ( result == 0 ) {
            if ( os_fd_valid(ignitionPayload.pl_shared_cache) ) {
                // Only use ignition shared cache if we don't have one set already.
                if ( this->cacheDirFD == -1  ) {
                    this->cacheDirFD = ignitionPayload.pl_shared_cache;
                    this->usesIgnition = true;
                } else {
                    // Manually close ignition cache fd since we won't use it.
                    this->syscall.close(ignitionPayload.pl_shared_cache);
                }
            }
            if ( os_fd_valid(ignitionPayload.pl_os_graft) ) {
                this->ignitionRootFD = ignitionPayload.pl_os_graft;
            }
        }
        else {
            if ( process.pid == 1 ) {
                dyld4::console("ignite() returned %d\n", result);
                switch ( result ) {
                    case ENOEXEC:
                        dyld4::console("ignition disabled\n");
                        break;
                    case EIDRM:
                        halt("ignition failed");
                        break;
                    case ECANCELED:
                        dyld4::console("ignition partially disabled\n");
                        break;
                    case ENODEV:
                        dyld4::console("no shared cache available\n");
                        break;
                    case EBADEXEC:
                        halt("no shared cache in cryptex");
                        break;
                    }
            }
            if (result == EBADEXEC ) {
                // This is fatal.  Should we terminate the process, or just run
                // without a cache?
                // For now, lets log it, and return with no cache dir FD
                // That means we
                if ( log.segments ) {
                    dyld4::console("ignite() returned %d\n", result);
                }
            }
        }

        // If we found a cache with ignition, we might prefer to use the system shared cache instead
        if ( this->usesIgnition ) {
            bool preferSystemCache = false;
            if ( process.platform == Platform::driverKit )
                preferSystemCache = process.commPage.useSystemDriverKitCache;
            else
                preferSystemCache = process.commPage.useSystemCache;

            if ( preferSystemCache ) {
                if ( this->cacheDirFD != -1 )
                    this->syscall.close(this->cacheDirFD);
                this->cacheDirFD = -1;
                this->usesIgnition = false;
            }
        }

        // If we found a cache with ignition, then use it.  Otherwise fall though
        // to the system default location
        if ( this->usesIgnition )
            return;
    }
#endif // SUPPORT_IGNITION

    if ( this->cacheDirFD != -1 )
        return;

    // Finally use the system path
    this->cacheDirFD = this->syscall.open(getSystemCacheDir(process.platform), O_RDONLY, 0);
#endif // BUILDING_DYLD
}

CacheFinder::~CacheFinder()
{
#if BUILDING_DYLD && SUPPORT_IGNITION
    if ( usesIgnition ) {
        // Let ignition close the FD
        auto* ignitionPayloadPtr = &this->ignitionPayload;
        jettison(&ignitionPayloadPtr);
        return;
    }
#endif // BUILDING_DYLD && SUPPORT_IGNITION

    if ( this->cacheDirFD != -1 ) {
        this->syscall.close(this->cacheDirFD);
    }
}
#endif // !TARGET_OS_EXCLAVEKIT


ProcessConfig::DyldCache::DyldCache(Process& process, const Security& security, const Logging& log, SyscallDelegate& syscall, Allocator& allocator, const ProcessConfig& config)
{
#if !TARGET_OS_EXCLAVEKIT
    bool forceCustomerCache = process.commPage.forceCustomerCache;
    bool forceDevCache      = process.commPage.forceDevCache;
#else
    bool forceCustomerCache = false;
    bool forceDevCache      = false;

    if ( !process.sharedCacheFileEnabled )
        return;
    // EK shared cache exists only on iOS and macOS
    if ( (process.platform != Platform::macOS_exclaveKit) && (process.platform != Platform::iOS_exclaveKit) )
        return;
#endif // !TARGET_OS_EXCLAVEKIT

#if !TARGET_OS_EXCLAVEKIT
    // Work out which directories to search for caches
    CacheFinder cacheFinder(process, log, syscall);

#if BUILDING_DYLD
    // in launchd commpage is not set up yet
    if ( process.pid == 1 ) {
        if ( security.internalInstall ) {
            // default to development cache for internal installs
            forceCustomerCache = false;
            if ( process.commPage.forceCustomerCache )
                forceCustomerCache = true;
            if ( process.commPage.forceDevCache ) {
                forceDevCache      = true;
                forceCustomerCache = false;
            }
        }
        else {
            // customer installs always get customer dyld cache
            forceCustomerCache = true;
            forceDevCache      = false;
        }
    }
#endif // BUILDING_DYLD

    // load dyld cache if needed
    const char*               cacheMode = process.environ("DYLD_SHARED_REGION");
#if TARGET_OS_SIMULATOR && __arm64__
    if ( cacheMode == nullptr ) {
        // A 2GB simulator app on Apple Silicon can overlay where the dyld cache is supposed to go
        // Luckily, simulators still have dylibs on disk, so we can run the process without a dyld cache
        // FIXME: Somehow get ARM64_SHARED_REGION_START = 0x180000000ULL
        if ( process.mainExecutableMF->intersectsRange(0x180000000ULL, 0x100000000ULL) ) {
            if ( log.segments )
                console("main executable resides where dyld cache would be, so not using a dyld cache\n");
            cacheMode = "avoid";
        }
    }
#endif
#endif // !TARGET_OS_EXCLAVEKIT

    dyld3::SharedCacheOptions opts;
#if !TARGET_OS_EXCLAVEKIT
    opts.cacheDirFD               = cacheFinder.cacheDirFD;
    opts.usePageInLinking         = (process.pageInLinkingMode >= 2) && !syscall.sandboxBlockedPageInLinking();
#else
    opts.cacheHeader              = (DyldSharedCache*)process.preMappedCache;
    opts.cacheSize                = process.preMappedCacheSize;
    opts.cachePath                = process.preMappedCachePath;
    opts.usePageInLinking         = process.sharedCachePageInLinking;
#endif // !TARGET_OS_EXCLAVEKIT

#if TARGET_OS_SIMULATOR || TARGET_OS_EXCLAVEKIT
    opts.forcePrivate             = true;
#else
    opts.forcePrivate             = security.allowEnvVarsSharedCache && (cacheMode != nullptr) && (strcmp(cacheMode, "private") == 0);
#endif // TARGET_OS_SIMULATOR || TARGET_OS_EXCLAVEKIT
    opts.verbose                  = log.segments;
#if TARGET_OS_OSX && BUILDING_DYLD
    // if this is host dyld about to switch to dyld_sim, suppress logging to avoid confusing double logging
    if ( opts.verbose && process.platform.isSimulator() )
        opts.verbose = false;
#endif
    opts.useHaswell               = syscall.onHaswell();
    opts.disableASLR              = false; // FIXME
    opts.enableReadOnlyDataConst  = process.enableDataConst;
    opts.enableTPRO               = process.defaultTproDataConst();
    opts.preferCustomerCache      = forceCustomerCache;
    opts.forceDevCache            = forceDevCache;
    opts.isTranslated             = process.isTranslated;
    opts.platform                 = process.platform;
    opts.config                   = &config;

    this->addr                    = nullptr;
#if SUPPORT_VM_LAYOUT
    this->slide                   = 0;
#endif
    this->unslidLoadAddress       = 0;
    this->rootsAreSupported       = true;
    this->dylibsExpectedOnDisk    = false;
    this->privateCache            = opts.forcePrivate;
    this->path                    = nullptr;
    this->objcHeaderInfoRO        = nullptr;
    this->objcHeaderInfoRW        = nullptr;
    this->objcSelectorHashTable   = nullptr;
    this->objcClassHashTable      = nullptr;
    this->objcProtocolHashTable   = nullptr;
    this->swiftCacheInfo          = nullptr;
    this->objcHeaderInfoROUnslidVMAddr = 0;
    this->objcProtocolClassCacheOffset = 0;
    this->platform                = Platform();
    this->osVersion               = 0;
    this->dylibCount              = 0;
#if !TARGET_OS_EXCLAVEKIT
    this->development             = false;
#else
    this->development             = true;
#endif // !TARGET_OS_EXCLAVEKIT

#if TARGET_OS_SIMULATOR
    // only support DYLD_SHARED_REGION=avoid on simulator
    if ( (cacheMode == nullptr) || (strcmp(cacheMode, "avoid") != 0) ) {
#endif
        dyld3::SharedCacheLoadInfo loadInfo;
        bool isSimHost = false;
#if TARGET_OS_OSX && BUILDING_DYLD
        isSimHost = process.platform.isSimulator();
#endif
        if ( !isSimHost ) {
            syscall.getDyldCache(opts, loadInfo);
        }

        if ( loadInfo.loadAddress != nullptr ) {
#if TARGET_OS_EXCLAVEKIT
            this->allowLibSystemOverrides = false;
#else
            this->allowLibSystemOverrides = security.allowLibSystemOverrides;
            this->mainFileID  = loadInfo.cacheFileID;
#endif // !TARGET_OS_EXCLAVEKIT
            this->addr        = loadInfo.loadAddress;
            this->development = loadInfo.development;
            this->dylibsExpectedOnDisk  = this->addr->header.dylibsExpectedOnDisk;

            // All of the following are manually set by the cache builder prior to building loaders
            // The cache builder won't use the calls here to set any initial values
#if SUPPORT_VM_LAYOUT
            this->slide     = loadInfo.slide;
            this->setPlatformOSVersion(process);

            this->unslidLoadAddress     = this->addr->unslidLoadAddress();
            this->objcHeaderInfoRO      = this->addr->objcHeaderInfoRO();
            this->objcHeaderInfoRW      = this->addr->objcHeaderInfoRW();
            this->objcSelectorHashTable = this->addr->objcSelectorHashTable();
            this->objcClassHashTable    = this->addr->objcClassHashTable();
            this->objcProtocolHashTable = this->addr->objcProtocolHashTable();
            this->swiftCacheInfo        = this->addr->swiftOpt();
            this->dylibCount            = this->addr->imagesCount();

            this->objcHeaderInfoROUnslidVMAddr = 0;
            if ( this->objcHeaderInfoRO != nullptr ) {
                uint64_t offsetInCache = (uint64_t)this->objcHeaderInfoRO - (uint64_t)this->addr;
                this->objcHeaderInfoROUnslidVMAddr = this->unslidLoadAddress + offsetInCache;
            }

            // In the cache builder, we'll set this manually before building Loader's.
            // In dyld at runtime, we'll calculate it lazily in PreBuiltObjC if we need it
            this->objcProtocolClassCacheOffset = 0;

            this->patchTable = PatchTable(this->addr->patchTable(), this->addr->header.patchInfoAddr);

#if !TARGET_OS_EXCLAVEKIT
            // The shared cache is mapped with RO __DATA_CONST, but this
            // process might need RW
            if ( !opts.enableReadOnlyDataConst )
                makeDataConstWritable(log, syscall, true);

            // If 'dyld_hw_tpro' is not set, then the shared cache for this process needs to use
            // mprotect and not TPRO, when changing its state for the TPRO_CONST segment specifically.
            // As ProcessConfig is constructed inside a withWriteableMemory block, we need to now make
            // the cache TPRO_CONST writable to match the expectations of the caller with that block
            if ( !process.defaultTproHW() ) {
                this->addr->forEachTPRORegion(^(const void *content, uint64_t unslidVMAddr, uint64_t vmSize, bool &stopRegion) {
                    void* regionBaseAddr = (void*)(unslidVMAddr + this->slide);
                    kern_return_t kr = ::vm_protect(mach_task_self(), (vm_address_t)regionBaseAddr, (vm_size_t)vmSize, false, VM_PROT_WRITE | VM_PROT_READ | VM_PROT_COPY);
                    if (kr != KERN_SUCCESS) {
                         // fprintf(stderr, "FAILED: %d", kr);
                    }
                });
            }
#endif // !TARGET_OS_EXCLAVEKIT
#endif // SUPPORT_VM_LAYOUT

#if !TARGET_OS_EXCLAVEKIT
#if BUILDING_DYLD
#if TARGET_OS_OSX
            // On macOS, we scan for roots at boot.  This is only done in PID 1, so we can only use
            // this result on the shared cache mapped at that point, not driverKit/Rosetta
            if ( !process.commPage.bootVolumeWritable
                && !process.commPage.foundRoot
                && !process.isTranslated ) {
                if ( (process.platform == Platform::macOS) || (process.platform == Platform::macCatalyst) ) {
                    // FIXME: Remove the development hack here and rely just on the roots variable
                    this->development = false;

                    this->rootsAreSupported = false;
                }
            }
#else
            // On embedded, assume no roots are supported on customer caches.  We'll later override this if
            // we detect env vars which force roots support
            if ( !this->development )
                this->rootsAreSupported = false;
#endif // TARGET_OS_OSX
#endif // BUILDING_DYLD

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_SHARED_CACHE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS
            this->path = allocator.strdup(getSystemCacheDir(process.platform));
#else
            if ( loadInfo.cacheFileID ) {
                char pathFromIDs[MAXPATHLEN];
                if ( loadInfo.cacheFileID.getPath(pathFromIDs) ) {
                    this->path = allocator.strdup(pathFromIDs);
                }
            } else {
#if BUILDING_DYLD
                halt("dyld shared region dynamic config data was not set\n");
#elif BUILDING_UNIT_TESTS
                // Not quite right, but hopefully close enough for what the unit tests need
                this->path = allocator.strdup(getSystemCacheDir(process.platform));
#else
                abort();
#endif
            }
#endif
#endif // !TARGET_OS_EXCLAVEKIT
        }
        else {
#if !TARGET_OS_EXCLAVEKIT
#if BUILDING_DYLD && !TARGET_OS_SIMULATOR
            // <rdar://74102798> log all shared cache errors except no cache file
            if ( loadInfo.cacheFileFound )
                console("dyld cache '%s' not loaded: %s\n", this->path, loadInfo.errorMessage);

            if ( cacheMode != nullptr ) {
                if ( strcmp(cacheMode, "private") == 0 && !loadInfo.cacheFileFound)
                    halt("dyld private shared cache could not be found\n");
            }
#endif // BUILDING_DYLD && !TARGET_OS_SIMULATOR
#else
            halt("dyld shared cache could not be mapped\n");
#endif // !TARGET_OS_EXCLAVEKIT
        }
#if TARGET_OS_SIMULATOR
    }
#endif

#if !TARGET_OS_EXCLAVEKIT
#if BUILDING_DYLD && SUPPORT_IGNITION
    if ( cacheFinder.ignitionRootFD != -1 ) {
        char buffer[PATH_MAX];
        if ( syscall.getpath(cacheFinder.ignitionRootFD, buffer) )
            this->cryptexOSPath = allocator.strdup(buffer);
    }
#endif // BUILDING_DYLD && SUPPORT_IGNITION

#if BUILDING_DYLD
    // in launchd we set up the dyld comm-page bits
    if ( process.pid == 1 )
#endif
        this->setupDyldCommPage(process, security, syscall);
#endif // !TARGET_OS_EXCLAVEKIT
}

void ProcessConfig::DyldCache::setPlatformOSVersion(const Process& proc)
{
    // new caches have OS version recorded
    if ( addr->header.mappingOffset >= 0x170 ) {
        // decide if process is using main platform or alternate platform
        if ( proc.platform == addr->header.platform ) {
            this->platform  = addr->header.platform;
            this->osVersion = addr->header.osVersion;
        }
        else {
            this->platform  = addr->header.altPlatform;
            this->osVersion = addr->header.altOsVersion;;
        }
    }
    else {
        // for older caches, need to find and inspect libdyld.dylib
        const char* libdyldPath = (proc.platform == Platform::driverKit) ? "/System/DriverKit/usr/lib/system/libdyld.dylib" : "/usr/lib/system/libdyld.dylib";
        if ( const Header* libdyldMH = this->addr->getImageFromPath(libdyldPath) ) {
            libdyldMH->platformAndVersions().unzip(^(mach_o::PlatformAndVersions pvs) {
                if ( pvs.platform == proc.platform ) {
                    this->platform  = pvs.platform;
                    this->osVersion = pvs.minOS.value();
                }
                else if ( (pvs.platform == Platform::macCatalyst) && proc.catalystRuntime ) {
                    // support iPad apps running on Apple Silicon
                    this->platform  = pvs.platform;
                    this->osVersion = pvs.minOS.value();
                }
            });
        }
        else {
            console("initializeCachePlatformOSVersion(): libdyld.dylib not found for OS version info\n");
        }
     }
}

#if !TARGET_OS_EXCLAVEKIT
#if SUPPORT_VM_LAYOUT
bool ProcessConfig::DyldCache::uuidOfFileMatchesDyldCache(const Process& proc, const SyscallDelegate& sys, const char* dylibPath) const
{
    // getLoader is going to find the path in the OS cryptex. We need to remove that prefix here, as the cache doesn't contain
    // cryptex paths
    std::string_view installName(dylibPath);
    if ( !this->cryptexOSPath.empty() ) {
        if ( installName.starts_with(this->cryptexOSPath) ) {
            installName.remove_prefix(this->cryptexOSPath.size());
        }
    }
    // get UUID of dylib in cache
    if ( const Header* cacheMH = this->addr->getImageFromPath(installName.data()) ) {
        uuid_t cacheUUID;
        if ( !cacheMH->getUuid(cacheUUID) )
            return false;

        // get UUID of file on disk
        uuid_t              diskUUID;
        uint8_t*            diskUUIDPtr   = diskUUID; // work around compiler bug with arrays and __block
        __block bool        diskUuidFound = false;
        __block Diagnostics diag;
        sys.withReadOnlyMappedFile(diag, dylibPath, false, ^(const void* mapping, size_t mappedSize, bool isOSBinary, const FileID& fileID, const char* canonicalPath, int fileDescriptor) {
            uint64_t sliceOffset = 0;
            uint64_t sliceSize = 0;
            if ( const Header* diskMH = (const Header*)MachOFile::compatibleSlice(diag, sliceOffset, sliceSize, mapping, mappedSize, dylibPath, proc.platform, isOSBinary, *proc.archs) ) {
                diskUuidFound = diskMH->getUuid(diskUUIDPtr);
            }
        });
        if ( !diskUuidFound )
            return false;

        return (::memcmp(diskUUID, cacheUUID, sizeof(uuid_t)) == 0);
    }
    return false;
}

#endif // SUPPORT_VM_LAYOUT

#if TARGET_OS_OSX && SUPPORT_VM_LAYOUT
// FIXME: Move to StringUtils and remove other copies
static void concatenatePaths(char *path, const char *suffix, size_t pathsize)
{
    if ( (path[strlen(path) - 1] == '/') && (suffix[0] == '/') )
        strlcat(path, &suffix[1], pathsize); // avoid double slash when combining path
    else
        strlcat(path, suffix, pathsize);
}
#endif

void ProcessConfig::DyldCache::setupDyldCommPage(Process& proc, const Security& sec, SyscallDelegate& sys)
{
#if !TARGET_OS_SIMULATOR
    // in launchd we compute the comm-page flags we want and set them for other processes to read
    proc.commPage.bootVolumeWritable = sys.bootVolumeWritable();
    // just in case, force these flags off for customer installs
    if ( !sec.internalInstall ) {
        proc.commPage.forceCustomerCache = true;
        proc.commPage.testMode           = false;
        proc.commPage.forceDevCache      = false;
        proc.commPage.bootVolumeWritable = false;
        proc.commPage.foundRoot          = false;
        proc.commPage.logRoots           = false;
    }
#endif

#if TARGET_OS_OSX && SUPPORT_VM_LAYOUT
    // on macOS, three dylibs under libsystem are on disk but may need to be ignored
    if ( this->addr != nullptr ) {
        auto uuidMatchesDylCache = ^(const char* dylibPath) {
            if ( !this->uuidOfFileMatchesDyldCache(proc, sys, dylibPath) )
                return false;

            // Also check the cryptex
            if ( !this->cryptexOSPath.empty() ) {
                char pathBuffer[PATH_MAX] = { 0 };
                strlcpy(pathBuffer, this->cryptexOSPath.data(), sizeof(pathBuffer));
                concatenatePaths(pathBuffer, dylibPath, sizeof(pathBuffer));
                if ( !this->uuidOfFileMatchesDyldCache(proc, sys, pathBuffer) )
                    return false;
            }

            return true;
        };
        proc.commPage.libKernelRoot   = !uuidMatchesDylCache("/usr/lib/system/libsystem_kernel.dylib");
        proc.commPage.libPlatformRoot = !uuidMatchesDylCache("/usr/lib/system/libsystem_platform.dylib");
        proc.commPage.libPthreadRoot  = !uuidMatchesDylCache("/usr/lib/system/libsystem_pthread.dylib");

        // If this prints any "true" value, then dyld will need to stat for roots at runtime
        // That is (false, false, false, false) means no roots, so we take the fast path at run time
#if BUILDING_DYLD
        dyld4::console("dyld: simulator status (/ rw: %s; kernel: %s, platform: %s; pthread: %s\n",
                       proc.commPage.bootVolumeWritable ? "true" : "false",
                       proc.commPage.libKernelRoot ? "true" : "false",
                       proc.commPage.libPlatformRoot ? "true" : "false",
                       proc.commPage.libPthreadRoot ? "true" : "false");
#endif
    }
#endif

    sys.setDyldCommPageFlags(proc.commPage);
}
#endif // !TARGET_OS_EXCLAVEKIT

bool ProcessConfig::DyldCache::indexOfPath(const char* dylibPath, uint32_t& dylibIndex) const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // The cache builder doesn't have a real cache, and instead uses the list of dylibs
    assert(!cacheBuilderDylibs->empty());
    for ( uint32_t i = 0; i != cacheBuilderDylibs->size(); ++i ) {
        const CacheDylib& cacheDylib = (*cacheBuilderDylibs)[i];
        if ( !strcmp(((Header*)cacheDylib.mf)->installName(), dylibPath) ) {
            dylibIndex = i;
            return true;
        }
    }
    return false;
#else
    if ( this->addr == nullptr )
        return false;
    return this->addr->hasImagePath(dylibPath, dylibIndex);
#endif
}

bool ProcessConfig::DyldCache::findMachHeaderImageIndex(const mach_header* mh, uint32_t& imageIndex) const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // The cache builder doesn't have a real cache, and instead uses the list of dylibs
    assert(!cacheBuilderDylibs->empty());
    for ( uint32_t i = 0; i != cacheBuilderDylibs->size(); ++i ) {
        const CacheDylib& cacheDylib = (*cacheBuilderDylibs)[i];
        if ( cacheDylib.mf == mh ) {
            imageIndex = i;
            return true;
        }
    }
    assert("Unknown dylib");
    return false;
#elif !TARGET_OS_EXCLAVEKIT
    return this->addr->findMachHeaderImageIndex(mh, imageIndex);
#else
    return false;
#endif
}


#if SUPPORT_VM_LAYOUT
void ProcessConfig::DyldCache::makeDataConstWritable(const Logging& lg, const SyscallDelegate& sys, bool writable) const
{
#if !TARGET_OS_EXCLAVEKIT
    const uint32_t perms = (writable ? VM_PROT_WRITE | VM_PROT_READ | VM_PROT_COPY : VM_PROT_READ);
    addr->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
        cache->forEachRegion(^(const void*, uint64_t vmAddr, uint64_t size, uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
            // Skip TPRO const until we can move this in to the MemoryManager to share state there
            // FIXME: Move to MemoryManager if needed, or make sure we never have weak-defs or binds to patch in __TPRO_CONST.
            if ( flags & DYLD_CACHE_MAPPING_CONST_TPRO_DATA )
                return;

            void* content = (void*)(vmAddr + slide);
            if ( flags & DYLD_CACHE_MAPPING_CONST_DATA ) {
                if ( lg.segments )
                    console("marking shared cache range 0x%x permissions: 0x%09lX -> 0x%09lX\n", perms, (long)content, (long)content + (long)size);
                kern_return_t result = sys.vm_protect(mach_task_self(), (vm_address_t)content, (vm_size_t)size, false, perms);
                if ( result != KERN_SUCCESS ) {
                    if ( lg.segments )
                        console("failed to mprotect shared cache due to: %d\n", result);
                }
            }
        });
    });
#else
    // cL4 does not allow for permissions to be dowgraded.
    // So dyld maps DATA_CONST as writable until the end of the startup process.
#endif // !TARGET_OS_EXCLAVEKIT
}
#endif // SUPPORT_VM_LAYOUT

bool ProcessConfig::DyldCache::isAlwaysOverridablePath(const char* dylibPath)
{
    return strcmp(dylibPath, "/usr/lib/system/libdispatch.dylib") == 0;
}

bool ProcessConfig::DyldCache::isProtectedLibSystemPath(const char* dylibPath)
{
    static constinit const char* protectedPaths[] = {
        "/usr/lib/libSystem.B.dylib",
        "/usr/lib/system/libsystem_secinit.dylib",
        "/usr/lib/system/libsystem_sandbox.dylib",
    };
    for ( const char* ppath : protectedPaths ) {
        if ( strcmp(dylibPath, ppath) == 0 )
            return true;
    }
    return false;
}

bool ProcessConfig::DyldCache::isOverridablePath(const char* dylibPath) const
{
    if ( this->development ) {
#if TARGET_OS_OSX && BUILDING_DYLD
        // rdar://121937452 (hardened apps cannot have libSystem overridden)
        if ( !this->allowLibSystemOverrides && DyldCache::isProtectedLibSystemPath(dylibPath) )
            return false;
#endif
        return true;
    }

    return DyldCache::isAlwaysOverridablePath(dylibPath);
}

const char* ProcessConfig::DyldCache::getCanonicalPath(const char *dylibPath) const
{
    uint32_t dyldCacheImageIndex;
    if ( this->indexOfPath(dylibPath, dyldCacheImageIndex) )
        return this->getIndexedImagePath(dyldCacheImageIndex);
    return nullptr;
}

const char* ProcessConfig::DyldCache::getIndexedImagePath(uint32_t dylibIndex) const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // The cache builder doesn't have a real cache, and instead uses the list of dylibs
    assert(!cacheBuilderDylibs->empty());
    const CacheDylib& cacheDylib = (*cacheBuilderDylibs)[dylibIndex];
    return ((Header*)cacheDylib.mf)->installName();
#else
    return this->addr->getIndexedImagePath(dylibIndex);
#endif
}

const dyld3::MachOFile* ProcessConfig::DyldCache::getIndexedImageEntry(uint32_t dylibIndex,
                                                                       uint64_t& mTime, uint64_t& inode) const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // The cache builder doesn't have a real cache, and instead uses the list of dylibs
    assert(!cacheBuilderDylibs->empty());
    const CacheDylib& cacheDylib = (*cacheBuilderDylibs)[dylibIndex];
    mTime = cacheDylib.mTime;
    inode = cacheDylib.inode;
    return cacheDylib.mf;
#else
    return (const dyld3::MachOFile*)this->addr->getIndexedImageEntry(dylibIndex, mTime, inode);
#endif
}

void ProcessConfig::DyldCache::adjustRootsSupportForEnvVars()
{
#if TARGET_OS_OSX && BUILDING_DYLD
    // FIXME: Remove the development hack here and rely just on the roots variable
    this->development = true;
#endif

#if BUILDING_DYLD
    // Env vars imply that we need to look for roots. The kind of cache will determine which paths can be roots
    this->rootsAreSupported = true;
#endif //BUILDING_DYLD
}

//
// MARK: --- PathOverrides methods ---
//

ProcessConfig::PathOverrides::PathOverrides(const Process& process, const Security& security, const Logging& log, const DyldCache& cache, SyscallDelegate& syscall, Allocator& allocator)
{
#if !TARGET_OS_EXCLAVEKIT
    // set fallback path mode
    _fallbackPathMode = security.allowClassicFallbackPaths ? FallbackPathMode::classic : FallbackPathMode::restricted;

    // process DYLD_* env variables if allowed
    if ( security.allowEnvVarsPath ) {
        char crashMsg[2048];
        strlcpy(crashMsg, "dyld config: ", sizeof(crashMsg));
        for (const char* const* p = process.envp; *p != nullptr; ++p) {
            this->addEnvVar(process, security, allocator, *p, false, crashMsg);
        }
        if ( strlen(crashMsg) > 15 ) {
            // if there is a crash, have DYLD_ env vars show up in crash log as secondary string
            // main string is missing symbol/dylib message
            CRSetCrashLogMessage2(allocator.strdup(crashMsg));
        }
    }
    else if ( log.searching ) {
        bool hasDyldEnvVars = false;
        for (const char* const* p = process.envp; *p != nullptr; ++p) {
            if ( strncmp(*p, "DYLD_", 5) == 0 )
                hasDyldEnvVars = true;
        }
        if ( hasDyldEnvVars )
            console("Note: DYLD_*_PATH env vars disabled by AMFI\n");
    }

    // process LC_DYLD_ENVIRONMENT variables if allowed
    if ( security.allowEmbeddedVars ) {
        process.mainExecutableHdr->forDyldEnv(^(const char* keyEqualValue, bool& stop) {
            this->addEnvVar(process, security, allocator, keyEqualValue, true, nullptr);
        });
    }
    else if ( log.searching ) {
        __block bool hasDyldEnvVars = false;
        process.mainExecutableHdr->forDyldEnv(^(const char* keyEqualValue, bool& stop) {
            hasDyldEnvVars = true;
            stop = true;
        });
        if ( hasDyldEnvVars )
            console("Note: LC_DYLD_ENVIRONMENT env vars disabled by AMFI\n");
    }

    if ( !cache.cryptexOSPath.empty() )
        this->_cryptexRootPath = cache.cryptexOSPath.data();

    // process DYLD_VERSIONED_* env vars
    this->processVersionedPaths(process, syscall, cache, process.platform, *process.archs, allocator);
#endif // !TARGET_OS_EXCLAVEKIT
}

#if !TARGET_OS_EXCLAVEKIT
void ProcessConfig::PathOverrides::checkVersionedPath(SyscallDelegate& sys, const DyldCache& cache, Allocator& allocator, const char* path, Platform platform, const GradedArchs& archs)
{
    static bool verbose = false;
    if (verbose) console("checkVersionedPath(%s)\n", path);
    uint32_t foundDylibVersion;
    char     foundDylibTargetOverridePath[PATH_MAX];
    if ( sys.getDylibInfo(path, platform, archs, foundDylibVersion, foundDylibTargetOverridePath) ) {
        if (verbose) console("   dylib vers=0x%08X (%s)\n", foundDylibVersion, path);
        uint32_t targetDylibVersion;
        uint32_t dylibIndex;
        char     targetInstallName[PATH_MAX];
        if (verbose) console("   look for OS dylib at %s\n", foundDylibTargetOverridePath);
        bool foundOSdylib = false;
        if ( sys.getDylibInfo(foundDylibTargetOverridePath, platform, archs, targetDylibVersion, targetInstallName) ) {
            foundOSdylib = true;
        }
        else if ( cache.indexOfPath(foundDylibTargetOverridePath, dylibIndex) )  {
            uint64_t unusedMTime = 0;
            uint64_t unusedINode = 0;
            const Header* cacheHdr = (const Header*)cache.getIndexedImageEntry(dylibIndex, unusedMTime, unusedINode);
            const char* dylibInstallName;
            Version32   compatVersion;
            Version32   currentVersion;
            if ( cacheHdr->getDylibInstallName(&dylibInstallName, &compatVersion, &currentVersion) ) {
                strlcpy(targetInstallName, dylibInstallName, PATH_MAX);
                foundOSdylib = true;
                targetDylibVersion = currentVersion.value();
            }
        }
        if ( foundOSdylib ) {
            if (verbose) console("   os dylib vers=0x%08X (%s)\n", targetDylibVersion, foundDylibTargetOverridePath);
            if ( foundDylibVersion > targetDylibVersion ) {
                // check if there already is an override path
                bool add = true;
                for (DylibOverride* existing=_versionedOverrides; existing != nullptr; existing=existing->next) {
                    if ( strcmp(existing->installName, targetInstallName) == 0 ) {
                        add = false; // already have an entry, don't add another
                        uint32_t previousDylibVersion;
                        char     previousInstallName[PATH_MAX];
                        if ( sys.getDylibInfo(existing->overridePath, platform, archs, previousDylibVersion, previousInstallName) ) {
                            // if already found an override and its version is greater that this one, don't add this one
                            if ( foundDylibVersion > previousDylibVersion ) {
                                existing->overridePath = allocator.strdup(path);
                                if (verbose) console("  override: alter to %s with: %s\n", targetInstallName, path);
                            }
                        }
                        break;
                    }
                }
                if ( add ) {
                    //console("  override: %s with: %s\n", installName, overridePath);
                    addPathOverride(allocator, targetInstallName, path);
                }
            }
        }
        else {
            // <rdar://problem/53215116> DYLD_VERSIONED_LIBRARY_PATH fails to load a dylib if it does not also exist at the system install path
            addPathOverride(allocator, foundDylibTargetOverridePath, path);
        }
    }
}

void ProcessConfig::PathOverrides::addPathOverride(Allocator& allocator, const char* installName, const char* overridePath)
{
    DylibOverride* newElement = (DylibOverride*)allocator.malloc(sizeof(DylibOverride));
    newElement->next         = nullptr;
    newElement->installName  = allocator.strdup(installName);
    newElement->overridePath = allocator.strdup(overridePath);
    // add to end of linked list
    if ( _versionedOverrides != nullptr )  {
        DylibOverride* last = _versionedOverrides;
        while ( last->next != nullptr )
            last = last->next;
        last->next = newElement;
    }
    else {
        _versionedOverrides = newElement;
    }
}

void ProcessConfig::PathOverrides::processVersionedPaths(const Process& proc, SyscallDelegate& sys, const DyldCache& cache, Platform platform,
                                                         const GradedArchs& archs, Allocator& allocator)
{
    // check DYLD_VERSIONED_LIBRARY_PATH for dylib overrides
    __block bool stop = false;
    if ( (_versionedDylibPathsEnv != nullptr) || (_versionedDylibPathExeLC != nullptr) ) {
        forEachInColonList(_versionedDylibPathsEnv, _versionedDylibPathExeLC, stop, ^(const char* searchDir, bool&) {
            sys.forEachInDirectory(searchDir, false, ^(const char* pathInDir, const char* leafName) {
                this->checkVersionedPath(sys, cache, allocator, pathInDir, platform, archs);
            });
        });
    }
    // check DYLD_VERSIONED_FRAMEWORK_PATH for framework overrides
    if ( (_versionedFrameworkPathsEnv != nullptr) || (_versionedFrameworkPathExeLC != nullptr) ) {
        forEachInColonList(_versionedFrameworkPathsEnv, _versionedFrameworkPathExeLC, stop, ^(const char* searchDir, bool&) {
            sys.forEachInDirectory(searchDir, true, ^(const char* pathInDir, const char* leafName) {
                // ignore paths that don't end in ".framework"
                size_t pathInDirLen = strlen(pathInDir);
                if ( (pathInDirLen < 10) || (strcmp(&pathInDir[pathInDirLen-10], ".framework") != 0)  )
                    return;
                // make ..path/Foo.framework/Foo
                char possibleFramework[PATH_MAX];
                strlcpy(possibleFramework, pathInDir, PATH_MAX);
                strlcat(possibleFramework, strrchr(pathInDir, '/'), PATH_MAX);
                *strrchr(possibleFramework, '.') = '\0';
                this->checkVersionedPath(sys, cache, allocator, possibleFramework, platform, archs);
            });
        });
    }
}


void ProcessConfig::PathOverrides::forEachInsertedDylib(void (^handler)(const char* dylibPath, bool&)) const
{
    __block bool stop = false;
    if ( _insertedDylibs != nullptr && _insertedDylibs[0] != '\0' ) {
        forEachInColonList(_insertedDylibs, nullptr, stop, ^(const char* path, bool&) {
            handler(path, stop);
        });
    }
}

void ProcessConfig::PathOverrides::handleEnvVar(const char* key, const char* value, void (^handler)(const char* envVar)) const
{
    if ( value == nullptr )
        return;
    size_t allocSize = strlen(key) + strlen(value) + 2;
    char buffer[allocSize];
    strlcpy(buffer, key, allocSize);
    strlcat(buffer, "=", allocSize);
    strlcat(buffer, value, allocSize);
    handler(buffer);
}

// Note, this method only returns variables set on the environment, not those from the load command
void ProcessConfig::PathOverrides::forEachEnvVar(void (^handler)(const char* envVar)) const
{
    handleEnvVar("DYLD_LIBRARY_PATH",             _dylibPathOverridesEnv,        handler);
    handleEnvVar("DYLD_FRAMEWORK_PATH",           _frameworkPathOverridesEnv,    handler);
    handleEnvVar("DYLD_FALLBACK_FRAMEWORK_PATH",  _frameworkPathFallbacksEnv,    handler);
    handleEnvVar("DYLD_FALLBACK_LIBRARY_PATH",    _dylibPathFallbacksEnv,        handler);
    handleEnvVar("DYLD_VERSIONED_FRAMEWORK_PATH", _versionedFrameworkPathsEnv,   handler);
    handleEnvVar("DYLD_VERSIONED_LIBRARY_PATH",   _versionedDylibPathsEnv,       handler);
    handleEnvVar("DYLD_INSERT_LIBRARIES",         _insertedDylibs,               handler);
    handleEnvVar("DYLD_IMAGE_SUFFIX",             _imageSuffix,                  handler);
    handleEnvVar("DYLD_ROOT_PATH",                _simRootPath,                  handler);
}

// Note, this method only returns variables set in the main executable's load command, not those from the environment
void ProcessConfig::PathOverrides::forEachExecutableEnvVar(void (^handler)(const char* envVar)) const
{
    handleEnvVar("DYLD_LIBRARY_PATH",             _dylibPathOverridesExeLC,        handler);
    handleEnvVar("DYLD_FRAMEWORK_PATH",           _frameworkPathOverridesExeLC,    handler);
    handleEnvVar("DYLD_FALLBACK_FRAMEWORK_PATH",  _frameworkPathFallbacksExeLC,    handler);
    handleEnvVar("DYLD_FALLBACK_LIBRARY_PATH",    _dylibPathFallbacksExeLC,        handler);
    handleEnvVar("DYLD_VERSIONED_FRAMEWORK_PATH", _versionedFrameworkPathExeLC,    handler);
    handleEnvVar("DYLD_VERSIONED_LIBRARY_PATH",   _versionedDylibPathExeLC,        handler);
}

void ProcessConfig::PathOverrides::setString(Allocator& allocator, const char*& var, const char* value)
{
    // ivar not set, just set to copy of string
    if ( var == nullptr ) {
        var = allocator.strdup(value);
        return;
    }
    // ivar already in use, build new appended string
    char tmp[strlen(var)+strlen(value)+2];
    strcpy(tmp, var);
    strcat(tmp, ":");
    strcat(tmp, value);
    var = allocator.strdup(tmp);
}

void ProcessConfig::PathOverrides::addEnvVar(const Process& proc, const Security& sec, Allocator& allocator,
                                             const char* keyEqualsValue, bool isLC_DYLD_ENV, char* crashMsg)
{
    // We have to make a copy of the env vars because the dyld semantics
    // is that the env vars are only looked at once at launch.
    // That is, using setenv() at runtime does not change dyld behavior.
    if ( const char* equals = ::strchr(keyEqualsValue, '=') ) {
        const char*      value = equals+1;
        std::string_view key(keyEqualsValue, value-keyEqualsValue-1);
        if ( !key.starts_with("DYLD_") )
            return;
        if ( isLC_DYLD_ENV && (strchr(value, '@') != nullptr) ) {
            const size_t bufferSize = PATH_MAX+strlen(keyEqualsValue); // value may contain multiple paths
            char         buffer[bufferSize];
            char*        expandedPaths = buffer; // compiler does not let you use arrays inside blocks ;-(
            __block bool needColon = false;
            buffer[0] = '\0';
            __block bool stop = false;
            forEachInColonList(value, nullptr, stop, ^(const char* aValue, bool&) {
                if ( !sec.allowAtPaths && (aValue[0] == '@') )
                    return;
                if ( needColon )
                    ::strlcat(expandedPaths, ":", bufferSize);
                if ( strncmp(aValue, "@executable_path/", 17) == 0 ) {
                    ::strlcat(expandedPaths, proc.mainExecutablePath, bufferSize);
                    if ( char* lastSlash = ::strrchr(expandedPaths, '/') ) {
                        size_t offset = lastSlash+1-expandedPaths;
                        ::strlcpy(&expandedPaths[offset], &aValue[17], bufferSize-offset);
                        needColon = true;
                    }
                }
                else if ( strncmp(aValue, "@loader_path/", 13) == 0 ) {
                    ::strlcat(expandedPaths, proc.mainExecutablePath, bufferSize);
                    if ( char* lastSlash = ::strrchr(expandedPaths, '/') ) {
                        size_t offset = lastSlash+1-expandedPaths;
                        ::strlcpy(&expandedPaths[offset], &aValue[13], bufferSize-offset);
                         needColon = true;
                   }
                }
                else {
                    ::strlcpy(expandedPaths, proc.mainExecutablePath, bufferSize);
                    needColon = true;
                }
            });
            value = allocator.strdup(expandedPaths);
        }
        if ( key == "DYLD_LIBRARY_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _dylibPathOverridesExeLC : _dylibPathOverridesEnv, value);
        }
        else if ( key == "DYLD_FRAMEWORK_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _frameworkPathOverridesExeLC : _frameworkPathOverridesEnv, value);
        }
        else if ( key == "DYLD_FALLBACK_FRAMEWORK_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _frameworkPathFallbacksExeLC : _frameworkPathFallbacksEnv, value);
        }
        else if ( key == "DYLD_FALLBACK_LIBRARY_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _dylibPathFallbacksExeLC : _dylibPathFallbacksEnv, value);
        }
        else if ( key == "DYLD_VERSIONED_FRAMEWORK_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _versionedFrameworkPathExeLC : _versionedFrameworkPathsEnv, value);
        }
        else if ( key == "DYLD_VERSIONED_LIBRARY_PATH" ) {
            setString(allocator, isLC_DYLD_ENV ? _versionedDylibPathExeLC : _versionedDylibPathsEnv, value);
        }
        else if ( key == "DYLD_INSERT_LIBRARIES" ) {
            setString(allocator, _insertedDylibs, value);
            if ( _insertedDylibs[0] != '\0' ) {
                _insertedDylibCount = 1;
                for (const char* s=_insertedDylibs; *s != '\0'; ++s) {
                    if ( *s == ':' )
                        _insertedDylibCount++;
                }
            }
        }
        else if ( key == "DYLD_IMAGE_SUFFIX" ) {
            setString(allocator, _imageSuffix, value);
        }
        else if ( (key == "DYLD_ROOT_PATH") && proc.platform.isSimulator() ) {
            setString(allocator, _simRootPath, value);
        }
        if ( (crashMsg != nullptr) && (strncmp(keyEqualsValue, "DYLD_", 5) == 0) ) {
            strlcat(crashMsg, keyEqualsValue, 2048);
            strlcat(crashMsg, " ", 2048);
        }
    }
}
#endif // !TARGET_OS_EXCLAVEKIT


void ProcessConfig::PathOverrides::forEachInColonList(const char* list1, const char* list2, bool& stop, void (^handler)(const char* path, bool&))
{
    for (const char* list : { list1, list2 }) {
        if (list == nullptr)
            continue;
        char buffer[strlen(list)+1];
        const char* t = list;
        for (const char* s=list; *s != '\0'; ++s) {
            if (*s != ':')
                continue;
            size_t len = s - t;
            memcpy(buffer, t, len);
            buffer[len] = '\0';
            handler(buffer, stop);
            if ( stop )
                return;
            t = s+1;
        }
        handler(t, stop);
        if (stop)
            return;
    }
}

void ProcessConfig::PathOverrides::forEachDylibFallback(Platform platform, bool requestorNeedsFallbacks, bool& stop,
                                                        void (^handler)(const char* fallbackDir, Type type, bool&)) const
{
    // DYLD_FALLBACK_LIBRARY_PATH works for all binaries, regardless of requestorNeedsFallbacks
    if ( (_dylibPathFallbacksEnv != nullptr) || (_dylibPathFallbacksExeLC != nullptr) ) {
        forEachInColonList(_dylibPathFallbacksEnv, _dylibPathFallbacksExeLC, stop, ^(const char* pth, bool&) {
            handler(pth, Type::customFallback, stop);
        });
    }
    else if ( requestorNeedsFallbacks ) {
        if ( platform.isExclave() || platform == Platform::driverKit )
            return;

        
        // if no FALLBACK env vars, then only do fallbacks for old binaries
        if ( platform == Platform::macOS ) {
            switch ( _fallbackPathMode ) {
                case FallbackPathMode::classic:
                    // "$HOME/lib"
                    handler("/usr/local/lib", Type::standardFallback, stop);
                    if ( stop )
                        break;
                    [[clang::fallthrough]];
                case FallbackPathMode::restricted:
                    handler("/usr/lib", Type::standardFallback, stop);
                    break;
                case FallbackPathMode::none:
                    break;
            }
            
            return;
        }
        
        if ( platform != Platform::macCatalyst ) {
            if ( _fallbackPathMode != FallbackPathMode::none ) {
                handler("/usr/local/lib", Type::standardFallback, stop);
                if ( stop )
                    return;
            }
        }
        
        if ( platform.isSimulator() || platform == Platform::macCatalyst ) {
            if ( _fallbackPathMode != FallbackPathMode::none )
                handler("/usr/lib", Type::standardFallback, stop);
        }
    }
}

void ProcessConfig::PathOverrides::forEachFrameworkFallback(Platform platform, bool requestorNeedsFallbacks, bool& stop,
                                                            void (^handler)(const char* fallbackDir, Type type, bool&)) const
{
    // DYLD_FALLBACK_FRAMEWORK_PATH works for all binaries, regardless of requestorNeedsFallbacks
    if ( (_frameworkPathFallbacksEnv != nullptr) || (_frameworkPathFallbacksExeLC != nullptr) ) {
        forEachInColonList(_frameworkPathFallbacksEnv, _frameworkPathFallbacksExeLC, stop, ^(const char* pth, bool&) {
            handler(pth, Type::customFallback, stop);
        });
    }
    else if ( requestorNeedsFallbacks ) {
        if ( platform.isExclave() || platform == Platform::driverKit )
            return;

        
        // if no FALLBACK env vars, then only do fallbacks for old binaries
        if ( platform == Platform::macOS ) {
            switch ( _fallbackPathMode ) {
                case FallbackPathMode::classic:
                    // "$HOME/Library/Frameworks"
                    handler("/Library/Frameworks", Type::standardFallback, stop);
                    if ( stop )
                        break;
                    // "/Network/Library/Frameworks"
                    // fall thru
                    [[clang::fallthrough]];
                case FallbackPathMode::restricted:
                    handler("/System/Library/Frameworks", Type::standardFallback, stop);
                    break;
                case FallbackPathMode::none:
                    break;
            }
            
            return;
        }
        
        if ( _fallbackPathMode != FallbackPathMode::none )
            handler("/System/Library/Frameworks", Type::standardFallback, stop);
    }
}


//
// copy path and add suffix to result
//
//  /path/foo.dylib      _debug   =>   /path/foo_debug.dylib
//  foo.dylib            _debug   =>   foo_debug.dylib
//  foo                  _debug   =>   foo_debug
//  /path/bar            _debug   =>   /path/bar_debug
//  /path/bar.A.dylib    _debug   =>   /path/bar.A_debug.dylib
//
void ProcessConfig::PathOverrides::addSuffix(const char* path, const char* suffix, char* result) const
{
    strlcpy(result, path, PATH_MAX);

    // find last slash
    char* start = strrchr(result, '/');
    if ( start != NULL )
        start++;
    else
        start = result;

    // find last dot after last slash
    char* dot = strrchr(start, '.');
    if ( dot != NULL ) {
        strlcpy(dot, suffix, PATH_MAX);
        strlcat(&dot[strlen(suffix)], &path[dot-result], PATH_MAX);
    }
    else {
        strlcat(result, suffix, PATH_MAX);
    }
}

void ProcessConfig::PathOverrides::forEachImageSuffix(const char* path, Type type, bool& stop,
                                                      void (^handler)(const char* possiblePath, Type type, bool&)) const
{
    if ( _imageSuffix == nullptr ) {
        handler(path, type, stop);
    }
    else {
        forEachInColonList(_imageSuffix, nullptr, stop, ^(const char* suffix, bool&) {
            char npath[strlen(path)+strlen(suffix)+8];
            addSuffix(path, suffix, npath);
            handler(npath, Type::suffixOverride, stop);
        });
        if ( !stop )
            handler(path, type, stop);
    }
}

void ProcessConfig::PathOverrides::forEachPathVariant(const char* initialPath, Platform platform, bool requestorNeedsFallbacks, bool skipFallbacks, bool& stop,
                                                      void (^handler)(const char* possiblePath, Type type, bool& stop)) const
{
    // check for overrides
    const char* frameworkPartialPath = getFrameworkPartialPath(initialPath);
    if ( frameworkPartialPath != nullptr ) {
        const size_t frameworkPartialPathLen = strlen(frameworkPartialPath);
        // look at each DYLD_FRAMEWORK_PATH directory
        if ( (_frameworkPathOverridesEnv != nullptr) || (_frameworkPathOverridesExeLC != nullptr) ) {
            forEachInColonList(_frameworkPathOverridesEnv, _frameworkPathOverridesExeLC, stop, ^(const char* frDir, bool&) {
                size_t npathSize = strlen(frDir)+frameworkPartialPathLen+8;
                char npath[npathSize];
                strlcpy(npath, frDir, npathSize);
                strlcat(npath, "/", npathSize);
                strlcat(npath, frameworkPartialPath, npathSize);
                forEachImageSuffix(npath, Type::pathDirOverride, stop, handler);
            });
        }
    }
    else {
        const char* libraryLeafName = getLibraryLeafName(initialPath);
        const size_t libraryLeafNameLen = strlen(libraryLeafName);
        // look at each DYLD_LIBRARY_PATH directory
        if ( (_dylibPathOverridesEnv != nullptr) || (_dylibPathOverridesExeLC != nullptr) ) {
            forEachInColonList(_dylibPathOverridesEnv, _dylibPathOverridesExeLC, stop, ^(const char* libDir, bool&) {
                size_t npathSize = strlen(libDir)+libraryLeafNameLen+8;
                char npath[npathSize];
                strlcpy(npath, libDir, npathSize);
                strlcat(npath, "/", npathSize);
                strlcat(npath, libraryLeafName, npathSize);
                forEachImageSuffix(npath, Type::pathDirOverride, stop, handler);
            });
        }
    }
    if ( stop )
        return;

    // check for versioned_path overrides
    for (DylibOverride* replacement=_versionedOverrides; replacement != nullptr; replacement=replacement->next) {
        if ( strcmp(replacement->installName, initialPath) == 0 ) {
            handler(replacement->overridePath, Type::versionedOverride, stop);
            // note: always stop searching when versioned override is found
            return;
       }
    }

    // paths staring with @ are never valid for finding in iOSSupport or simulator
    if ( initialPath[0] != '@' ) {
#if TARGET_OS_SIMULATOR
        if ( _simRootPath != nullptr ) {
            // try simulator prefix
            size_t rtpathSize = strlen(_simRootPath)+strlen(initialPath)+8;
            char rtpath[rtpathSize];
            strlcpy(rtpath, _simRootPath, rtpathSize);
            strlcat(rtpath, initialPath, rtpathSize);
            forEachImageSuffix(rtpath, Type::simulatorPrefix, stop, handler);
            if ( stop )
                return;
        }
#endif

        // try rootpaths
        bool searchiOSSupport = (platform == Platform::macCatalyst);
#if (TARGET_OS_OSX && TARGET_CPU_ARM64 && BUILDING_DYLD)
        if ( platform == Platform::iOS ) {
            searchiOSSupport = true;
            // <rdar://problem/58959974> some old Almond apps reference old WebKit location
            if ( strcmp(initialPath, "/System/Library/PrivateFrameworks/WebKit.framework/WebKit") == 0 )
                initialPath = "/System/Library/Frameworks/WebKit.framework/WebKit";
        }
#endif

        if ( searchiOSSupport && (strncmp(initialPath, "/System/iOSSupport/", 19) == 0) )
            searchiOSSupport = false;

        bool searchCryptexPrefix = (_cryptexRootPath != nullptr);
        if ( searchCryptexPrefix ) {

            // try looking in Catalyst support dir, but not in the shared cache
            if ( searchiOSSupport ) {
                {
                    size_t rtpathLen = strlen("/System/iOSSupport")+strlen(initialPath)+8;
                    char rtpath[rtpathLen];
                    strlcpy(rtpath, "/System/iOSSupport", rtpathLen);
                    strlcat(rtpath, initialPath, rtpathLen);
                    forEachImageSuffix(rtpath, Type::catalystPrefixOnDisk, stop, handler);
                    if ( stop )
                        return;
                }

                {
                    // try cryptex mount
                    // Note this is after the above call due to:
                    // rdar://91027811 (dyld should search for dylib overrides in / before /System/Cryptexes/OS)
                    size_t rtpathLen = strlen(_cryptexRootPath)+strlen("/System/iOSSupport")+strlen(initialPath)+8;
                    char rtpath[rtpathLen];
                    strlcpy(rtpath, _cryptexRootPath, rtpathLen);
                    strlcat(rtpath, "/System/iOSSupport", rtpathLen);
                    strlcat(rtpath, initialPath, rtpathLen);
                    forEachImageSuffix(rtpath, Type::cryptexCatalystPrefix, stop, handler);
                    if ( stop )
                        return;
                }

                // try looking in Catalyst support dir
                if ( searchiOSSupport ) {
                    size_t rtpathLen = strlen("/System/iOSSupport")+strlen(initialPath)+8;
                    char rtpath[rtpathLen];
                    strlcpy(rtpath, "/System/iOSSupport", rtpathLen);
                    strlcat(rtpath, initialPath, rtpathLen);
                    forEachImageSuffix(rtpath, Type::catalystPrefix, stop, handler);
                    if ( stop )
                        return;

                    searchiOSSupport = false;
                }
            }

            // try original path on disk, but not in the shared cache
            forEachImageSuffix(initialPath, Type::rawPathOnDisk, stop, handler);
            if ( stop )
                return;

            // try cryptex mount
            // Note this is after the above call due to:
            // rdar://91027811 (dyld should search for dylib overrides in / before /System/Cryptexes/OS)
            size_t rtpathLen = strlen(_cryptexRootPath)+strlen(initialPath)+8;
            char rtpath[rtpathLen];
            strlcpy(rtpath, _cryptexRootPath, rtpathLen);
            strlcat(rtpath, initialPath ,rtpathLen);
            forEachImageSuffix(rtpath, Type::cryptexPrefix, stop, handler);
            if ( stop )
                return;
        }

        // try looking in Catalyst support dir
        if ( searchiOSSupport ) {
            size_t rtpathLen = strlen("/System/iOSSupport")+strlen(initialPath)+8;
            char rtpath[rtpathLen];
            strlcpy(rtpath, "/System/iOSSupport", rtpathLen);
            strlcat(rtpath, initialPath, rtpathLen);
            forEachImageSuffix(rtpath, Type::catalystPrefix, stop, handler);
            if ( stop )
                return;
        }
    }

    // try original path, including in the shared cache
    forEachImageSuffix(initialPath, Type::rawPath, stop, handler);
    if ( stop )
        return;

    // check fallback paths
    if ( !skipFallbacks ) {
        if ( frameworkPartialPath != nullptr ) {
            const size_t frameworkPartialPathLen = strlen(frameworkPartialPath);
            // look at each DYLD_FALLBACK_FRAMEWORK_PATH directory
            forEachFrameworkFallback(platform, requestorNeedsFallbacks, stop, ^(const char* dir, Type type, bool&) {
                size_t npathSize = strlen(dir)+frameworkPartialPathLen+8;
                char npath[npathSize];
                strlcpy(npath, dir, npathSize);
                strlcat(npath, "/", npathSize);
                strlcat(npath, frameworkPartialPath, npathSize);
                // don't try original path again
                if ( strcmp(initialPath, npath) != 0 ) {
                    forEachImageSuffix(npath, type, stop, handler);
                }
            });

        }
       else {
            const char* libraryLeafName = getLibraryLeafName(initialPath);
            const size_t libraryLeafNameLen = strlen(libraryLeafName);
            // look at each DYLD_FALLBACK_LIBRARY_PATH directory
            forEachDylibFallback(platform, requestorNeedsFallbacks, stop, ^(const char* dir, Type type, bool&) {
                size_t libpathSize = strlen(dir)+libraryLeafNameLen+8;
                char libpath[libpathSize];
                strlcpy(libpath, dir, libpathSize);
                strlcat(libpath, "/", libpathSize);
                strlcat(libpath, libraryLeafName, libpathSize);
                if ( strcmp(libpath, initialPath) != 0 ) {
                    forEachImageSuffix(libpath, type, stop, handler);
                }
            });
        }
    }
}



//
// Find framework path
//
//  /path/foo.framework/foo                             =>   foo.framework/foo
//  /path/foo.framework/Versions/A/foo                  =>   foo.framework/Versions/A/foo
//  /path/foo.framework/Frameworks/bar.framework/bar    =>   bar.framework/bar
//  /path/foo.framework/Libraries/bar.dylb              =>   NULL
//  /path/foo.framework/bar                             =>   NULL
//
// Returns nullptr if not a framework path
//
const char* ProcessConfig::PathOverrides::getFrameworkPartialPath(const char* path) const
{
    const char* dirDot = Utils::strrstr(path, ".framework/");
    if ( dirDot != nullptr ) {
        const char* dirStart = dirDot;
        for ( ; dirStart >= path; --dirStart) {
            if ( (*dirStart == '/') || (dirStart == path) ) {
                const char* frameworkStart = &dirStart[1];
                if ( dirStart == path )
                    --frameworkStart;
                size_t len = dirDot - frameworkStart;
                char framework[len+1];
                strncpy(framework, frameworkStart, len);
                framework[len] = '\0';
                const char* leaf = strrchr(path, '/');
                if ( leaf != nullptr ) {
                    if ( strcmp(framework, &leaf[1]) == 0 ) {
                        return frameworkStart;
                    }
                    if (  _imageSuffix != nullptr ) {
                        // some debug frameworks have install names that end in _debug
                        if ( strncmp(framework, &leaf[1], len) == 0 ) {
                            if ( strcmp( _imageSuffix, &leaf[len+1]) == 0 )
                                return frameworkStart;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}


const char* ProcessConfig::PathOverrides::getLibraryLeafName(const char* path)
{
    const char* start = strrchr(path, '/');
    if ( start != nullptr )
        return &start[1];
    else
        return path;
}

const char* ProcessConfig::PathOverrides::typeName(Type type)
{
    switch (type) {
        case pathDirOverride:
            return "DYLD_FRAMEWORK/LIBRARY_PATH";
        case versionedOverride:
            return "DYLD_VERSIONED_FRAMEWORK/LIBRARY_PATH";
        case suffixOverride:
            return "DYLD_IMAGE_SUFFIX";
        case catalystPrefixOnDisk:
            return "Catalyst prefix on disk";
        case catalystPrefix:
            return "Catalyst prefix";
        case simulatorPrefix:
            return "simulator prefix";
        case cryptexCatalystPrefix:
            return "cryptex Catalyst prefix";
        case cryptexPrefix:
            return "cryptex prefix";
        case rawPathOnDisk:
            return "original path on disk";
        case rawPath:
            return "original path";
        case rpathExpansion:
            return "@path expansion";
        case loaderPathExpansion:
            return "@loader_path expansion";
        case executablePathExpansion:
            return "@executable_path expansion";
        case implictRpathExpansion:
            return "leaf name using rpath";
        case customFallback:
            return "DYLD_FRAMEWORK/LIBRARY_FALLBACK_PATH";
        case standardFallback:
            return "default fallback";
    }
    return "unknown";
}

bool ProcessConfig::PathOverrides::dontUsePrebuiltForApp() const
{
    // DYLD_LIBRARY_PATH and DYLD_FRAMEWORK_PATH disable building PrebuiltLoader for app
    if ( _dylibPathOverridesEnv || _frameworkPathOverridesEnv )
        return true;

    // DYLD_VERSIONED_LIBRARY_PATH and DYLD_VERSIONED_FRAMEWORK_PATH disable building PrebuiltLoader for app
    if ( _versionedDylibPathsEnv || _versionedFrameworkPathsEnv )
        return true;

    // DYLD_INSERT_LIBRARIES and DYLD_IMAGE_SUFFIX disable building PrebuiltLoader for app
    if ( _insertedDylibs || _imageSuffix )
        return true;

    // LC_DYLD_ENVIRONMENT VERSIONED* paths disable building PrebuiltLoader for app
    // TODO: rdar://73360795 (need a way to allow PrebuiltLoaderSets to work the VERSIONED_PATH)
    if ( _versionedDylibPathExeLC || _versionedFrameworkPathExeLC)
        return true;

    // macOS needs us to stat for roots if the load command sets library/framework path
    if ( _dylibPathOverridesExeLC || _frameworkPathOverridesExeLC )
        return true;

    return false;
}


//
// MARK: --- ProcessConfig methods ---
//

#if TARGET_OS_OSX && SUPPORT_VM_LAYOUT && !TARGET_OS_EXCLAVEKIT
bool ProcessConfig::simulatorFileMatchesDyldCache(const char *path) const
{
    // On macOS there are three dylibs under libSystem that exist for the simulator to use,
    // but we do not consider them "roots", so fileExists() returns false for them
    if ( this->dyldCache.addr == nullptr )
        return false;

    std::string_view tempPath(path);
    if ( !this->dyldCache.cryptexOSPath.empty() ) {
        if ( tempPath.starts_with(this->dyldCache.cryptexOSPath) )
            tempPath.remove_prefix(dyldCache.cryptexOSPath.size());
    }

    const char* ending = nullptr;
    if ( tempPath.starts_with("/usr/lib/system/libsystem_") ) {
        tempPath.remove_prefix(strlen("/usr/lib/system/libsystem_"));
        ending = tempPath.data();
    } else {
        return false;
    }

    if ( strcmp(ending, "platform.dylib") == 0 ) {
        // If this was a root when launchd checked, then assume we are a root now
        if ( this->process.commPage.libPlatformRoot )
            return false;

        // If the file system is read-only, then this cannot be a root now
        if ( !this->process.commPage.bootVolumeWritable )
            return true;

        // Possibly a root, open the file and compare UUID to one in dyld cache
        return this->dyldCache.uuidOfFileMatchesDyldCache(process, syscall, path);
    }
    else if ( strcmp(ending, "pthread.dylib") == 0 ) {
        // If this was a root when launchd checked, then assume we are a root now
        if ( this->process.commPage.libPthreadRoot )
            return false;

        // If the file system is read-only, then this cannot be a root now
        if ( !this->process.commPage.bootVolumeWritable )
            return true;

        // Possibly a root, open the file and compare UUID to one in dyld cache
        return this->dyldCache.uuidOfFileMatchesDyldCache(process, syscall, path);
    }
    else if ( strcmp(ending, "kernel.dylib") == 0 ) {
        // If this was a root when launchd checked, then assume we are a root now
        if ( this->process.commPage.libKernelRoot )
            return false;

        // If the file system is read-only, then this cannot be a root now
        if ( !this->process.commPage.bootVolumeWritable )
            return true;

        // Possibly a root, open the file and compare UUID to one in dyld cache
        return this->dyldCache.uuidOfFileMatchesDyldCache(process, syscall, path);
    }
    return false;
}
#endif // TARGET_OS_OSX

bool ProcessConfig::fileExists(const char* path, FileID* fileID, int* errNum) const
{
#if TARGET_OS_EXCLAVEKIT
    return false;
#else
  #if TARGET_OS_OSX && BUILDING_DYLD
    if ( errNum != nullptr )
        *errNum = ENOENT;
    // On macOS there are three dylibs under libSystem that exist for the simulator to use,
    // but we do not consider them "roots", so fileExists() returns false for them
    if ( simulatorFileMatchesDyldCache(path) )
        return false;

  #endif // TARGET_OS_OSX
    return syscall.fileExists(path, fileID, errNum);
#endif // TARGET_OS_EXCLAVEKIT
}

const char* ProcessConfig::canonicalDylibPathInCache(const char* dylibPath) const
{
    if ( const char* result = this->dyldCache.getCanonicalPath(dylibPath) )
        return result;

#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // on macOS support "Foo.framework/Foo" symlink
    char resolvedPath[PATH_MAX];
    if ( this->syscall.realpath(dylibPath, resolvedPath) ) {
        return this->dyldCache.getCanonicalPath(resolvedPath);
    }
#endif
    return nullptr;
}


FunctionVariantFlags ProcessConfig::evaluatePerProcessVariantFlags(const ProcessConfig& config)
{
    FunctionVariantFlags result = 1; // "default" case is always true
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
    #define FUNCTION_VARIANT_PER_PROCESS(_reqFlagBitNum, _name, _flagBitsInitialization) \
    { \
        bool evaluated = _flagBitsInitialization; \
        if ( evaluated ) \
            result |= ((FunctionVariantFlags)1 << _reqFlagBitNum); \
    }

    #include "FunctionVariantsPerProcess.inc"
#endif
    return result;
}

FunctionVariantFlags ProcessConfig::evaluateSystemWideFunctionVariantFlags(const ProcessConfig& config)
{
    FunctionVariantFlags result = 1; // "default" case is always true
#if BUILDING_DYLD
    #define FUNCTION_VARIANT_SYSTEM_WIDE(_reqFlagBitNum, _name, _flagBitsInitialization) \
    { \
        bool evaluated = _flagBitsInitialization; \
        if ( evaluated ) \
            result |= ((FunctionVariantFlags)1 << _reqFlagBitNum); \
    }

    #include "FunctionVariantsSystemWide.inc"
#endif
    return result;
}

FunctionVariantFlags ProcessConfig::evaluateProcessorSpecificFunctionVariantFlags(const ProcessConfig& config)
{
    FunctionVariantFlags result = 1; // "default" case is always true
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
#if __arm64__
    FunctionVariantFlags hw_opt_arm_caps  = 0;
    size_t     size = sizeof(hw_opt_arm_caps);
    if ( sysctlbyname("hw.optional.arm.caps", &hw_opt_arm_caps, &size, NULL, 0) != 0 )
        return 0;
    #define FUNCTION_VARIANT_ARM64(_reqFlagBitNum, _name, _flagBitsInitialization) \
    { \
        bool evaluated = _flagBitsInitialization; \
        if ( evaluated ) \
            result |= ((FunctionVariantFlags)1 << _reqFlagBitNum); \
    }

    #include "FunctionVariantsArm64.inc"
#elif __x86_64__
    #define FUNCTION_VARIANT_X86_64(_reqFlagBitNum, _name, _flagBitsInitialization) \
    { \
        bool evaluated = _flagBitsInitialization; \
        if ( evaluated ) \
            result |= ((FunctionVariantFlags)1 << _reqFlagBitNum); \
    }

    #include "FunctionVariantsX86_64.inc"
#endif
#endif
    return result;
}




//
// MARK: --- global functions ---
//

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
#if BUILDING_DYLD && !TARGET_OS_EXCLAVEKIT
static char error_string[1024]; // FIXME: check if anything still needs the error_string global symbol, or if abort_with_payload superceeds it
#endif // !TARGET_OS_EXCLAVEKIT

extern void setExternallyViewableStateToTerminated(const char* message);

void missing_symbol_abort()
{
    halt("missing lazy symbol called");
}

void halt(const char* message, const StructuredError* errorInfo)
{
#if TARGET_OS_EXCLAVEKIT || BUILDING_UNIT_TESTS
    console("%s\n", message);
    abort();
#else
    strlcpy(error_string, message, sizeof(error_string));
    CRSetCrashLogMessage(error_string);
    console("%s\n", message);
    setExternallyViewableStateToTerminated(error_string);
    /*
    if ( sSharedCacheLoadInfo.errorMessage != nullptr ) {
        // <rdar://problem/45957449> if dyld fails with a missing dylib and there is no shared cache, display the shared cache load error message
        log2("dyld cache load error: %s\n", sSharedCacheLoadInfo.errorMessage);
        log2("%s\n", message);
        strlcpy(error_string, "dyld cache load error: ", sizeof(error_string));
        strlcat(error_string, sSharedCacheLoadInfo.errorMessage, sizeof(error_string));
        strlcat(error_string, "\n", sizeof(error_string));
        strlcat(error_string, message, sizeof(error_string));
    }
    else {
        log2("%s\n", message);
        strlcpy(error_string, message, sizeof(error_string));
    }
*/
    char                payloadBuffer[EXIT_REASON_PAYLOAD_MAX_LEN];
    dyld_abort_payload* payload    = (dyld_abort_payload*)payloadBuffer;
    payload->version               = 1;
    payload->flags                 = 0;
    payload->targetDylibPathOffset = 0;
    payload->clientPathOffset      = 0;
    payload->symbolOffset          = 0;
    int payloadSize                = sizeof(dyld_abort_payload);

    uintptr_t kind = DYLD_EXIT_REASON_OTHER;
    if ( errorInfo != nullptr ) {
        // don't show back trace, during launch if symbol or dylib missing.  All info is in the error message
        kind = errorInfo->kind;
        if ( (kind == DYLD_EXIT_REASON_SYMBOL_MISSING) || (kind == DYLD_EXIT_REASON_DYLIB_MISSING) )
            payload->flags = 1;
        if ( errorInfo->targetDylibPath != nullptr ) {
            payload->targetDylibPathOffset = payloadSize;
            payloadSize += strlcpy(&payloadBuffer[payloadSize], errorInfo->targetDylibPath, sizeof(payloadBuffer) - payloadSize) + 1;
        }
        if ( errorInfo->clientOfDylibPath != nullptr ) {
            payload->clientPathOffset = payloadSize;
            payloadSize += strlcpy(&payloadBuffer[payloadSize], errorInfo->clientOfDylibPath, sizeof(payloadBuffer) - payloadSize) + 1;
        }
        if ( errorInfo->symbolName != nullptr ) {
            payload->symbolOffset = payloadSize;
            payloadSize += strlcpy(&payloadBuffer[payloadSize], errorInfo->symbolName, sizeof(payloadBuffer) - payloadSize) + 1;
        }
    }
    char truncMessage[EXIT_REASON_USER_DESC_MAX_LEN];
    strlcpy(truncMessage, message, EXIT_REASON_USER_DESC_MAX_LEN);
    const bool verbose = false;
    if ( verbose ) {
        console("dyld_abort_payload.version               = 0x%08X\n", payload->version);
        console("dyld_abort_payload.flags                 = 0x%08X\n", payload->flags);
        console("dyld_abort_payload.targetDylibPathOffset = 0x%08X (%s)\n", payload->targetDylibPathOffset, payload->targetDylibPathOffset ? &payloadBuffer[payload->targetDylibPathOffset] : "");
        console("dyld_abort_payload.clientPathOffset      = 0x%08X (%s)\n", payload->clientPathOffset, payload->clientPathOffset ? &payloadBuffer[payload->clientPathOffset] : "");
        console("dyld_abort_payload.symbolOffset          = 0x%08X (%s)\n", payload->symbolOffset, payload->symbolOffset ? &payloadBuffer[payload->symbolOffset] : "");
    }
    abort_with_payload(OS_REASON_DYLD, kind, payloadBuffer, payloadSize, truncMessage, 0);
#endif // TARGET_OS_EXCLAVEKIT
}
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS

void console(const char* format, ...)
{
#if TARGET_OS_EXCLAVEKIT
    va_list list;
    va_start(list, format);
    vfprintf(stderr, format, list);
    va_end(list);
#else
    if ( getpid() == 1 ) {
  #if BUILDING_DYLD
        int logFD = open("/dev/console", O_WRONLY | O_NOCTTY, 0);
        ::_simple_dprintf(2, "dyld[%d]: ", getpid());
        va_list list;
        va_start(list, format);
        ::_simple_vdprintf(logFD, format, list);
        va_end(list);
        ::close(logFD);
  #endif // BUILDING_DYLD
    } else {
        ::_simple_dprintf(2, "dyld[%d]: ", getpid());
        va_list list;
        va_start(list, format);
        ::_simple_vdprintf(2, format, list);
        va_end(list);
    }
#endif // TARGET_OS_EXCLAVEKIT
}




} // namespace dyld4
