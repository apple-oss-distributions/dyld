/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
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

#if __has_include(<AppleFeatures/AppleFeatures.h>)
#include <AppleFeatures/AppleFeatures.h>
#endif

#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <TargetConditionals.h>
#if !TARGET_OS_EXCLAVEKIT
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <pthread.h>
  #include <libproc.h>
  #include <mach/mach_time.h> // mach_absolute_time()
  #include <mach/mach_init.h>
  #include <mach/shared_region.h>
  #include <sys/param.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <sys/sysctl.h>
  #include <sys/mman.h>
  #include <sys/ioctl.h>
  #include <libkern/OSAtomic.h>
  #include <_simple.h>
  #include <os/lock_private.h>
  #include <Availability.h>
  #include <System/sys/codesign.h>
  #include <System/sys/csr.h>
  #include <System/sys/reason.h>
  #include <System/machine/cpu_capabilities.h>
  #include <CrashReporterClient.h>
  #include <libproc_internal.h>
  #if !TARGET_OS_SIMULATOR
    #include <libamfi.h>
  #endif // !TARGET_OS_SIMULATOR
#else
  #include <liblibc/plat/dyld/exclaves_dyld.h>
#endif // !TARGET_OS_EXCLAVEKIT
#if __has_feature(ptrauth_calls)
    #include <ptrauth.h>
#endif

#include "Defines.h"
#include "StringUtils.h"
#include "MachOLoaded.h"
#include "DyldSharedCache.h"
#include "SharedCacheRuntime.h"
#include "Tracing.h"
#include "Loader.h"
#include "JustInTimeLoader.h"
#include "PrebuiltLoader.h"
#include "PremappedLoader.h"
#include "DyldProcessConfig.h"
#include "DyldRuntimeState.h"
#include "DyldAPIs.h"
#include "ExternallyViewableState.h"

#if !TARGET_OS_EXCLAVEKIT
  #include "dyld_process_info.h"
  #include "dyld_process_info_internal.h"
  #include "dyldSyscallInterface.h"
#endif // !TARGET_OS_EXCLAVEKIT


using dyld3::FatFile;
using dyld3::GradedArchs;
using dyld3::MachOAnalyzer;
using dyld3::MachOFile;
using dyld3::MachOLoaded;
using mach_o::Header;
using lsl::Allocator;

#if TARGET_OS_EXCLAVEKIT
  extern "C" void bootinfo_init(uintptr_t bootinfo);
  extern "C" void plat_common_parse_entry_vec(xrt__entry_vec_t vec[10], xrt__entry_args_t *args);
  extern "C" void _liblibc_stack_guard_init(void);
  extern "C" void _secure_runtime_init(void);

#else
  extern "C" void mach_init();
  extern "C" void __guard_setup(const char* apple[]);
  extern "C" void _subsystem_init(const char* apple[]);
#endif

#if !TARGET_OS_SIMULATOR
static const MachOAnalyzer* getDyldMH()
{
#if __LP64__
    extern const MachOAnalyzer __dso_handle;
    return &__dso_handle;
#else
    // on 32-bit arm, __dso_handle is access through a GOT slot.  Since rebasing has not happened yet, that value is incorrect.
    // instead we scan backwards from this function looking for mach_header
    uintptr_t p = (uintptr_t)&getDyldMH;
    p = p & (-0x1000);
    while ( *((uint32_t*)p) != MH_MAGIC ) {
        p -= 0x1000;
    }
    return (MachOAnalyzer*)p;
#endif // __LP64__
}
#endif // !TARGET_OS_SIMULATOR

#if TARGET_OS_SIMULATOR
const dyld::SyscallHelpers* gSyscallHelpers = nullptr;

// <rdar://problem/100180105> We need to guarantee there is some non-zerofill content to prevent crashes in old dylds
__attribute__((used, section("__DATA,__sim_fix"))) uint64_t r100180105 = 1;
#endif


namespace dyld4 {

#if SUPPPORT_PRE_LC_MAIN
// this is defined in dyldStartup.s
extern void gotoAppStart(uintptr_t start, const KernelArgs* kernArgs) __attribute__((__noreturn__));
#endif

// this is defined in dyldStartup.s
void restartWithDyldInCache(const KernelArgs* kernArgs, const Header* dyldOnDisk, const DyldSharedCache*, void* dyldStart);

// no header because only called from assembly
extern void start(const KernelArgs* kernArgs);

#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT

RuntimeState* sHostState = nullptr;

__attribute__((format(printf, 1, 0)))
static void sim_vlog(const char* format, va_list list)
{
    sHostState->vlog(format, list);
}

static char* getcwd_sans_malloc(char* buf, size_t size)
{
    SyscallDelegate syscall;
    if ( syscall.getCWD(buf) )
        return buf;
    return nullptr;
}

static char* realpath_sans_malloc(const char* file_name, char* resolved_name)
{
    SyscallDelegate syscall;
    if ( syscall.realpath(file_name, resolved_name) )
        return resolved_name;
    return nullptr;
}

static DIR* opendir_fake(const char*) {
    // <rdar://81126810> Allow old simulator binaries to call back opendir
    return nullptr;
}

static void sim_coresymbolication_load_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh)
{
    // No simulators that are still supported use this interface, do nothing
}

static void sim_coresymbolication_unload_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh)
{
    // No simulators that are still supported use this interface, do nothing
}

static void sim_notifyMonitorOfImageListChanges(bool unloading, unsigned imageCount, const mach_header* loadAddresses[], const char* imagePaths[])
{
    sHostState->externallyViewable->notifyMonitorOfImageListChangesSim(unloading, imageCount, loadAddresses, imagePaths);
}

static void sim_notifyMonitorOfMainCalled()
{
    sHostState->externallyViewable->notifyMonitorOfMainCalled();
}

static void sim_notifyMonitorOfDyldBeforeInitializers()
{
    sHostState->externallyViewable->notifyMonitorOfDyldBeforeInitializers();
}

// These are syscalls that the macOS dyld makes available to dyld_sim
static const dyld::SyscallHelpers sSysCalls = {
    18,
    // added in version 1
    &open,
    &close,
    &pread,
    &write,
    &mmap,
    &munmap,
    &madvise,
    &stat,
    &fcntl,
    &ioctl,
    &issetugid,
    &getcwd_sans_malloc,
    &realpath_sans_malloc,
    &vm_allocate,
    &vm_deallocate,
    &vm_protect,
    &sim_vlog,
    &sim_vlog,
    &pthread_mutex_lock,
    &pthread_mutex_unlock,
    &mach_thread_self,
    &mach_port_deallocate,
    &task_self_trap,
    &mach_timebase_info,
    &OSAtomicCompareAndSwapPtrBarrier,
    &OSMemoryBarrier,
    &ExternallyViewableState::getProcessInfo,
    &__error,
    &mach_absolute_time,
    // added in version 2
    &thread_switch,
    // added in version 3 (no longer used)
    &opendir_fake,
    nullptr, // &readdir_r,
    nullptr, // &closedir,
    // added in version 4
    &sim_coresymbolication_load_notifier,
    &sim_coresymbolication_unload_notifier,
    // Added in version 5
    &proc_regionfilename,
    &getpid,
    &mach_port_insert_right,
    &mach_port_allocate,
    &mach_msg,
    // Added in version 6
    &abort_with_payload,
    // Added in version 7
    &task_register_dyld_image_infos,
    &task_unregister_dyld_image_infos,
    &task_get_dyld_image_infos,
    &task_register_dyld_shared_cache_image_info,
    &task_register_dyld_set_dyld_state,
    &task_register_dyld_get_process_state,
    // Added in version 8
    &task_info,
    &thread_info,
    &kdebug_is_enabled,
    &kdebug_trace,
    // Added in version 9
    &kdebug_trace_string,
    // Added in version 10
    &amfi_check_dyld_policy_self,
    // Added in version 11
    &sim_notifyMonitorOfMainCalled,
    &sim_notifyMonitorOfImageListChanges,
    // Add in version 12
    &mach_msg_destroy,
    &mach_port_construct,
    &mach_port_destruct,
    // Add in version 13
    &fstat,
    &vm_copy,
    // Add in version 14
    &task_dyld_process_info_notify_get,
    // Add in version 15
    &fsgetpath,
    // Add in version 16
    &getattrlistbulk,
    // Add in version 17
    &getattrlist,
    &getfsstat,
    &sim_notifyMonitorOfDyldBeforeInitializers,
    // Add in version 18
    &sysctlbyname,
};

