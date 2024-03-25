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
#include "ProcessAtlas.h"
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
using lsl::EphemeralAllocator;

#if TARGET_OS_EXCLAVEKIT
  extern "C" void bootinfo_init(uintptr_t bootinfo);
  extern "C" void plat_common_parse_entry_vec(xrt__entry_vec_t vec[10], xrt__entry_args_t *args);
  extern "C" void _liblibc_stack_guard_init(void);
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
extern void gotoAppStart(uintptr_t start, const KernelArgs* kernArgs) __attribute__((noreturn));
#endif

// this is defined in dyldStartup.s
void restartWithDyldInCache(const KernelArgs* kernArgs, const MachOFile* dyldOnDisk, void* dyldStart);

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
    sHostState->externallyViewable.coresymbolication_load_notifier(connection, timestamp, path, mh);
}

static void sim_coresymbolication_unload_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh)
{
    sHostState->externallyViewable.coresymbolication_unload_notifier(connection, timestamp, path, mh);
}

static void sim_notifyMonitorOfImageListChanges(bool unloading, unsigned imageCount, const mach_header* loadAddresses[], const char* imagePaths[])
{
    sHostState->externallyViewable.notifyMonitorOfImageListChangesSim(unloading, imageCount, loadAddresses, imagePaths);
}

static void sim_notifyMonitorOfMainCalled()
{
    sHostState->externallyViewable.notifyMonitorOfMainCalled();
}

static void sim_notifyMonitorOfDyldBeforeInitializers()
{
    sHostState->externallyViewable.notifyMonitorOfDyldBeforeInitializers();
}

// These are syscalls that the macOS dyld makes available to dyld_sim
static const dyld::SyscallHelpers sSysCalls = {
    17,
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
    const GradedArchs&   archs        = GradedArchs::forCurrentOS(state.config.process.mainExecutable, false);
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
    if ( !sliceMapping->hasCodeSignature(codeSigFileOffset, codeSigSize) )
        halt("dyld_sim is not code signed");

    // register code signature with kernel before mmap()ing segments
    fsignatures_t siginfo;
    siginfo.fs_file_start = fileOffset;                       // start of mach-o slice in fat file
    siginfo.fs_blob_start = (void*)(long)(codeSigFileOffset); // start of code-signature in mach-o file
    siginfo.fs_blob_size  = codeSigSize;                      // size of code-signature
    int result            = fcntl(fd, F_ADDFILESIGS_FOR_DYLD_SIM, &siginfo);
    if ( result == -1 ) {
        halt("dyld_sim fcntl(F_ADDFILESIGS_FOR_DYLD_SIM) failed");
    }
    // file range covered by code signature must extend up to code signature itself
    if ( siginfo.fs_file_start < codeSigFileOffset )
        halt("dyld_sim code signature does not cover all of dyld_sim");

    // reserve space, then mmap each segment
    const uint64_t mappedSize                  = sliceMapping->mappedSize();
    uint64_t       dyldSimPreferredLoadAddress = sliceMapping->preferredLoadAddress();
    vm_address_t   dyldSimLoadAddress          = 0;
    if ( ::vm_allocate(mach_task_self(), &dyldSimLoadAddress, (vm_size_t)mappedSize, VM_FLAGS_ANYWHERE) != 0 )
        halt("dyld_sim cannot allocate space");
    __block const char* mappingStr = nullptr;
    sliceMapping->forEachSegment(^(const MachOAnalyzer::SegmentInfo& info, bool& stop) {
        // <rdar://problem/100180105> Mapping zero filled regions fails with mmap of size 0
        if ( info.fileSize == 0)
            return;

        uintptr_t requestedLoadAddress = (uintptr_t)(info.vmAddr - dyldSimPreferredLoadAddress + dyldSimLoadAddress);
        void*     segAddress           = ::mmap((void*)requestedLoadAddress, (size_t)info.fileSize, info.protections, MAP_FIXED | MAP_PRIVATE, fd, fileOffset + info.fileOffset);
        //state.log("dyld_sim %s mapped at %p\n", seg->segname, segAddress);
        if ( segAddress == (void*)(-1) ) {
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

    // walk newly mapped dyld_sim __TEXT load commands to find entry point
    uint64_t entryOffset;
    bool     usesCRT;
    if ( !((MachOAnalyzer*)dyldSimLoadAddress)->getEntry(entryOffset, usesCRT) )
        halt("dyld_sim entry not found");

    // save off host state object for use later if dyld_sim calls back into host to notify
    sHostState = &state;

    // add dyld_sim to the image list for the debugger to see
    EphemeralAllocator ephemeralAllocator;
    FileRecord dyldSimFile = state.fileManager.fileRecordForStat(sb);
    state.externallyViewable.addImageInfo(ephemeralAllocator, {state.fileManager.fsidForUUID(dyldSimFile.volume()), dyldSimFile.objectID(), state.persistentAllocator.strdup(dyldSimPath), (void*)dyldSimLoadAddress});

	// <rdar://problem/5077374> have host dyld detach macOS shared cache from process before jumping into dyld_sim
    if ( state.config.log.segments )
        console("deallocating host dyld shared cache\n");
    dyld3::deallocateExistingSharedCache();
    state.externallyViewable.detachFromSharedRegion();

    // call kdebug trace for each image
    if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
        // add trace for dyld_sim itself
        uuid_t dyldUuid;
        ((MachOAnalyzer*)dyldSimLoadAddress)->getUuid(dyldUuid);
        fsid_t             dyldFsid    = { { sb.st_dev, 0 } };
        fsobj_id_t         dyldFfsobjid = *(fsobj_id_t*)&sb.st_ino;
        dyld3::kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, dyldSimPath, &dyldUuid, dyldFfsobjid, dyldFsid, (mach_header*)dyldSimLoadAddress);
    }

    //TODO: Remove once drop support for simulators older than iOS 17, tvOS 15, and watchOS 8
    // Old simulators do not correctly fill out the private cache fields in the all_image_info, so do it for them
    __block bool setSimulatorSharedCachePath = false;
    // Old simulators add the main executable to all_image_info in the simulator process, not in the host
    __block bool removeMainExec = false;
    ((dyld3::MachOFile*)dyldSimLoadAddress)->forEachSupportedPlatform(^(dyld3::Platform platform, uint32_t minOS, uint32_t sdk) {
        switch ( platform ) {
            case dyld3::Platform::iOS:
            case dyld3::Platform::tvOS:
            case dyld3::Platform::iOS_simulator:
            case dyld3::Platform::tvOS_simulator:
                if ( minOS <= 0x000F0000 )  // iOS 15.0
                    setSimulatorSharedCachePath = true;
                if ( minOS <= 0x00100000 )  // iOS 16.0
                    removeMainExec = true;
                break;
            case dyld3::Platform::watchOS:
            case dyld3::Platform::watchOS_simulator:
                if ( minOS <= 0x00080000 )  // watchOS 8.0
                    setSimulatorSharedCachePath = true;
                if ( minOS <= 0x00090000 )  // watchOS 9.0
                    removeMainExec = true;
                break;
            default: break;
        }
    });

    if ( removeMainExec ) {
        STACK_ALLOC_ARRAY(const mach_header*, mhs, 1);
        mhs.push_back(state.config.process.mainExecutable);
        std::span<const mach_header*> mhSpan(&mhs[0], 1);
        state.externallyViewable.removeImages(state.persistentAllocator, ephemeralAllocator, mhSpan);
    }

    if (setSimulatorSharedCachePath) {
        struct stat cacheStatBuf;
        char cachePath[PATH_MAX];
        const char* cacheDir = state.config.process.environ("DYLD_SHARED_CACHE_DIR");
        if (cacheDir) {
            strlcpy(cachePath, cacheDir, PATH_MAX);
            strlcat(cachePath, "/dyld_sim_shared_cache_", PATH_MAX);
            strlcat(cachePath, ((dyld3::MachOFile*)dyldSimLoadAddress)->archName(), PATH_MAX);
            if (state.config.syscall.stat(cachePath, &cacheStatBuf) == 0) {
                state.externallyViewable.setSharedCacheInfo(ephemeralAllocator, 0, {(uint64_t)cacheStatBuf.st_dev, (uint64_t)cacheStatBuf.st_ino, nullptr, nullptr}, true);
            }
        }
    }
    state.externallyViewable.commit(state.persistentAllocator, ephemeralAllocator);

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
                      state.config.process.mainExecutable, (mach_header*)dyldSimLoadAddress,
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

//
// Load any dependent dylibs and bind all together.
// Returns address of main() in target.
//
__attribute__((noinline)) static MainFunc prepare(APIs& state, const MachOAnalyzer* dyldMH)
{
#if TARGET_OS_EXCLAVEKIT
    Diagnostics diag;
    Loader* mainLoader = PremappedLoader::makeLaunchLoader(diag, state, state.config.process.mainExecutable, state.config.process.mainExecutablePath, nullptr);
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
        state.log("%s loading dependents of %s\n", depsDiag.errorMessage(), mainLoader->path());
        // let crashreporter know about dylibs we were able to load
        halt(depsDiag.errorMessage(), &state.structuredError);
    }

    // notify debugger about all loaded images after the main executable
    STACK_ALLOC_VECTOR(const Loader*, newLoaders, state.loaded.size());
    for (const Loader* ldr : state.loaded)
        newLoaders.push_back(ldr);
    std::span<const Loader*> unnotifiedNewLoaders(&newLoaders[0], newLoaders.size());
    state.notifyDebuggerLoad(unnotifiedNewLoaders);

    // do fixups
    DyldCacheDataConstLazyScopedWriter  cacheDataConst(state);
    for ( const Loader* ldr : state.loaded ) {
        Diagnostics fixupDiag;
        ldr->applyFixups(fixupDiag, state, cacheDataConst, true);
        if ( fixupDiag.hasError() ) {
            halt(fixupDiag.errorMessage());
        }
    }
#else
    uint64_t launchTraceID = 0;
    if ( dyld3::kdebug_trace_dyld_enabled(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE) ) {
        launchTraceID = dyld3::kdebug_trace_dyld_duration_start(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE, (uint64_t)state.config.process.mainExecutable, 0, 0);
    }

    // if DYLD_PRINT_SEARCHING is used, be helpful and list stuff that is disabled
    if ( state.config.log.searching ) {
        if ( !state.config.security.allowEnvVarsPrint )
            state.log("Note: DYLD_PRINT_* disabled by AMFI\n");
        if ( !state.config.security.allowInterposing )
            state.log("Note: interposing disabled by AMFI\n");
   }

#if TARGET_OS_OSX
    const bool isSimulatorProgram = MachOFile::isSimulatorPlatform(state.config.process.platform);
    if ( const char* simPrefixPath = state.config.pathOverrides.simRootPath() ) {
#if __arm64e__
        if ( strcmp(state.config.process.mainExecutable->archName(), "arm64e") == 0 )
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
        state.loaded.reserve(state.initialImageCount()); // help avoid reallocations of Vector
    }
#endif // SUPPORT_PREBUILTLOADERS
    if ( mainLoader == nullptr ) {
        // if no pre-built Loader, make a just-in-time one
        state.loaded.reserve(512);  // guess starting point for Vector size
        Diagnostics buildDiag;
        mainLoader = JustInTimeLoader::makeLaunchLoader(buildDiag, state, state.config.process.mainExecutable,
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
    Loader::LoadChain   loadChainMain { nullptr, mainLoader };

    Loader::LoadOptions options;
    options.staticLinkage   = true;
    options.launching       = true;
    options.insertedDylib   = true;
    options.canBeDylib      = true;
    options.rpathStack      = &loadChainMain;
    state.config.pathOverrides.forEachInsertedDylib(^(const char* dylibPath, bool& stop) {
        Diagnostics insertDiag;
        if ( Loader* insertedDylib = (Loader*)Loader::getLoader(insertDiag, state, dylibPath, options) ) {
            // Make sure we haven't already loaded this dylib
            // This can happen if we have 2 of the same dylib in the DYLD_INSERT_LIBRARIES, or
            // if we have 2 different libraries with the same leaf name and DYLD_FALLBACK_LIBRARY_PATH
            // ends up with them resolving to the same dylib
            for ( const Loader* ldr : topLevelLoaders ) {
                if ( ldr == insertedDylib ) {
                    if ( state.config.log.libraries )
                        state.log("skipping duplicate inserted dylib '%s'\n", dylibPath);
                    return;
                }
            }
            topLevelLoaders.push_back(insertedDylib);
            state.notifyDebuggerLoad(insertedDylib);
            if ( insertedDylib->isPrebuilt )
                state.add(insertedDylib);
        }
        else if ( insertDiag.hasError() && !state.config.security.allowInsertFailures  ) {
            state.log("terminating because inserted dylib '%s' could not be loaded: %s\n", dylibPath, insertDiag.errorMessageCStr());
            halt(insertDiag.errorMessage());
        }
    });

    // move inserted libraries ahead of main executable in state.loaded, for correct flat namespace lookups
    if ( topLevelLoaders.count() != 1 ) {
        state.loaded.erase(state.loaded.begin());
        state.loaded.push_back(mainLoader);
    }

#if SUPPORT_PREBUILTLOADERS
    // for recording files that must be missing
    __block MissingPaths missingPaths;
    auto missingLogger = ^(const char* mustBeMissingPath) {
        missingPaths.addPath(mustBeMissingPath);
    };
#endif

    // recursively load everything needed by main executable and inserted dylibs
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
            state.externallyViewable.disableCrashReportBacktrace();
            halt(depsDiag.errorMessage(), &state.structuredError);
        }
    }

    uint64_t topCount = topLevelLoaders.count();
    {
        STACK_ALLOC_VECTOR(const Loader*, newLoaders, state.loaded.size());
        for (const Loader* ldr : state.loaded)
            newLoaders.push_back(ldr);

        // notify debugger about all loaded images after the main executable
        std::span<const Loader*> unnotifiedNewLoaders(&newLoaders[topCount], (size_t)(newLoaders.size()-topCount));
        state.notifyDebuggerLoad(unnotifiedNewLoaders);

        // notify kernel about any dtrace static user probes
        state.notifyDtrace(newLoaders);
    }

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
            ldr->applyFixups(fixupDiag, state, cacheDataConst, true);
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
            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, state.config.process.dyldPath, &dyldUuid, dyldFfsobjid, dyldFsid, dyldMH);
        }

        // add trace for each image loaded
        for ( const Loader* ldr :  state.loaded ) {
            const MachOLoaded* ml = ldr->loadAddress(state);
            fsid_t             fsid    = { { 0, 0 } };
            fsobj_id_t         fsobjid = { 0, 0 };
            struct stat        stat_buf;
            if ( !ldr->dylibInDyldCache && (dyld3::stat(ldr->path(), &stat_buf) == 0) ) { //FIXME Loader knows inode
                fsobjid = *(fsobj_id_t*)&stat_buf.st_ino;
                fsid    = { { stat_buf.st_dev, 0 } };
            }
            uuid_t uuid;
            ml->getUuid(uuid);
            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, ldr->path(), &uuid, fsobjid, fsid, ml);
        }
    }