__attribute__((noinline)) static MainFunc prepareSim(RuntimeState& state, const char* dyldSimPath)
{
    // open dyld_sim
    int fd = dyld3::open(dyldSimPath, O_RDONLY, 0);
    if ( fd == -1 )
        halt("dyld_sim file could not be opened");

    // get file size of dyld_sim
    struct stat sb;
    if ( fstat(fd, &sb) == -1 )
        halt("stat(dyld_sim) failed");

    // mmap whole file temporarily
    void* tempMapping = ::mmap(nullptr, (size_t)sb.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    if ( tempMapping == MAP_FAILED )
        halt("mmap(dyld_sim) failed");

    // if fat file, pick matching slice
    uint64_t             fileOffset = 0;
    uint64_t             fileLength = sb.st_size;
    const FatFile*       ff         = (FatFile*)tempMapping;
    Diagnostics          diag;
    bool                 missingSlice;
    const MachOAnalyzer* sliceMapping = nullptr;
    const GradedArchs&   archs        = GradedArchs::forCurrentOS(state.config.process.mainExecutableMF, false);
    if ( ff->isFatFileWithSlice(diag, sb.st_size, archs, true, fileOffset, fileLength, missingSlice) ) {
        sliceMapping = (MachOAnalyzer*)((uint8_t*)tempMapping + fileOffset);
    }
    else if ( ((MachOFile*)tempMapping)->isMachO(diag, fileLength) ) {
        sliceMapping = (MachOAnalyzer*)tempMapping;
    }
    else {
        halt("dyld_sim is not compatible with the loaded process, likely due to architecture mismatch");
    }

    // validate load commands
    if ( !sliceMapping->validMachOForArchAndPlatform(diag, (size_t)fileLength, "dyld_sim", archs, state.config.process.platform, true) )
        halt(diag.errorMessage()); //"dyld_sim is malformed");

    // dyld_sim has to be code signed
    uint32_t codeSigFileOffset;
    uint32_t codeSigSize;
    if ( !((const Header*)sliceMapping)->hasCodeSignature(codeSigFileOffset, codeSigSize) )
        halt("dyld_sim is not code signed");

    int codeSigCommand = F_ADDFILESIGS_FOR_DYLD_SIM;
    if (state.config.security.internalInstall && state.config.process.commPage.disableProdSimChecks) {
        // If we are on an internal and the appropriate boot-args is set degrade to a normal code signature check
        codeSigCommand = F_ADDFILESIGS_RETURN;
    }

    // register code signature with kernel before mmap()ing segments
    fsignatures_t siginfo;
    siginfo.fs_file_start = fileOffset;                       // start of mach-o slice in fat file
    siginfo.fs_blob_start = (void*)(long)(codeSigFileOffset); // start of code-signature in mach-o file
    siginfo.fs_blob_size  = codeSigSize;                      // size of code-signature
    int result            = fcntl(fd, codeSigCommand, &siginfo);
    if ( result == -1 ) {
        halt("dyld_sim fcntl(F_ADDFILESIGS_FOR_DYLD_SIM) failed");
    }
    // file range covered by code signature must extend up to code signature itself
    if ( siginfo.fs_file_start < codeSigFileOffset )
        halt("dyld_sim code signature does not cover all of dyld_sim");

    // reserve space, then mmap each segment
    const uint64_t mappedSize                  = sliceMapping->mappedSize();
    uint64_t       dyldSimPreferredLoadAddress = ((const Header*)sliceMapping)->preferredLoadAddress();
    vm_address_t   dyldSimLoadAddress          = 0;
    if ( ::vm_allocate(mach_task_self(), &dyldSimLoadAddress, (vm_size_t)mappedSize, VM_FLAGS_ANYWHERE) != 0 )
        halt("dyld_sim cannot allocate space");
    __block const char* mappingStr = nullptr;
    const Header* sliceMappingHeader = (const Header*)sliceMapping;
    sliceMappingHeader->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
        // <rdar://problem/100180105> Mapping zero filled regions fails with mmap of size 0
        if ( info.fileSize == 0)
            return;

        uintptr_t requestedLoadAddress = (uintptr_t)(info.vmaddr - dyldSimPreferredLoadAddress + dyldSimLoadAddress);
        void*     segAddress           = ::mmap((void*)requestedLoadAddress, (size_t)info.fileSize, info.initProt, MAP_FIXED | MAP_PRIVATE, fd, fileOffset + info.fileOffset);
        //state.log("dyld_sim %s mapped at %p\n", seg->segname, segAddress);
        if ( segAddress == MAP_FAILED ) {
            mappingStr = "dyld_sim mmap() of segment failed";
            stop       = true;
        }
        else if ( ((uintptr_t)segAddress < dyldSimLoadAddress) || ((uintptr_t)segAddress + info.fileSize > dyldSimLoadAddress + mappedSize) ) {
            mappingStr = "dyld_sim mmap() to wrong location";
            stop       = true;
        }
    });
    if ( mappingStr != nullptr )
        halt(mappingStr);
    ::close(fd);
    ::munmap(tempMapping, (size_t)sb.st_size);

    const Header* dyldSimHdr = (const Header*)dyldSimLoadAddress;

    // walk newly mapped dyld_sim __TEXT load commands to find entry point
    uint64_t entryOffset;
    bool     usesCRT;
    if ( !dyldSimHdr->getEntry(entryOffset, usesCRT) )
        halt("dyld_sim entry not found");

    // save off host state object for use later if dyld_sim calls back into host to notify
    sHostState = &state;

    // add dyld_sim to the image list for the debugger to see
    STACK_ALLOCATOR(ephemeralAllocator, 0);
    state.externallyViewable->addDyldSimInfo(dyldSimPath, dyldSimLoadAddress);

	// <rdar://problem/5077374> have host dyld detach macOS shared cache from process before jumping into dyld_sim
    if ( state.config.log.segments )
        console("deallocating host dyld shared cache\n");
    dyld3::deallocateExistingSharedCache();
    state.externallyViewable->detachFromSharedRegion();

    // call kdebug trace for each image
    if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
        // add trace for dyld_sim itself
        uuid_t dyldUuid;
        dyldSimHdr->getUuid(dyldUuid);
        fsid_t             dyldFsid    = { { sb.st_dev, 0 } };
        fsobj_id_t         dyldFfsobjid = *(fsobj_id_t*)&sb.st_ino;
        dyld3::kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, dyldSimPath, &dyldUuid, dyldFfsobjid, dyldFsid, (void*)dyldSimLoadAddress,
                                       dyldSimHdr->arch().cpuSubtype());
    }

    //TODO: Remove once drop support for simulators older than iOS 17, tvOS 15, and watchOS 8
    mach_o::PlatformAndVersions pvs = dyldSimHdr->platformAndVersions();
    mach_o::Policy policy(mach_o::Architecture(), pvs, 0);

    // Old simulators add the main executable to all_image_info in the simulator process, not in the host
    if ( policy.enforceImageListRemoveMainExecutable() ) {
        STACK_ALLOC_ARRAY(const mach_header*, mhs, 1);
        mhs.push_back(state.config.process.mainExecutableMF);
        std::span<const mach_header*> mhSpan(&mhs[0], 1);
        state.externallyViewable->removeImages(state.persistentAllocator, ephemeralAllocator, mhSpan);
    }

    // Old simulators do not correctly fill out the private cache fields in the all_image_info, so do it for them
    if ( policy.enforceSetSimulatorSharedCachePath() ) {
        struct stat cacheStatBuf;
        char cachePath[PATH_MAX];
        const char* cacheDir = state.config.process.environ("DYLD_SHARED_CACHE_DIR");
        if (cacheDir) {
            strlcpy(cachePath, cacheDir, PATH_MAX);
            strlcat(cachePath, "/dyld_sim_shared_cache_", PATH_MAX);
            strlcat(cachePath, dyldSimHdr->archName(), PATH_MAX);
            if (state.config.syscall.stat(cachePath, &cacheStatBuf) == 0) {
                state.externallyViewable->setSharedCacheInfo(0, {(uint64_t)cacheStatBuf.st_dev, (uint64_t)cacheStatBuf.st_ino, nullptr, nullptr}, true);
            }
        }
    }

    // jump into new simulator dyld
    typedef MainFunc (*sim_entry_proc_t)(int argc, const char* const argv[], const char* const envp[], const char* const apple[],
                                         const mach_header* mainExecutableMH, const mach_header* dyldMH, uintptr_t dyldSlide,
                                         const dyld::SyscallHelpers* vtable, uintptr_t* startGlue);
    sim_entry_proc_t newDyld = (sim_entry_proc_t)(dyldSimLoadAddress + entryOffset);
#if __has_feature(ptrauth_calls)
    newDyld = (sim_entry_proc_t)__builtin_ptrauth_sign_unauthenticated((void*)newDyld, 0, 0);