#endif // TARGET_OS_EXCLAVEKIT

    if ( state.libdyldLoader == nullptr )
        halt("libdyld.dylib not found");

    // wire up libdyld.dylib to dyld
    const MachOLoaded* libdyldML = state.libdyldLoader->loadAddress(state);
    uint64_t           sectSize;
    LibdyldDyld4Section* libdyld4Section = (LibdyldDyld4Section*)libdyldML->findSectionContent("__DATA", "__dyld4", sectSize, true);
#if __has_feature(ptrauth_calls)
    if ( libdyld4Section == nullptr )
        libdyld4Section = (LibdyldDyld4Section*)libdyldML->findSectionContent("__AUTH", "__dyld4", sectSize, true);
#endif
    if ( libdyld4Section == nullptr )
        halt("compatible libdyld.dylib not found");

    // set pointer to global APIs object
    libdyld4Section->apis = &state;
    state.externallyViewable.storeProcessInfoPointer((dyld_all_image_infos**)&libdyld4Section->allImageInfos); // FIXME: only needed for dyld_process_info_base::make() to find dyld cache
    // program vars (e.g. environ) are usually defined in libdyld.dylib (but might be defined in main excutable for old macOS binaries)
    // remember location of progams vars so libc can sync them
    state.vars                 = &libdyld4Section->defaultVars;
    state.vars->mh             = state.config.process.mainExecutable;
    *state.vars->__prognamePtr = state.config.process.progname;
#if !TARGET_OS_EXCLAVEKIT
    *state.vars->NXArgcPtr     = state.config.process.argc;
    *state.vars->NXArgvPtr     = (const char**)state.config.process.argv;
    *state.vars->environPtr    = (const char**)state.config.process.envp;
#else
    // fill in the ExclaveKit parts of ProgramVars, to be passed to Libsystem's initializer
    state.vars->entry_vec      = state.config.process.entry_vec;
#endif
    if ( state.libSystemLoader == nullptr )
        halt("program does not link with libSystem.B.dylib");
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

#if !TARGET_OS_EXCLAVEKIT
    // split off delay loaded dylibs into delayLoaded vector
    Vector<const Loader*> undelayedLoaders(state.persistentAllocator);
    STACK_ALLOC_ARRAY(const Loader*, loadersTemp, state.loaded.size());
    for (const Loader* ldr : state.loaded)
        loadersTemp.push_back(ldr);
    std::span<const Loader*> allLoaders(&loadersTemp[0], (size_t)loadersTemp.count());
    state.partitionDelayLoads(allLoaders, allLoaders.subspan(0,(size_t)topCount), undelayedLoaders);
#endif

#if !SUPPPORT_PRE_LC_MAIN
    // run all initializers
#if !TARGET_OS_EXCLAVEKIT
    if ( state.externallyViewable.notifyMonitorNeeded() )
        state.externallyViewable.notifyMonitorOfDyldBeforeInitializers();
#endif // !TARGET_OS_EXCLAVEKIT
    state.runAllInitializersForMain();
#else
    uint32_t                progVarsOffset;
    dyld3::DyldLookFunc*    dyldLookupFuncAddr = nullptr;
    bool                    crtRunsInitializers = false;
    if ( state.config.process.mainExecutable->hasProgramVars(progVarsOffset, crtRunsInitializers, dyldLookupFuncAddr) ) {
        // this is old macOS app which has its own NXArgv, etc global variables.  We need to use them.
        ProgramVars* varsInApp    = (ProgramVars*)(((uint8_t*)state.config.process.mainExecutable) + progVarsOffset);
        varsInApp->mh             = state.config.process.mainExecutable;
        *varsInApp->NXArgcPtr     = state.config.process.argc;
        *varsInApp->NXArgvPtr     = (const char**)state.config.process.argv;
        *varsInApp->environPtr    = (const char**)state.config.process.envp;
        *varsInApp->__prognamePtr = state.config.process.progname;
        state.vars                = varsInApp;
    }
    if ( dyldLookupFuncAddr ) {
        if ( libdyld4Section == nullptr )
            halt("compatible libdyld.dylib not found");
        *dyldLookupFuncAddr = (dyld3::DyldLookFunc)libdyld4Section->dyldLookupFuncAddr;
    }

    if ( !crtRunsInitializers )
        state.runAllInitializersForMain();