#endif
    uintptr_t        startGlue;
    return (*newDyld)(state.config.process.argc, state.config.process.argv, state.config.process.envp, state.config.process.apple,
                      state.config.process.mainExecutableMF, (mach_header*)dyldSimLoadAddress,
                      (uintptr_t)(dyldSimLoadAddress - dyldSimPreferredLoadAddress), &sSysCalls, &startGlue);
}
#endif // TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT

//
// If the DYLD_SKIP_MAIN environment is set to 1, dyld will return the
// address of this function instead of main() in the target program which
// __dyld_start jumps to. Useful for qualifying dyld itself.
//
#if !TARGET_OS_EXCLAVEKIT
static int fake_main(int argc, const char* const argv[], const char* const envp[], const char* const apple[])
{
#if TARGET_OS_SIMULATOR
    return 0;
#else
    _exit(0);
#endif
}
#endif // !TARGET_OS_EXCLAVEKIT


#if SUPPPORT_PRE_LC_MAIN
static bool hasProgramVars(const Header* mainHdr, ProgramVars*& progVars, bool& crtRunsInitializers, FuncLookup*& dyldLookupFuncAddr)
{
    progVars            = nullptr;
    crtRunsInitializers = false;
    dyldLookupFuncAddr  = nullptr;

    // macOS 10.8+              program uses LC_MAIN and ProgramVars are in libdyld.dylib
    // macOS 10.6 -> 10.7       ProgramVars are in __program_vars section in main executable
    // macOS 10.5               ProgramVars are in __dyld section in main executable and 7 pointers in size
    // macOS 10.4 and earlier   ProgramVars need to be looked up by name in nlist of main executable

    uint64_t offset;
    bool     usesCRT;
    if ( !mainHdr->getEntry(offset, usesCRT) || !usesCRT )
        return false; // macOS 10.8 or later

    // is pre-10.8 program
    bool result = false;
    std::span<const uint8_t> programVarsSection = mainHdr->findSectionContent("__DATA", "__program_vars", true/*vm layout*/);
    if ( programVarsSection.size() >= sizeof(ProgramVars) ) {
        // macOS 10.6 or 10.7 binary
        progVars = (ProgramVars*)programVarsSection.data();
        result = true;
    }

    // macOS 10.5 binary or earlier
    std::span<const uint8_t> dyldSection = mainHdr->findSectionContent("__DATA", "__dyld", true/*vm layout*/);
#if SUPPPORT_PRE_LC_MAIN
    if ( dyldSection.size() >= 16 ) {
        // second slot is where dyld should store a function pointer for looking up dyld functions by name
        dyldLookupFuncAddr = (FuncLookup*)(dyldSection.data() + 8);
    }
#endif
    if ( dyldSection.size() >= 56 ) {
        // range 16 to 56 is ProgramVars
        progVars = (ProgramVars*)(dyldSection.data() + 16);
        result = true;
    }
    else if ( dyldSection.size() >= 8 ) {
        // macOS 10.4 binary has __dyld section
        // if binary does not have __dyld section, dyld needs to run initializers
        crtRunsInitializers = true;
    }

    return result;
}
#endif


//
// Load any dependent dylibs and bind all together.
// Returns address of main() in target.
//
__attribute__((noinline)) static MainFunc prepare(APIs& state, const Header* dyldMH)
{
#if TARGET_OS_EXCLAVEKIT
    // now that we can allocate memory and the dyld cache is mapped, we can register
    // page-fault handlers to do page-in linking for shared cache pages
    if ( state.config.process.sharedCachePageInLinking && (state.config.dyldCache.addr != nullptr) )
        Loader::setUpExclaveKitSharedCachePageInLinking(state);

    Diagnostics diag;
    Loader* mainLoader = PremappedLoader::makeLaunchLoader(diag, state, state.config.process.mainExecutableMF, state.config.process.mainExecutablePath, nullptr);
    state.setMainLoader(mainLoader);

    Loader::LoadChain   loadChainMain { nullptr, mainLoader };
    Loader::LoadOptions depOptions;
    depOptions.staticLinkage   = true;
    depOptions.launching       = true;
    depOptions.insertedDylib   = false;
    depOptions.canBeDylib      = true;
    depOptions.rpathStack      = &loadChainMain;
    Diagnostics depsDiag;
    mainLoader->loadDependents(depsDiag, state, depOptions);
    if ( depsDiag.hasError() ) {
        state.log("%s loading dependents of %s\n", depsDiag.errorMessage(), mainLoader->path(state));
        // let crashreporter know about dylibs we were able to load
        halt(depsDiag.errorMessage(), &state.structuredError);
    }
    uint64_t topCount = 1; // no DYLD_INSERT_LIBRARIES for EK

    // do fixups
    DyldCacheDataConstLazyScopedWriter  cacheDataConst(state);

    // The C++ spec says main executables can define non-weak functions which override weak-defs in dylibs
    // This happens automatically for anything bound at launch, but the dyld cache is pre-bound so we need
    // to patch any binds that are overridden by this non-weak in the main executable.
    PremappedLoader::handleStrongWeakDefOverrides(state, cacheDataConst);

    for ( const Loader* ldr : state.loaded ) {
        Diagnostics fixupDiag;
        ldr->applyFixups(fixupDiag, state, cacheDataConst, true, nullptr);
        if ( fixupDiag.hasError() ) {
            halt(fixupDiag.errorMessage());
        }

        // Roots need to patch the uniqued GOTs in the cache
        if ( state.config.process.sharedCacheFileEnabled ) {
            if ( ( state.config.process.platform == mach_o::Platform::macOS_exclaveKit)
                || (state.config.process.platform == mach_o::Platform::iOS_exclaveKit) ) {
                ldr->applyCachePatches(state, cacheDataConst);
            }
        }
    }

    if ( state.config.process.sharedCacheFileEnabled ) {
        if ( ( state.config.process.platform == mach_o::Platform::macOS_exclaveKit)
            || (state.config.process.platform == mach_o::Platform::iOS_exclaveKit) ) {
            // Notify ExclavePlatform that it is safe to setup endpoints in Mach-O sections
#ifdef XRT_PLATFORM_PREMAPPED_CACHE_MACHO_FINALIZE_MEMORY_STATE
            for ( const Loader* ldr : state.loaded ) {
                if ( !ldr->dylibInDyldCache )
                    continue;
                const Header* hdr = ldr->header(state);
                int64_t slide = hdr->getSlide();
                xrt_platform_premapped_cache_macho_finalize_memory_state((void*)hdr, slide);
            }
#endif // XRT_PLATFORM_PREMAPPED_CACHE_MACHO_FINALIZE_MEMORY_STATE
            
            // Mark __DATA_CONST segment as read-only
            const DyldSharedCache* dyldCache = state.config.dyldCache.addr;
            dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
                cache->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size,
                                       uint32_t initProt, uint32_t maxProt, uint64_t flags, bool& stopRegion) {
                    if ( flags & DYLD_CACHE_MAPPING_CONST_DATA ) {
                        xrt_dyld_permissions_t protection = PAGE_PERM_READ;
                        xrt_dyld_mprotect_region((void*)(vmAddr + dyldCache->slide()), 0, size, protection, protection);
                    }
                });
            });
        }
    }
#else
    uint64_t launchTraceID = 0;
    if ( dyld3::kdebug_trace_dyld_enabled(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE) ) {
        uint64_t flags = (uint64_t)dyld3::DyldLaunchExecutableFlags::None;
        if ( state.config.process.enableTproHeap )
            flags |= (uint64_t)dyld3::DyldLaunchExecutableFlags::HasTPROHeap;
        if ( state.config.process.enableTproDataConst )
            flags |= (uint64_t)dyld3::DyldLaunchExecutableFlags::HasTPRODataConst;
        if ( state.config.process.enableProtectedStack )
            flags |= (uint64_t)dyld3::DyldLaunchExecutableFlags::HasTPROStacks;

        launchTraceID = dyld3::kdebug_trace_dyld_duration_start(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE, (uint64_t)state.config.process.mainExecutableHdr, flags, 0);
    }

    // if DYLD_PRINT_SEARCHING is used, be helpful and list stuff that is disabled
    if ( state.config.log.searching ) {
        if ( !state.config.security.allowEnvVarsPrint )
            state.log("Note: DYLD_PRINT_* disabled by AMFI\n");
        if ( !state.config.security.allowInterposing )
            state.log("Note: interposing disabled by AMFI\n");
   }