#endif // !SUPPPORT_PRE_LC_MAIN


#if !TARGET_OS_EXCLAVEKIT
    // notify we are about to call main
    if ( state.externallyViewable.notifyMonitorNeeded() )
        state.externallyViewable.notifyMonitorOfMainCalled();
#endif // !TARGET_OS_EXCLAVEKIT

    void *result;

#if !TARGET_OS_EXCLAVEKIT
    if ( dyld3::kdebug_trace_dyld_enabled(DBG_DYLD_TIMING_LAUNCH_EXECUTABLE) ) {
        dyld3::kdebug_trace_dyld_duration_end(launchTraceID, DBG_DYLD_TIMING_LAUNCH_EXECUTABLE, 0, 0, 4);
    }

    ARIADNEDBG_CODE(220, 1);

    if ( state.config.security.skipMain ) {
        return &fake_main;
    }

    if ( state.config.process.platform == dyld3::Platform::driverKit ) {
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
    if ( !state.config.process.mainExecutable->getEntry(entryOffset, usesCRT) ) {
        halt("main executable has no entry point");
    }
    result = (void*)((uintptr_t)state.config.process.mainExecutable + entryOffset);
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
SyscallDelegate sSyscallDelegate;


#if !TARGET_OS_SIMULATOR && !TARGET_OS_EXCLAVEKIT
static void handleDyldInCache(const MachOFile* dyldMF, const KernelArgs* kernArgs, const MachOFile* prevDyldMH)
{
#if __i386__
    // dyld-in-cache not supported for i386
    return;
#else
    if ( dyldMF->inDyldCache() ) {
        // We need to drop the additional send right we got by calling task_self_trap() via mach_init() a second time
        mach_port_mod_refs(mach_task_self(), mach_task_self(), MACH_PORT_RIGHT_SEND, -1);
        bool    usingNewProcessInfo = false;
        usingNewProcessInfo = ExternallyViewableState::switchToDyldInDyldCache(dyldMF);
        // Instruments tracks mapped images.  dyld is considered mapped from the process info
        // but we now need to tell Instruments that we are unmapping the dyld its tracking.
        // Note there was no previous MAP event for dyld, just the process info
        if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_UNMAP_A)) ) {
            // add trace for unmapping dyld itself
            uuid_t dyldUuid;
            dyldMF->getUuid(dyldUuid);
            struct stat        stat_buf;
            fsid_t             dyldFsid    = { { 0, 0 } };
            fsobj_id_t         dyldFfsobjid = { 0, 0 };
            if ( dyld3::stat("/usr/lib/dyld", &stat_buf) == 0 ) {
                dyldFfsobjid  = *(fsobj_id_t*)&stat_buf.st_ino;
                dyldFsid      = { { stat_buf.st_dev, 0 } };
            }
            kdebug_trace_dyld_image(DBG_DYLD_UUID_UNMAP_A, "/usr/lib/dyld", &dyldUuid, dyldFfsobjid, dyldFsid, prevDyldMH);
        }

        // Also use the older notifiers to tell Instruments that we unloaded dyld
        // First update the timestamp as load/unload events historically always had a non-zero timestamp
#if TARGET_OS_OSX
        ExternallyViewableState::getProcessInfo()->infoArrayChangeTimestamp = mach_absolute_time();
        {
            const char* pathsBuffer[1] = { "/usr/lib/dyld" };
            const mach_header* mhBuffer[1] = { prevDyldMH };
            ExternallyViewableState::notifyMonitoringDyld(true, 1, &mhBuffer[0], &pathsBuffer[0]);
        }
#endif // TARGET_OS_OSX

        // We then need to tell Instruments that we have mapped a new dyld.
        // Note we really need to keep this adjacent to the unmap event above, as we don't want Instruments to see
        // code running in a memory range which is untracked.
        if ( kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, DBG_DYLD_UUID_MAP_A)) ) {
            // add trace for dyld itself
            uuid_t dyldUuid;
            dyldMF->getUuid(dyldUuid);
            fsid_t             dyldFsid    = { { 0, 0 } };
            fsobj_id_t         dyldFfsobjid = { 0, 0 };
            kdebug_trace_dyld_image(DBG_DYLD_UUID_MAP_A, "/usr/lib/dyld", &dyldUuid, dyldFfsobjid, dyldFsid, dyldMF);
        }

#if TARGET_OS_OSX
        // Also use the older notifiers to tell Instruments that we loaded a new dyld
        {
            const char* pathsBuffer[1] = { "/usr/lib/dyld" };
            const mach_header* mhBuffer[1] = { dyldMF };
            ExternallyViewableState::notifyMonitoringDyld(false, 1, &mhBuffer[0], &pathsBuffer[0]);
        }
#endif // TARGET_OS_OSX

        // unload disk based dyld now that we are running with one in the dyld cache
        struct Seg { void* start; size_t size; };
        STACK_ALLOC_ARRAY(Seg, segRanges, 16);
        uint64_t prevDyldSlide = ((MachOAnalyzer*)prevDyldMH)->getSlide();
        prevDyldMH->forEachSegment(^(const MachOFile::SegmentInfo& info, bool& stop) {
            // don't unload  __DATA_DIRTY if still using the original dyld_all_image_infos
            if ( !usingNewProcessInfo && (strcmp(info.segName, "__DATA_DIRTY") == 0) )
                return;
            void*   segStart = (void*)(long)(info.vmAddr+prevDyldSlide);
            size_t  segSize  = (size_t)info.vmSize;
            segRanges.push_back({segStart, segSize});
        });
        // we cannot unmap above because unmapping TEXT segment will crash forEachSegment(), do the unmap now
        for (const Seg& s : segRanges)
            ::munmap(s.start, s.size);
    }
    else {
  #if TARGET_OS_OSX
        // simulator programs do not use dyld-in-cache
        if ( kernArgs->mainExecutable->isBuiltForSimulator() )
            return;
    #if SUPPORT_ROSETTA
        // rosetta translated processes don't use dyld-in-cache
        if ( sSyscallDelegate.isTranslated() )
             return;
    #endif // SUPPORT_ROSETTA
  #endif // TARGET_OS_OSX
        // don't use dyld-in-cache with private dyld caches
        if ( _simple_getenv(kernArgs->findEnvp(), "DYLD_SHARED_REGION") != nullptr )
            return;

        // check if this same dyld is in dyld cache
        uuid_t thisDyldUuid;
        if ( dyldMF->getUuid(thisDyldUuid) ) {
            uint64_t cacheBaseAddress;
            uint64_t cacheFSID;
            uint64_t cacheFSObjID;
            if ( sSyscallDelegate.hasExistingDyldCache(cacheBaseAddress, cacheFSID, cacheFSObjID) ) {
                const DyldSharedCache* dyldCacheHeader = (DyldSharedCache*)(long)cacheBaseAddress;
                uint64_t cacheSlide = dyldCacheHeader->slide();
                if ( dyldCacheHeader->header.dyldInCacheMH != 0 ) {
                    const MachOFile* dyldInCacheMF = (MachOFile*)(long)(dyldCacheHeader->header.dyldInCacheMH + cacheSlide);
                    uuid_t           dyldInCacheUuid;
                    if ( dyldInCacheMF->getUuid(dyldInCacheUuid) ) {
                        if ( ::memcmp(thisDyldUuid, dyldInCacheUuid, sizeof(uuid_t)) == 0 ) {
                            // same dyld as in cache
                            const char* overrideStr = _simple_getenv(kernArgs->findEnvp(), "DYLD_IN_CACHE");
                             if ( (overrideStr == nullptr) || (strcmp(overrideStr, "0") != 0) ) {
                                // update all_image_info in case lldb attaches during transition
                                ExternallyViewableState::switchDyldLoadAddress(dyldInCacheMF);
                                 // Tell Instruments we have a shared cache before we start using an image in the cache
                                 dyld3::kdebug_trace_dyld_cache(cacheFSObjID, cacheFSID, cacheBaseAddress,
                                                                dyldCacheHeader->header.uuid);
                                // cut back stack and restart but using dyld in the cache
                                restartWithDyldInCache(kernArgs, dyldMF, (void*)(long)(dyldCacheHeader->header.dyldInCacheEntry + cacheSlide));
                            }
                        }
                    }
                }
            }
        }
    }
#endif // __i386__
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
    dyldMA->forEachSegment(^(const MachOAnalyzer::SegmentInfo& segInfo, bool& stop) {
        if ( segInfo.readOnlyData ) {
            const uint8_t* start = (uint8_t*)(segInfo.vmAddr + slide);
            size_t         size  = (size_t)segInfo.vmSize;
            sSyscallDelegate.mprotect((void*)start, size, PROT_READ);
        }
    });
#endif
}

// Do any set up needed by any linked static libraries
static void initializeLibc(KernelArgs* kernArgs) __attribute__((no_stack_protector))
{
#if TARGET_OS_EXCLAVEKIT
    xrt__entry_args_t args = {
            .launched_roottask = 0,
    };
    plat_common_parse_entry_vec((xrt__entry_vec_t *)kernArgs->entry_vec, &args);
    bootinfo_init(args.bootinfo_virt);
    kernArgs->mappingDescriptor = (const void*)args.dyld_mapping_descriptor;

    // set up stack canary
    _liblibc_stack_guard_init();
#else
    mach_init();

    // set up random value for stack canary
    const char** apple = kernArgs->findApple();
    __guard_setup(apple);

    // setup so that open_with_subsystem() works
    _subsystem_init(apple);
#endif // TARGET_OS_EXCLAVEKIT
}