#if TARGET_OS_OSX
    const bool isSimulatorProgram = state.config.process.platform.isSimulator();
    if ( const char* simPrefixPath = state.config.pathOverrides.simRootPath() ) {
#if __arm64e__
        if ( strcmp(state.config.process.mainExecutableMF->archName(), "arm64e") == 0 )
            halt("arm64e not supported for simulator programs");
#endif
        if ( isSimulatorProgram ) {
            char simDyldPath[PATH_MAX];
            strlcpy(simDyldPath, simPrefixPath, PATH_MAX);
            strlcat(simDyldPath, "/usr/lib/dyld_sim", PATH_MAX);
            return prepareSim(state, simDyldPath);
        }
        halt("DYLD_ROOT_PATH only allowed with simulator programs");
    }
    else if ( isSimulatorProgram ) {
        halt("DYLD_ROOT_PATH not set for simulator program");
    }
#endif // TARGET_OS_OSX

#if 0
    // check if main executable is valid
    Diagnostics diag;
    bool validMainExec = state.config.process.mainExecutable->isValidMainExecutable(diag, state.config.process.mainExecutablePath, -1, *(state.config.process.archs), state.config.process.platform);
    if ( !validMainExec && state.config.process.mainExecutable->enforceFormat(dyld3::MachOAnalyzer::Malformed::sdkOnOrAfter2021)) {
        state.log("%s in %s", diag.errorMessage(), state.config.process.mainExecutablePath);
        halt(diag.errorMessage());
    }
#endif

    // log env variables if asked
    if ( state.config.log.env ) {
        for (const char* const* p=state.config.process.envp; *p != nullptr; ++p) {
            state.log("%s\n", *p);
        }
    }

    Loader*                  mainLoader = nullptr;
#if SUPPORT_PREBUILTLOADERS
    // check for pre-built Loader
    state.initializeClosureMode();
    const PrebuiltLoaderSet* mainSet    = state.processPrebuiltLoaderSet();
    if ( mainSet != nullptr ) {
        mainLoader = (Loader*)mainSet->atIndex(0);
    }
#endif // SUPPORT_PREBUILTLOADERS
    if ( mainLoader == nullptr ) {
        // if no pre-built Loader, make a just-in-time one
        state.loaded.reserve(512);  // guess starting point for Vector size
        Diagnostics buildDiag;
        mainLoader = JustInTimeLoader::makeLaunchLoader(buildDiag, state, state.config.process.mainExecutableMF,
                                                        state.config.process.mainExecutablePath, nullptr);
        if ( buildDiag.hasError() ) {
            state.log("%s in %s\n", buildDiag.errorMessage(), state.config.process.mainExecutablePath);
            halt(buildDiag.errorMessage(), &state.structuredError);
        }
    }
    state.setMainLoader(mainLoader);
    // start by just adding main executable to debuggers's known image list
    state.notifyDebuggerLoad(mainLoader);

#if SUPPORT_PREBUILTLOADERS
    const bool needToWritePrebuiltLoaderSet = !mainLoader->isPrebuilt && (state.saveAppClosureFile() || state.failIfCouldBuildAppClosureFile());
#endif // SUPPORT_PREBUILTLOADERS

    // load any inserted dylibs
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Loader*, topLevelLoaders, 16);
    topLevelLoaders.push_back(mainLoader);

    if ( mach_o::Error err = state.loadInsertedLibraries(topLevelLoaders, mainLoader) )
        halt(err.message());

#if SUPPORT_PREBUILTLOADERS
    // for recording files that must be missing
    __block MissingPaths missingPaths;
    auto missingLogger = ^(const char* mustBeMissingPath) {
        missingPaths.addPath(mustBeMissingPath);
    };
#endif

    // if there is a dyld cache, add dyld shared cache info to ExternallyViewableState
    if ( state.config.dyldCache.addr != nullptr ) {
        state.externallyViewable->setSharedCacheAddress(state.config.dyldCache.slide, (uintptr_t)state.config.dyldCache.addr);
    }

    // recursively load everything needed by main executable and inserted dylibs
    Loader::LoadChain   loadChainMain { nullptr, mainLoader };

    Diagnostics depsDiag;
    Loader::LoadOptions depOptions;
    depOptions.staticLinkage   = true;
    depOptions.launching       = true;
    depOptions.insertedDylib   = false;
    depOptions.canBeDylib      = true;
    depOptions.rpathStack      = &loadChainMain;
#if SUPPORT_PREBUILTLOADERS
    if ( needToWritePrebuiltLoaderSet )
        depOptions.pathNotFoundHandler = missingLogger;
#endif
    for ( Loader* ldr : topLevelLoaders ) {
        ldr->loadDependents(depsDiag, state, depOptions);
        if ( depsDiag.hasError() ) {
            //state.log("%s loading dependents of %s\n", depsDiag.errorMessage(), ldr->path());
            // let debugger/crashreporter know about dylibs we were able to load
            uint64_t topCount = topLevelLoaders.count();
            STACK_ALLOC_VECTOR(const Loader*, newLoaders, state.loaded.size() - topCount);
            for (uint64_t i = topCount; i != state.loaded.size(); ++i)
                newLoaders.push_back(state.loaded[i]);
            state.notifyDebuggerLoad(newLoaders);
            state.externallyViewable->setDyldState(dyld_process_state_terminated_before_inits);
            state.externallyViewable->disableCrashReportBacktrace();
            halt(depsDiag.errorMessage(), &state.structuredError);
        }
    }

    uint64_t topCount = topLevelLoaders.count();
 
    // add to permanent ranges
    STACK_ALLOC_ARRAY(const Loader*, nonCacheNeverUnloadLoaders, state.loaded.size());
    for (const Loader* ldr : state.loaded) {
        if ( !ldr->dylibInDyldCache )
            nonCacheNeverUnloadLoaders.push_back(ldr);
    }
    state.addPermanentRanges(nonCacheNeverUnloadLoaders);

    // proactive weakDefMap means we build the weakDefMap before doing any binding
    if ( state.config.process.proactivelyUseWeakDefMap ) {
        state.weakDefMap = new (state.persistentAllocator.malloc(sizeof(WeakDefMap))) WeakDefMap();
        STACK_ALLOC_VECTOR(const Loader*, allLoaders, state.loaded.size());
        for (const Loader* ldr : state.loaded)
            allLoaders.push_back(ldr);
        Loader::addWeakDefsToMap(state, allLoaders);
    }

    // check for interposing tuples before doing fixups
    state.buildInterposingTables();

    // do fixups
    {
        dyld3::ScopedTimer timer(DBG_DYLD_TIMING_APPLY_FIXUPS, 0, 0, 0);
        // just in case we need to patch the case
        DyldCacheDataConstLazyScopedWriter  cacheDataConst(state);

        // The C++ spec says main executables can define non-weak functions which override weak-defs in dylibs
        // This happens automatically for anything bound at launch, but the dyld cache is pre-bound so we need
        // to patch any binds that are overridden by this non-weak in the main executable.
        // Note on macOS we also allow dylibs to have non-weak overrides of weak-defs
        JustInTimeLoader::handleStrongWeakDefOverrides(state, cacheDataConst);

        for ( const Loader* ldr : state.loaded ) {
            Diagnostics fixupDiag;
            ldr->applyFixups(fixupDiag, state, cacheDataConst, true, nullptr);
            if ( fixupDiag.hasError() ) {
                halt(fixupDiag.errorMessage(), &state.structuredError);
            }

            // Roots need to patch the uniqued GOTs in the cache
            ldr->applyCachePatches(state, cacheDataConst);
        }

        // Do singleton patching if we have it
        state.doSingletonPatching(cacheDataConst);
    }

    // if there is interposing, the apply interpose tuples to the dyld cache
    if ( !state.interposingTuplesAll.empty() ) {
        Loader::applyInterposingToDyldCache(state);
    }

    // optimize any function-variants in the dyld cache
    Loader::adjustFunctionVariantsInDyldCache(state);

#if SUPPORT_PREBUILTLOADERS
    // if mainLoader is prebuilt, there may be overrides of weak-defs in the dyld cache
    if ( mainLoader->isPrebuilt ) {
        DyldCacheDataConstLazyScopedWriter  dataConstWriter(state);
        DyldCacheDataConstLazyScopedWriter* dataConstWriterPtr = &dataConstWriter; // work around to make accessible in cacheWeakDefFixup
        state.processPrebuiltLoaderSet()->forEachCachePatch(^(const PrebuiltLoaderSet::CachePatch& patch) {
            uintptr_t newImpl = (uintptr_t)patch.patchTo.value(state);
            state.config.dyldCache.addr->forEachPatchableUseOfExport(patch.cacheDylibIndex, patch.cacheDylibVMOffset,
                                                                     ^(uint64_t cacheVMOffset,
                                                                       dyld3::MachOLoaded::PointerMetaData pmd, uint64_t addend,
                                                                       bool isWeakImport) {
                uintptr_t* loc      = (uintptr_t*)(((uint8_t*)state.config.dyldCache.addr) + cacheVMOffset);
                uintptr_t  newValue = newImpl + (uintptr_t)addend;
#if __has_feature(ptrauth_calls)
                if ( pmd.authenticated )
                    newValue = MachOLoaded::ChainedFixupPointerOnDisk::Arm64e::signPointer(newValue, loc, pmd.usesAddrDiversity, pmd.diversity, pmd.key);
#endif
                // ignore duplicate patch entries
                if ( *loc != newValue ) {
                    dataConstWriterPtr->makeWriteable();
                    if ( state.config.log.fixups )
                        state.log("cache patch: %p = 0x%0lX\n", loc, newValue);
                    *loc = newValue;
                }
            });
        });
    }
#endif // SUPPORT_PREBUILTLOADERS

    // call kdebug trace for each image
    if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
        // dyld in the cache event was sent earlier when we unmapped the on-disk dyld
        if ( !dyldMH->inDyldCache() ) {
            // add trace for dyld itself
            uuid_t dyldUuid;
            dyldMH->getUuid(dyldUuid);
            struct stat        stat_buf;
            fsid_t             dyldFsid    = { { 0, 0 } };
            fsobj_id_t         dyldFfsobjid = { 0, 0 };
            if ( dyld3::stat(state.config.process.dyldPath, &stat_buf) == 0 ) {
                dyldFfsobjid  = *(fsobj_id_t*)&stat_buf.st_ino;
                dyldFsid      = { { stat_buf.st_dev, 0 } };
            }
            dyld3::kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, state.config.process.dyldPath, &dyldUuid, dyldFfsobjid, dyldFsid, dyldMH, dyldMH->arch().cpuSubtype());
        }
    }
#endif // TARGET_OS_EXCLAVEKIT

    if ( state.libdyldLoader == nullptr )
        halt("libdyld.dylib not found");

    // wire up libdyld.dylib to dyld
    bool                        libdyldSetup = false;
    const Header*               libdyldHdr   = state.libdyldLoader->header(state);
    std::span<const uint8_t>    apiSection   = libdyldHdr->findSectionContent("__TPRO_CONST", "__dyld_apis", true/*vm layout*/);
    if ( apiSection.size() == sizeof(void*) ) {
        // set global variable in libdyld.dylib to point to dyld's global APIs object
        LibdyldAPIsSection* section = (LibdyldAPIsSection*)apiSection.data();
        section->apis = &state;
        libdyldSetup  = true;
    }

    // wire up dyld to libdyld.dylib
    bool                        dyldSetup     = false;
    std::span<const uint8_t>    helperSection = libdyldHdr->findSectionContent("__DATA_CONST", "__helper", true/*vm layout*/);
    if ( helperSection.size() == sizeof(void*) ) {
        LibdyldHelperSection* section = (LibdyldHelperSection*)helperSection.data();
        // set field in `state` object to point to LibSystemHelpers object in libdyld.dylib
        state.libSystemHelpers = { &section->helper, &MemoryManager::memoryManager() };
        dyldSetup = ( state.libSystemHelpers.version() >= 7 );
    }
    if ( !libdyldSetup || !dyldSetup ) {
        mach_o::Error err("'%s' not compatible with '%s'", state.libdyldLoader->path(state), state.config.process.dyldPath);
        halt(err.message());
    }

    // program vars (e.g. environ) are usually defined in libdyld.dylib (but might be defined in main excutable for old macOS binaries)
    state.libSystemHelpers.setDefaultProgramVars(state.vars);
    state.vars.mh             = state.config.process.mainExecutableMF;
    *state.vars.__prognamePtr = state.config.process.progname;
#if !TARGET_OS_EXCLAVEKIT
    *state.vars.NXArgcPtr     = state.config.process.argc;
    *state.vars.NXArgvPtr     = (const char**)state.config.process.argv;
    *state.vars.environPtr    = (const char**)state.config.process.envp;
#else
    // fill in the ExclaveKit parts of ProgramVars, to be passed to Libsystem's initializer
    state.vars.entry_vec      = state.config.process.entry_vec;
#endif
    if ( state.libSystemLoader == nullptr )
        halt("program does not link with libSystem.B.dylib");



#if !TARGET_OS_EXCLAVEKIT
    // split off delay loaded dylibs into delayLoaded vector
    // We have to do this before making the PrebuiltLoaderSet as objc in the closure needs
    // to know which shared cache dylibs are delay or not
    STACK_ALLOC_ARRAY(const Loader*, loadersTemp, state.loaded.size());
    for (const Loader* ldr : state.loaded)
        loadersTemp.push_back(ldr);
    std::span<const Loader*> allLoaders(&loadersTemp[0], (size_t)loadersTemp.count());
    std::span<const Loader*> topLoaders = allLoaders.subspan(0,(size_t)topCount);
    state.partitionDelayLoads(allLoaders, topLoaders);
    if ( !state.config.log.linksWith.empty() ) {
        for (const Loader* topLoader : topLoaders) {
            if ( topLoader->mf(state)->isMainExecutable() )
                topLoader->logChainToLinksWith(state, "main");
            else
                topLoader->logChainToLinksWith(state, "insert");
        }
    }

    // call kdebug trace for each image
    if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
        // add trace for each image loaded
        for ( const Loader* ldr :  state.loaded ) {
            const MachOLoaded* ml = ldr->loadAddress(state);
            fsid_t             fsid    = { { 0, 0 } };
            fsobj_id_t         fsobjid = { 0, 0 };
            struct stat        stat_buf;
            if ( !ldr->dylibInDyldCache && (dyld3::stat(ldr->path(state), &stat_buf) == 0) ) { //FIXME Loader knows inode
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid    = { { stat_buf.st_dev, 0 } };
            }

            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, ldr->path(state), &ldr->uuid, fsobjid, fsid, ml, ldr->cpusubtype);
        }
    }
#endif // TARGET_OS_EXCLAVEKIT

#if SUPPORT_ON_DISK_PREBUILTLOADERS
    // if launched with JustInTimeLoader, may need to serialize it
    if ( needToWritePrebuiltLoaderSet ) {
        dyld3::ScopedTimer timer(DBG_DYLD_TIMING_BUILD_CLOSURE, 0, 0, 0);
        if ( state.config.log.loaders )
            state.log("building PrebuiltLoaderSet for main executable\n");
        Diagnostics              prebuiltDiag;
        const PrebuiltLoaderSet* prebuiltAppSet = PrebuiltLoaderSet::makeLaunchSet(prebuiltDiag, state, missingPaths);
        if ( (prebuiltAppSet != nullptr) && prebuiltDiag.noError() ) {
            if ( state.failIfCouldBuildAppClosureFile() )
                halt("dyld: PrebuiltLoaderSet expected but not found");
            // save PrebuiltLoaderSet to disk for use by next launch, continue running with JustInTimeLoaders
            if ( state.saveAppPrebuiltLoaderSet(prebuiltAppSet) )
                state.setSavedPrebuiltLoaderSet();
            prebuiltAppSet->deallocate();
            timer.setData4(dyld3::DyldTimingBuildClosure::LaunchClosure_Built);
        }
        else if ( state.config.log.loaders ) {
            state.log("could not build PrebuiltLoaderSet: %s\n", prebuiltDiag.errorMessage());
        }
    }
    // if app launched to pre-warm, exit early
    if ( state.config.security.justBuildClosure ) {
        return &fake_main;
    }
#endif // SUPPORT_ON_DISK_PREBUILTLOADERS

    // update externally viewable list of images and tell lldb about loaded images
    {
        STACK_ALLOC_VECTOR(const Loader*, newLoaders, state.loaded.size());
        for (const Loader* ldr : state.loaded)
            newLoaders.push_back(ldr);

        // notify debugger about all loaded images after the main executable
        std::span<const Loader*> unnotifiedNewLoaders(&newLoaders[topCount], (size_t)(newLoaders.size()-topCount));
        state.notifyDebuggerLoad(unnotifiedNewLoaders);
#if !TARGET_OS_EXCLAVEKIT
        // notify kernel about any dtrace static user probes
        state.notifyDtrace(newLoaders);
#endif
    }

#if !SUPPPORT_PRE_LC_MAIN
    // run all initializers
    state.externallyViewable->notifyMonitorOfDyldBeforeInitializers();
    state.runAllInitializersForMain();