static void setInitialExternallyVisibleState(RuntimeState& state, Allocator& ephemeralAllocator, const MachOAnalyzer* dyldMA)
{
#if !TARGET_OS_EXCLAVEKIT
    // if there is a dyld cache, add dyld shared cache info to ExternallyViewableState
    if ( state.config.dyldCache.addr != nullptr ) {
        state.externallyViewable.setSharedCacheInfo(ephemeralAllocator, state.config.dyldCache.slide,
                                                    {state.config.dyldCache.fsID, state.config.dyldCache.fsObjID,state.config.dyldCache.path, state.config.dyldCache.addr},
                                                    state.config.dyldCache.privateCache);
    }

#if TARGET_OS_SIMULATOR
    // If this is a simulator then the host already registered the main executable,
    // dyld and dyld_sim before transferring to dyld_sim, no need to do it again.
    // Add the main executable to ExternallyViewableState to support older hosts that only add dyld_sim to the image list.
    if ( state.externallyViewable.imageInfoCount() == 1 ) {
        state.externallyViewable.addImageInfoOld({state.config.process.mainExecutableFSID, state.config.process.mainExecutableObjID, state.config.process.mainExecutablePath, state.config.process.mainExecutable}, /*timestamp*/ 0, /*mod time */ 0 );
    }
#else
    // Add dyld info to ExternallyViewableState
    state.externallyViewable.setDyld(ephemeralAllocator, {state.config.process.dyldFSID, state.config.process.dyldObjID, state.config.process.dyldPath, dyldMA});

    // Add the main executable to ExternallyViewableState
    state.externallyViewable.addImageInfo(ephemeralAllocator, {state.config.process.mainExecutableFSID, state.config.process.mainExecutableObjID, state.config.process.mainExecutablePath, state.config.process.mainExecutable});

    // Set the initial number of images, before initializers are run
    state.setInitialImageCount(1);
    state.externallyViewable.setInitialImageCount(state.initialImageCount());

    // For simulator processes, wait until dyld_sim is added to image list before committing
    if ( !MachOFile::isSimulatorPlatform(state.config.process.platform) ) {
        // now let rest of world see the main executable and dyld in this process
        state.externallyViewable.commit(state.persistentAllocator, ephemeralAllocator);
    }
#endif // !TARGET_OS_SIMULATOR
#else
    // add dyld info
    state.externallyViewable.setDyldOld({state.config.process.dyldFSID, state.config.process.dyldObjID, state.config.process.dyldPath, dyldMA});

    // add the main executable
    state.externallyViewable.addImageInfoOld({state.config.process.mainExecutableFSID, state.config.process.mainExecutableObjID, state.config.process.mainExecutablePath, state.config.process.mainExecutable}, /*timestamp*/ 0, /*mod time */ 0 );

    // Set the initial number of images, before initializers are run
    state.setInitialImageCount(1);
    state.externallyViewable.setInitialImageCountOld(state.initialImageCount());
#endif // !TARGET_OS_EXCLAVEKIT
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
void start(KernelArgs* kernArgs, void* prevDyldMH) __attribute__((noreturn)) __asm("start");
void start(KernelArgs* kernArgs, void* prevDyldMH)
{
    // Emit kdebug tracepoint to indicate dyld bootstrap has started <rdar://46878536>
#if !TARGET_OS_EXCLAVEKIT
    // Note: this is called before dyld is rebased, so kdebug_trace_dyld_marker() cannot use any global variables
    dyld3::kdebug_trace_dyld_marker(DBG_DYLD_TIMING_BOOTSTRAP_START, 0, 0, 0, 0);
#endif // !TARGET_OS_EXCLAVEKIT

    // walk all fixups chains and rebase dyld
    const MachOAnalyzer* dyldMA = getDyldMH();
    if ( !dyldMA->inDyldCache() )
        rebaseSelf(dyldMA);

#if TARGET_OS_EXCLAVEKIT
    KernelArgs actualKernelArgs = {
        .entry_vec = (xrt__entry_vec_t *)kernArgs,
        .mappingDescriptor = nullptr,
    };
    kernArgs = &actualKernelArgs;
#endif
    // Do any set up needed by any linked static libraries
    initializeLibc(kernArgs);

#if !TARGET_OS_EXCLAVEKIT
    // handle switching to dyld in dyld cache for native platforms
    handleDyldInCache(dyldMA, kernArgs, (MachOFile*)prevDyldMH);
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
    Allocator*      allocator   = nullptr;
    APIs*           state       = nullptr;
    MainFunc        appMain     = nullptr;
    
#if !TARGET_OS_EXCLAVEKIT
    MemoryManager bootStrapMemoryManager((const char**)kernArgs->findApple());
#else
    MemoryManager bootStrapMemoryManager;
#endif // !TARGET_OS_EXCLAVEKIT

    bootStrapMemoryManager.withWritableMemory([&] {
        EphemeralAllocator ephemeralAllocator;
        // Setup the persistent allocator
        allocator = &Allocator::persistentAllocator(std::move(bootStrapMemoryManager));
        
        // use placement new to construct ProcessConfig object in the Allocator pool
        ProcessConfig& config  = *new (allocator->aligned_alloc(alignof(ProcessConfig), sizeof(ProcessConfig))) ProcessConfig(kernArgs, sSyscallDelegate, *allocator);

        // create APIs (aka RuntimeState) object in the allocator
        state = new (allocator->aligned_alloc(alignof(APIs), sizeof(APIs))) APIs(config, locks, *allocator);

        // set initial state for ExternallyViewableState
#if !TARGET_OS_EXCLAVEKIT
        state->externallyViewable.init(state->persistentAllocator, ephemeralAllocator, state->fileManager, state->config.process.platform);
#else
        state->externallyViewable.initOld(state->persistentAllocator, state->config.process.platform);
#endif // !TARGET_OS_EXCLAVEKIT
        // publish initial externally visible state that has main program and dyld
        setInitialExternallyVisibleState(*state, ephemeralAllocator, dyldMA);

        // load all dependents of program and bind them together
        appMain = prepare(*state, dyldMA);
    });

#if TARGET_OS_EXCLAVEKIT
    // inform liblibc_plat that all static initializers have run and let it finalize the process startup
    state->vars->finalize_process_startup(appMain);

    // if we get here, finalize_process_startup returned (it's not supposed to)
    halt("finalize_process_startup wrongly returned");
#else
    // call main() and if it returns, call exit() with the result
    // Note: this is organized so that a backtrace in a program's main thread shows just "start" below "main"
    int result = appMain(state->config.process.argc, state->config.process.argv, state->config.process.envp, state->config.process.apple);

    // if we got here, main() returned (as opposed to program calling exit())
#if TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    // <rdar://74518676> libSystemHelpers is not set up for simulators, so directly call _exit()
    if ( MachOFile::isSimulatorPlatform(state->config.process.platform) )
        _exit(result);
#endif // TARGET_OS_OSX && !TARGET_OS_EXCLAVEKIT
    state->libSystemHelpers->exit(result);
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
    sAPIsForExit->libSystemHelpers->exit(result);
    return 0;
}

extern "C" MainFunc _dyld_sim_prepare(int argc, const char* argv[], const char* envp[], const char* apple[],
                                      const mach_header* mainExecutableMH, const MachOAnalyzer* dyldMA, uintptr_t dyldSlide,
                                      const dyld::SyscallHelpers*, uintptr_t* startGlue);

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
    initializeLibc(kernArgs);

     // create an Allocator inside its own allocation pool
    // Setup the memory manager object before the allocator so the allocator can use it before copying it internally
    MemoryManager bootStapMemoryManager((const char**)apple);
    Allocator& allocator = Allocator::persistentAllocator(std::move(bootStapMemoryManager));

    // use placement new to construct ProcessConfig object in the Allocator pool
    ProcessConfig& config = *new (allocator.aligned_alloc(alignof(ProcessConfig), sizeof(ProcessConfig))) ProcessConfig(kernArgs, sSyscallDelegate, allocator);

    // we cannot use "static RuntimeLocks locks;" because the compiler will generate an initializer or guards
    static uint8_t sLocksStaticStorage[sizeof(RuntimeLocks)] __attribute__((aligned(alignof(RuntimeLocks))));
    RuntimeLocks& locks = *new (sLocksStaticStorage) RuntimeLocks();

    // create APIs (aka RuntimeState) object in the allocator
    APIs& state = *new (allocator.aligned_alloc(alignof(APIs), sizeof(APIs))) APIs(config, locks, allocator);

    // function pointer that will be set to the entry point. Declare it here so the value can escape from withWritableMemory()
    MainFunc result = nullptr;
    allocator.memoryManager()->withWritableMemory([&] {
        EphemeralAllocator ephemeralAllocator;
        // now that allocator is up, we can update image list
        // set initial state for ExternallyViewableState
        state.externallyViewable.initSim(state.persistentAllocator, ephemeralAllocator, state.config.process.platform,
                                         (struct dyld_all_image_infos*)(sc->getProcessInfo()), sc);

        // publish initial externally visible state that has main program and dyld
        setInitialExternallyVisibleState(state, ephemeralAllocator, dyldMA);
        // load all dependents of program and bind them together, then return address of main()
        result = prepare(state, dyldMA);
    });

    // return fake main, which calls real main() then simulator exit()
    *startGlue   = 1;  // means result is pointer to main(), as opposed to crt1.o entry
    sRealMain    = result;
    sAPIsForExit = &state;
    return &start_sim;
}
#endif // TARGET_OS_SIMULATOR