#else
    ProgramVars*    progVarsInApp       = nullptr;
    FuncLookup*     dyldLookupFuncAddr  = nullptr;
    bool            crtRunsInitializers = false;
    if ( hasProgramVars(state.config.process.mainExecutableHdr, progVarsInApp, crtRunsInitializers, dyldLookupFuncAddr) ) {
        // this is old macOS app which has its own NXArgv, etc global variables.  We need to use them.
        progVarsInApp->mh             = state.config.process.mainExecutableMF;
        *progVarsInApp->NXArgcPtr     = state.config.process.argc;
        *progVarsInApp->NXArgvPtr     = (const char**)state.config.process.argv;
        *progVarsInApp->environPtr    = (const char**)state.config.process.envp;
        *progVarsInApp->__prognamePtr = state.config.process.progname;
        state.vars                    = *progVarsInApp;
    }
    if ( dyldLookupFuncAddr )
        *dyldLookupFuncAddr = state.libSystemHelpers.legacyDyldFuncLookup();

    if ( !crtRunsInitializers )
        state.runAllInitializersForMain();
#endif // !SUPPPORT_PRE_LC_MAIN

    // notify we are about to call main
    state.externallyViewable->notifyMonitorOfMainCalled();

    void *result;

#if !TARGET_OS_EXCLAVEKIT
    if ( dyld3::kdebug_trace_dyld_enabled(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE) ) {
        dyld3::kdebug_trace_dyld_duration_end(launchTraceID, DBG_DYLD_TIMING_LAUNCH_EXECUTABLE, 0, 0, 0);
    }

    state.externallyViewable->setDyldState(dyld_process_state_program_running);
    ARIADNEDBG_CODE(220, 1);

    if ( state.config.security.skipMain ) {
        return &fake_main;
    }

    if ( state.config.process.platform == mach_o::Platform::driverKit ) {
        result = (void*)state.mainFunc();
        if ( result == 0 )
            halt("DriverKit main entry point not set");
#if __has_feature(ptrauth_calls)
        // DriverKit signs the pointer with a diversity different than dyld expects when calling the pointer.
        result = ptrauth_auth_and_resign(result, ptrauth_key_function_pointer, ptrauth_type_discriminator(void (*)(void)), ptrauth_key_function_pointer, 0);
#endif // __has_feature(ptrauth_calls)
        return (MainFunc)result;
    }
#endif // !TARGET_OS_EXCLAVEKIT

    // find entry point for main executable
    uint64_t entryOffset;
    bool     usesCRT;
    if ( !state.config.process.mainExecutableHdr->getEntry(entryOffset, usesCRT) ) {
        halt("main executable has no entry point");
    }
    result = (void*)((uintptr_t)state.config.process.mainExecutableMF + entryOffset);
    if ( usesCRT ) {
        // main executable uses LC_UNIXTHREAD, dyld needs to cut back kernel arg stack and jump to "start"
#if SUPPPORT_PRE_LC_MAIN
        // backsolve for KernelArgs (original stack entry point in _dyld_start)
        const KernelArgs* kernArgs = (KernelArgs*)(&state.config.process.argv[-2]);
        gotoAppStart((uintptr_t)result, kernArgs);
#else
        halt("main executable is missing LC_MAIN");
#endif
    }
#if __has_feature(ptrauth_calls)
        result = (void*)__builtin_ptrauth_sign_unauthenticated(result, 0, 0);
#endif

    return (MainFunc)result;
}


// SyscallDelegate object which is held onto by config object for life of process
constinit SyscallDelegate sSyscallDelegate;

#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT
static void getDyldPath(const char* apple[], char path[MAXPATHLEN], fsid_t& fsid, fsobj_id_t& fsobj_id) {
    const char* dyldFileIDString  = _simple_getenv(apple, "dyld_file");
    // kernel passes fsID and objID encoded as two hex values (e.g. 0x123,0x456)
    const char* endPtr  = nullptr;
    uint64_t fsID    = hexToUInt64(dyldFileIDString, &endPtr);
    if ( endPtr == nullptr ) {
        strlcpy(path, "/usr/lib/dyld", MAXPATHLEN);
        return;
    }
    uint64_t objID = hexToUInt64(endPtr+1, &endPtr);
    if ( endPtr == nullptr ) {
        strlcpy(path, "/usr/lib/dyld", MAXPATHLEN);
        return;
    }
    FileIdTuple dyldFileID(fsID, objID);
    if (!dyldFileID.getPath(path)) {
        strlcpy(path, "/usr/lib/dyld", MAXPATHLEN);
        return;
    }
    fsobj_id = *(fsobj_id_t*)&objID;
    fsid = *(fsid_t*)&fsID;
}

static ExternallyViewableState* handleDyldInCache(Allocator& allocator, const Header* dyldMH, const KernelArgs* kernArgs, const Header* prevDyldMH)
{
    char        dyldPath[MAXPATHLEN]    = {0};
    fsid_t      dyldFsId                = { { 0, 0 } };
    fsobj_id_t  dyldFsObjId             = { 0, 0 };
    getDyldPath(kernArgs->findApple(), dyldPath, dyldFsId, dyldFsObjId);
    const char* mainExecutablePath      = _simple_getenv(kernArgs->findApple(), "executable_path");;
    uint64_t    cacheBaseAddress;
    FileIdTuple cacheFileID;
    bool hasExistingCache = sSyscallDelegate.hasExistingDyldCache(cacheBaseAddress, cacheFileID);

    if ( dyldMH->inDyldCache() ) {
        // We need to drop the additional send right we got by calling task_self_trap() via mach_init() a second time
        mach_port_mod_refs(mach_task_self(), mach_task_self(), MACH_PORT_RIGHT_SEND, -1);
        ExternallyViewableState* result = nullptr;
        bool usingNewProcessInfo = false;
        MemoryManager::withWritableMemory([&] {
            result = new (allocator.aligned_alloc(alignof(ExternallyViewableState), sizeof(ExternallyViewableState))) ExternallyViewableState(allocator);
            usingNewProcessInfo = result->completeAllImageInfoTransition(allocator, (const dyld3::MachOFile*)dyldMH);
            // Create new minimal info. This replace the existing info and implicitly drop the original dyld and all entries pointing to it from the
            // all image info, which we need to do before we ecentually unmap the on disk dyld.
            result->createMinimalInfo(allocator, (uint64_t)dyldMH, "/usr/lib/dyld", (uint64_t)kernArgs->mainExecutable,
                                      mainExecutablePath, (const DyldSharedCache*)cacheBaseAddress);
        });

        // Instruments tracks mapped images.  dyld is considered mapped from the process info
        // but we now need to tell Instruments that we are unmapping the dyld its tracking.
        // Note there was no previous MAP event for dyld, just the process info

        if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_UNMAP_A)) ) {
            // add trace for unmapping dyld itself
            uuid_t dyldUuid;
            dyldMH->getUuid(dyldUuid);
            dyld3::kdebug_trace_dyld_image(DBG_DYLD_UUID_UNMAP_A, dyldPath, &dyldUuid, dyldFsObjId, dyldFsId, prevDyldMH, prevDyldMH->arch().cpuSubtype());
        }

        // We then need to tell Instruments that we have mapped a new dyld.
        // Note we really need to keep this adjacent to the unmap event above, as we don't want Instruments to see
        // code running in a memory range which is untracked.
        if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
            // add trace for dyld itself
            uuid_t dyldUuid;
            dyldMH->getUuid(dyldUuid);
            fsid_t             dyldFsid    = { { 0, 0 } };
            fsobj_id_t         dyldFfsobjid = { 0, 0 };
            dyld3::kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, "/usr/lib/dyld", &dyldUuid, dyldFfsobjid, dyldFsid,
                                           dyldMH, dyldMH->arch().cpuSubtype());
        }

        // unload disk based dyld now that we are running with one in the dyld cache
        struct Seg { void* start; size_t size; };
        STACK_ALLOC_ARRAY(Seg, segRanges, 16);
        uint64_t prevDyldSlide = ((MachOAnalyzer*)prevDyldMH)->getSlide();
        prevDyldMH->forEachSegment(^(const Header::SegmentInfo& info, bool& stop) {
            // don't unload  __DATA_DIRTY if still using the original dyld_all_image_infos
            if ( !usingNewProcessInfo && (info.segmentName == "__DATA_DIRTY") )
                return;
            void*   segStart = (void*)(long)(info.vmaddr+prevDyldSlide);
            size_t  segSize  = (size_t)info.vmsize;
            segRanges.push_back({segStart, segSize});
        });
        // we cannot unmap above because unmapping TEXT segment will crash forEachSegment(), do the unmap now
        for (const Seg& s : segRanges)
            ::munmap(s.start, s.size);

        return result;
    }
    else {
        ExternallyViewableState* result = nullptr;
        MemoryManager::withWritableMemory([&] {
            result = new (allocator.aligned_alloc(alignof(ExternallyViewableState), sizeof(ExternallyViewableState))) ExternallyViewableState(allocator);
            // Create an minimal atlas with dyld and the main executable
            result->createMinimalInfo(allocator, (uint64_t)dyldMH, dyldPath, (uint64_t)kernArgs->mainExecutable, mainExecutablePath, nullptr);
        });
  #if TARGET_OS_OSX
        // simulator programs do not use dyld-in-cache
        if ( ((Header*)kernArgs->mainExecutable)->builtForSimulator() )
            return result;
    #if SUPPORT_ROSETTA
        // rosetta translated processes don't use dyld-in-cache
        if ( sSyscallDelegate.isTranslated() )
             return result;
    #endif // SUPPORT_ROSETTA
  #endif // TARGET_OS_OSX

        // don't use dyld-in-cache with private dyld caches
        if ( _simple_getenv(kernArgs->findEnvp(), "DYLD_SHARED_REGION") != nullptr )
            return result;

        // check if this same dyld is in dyld cache
        uuid_t thisDyldUuid;
        if ( dyldMH->getUuid(thisDyldUuid) ) {
            if ( hasExistingCache ) {
                const DyldSharedCache* dyldCacheHeader = (DyldSharedCache*)(long)cacheBaseAddress;
                const DyldSharedCache::DynamicRegion* dynamicRegion = dyldCacheHeader->dynamicRegion();
                FileIdTuple fileTuple;
                if (dynamicRegion) {
                    dynamicRegion->getDyldCacheFileID(fileTuple);
                }
                uint64_t cacheSlide = dyldCacheHeader->slide();
                if ( dyldCacheHeader->header.dyldInCacheMH != 0 ) {
                    const Header* dyldInCacheMH = (Header*)(long)(dyldCacheHeader->header.dyldInCacheMH + cacheSlide);
                    uuid_t  dyldInCacheUuid;
                    bool    useDyldInCache = true;

                    // not the same dyld as in cache
                    if (!dyldInCacheMH->getUuid(dyldInCacheUuid) || ::memcmp(thisDyldUuid, dyldInCacheUuid, sizeof(uuid_t)) != 0 ) {
                        useDyldInCache = false;
                    }
                    // check for overrides
                    if (sSyscallDelegate.internalInstall()) {
                        const char* overrideStr = _simple_getenv(kernArgs->findEnvp(), "DYLD_IN_CACHE");
                        if ( overrideStr != nullptr ) {
                            if ( strcmp(overrideStr, "0") == 0 ) {
                                useDyldInCache = false;
                            } else if ( strcmp(overrideStr, "1") == 0 ) {
                                useDyldInCache = true;
                            }
                        }
                    }
                    if ( useDyldInCache ) {
                        MemoryManager::withWritableMemory([&] {
                            // We are using dyld in the cache, update the atlas to use the new dyld
                            result->createMinimalInfo(allocator, (uint64_t)dyldMH, dyldPath, (uint64_t)kernArgs->mainExecutable,
                                                      mainExecutablePath, (const DyldSharedCache*)cacheBaseAddress);
                        });
                        // update all_image_info in case lldb attaches during transition
                        result->prepareInCacheDyldAllImageInfos(dyldInCacheMH);
                         // Tell Instruments we have a shared cache before we start using an image in the cache
                         dyld3::kdebug_trace_dyld_cache(fileTuple.inode(), fileTuple.fsID(), cacheBaseAddress,
                                                        dyldCacheHeader->header.uuid);
                        // cut back stack and restart but using dyld in the cache
                        // cut back stack and restart but using dyld in the cache
                        restartWithDyldInCache(kernArgs, dyldMH, dyldCacheHeader, (void*)(long)(dyldCacheHeader->header.dyldInCacheEntry + cacheSlide));
                    }
                }
            }
        }
        return result;
    }
}
#endif // !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT

static void rebaseSelf(const MachOAnalyzer* dyldMA)
{
    assert(dyldMA->hasChainedFixups());
    // Note: withChainStarts() and fixupAllChainedFixups() cannot use any static DATA pointers as they are not rebased yet
    uintptr_t slide = dyldMA->getSlide();
    __block Diagnostics diag;
    dyldMA->withChainStarts(diag, 0, ^(const dyld_chained_starts_in_image* starts) {
        dyldMA->fixupAllChainedFixups(diag, starts, slide, dyld3::Array<const void*>(), nullptr);
    });
    diag.assertNoError();

#if !TARGET_OS_EXCLAVEKIT
    // make __DATA_CONST read-only (kernel maps it r/w)
    const Header* dyldMH = (const Header*)dyldMA;
    dyldMH->forEachSegment(^(const Header::SegmentInfo& segInfo, bool& stop) {
        if ( segInfo.readOnlyData() ) {
            const uint8_t* start = (uint8_t*)(segInfo.vmaddr + slide);
            size_t         size  = (size_t)segInfo.vmsize;
            sSyscallDelegate.mprotect((void*)start, size, PROT_READ);
        }
    });
#endif
}

// Do any set up needed by any linked static libraries

// This function sets the value in the stack canary, which means the compiler actually adds a canary check it will fail, as will any function that calls
// this function. We need to specifically anotate it to guarantee a stack protector is not used.
__attribute__ ((no_stack_protector))
static void initializeLibc(KernelArgs* kernArgs, void* dyldSharedCache) __attribute__((no_stack_protector))
{
#if TARGET_OS_EXCLAVEKIT
    MemoryManager::init();
    xrt__entry_args_t args = {
            .launched_roottask = 0,
    };
    plat_common_parse_entry_vec((xrt__entry_vec_t *)kernArgs->entry_vec, &args);
    bootinfo_init(args.bootinfo_virt);
    kernArgs->mappingDescriptor = (const void*)args.dyld_mapping_descriptor;
    kernArgs->dyldSharedCacheEnabled = (args.dyld_props.shared_cache_flags == XRT__ENTRY_VEC_EKIT_SHARED_CACHE_ENABLED);

    // set up stack canary
    _liblibc_stack_guard_init();

    // initialize secure runtime bits
    _secure_runtime_init();
#else
    mach_init();

    // set up random value for stack canary
    const char** apple = kernArgs->findApple();


    // FIXME: Refactor this to be cleaner
    // We intialize the memory manager here even though it is not technically part of libc, because we need
    // to do it after mach_init() is run, but before we setup the stack guards.
    MemoryManager::init((const char**)kernArgs->findEnvp(), apple, dyldSharedCache);

    // TPRO memory is RO at this point, so make it RW so that we can set the __stack_chk_guard
    MemoryManager::withWritableMemory([&] {
        __guard_setup(apple);
    });

    // setup so that open_with_subsystem() works
    _subsystem_init(apple);
#endif // TARGET_OS_EXCLAVEKIT
}

//
// Entry point for dyld.  The kernel loads dyld and jumps to __dyld_start which
// sets up some registers and call this function.
//
// For ExclaveKit, ExclavePlatform jumps to __dyld_start (defined in a crt0),
// creates the entry vector containing the type and value of arguments passed by the launcher,
// and calls this function with the entry vector as argument. The function does not call main,
// nor exit, but finalize_process_startup, which never returns.
//
// Note: this function never returns, it calls exit().  Therefore stack protectors
// are useless, since the epilog is never executed.  Marking the fucntion no-return
// disable the stack protector.  The stack protector was also causing problems
// with armv7k codegen since it access the random value through a GOT slot in the
// prolog, but dyld is not rebased yet.
//

#if !TARGET_OS_SIMULATOR
__attribute__ ((no_stack_protector))
void start(KernelArgs* kernArgs, void* prevDyldMH, void* dyldSharedCache) __attribute__((__noreturn__)) __asm("start");
void start(KernelArgs* kernArgs, void* prevDyldMH, void* dyldSharedCache)
{
    // Emit kdebug tracepoint to indicate dyld bootstrap has started <rdar://46878536>
#if !TARGET_OS_EXCLAVEKIT
    // Note: this is called before dyld is rebased, so kdebug_trace_dyld_marker() cannot use any global variables
    dyld3::kdebug_trace_dyld_marker(DBG_DYLD_TIMING_BOOTSTRAP_START, 0, 0, 0, 0);
#endif // !TARGET_OS_EXCLAVEKIT

    // walk all fixups chains and rebase dyld
    const MachOAnalyzer* dyldMA = getDyldMH();
    if ( !dyldMA->inDyldCache() ) {
        rebaseSelf(dyldMA);

        // zero out the parameters that should be null here, just in case they weren't
        prevDyldMH = nullptr;
        dyldSharedCache = nullptr;
    }

#if TARGET_OS_EXCLAVEKIT
    KernelArgs actualKernelArgs = {
        .entry_vec = (xrt__entry_vec_t *)kernArgs,
        .mappingDescriptor = nullptr,
    };
    kernArgs = &actualKernelArgs;
#endif
    // Do any set up needed by any linked static libraries
    initializeLibc(kernArgs, dyldSharedCache);


    Allocator&      allocator   = MemoryManager::defaultAllocator();

#if !TARGET_OS_EXCLAVEKIT
    // handle switching to dyld in dyld cache for native platforms
    // The externally viewable state is setup in handleDyldInCache, since that is where we find out if there is already state setup from the bootstrap dyld
    ExternallyViewableState* externalState = handleDyldInCache(allocator, (Header*)dyldMA, kernArgs, (Header*)prevDyldMH);
#else
    ExternallyViewableState* externalState = nullptr;
    MemoryManager::withWritableMemory([&] {
        externalState = new (allocator.aligned_alloc(alignof(ExternallyViewableState), sizeof(ExternallyViewableState))) ExternallyViewableState(allocator);
    });

    uint32_t* data = (uint32_t*)kernArgs->mappingDescriptor;
    data++;
    uintptr_t mainExecutableAddr;
    memcpy(&mainExecutableAddr, data, sizeof(mainExecutableAddr));
    data += sizeof(mainExecutableAddr);
    uintptr_t size;
    memcpy(&size, data, sizeof(size));
    data += sizeof(size);
    const char *mainExecutablePath = (const char *)data;

    externalState->createMinimalInfo(allocator, (uint64_t)dyldMA, "/usr/lib/dyld", (uint64_t)mainExecutableAddr, mainExecutablePath, nullptr);
#endif // !TARGET_OS_EXCLAVEKIT
    
#if SUPPPORT_PRE_LC_MAIN
    // old macOS binaries reset the stack and jump into crt1.o glue, so RuntimeLocks cannot be stack allocated
    // we cannot use "static RuntimeLocks locks;" because the compiler will generate an initializer or guards
    static uint8_t sLocksStaticStorage[sizeof(RuntimeLocks)] __attribute__((aligned(alignof(RuntimeLocks))));
    RuntimeLocks& locks = *new (sLocksStaticStorage) RuntimeLocks();
#else
    // stack allocate RuntimeLocks. They cannot be in the Allocator pool because the pool is usually read-only
    RuntimeLocks locks;
#endif // SUPPPORT_PRE_LC_MAIN

    // Declare everything we need outside of the allocator scope
    APIs*           state       = nullptr;
    MainFunc        appMain     = nullptr;

    MemoryManager::withWritableMemory([&] {
        allocator.setBestFit(true);
        // use placement new to construct ProcessConfig object in the Allocator pool
        ProcessConfig& config  = *new (allocator.aligned_alloc(alignof(ProcessConfig), sizeof(ProcessConfig))) ProcessConfig(kernArgs, sSyscallDelegate, allocator);
        // create APIs (aka RuntimeState) object in the allocator
        state = new (allocator.aligned_alloc(alignof(APIs), sizeof(APIs))) APIs(config, locks, allocator);
        MemoryManager::memoryManager().setDyldCacheAddr((void*)state->config.dyldCache.addr);
        MemoryManager::memoryManager().setProtectedStack(state->protectedStack());
        // set initial state for ExternallyViewableState
        state->externallyViewable = externalState;
        state->externallyViewable->setRuntimeState(state);

        // load all dependents of program and bind them together
        appMain = prepare(*state, (const Header*)dyldMA);
    });

#if TARGET_OS_EXCLAVEKIT
    // inform liblibc_plat that all static initializers have run and let it finalize the process startup
    state->vars.finalize_process_startup(appMain);

    // if we get here, finalize_process_startup returned (it's not supposed to)
    halt("finalize_process_startup wrongly returned");
#else
    // call main() and if it returns, call exit() with the result
    // Note: this is organized so that a backtrace in a program's main thread shows just "start" below "main"
    int result = appMain(state->config.process.argc, state->config.process.argv, state->config.process.envp, state->config.process.apple);

    // if we got here, main() returned (as opposed to program calling exit())
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // <rdar://74518676> libSystemHelpers is not set up for simulators, so directly call _exit()
    if ( state->config.process.platform.isSimulator() )
        _exit(result);
#endif // TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    state->libSystemHelpers.exit(result);
#endif // TARGET_OS_EXCLAVEKIT
}
#endif // !TARGET_OS_SIMULATOR

} // namespace

#if TARGET_OS_SIMULATOR
using namespace dyld4;


// glue to handle if main() in simulator program returns
// if _dyld_sim_prepare() returned main() then main() would return
// to the host dyld, which would be unable to run termination functions
// (e.g atexit()) in the simulator environment.  So instead, we wrap
// main() in start_sim() which can call simualtors exit() is main returns.
static APIs*    sAPIsForExit = nullptr;
static MainFunc sRealMain = nullptr;
static int start_sim(int argc, const char* const argv[], const char* const envp[], const char* const apple[]) __asm("start_sim");
static int start_sim(int argc, const char* const argv[], const char* const envp[], const char* const apple[])
{
    int result = sRealMain(argc, argv, envp, apple);
    sAPIsForExit->libSystemHelpers.exit(result);
    return 0;
}

extern "C" MainFunc _dyld_sim_prepare(int argc, const char* argv[], const char* envp[], const char* apple[],
                                      const mach_header* mainExecutableMH, const MachOAnalyzer* dyldMA, uintptr_t dyldSlide,
                                      const dyld::SyscallHelpers*, uintptr_t* startGlue);

__attribute__ ((no_stack_protector))
MainFunc _dyld_sim_prepare(int argc, const char* argv[], const char* envp[], const char* apple[],
                           const mach_header* mainExecutableMH, const MachOAnalyzer* dyldMA, uintptr_t dyldSimSlide,
                           const dyld::SyscallHelpers* sc, uintptr_t* startGlue)
{
    // save table of syscall pointers
    gSyscallHelpers = sc;

    // walk all fixups chains and rebase dyld_sim and make DATA_CONST r/o
    rebaseSelf(dyldMA);

    // back solve for KernelArgs because host dyld does not pass it
    KernelArgs* kernArgs = (KernelArgs*)(((uint8_t*)argv) - 2 * sizeof(void*));

    // before dyld4, the main executable mach_header was removed from the stack
    // so we need to force it back to allow KernelArgs to work like non-simulator processes
    // FIXME: remove when sims only run on dyld4 based macOS hosts
    kernArgs->mainExecutable = (MachOAnalyzer*)mainExecutableMH;

    // Do any set up needed by any linked static libraries
    initializeLibc(kernArgs, nullptr);

    // we cannot use "static RuntimeLocks locks;" because the compiler will generate an initializer or guards
    static uint8_t sLocksStaticStorage[sizeof(RuntimeLocks)] __attribute__((aligned(alignof(RuntimeLocks))));
    RuntimeLocks& locks = *new (sLocksStaticStorage) RuntimeLocks();

    // Declare everything we need outside of the allocator scope
    Allocator& allocator = MemoryManager::memoryManager().defaultAllocator();

    // set initial state for ExternallyViewableState
    ExternallyViewableState* externalState = nullptr;
    MemoryManager::withWritableMemory([&] {
        externalState = new (allocator.aligned_alloc(alignof(ExternallyViewableState), sizeof(ExternallyViewableState))) ExternallyViewableState(allocator, sc);
    });

    // create APIs (aka RuntimeState) object in the allocator
    APIs* state = nullptr;

    // function pointer that will be set to the entry point. Declare it here so the value can escape from withWritableMemory()
    MainFunc result = nullptr;
    MemoryManager::withWritableMemory([&] {
        allocator.setBestFit(true);

        // use placement new to construct ProcessConfig object in the Allocator pool
        ProcessConfig& config = *new (allocator.aligned_alloc(alignof(ProcessConfig), sizeof(ProcessConfig))) ProcessConfig(kernArgs, sSyscallDelegate, allocator);

        state = new (allocator.aligned_alloc(alignof(APIs), sizeof(APIs))) APIs(config, locks, allocator);

        // now that allocator is up, we can update image list
        // set initial state for ExternallyViewableState
        state->externallyViewable = externalState;
        state->externallyViewable->setRuntimeState(state);

        // load all dependents of program and bind them together, then return address of main()
        result = prepare(*state, (const Header*)dyldMA);
    });

    // return fake main, which calls real main() then simulator exit()
    *startGlue   = 1;  // means result is pointer to main(), as opposed to crt1.o entry
    sRealMain    = result;
    sAPIsForExit = state;
    return &start_sim;
}
#endif // TARGET_OS_SIMULATOR
