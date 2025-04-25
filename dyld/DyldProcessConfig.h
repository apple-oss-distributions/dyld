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


#ifndef DyldProcessConfig_h
#define DyldProcessConfig_h

#include <TargetConditionals.h>

#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>

#if TARGET_OS_EXCLAVEKIT
#include <platform/platform.h>
#endif

// mach_o
#include "Platform.h"
#include "Header.h"
#include "FunctionVariants.h"

// common
#include "Defines.h"
#include "Array.h"
#include "CString.h"
#if BUILDING_CACHE_BUILDER
#include "MachOFile.h"
#else
#include "MachOAnalyzer.h"
#endif

// dyld
#include "DyldDelegates.h"
#include "Allocator.h"
#include "SharedCacheRuntime.h"
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "Vector.h"
#endif

class DyldSharedCache;

namespace dyld4 {

using lsl::Allocator;
using dyld3::MachOFile;
using dyld3::MachOLoaded;
using dyld3::MachOAnalyzer;
using dyld3::GradedArchs;
using mach_o::Header;
using mach_o::FunctionVariants;


class APIs;

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
struct StructuredError {
    uintptr_t     kind              = 0;
    const char*   clientOfDylibPath = nullptr;
    const char*   targetDylibPath   = nullptr;
    const char*   symbolName        = nullptr;
 };
void halt(const char* message, const StructuredError* errInfo=nullptr)  __attribute__((__noreturn__));
void missing_symbol_abort()  __attribute__((__noreturn__));
#endif // BUILDING_DYLD || BUILDING_UNIT_TESTS
void console(const char* format, ...) __attribute__((format(printf, 1, 2)));

struct ProgramVars
{
    const void*      mh             = nullptr;
    int*             NXArgcPtr      = nullptr;
    const char***    NXArgvPtr      = nullptr;
    const char***    environPtr     = nullptr;
    const char**     __prognamePtr  = nullptr;
#if TARGET_OS_EXCLAVEKIT
    void             *entry_vec;
    void            (*finalize_process_startup)(int(*main)(int argc, const char* const argv[], const char* const envp[], const char* const apple[]));
#endif
};

// for use with function-variant feature bits
typedef __uint128_t  FunctionVariantFlags;


// how static initializers are called
  typedef void (*Initializer)(int argc, const char* const argv[], const char* const envp[], const char* const apple[], const ProgramVars* vars);


//
// At launch, dyld stuffs the start of __TPRO_CONST,__dyld_apis section with a pointer to a v-table in dyld. 
//
struct LibdyldAPIsSection {
    APIs*               apis;
};


#if TARGET_OS_EXCLAVEKIT
struct PreMappedFileEntry {
    const mach_header*  loadAddress;
    size_t              mappedSize;
    const char*         path;
};
#endif

// how kernel pass argc,argv,envp on the stack to main executable
struct KernelArgs
{
#if TARGET_OS_EXCLAVEKIT
    xrt__entry_vec_t*           entry_vec;
    const void*                 mappingDescriptor; // set only after call to liblibc_plat_parse_entry_vec
    bool                        dyldSharedCacheEnabled;
#else
  #if SUPPORT_VM_LAYOUT
    const MachOAnalyzer*  mainExecutable;
  #else
    const MachOFile*      mainExecutable;
  #endif
    uintptr_t             argc;
  #define MAX_KERNEL_ARGS   128
    const char*           args[MAX_KERNEL_ARGS]; // argv[], then envp[], then apple[]

    const char**          findArgv() const;
    const char**          findEnvp() const;
    const char**          findApple() const;
#endif

#if !BUILDING_DYLD
                          KernelArgs(const MachOFile* mh, const std::vector<const char*>& argv, const std::vector<const char*>& envp, const std::vector<const char*>& apple);
#endif
};

#if !TARGET_OS_EXCLAVEKIT
typedef dyld4::SyscallDelegate::DyldCommPage   DyldCommPage;
#endif

//
// ProcessConfig holds the fixed, initial state of the process. That is, all the information
// about the process that won't change through the life of the process. This is a singleton
// which is constructed via a static initializer in dyld itself.
//
class VIS_HIDDEN ProcessConfig
{
public:
                            // used in unit tests to config and test ProcessConfig objects
                            ProcessConfig(const KernelArgs* kernArgs, SyscallDelegate&, Allocator&);

#if !BUILDING_DYLD
    void                    reset(const MachOFile* mainExe, const char* mainPath, const DyldSharedCache* cache);
#endif

    void                    scanForRoots() const;
    static void*            scanForRoots(void* context);

    //
    // Contains config info derived from Kernel arguments
    //
    class Process
    {
    public:
                                    Process(const KernelArgs* kernArgs, SyscallDelegate&, Allocator&);
#if SUPPORT_VM_LAYOUT
        const MachOAnalyzer*        mainExecutableMF;
#else
        mach_o::MachOFileRef        mainExecutableMF = { nullptr };
#endif
        const mach_o::Header*       mainExecutableHdr;
        const char*                 mainExecutablePath;
        const char*                 mainUnrealPath;             // raw path used to launch
        uint64_t                    mainExecutableFSID;
        uint64_t                    mainExecutableObjID;
        uint32_t                    mainExecutableSDKVersion;
        uint32_t                    mainExecutableSDKVersionSet;
        uint32_t                    mainExecutableMinOSVersion;
        uint32_t                    mainExecutableMinOSVersionSet;
        mach_o::Platform            basePlatform;
        mach_o::Platform            platform;
        const char*                 dyldPath;
        const Header*               dyldHdr;
        uint64_t                    dyldFSID;
        uint64_t                    dyldObjID;
#if TARGET_OS_SIMULATOR
        uint64_t                    dyldSimFSID;
        uint64_t                    dyldSimObjID;
#endif
#if TARGET_OS_EXCLAVEKIT
        xrt__entry_vec_t*                              entry_vec;
        uint32_t                                       startupContractVersion;
        dyld3::OverflowSafeArray<PreMappedFileEntry>   preMappedFiles;
        void*                                          preMappedCache;
        const char*                                    preMappedCachePath;
        size_t                                         preMappedCacheSize;
        bool                                           sharedCacheFileEnabled; //TODO: remove when transition file is removed
        bool                                           sharedCachePageInLinking;
#else
        DyldCommPage                commPage;
#endif
        int                         argc;
        const char* const*          argv;
        const char* const*          envp;
        const char* const*          apple;
        const char*                 progname;
        const GradedArchs*          archs;
        int                         pid;
        bool                        isTranslated;
        bool                        catalystRuntime; // Mac Catalyst app or iOS-on-mac app
        bool                        enableDataConst; // Temporarily allow disabling __DATA_CONST for bringup
        bool                        enableTproHeap; // Enable HW TPRO protections for the dyld heap (and TPRO_CONST)
        bool                        enableTproDataConst; // Enable HW TPRO protections for __DATA_CONST
        bool                        enableProtectedStack; // Enable HW TPRO protections for the stack
        bool                        proactivelyUseWeakDefMap;
        int                         pageInLinkingMode;
        FunctionVariantFlags        perProcessFunctionVariantFlags;    // evaluated in-process
        FunctionVariantFlags        systemWideFunctionVariantFlags;    // copied from dyld cache or evaluted in-process
        FunctionVariantFlags        processorFunctionVariantFlags;     // copied from dyld cache or evaluted in-process

        const char*                 appleParam(const char* key) const;
        const char*                 environ(const char* key) const;
        void                        evaluateFunctionVariantFlags(const ProcessConfig& config);
        uint64_t                    selectFromFunctionVariants(const FunctionVariants& fvs, uint32_t fvTableIndex) const;

    private:
        friend class ProcessConfig;

        uint32_t                        findVersionSetEquivalent(mach_o::Platform versionPlatform, uint32_t version) const;
        std::pair<uint64_t, uint64_t>   fileIDFromFileHexStrings(const char* encodedFileInfo);
        const char*                     pathFromFileHexStrings(SyscallDelegate& sys, Allocator& allocator, const char* encodedFileInfo);
        std::pair<uint64_t, uint64_t>   getDyldFileID();
        const char*                     getDyldPath(SyscallDelegate& sys, Allocator& allocator);
        std::pair<uint64_t, uint64_t>   getDyldSimFileID(SyscallDelegate& syscall);
        const char*                     getMainPath(SyscallDelegate& syscall, Allocator& allocator);
        std::pair<uint64_t, uint64_t>   getMainFileID();
        const char*                     getMainUnrealPath(SyscallDelegate& syscall, Allocator& allocator);
        mach_o::Platform                getMainPlatform();
        const GradedArchs*              getMainArchs(SyscallDelegate& osDelegate);
        bool                            isInternalSimulator(SyscallDelegate& syscall) const;
        bool                            usesCatalyst() const;
        bool                            defaultDataConst() const;
        bool                            defaultTproDataConst() const;
        bool                            defaultTproStack() const;
        bool                            defaultTproHW() const;
        void                            setPerProcessFunctionVariantFlags(const ProcessConfig& config);
    };

    //
    // Contains security related config info
    //
    class Security
    {
    public:
                                    Security(Process& process, SyscallDelegate&);

        bool                        isInternalOS; // works with device and simulator
        bool                        internalInstall; // always returns false for simulator
        bool                        dlsymBlocked;
        bool                        dlsymAbort;
        const char*                 dlsymAllowList;
        bool                        lockdownMode;
#if !TARGET_OS_EXCLAVEKIT
        bool                        allowAtPaths;
        bool                        allowEnvVarsPrint;
        bool                        allowEnvVarsPath;
        bool                        allowEnvVarsSharedCache;
        bool                        allowClassicFallbackPaths;
        bool                        allowInsertFailures;
        bool                        allowInterposing;
        bool                        allowEmbeddedVars;
        bool                        allowDevelopmentVars;
        bool                        allowLibSystemOverrides;
        bool                        skipMain;
        bool                        justBuildClosure;

     private:
        uint64_t                    getAMFI(const Process& process, SyscallDelegate& syscall);
        void                        pruneEnvVars(Process& process);
#endif // !TARGET_OS_EXCLAVEKIT
   };

    //
    // Contains logging related config info
    //
    class Logging
    {
    public:
                                    Logging(const Process& process, const Security& security, SyscallDelegate&);

        bool                        libraries;
        bool                        segments;
        bool                        fixups;
        bool                        initializers;
        bool                        apis;
        bool                        notifications;
        bool                        interposing;
        bool                        loaders;
        bool                        searching;
        bool                        env;
        int                         descriptor;
        bool                        useStderr;
        bool                        useFile;
        CString                     linksWith;

     private:
    };

    //
    // Contains dyld cache related config info
    //
    class DyldCache
    {
    public:
                                    DyldCache(Process&, const Security&, const Logging&, SyscallDelegate&, Allocator&, const ProcessConfig&);

        const DyldSharedCache*          addr;
#if !TARGET_OS_EXCLAVEKIT
        FileIdTuple                     mainFileID;
#endif // !TARGET_OS_EXCLAVEKIT
#if SUPPORT_VM_LAYOUT
        uintptr_t                       slide;
#endif
        uint64_t                        unslidLoadAddress;
        const char*                     path;
        const objc::HeaderInfoRO*       objcHeaderInfoRO;
        const objc::HeaderInfoRW*       objcHeaderInfoRW;
        const objc::SelectorHashTable*  objcSelectorHashTable;
        const objc::ClassHashTable*     objcClassHashTable;
        const objc::ProtocolHashTable*  objcProtocolHashTable;
        std::string_view                cryptexOSPath;
        const SwiftOptimizationHeader*  swiftCacheInfo;
        uint64_t                        objcHeaderInfoROUnslidVMAddr;
        uint64_t                        objcProtocolClassCacheOffset;
        PatchTable                      patchTable;
        mach_o::Platform                platform;
        uint32_t                        osVersion;
        uint32_t                        dylibCount;
        bool                            development;
        bool                            dylibsExpectedOnDisk;
        bool                            privateCache;
        bool                            allowLibSystemOverrides;
        FunctionVariantFlags            systemWideFunctionVariantFlags;
        FunctionVariantFlags            processorFunctionVariantFlags;

        // This is used to track the cases where the shared cache supports roots, and which roots are eligible.
        // This is a combination of the kind of shared cache (customer vs development) but also whether
        // other circumstances such as boot-time/env-var checks mean that we know for sure roots are or are not supported
        bool                            rootsAreSupported;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
        // In the cache builder, the dylibs might not be mapped in their runtime layout,
        // so use the layout the builder gives us
        struct CacheDylib
        {
            const MachOFile*        mf        = nullptr;
            uint64_t                mTime     = 0;
            uint64_t                inode     = 0;
            const mach_o::Layout*   layout    = nullptr;
        };
        // FIXME: Use std::span once we have it
        const lsl::Vector<CacheDylib>* cacheBuilderDylibs = nullptr;
#endif

        bool                        indexOfPath(const char* dylibPath, uint32_t& dylibIndex) const;
        bool                        findMachHeaderImageIndex(const mach_header* mh, uint32_t& imageIndex) const;
        void                        makeDataConstWritable(const Logging&, const SyscallDelegate&, bool writable) const;

        // Moved from DyldSharedCache as they are only used from Loader's anyway, and this allows
        // us to not link DyldSharedCache in to the cache builder, where it would be unsafe to use due
        // to file layout vs VM layout vs builder layout
        static bool                 isAlwaysOverridablePath(const char* dylibPath);
        static bool                 isProtectedLibSystemPath(const char* dylibPath);
        bool                        isOverridablePath(const char* dylibPath) const;
        const char*                 getCanonicalPath(const char *path) const;
        const char*                 getIndexedImagePath(uint32_t index) const;
        const dyld3::MachOFile*     getIndexedImageEntry(uint32_t index,
                                                         uint64_t& mTime, uint64_t& inode) const;

        // There are env vars which can change the default search paths, eg, DYLD_LIBRARY_PATH.
        // Adjust whether roots are supported, given that these are set.
        void                        adjustRootsSupportForEnvVars();

        bool                        sharedCacheLoadersAreAlwaysValid() const { return !rootsAreSupported; }

    private:
        friend class ProcessConfig;
        bool                        uuidOfFileMatchesDyldCache(const Process& process, const SyscallDelegate& syscall, const char* path) const;
        void                        setPlatformOSVersion(const Process& proc);
        void                        setupDyldCommPage(Process& process, const Security& security, SyscallDelegate& syscall);
        void                        setSystemWideFunctionVariantFlags();
    };

    //
    // Contains path searching config info
    //
    class PathOverrides
    {
    public:
                                        PathOverrides(const Process&, const Security&, const Logging&, const DyldCache&, SyscallDelegate&, Allocator&);

        enum Type { pathDirOverride, versionedOverride, suffixOverride, catalystPrefixOnDisk, catalystPrefix, simulatorPrefix, cryptexCatalystPrefix, cryptexPrefix,
                    rawPathOnDisk, rawPath, rpathExpansion, loaderPathExpansion, executablePathExpansion, implictRpathExpansion,
                    customFallback, standardFallback };

        // rawPathOnDisk and catalystPrefixOnDisk should both check only the disk and not the shared cache.
        // This returns true if we are a Type such as those
        static bool                     isOnDiskOnlyType(Type& t) { return (t == catalystPrefixOnDisk) || (t == rawPathOnDisk); }

        void                            forEachPathVariant(const char* requestedPath, mach_o::Platform platform, bool requestorNeedsFallbacks, bool skipFallbacks, bool& stop,
                                                           void (^handler)(const char* possiblePath, Type type, bool& stop)) const;
        void                            forEachInsertedDylib(void (^handler)(const char* dylibPath, bool &stop)) const;
        bool                            dontUsePrebuiltForApp() const;
        bool                            hasInsertedDylibs() const { return (_insertedDylibCount != 0); }
        uint32_t                        insertedDylibCount() const { return _insertedDylibCount; }
        const char*                     simRootPath() const { return _simRootPath; }
        const char*                     cryptexRootPath() const { return _cryptexRootPath; }
        const char*                     getFrameworkPartialPath(const char* path) const;

#if BUILDING_UNIT_TESTS
        void                            setCryptexRootPath(const char* path) { this->_cryptexRootPath = path; }
#endif

        static const char*              getLibraryLeafName(const char* path);
        static const char*              typeName(Type);

    private:
        void                            setEnvVars(const char* envp[], const char* mainExePath);
        void                            addEnvVar(const Process& process, const Security& security, Allocator&, const char* keyEqualsValue, bool isLC_DYLD_ENV, char* crashMsg);
        void                            processVersionedPaths(const Process& proc, SyscallDelegate&, const DyldCache&, mach_o::Platform, const GradedArchs&, Allocator& allocator);
        void                            forEachEnvVar(void (^handler)(const char* envVar)) const;
        void                            forEachExecutableEnvVar(void (^handler)(const char* envVar)) const;

        void                            setString(Allocator&, const char*& var, const char* value);
        static void                     forEachInColonList(const char* list1, const char* list2, bool& stop, void (^callback)(const char* path, bool& stop));
        void                            handleListEnvVar(const char* key, const char** list, void (^handler)(const char* envVar)) const;
        void                            handleEnvVar(const char* key, const char* value, void (^handler)(const char* envVar)) const;
        void                            forEachDylibFallback(mach_o::Platform platform, bool requestorNeedsFallbacks, bool& stop, void (^handler)(const char* fallbackDir, Type type, bool& stop)) const;
        void                            forEachFrameworkFallback(mach_o::Platform platform, bool requestorNeedsFallbacks, bool& stop, void (^handler)(const char* fallbackDir, Type type, bool& stop)) const;
        void                            forEachImageSuffix(const char* path, Type type, bool& stop, void (^handler)(const char* possiblePath, Type type, bool& stop)) const;
        void                            addSuffix(const char* path, const char* suffix, char* result) const;
        void                            checkVersionedPath(SyscallDelegate& delegate, const DyldCache&, Allocator&, const char* path, mach_o::Platform, const GradedArchs&);
        void                            addPathOverride(Allocator& allocator, const char* installName, const char* overridePath);

        enum class FallbackPathMode { classic, restricted, none };
        struct DylibOverride { DylibOverride* next; const char* installName; const char* overridePath; };

        const char*                     _dylibPathOverridesEnv       = nullptr;
        const char*                     _frameworkPathOverridesEnv   = nullptr;
        const char*                     _dylibPathFallbacksEnv       = nullptr;
        const char*                     _frameworkPathFallbacksEnv   = nullptr;
        const char*                     _versionedDylibPathsEnv      = nullptr;
        const char*                     _versionedFrameworkPathsEnv  = nullptr;
        const char*                     _dylibPathOverridesExeLC     = nullptr;
        const char*                     _frameworkPathOverridesExeLC = nullptr;
        const char*                     _dylibPathFallbacksExeLC     = nullptr;
        const char*                     _frameworkPathFallbacksExeLC = nullptr;
        const char*                     _versionedFrameworkPathExeLC = nullptr;
        const char*                     _versionedDylibPathExeLC     = nullptr;
        const char*                     _insertedDylibs              = nullptr;
        const char*                     _imageSuffix                 = nullptr;
        const char*                     _simRootPath                 = nullptr;  // simulator only
        const char*                     _cryptexRootPath             = nullptr;  // cryptex only
        DylibOverride*                  _versionedOverrides          = nullptr;  // linked list of VERSIONED overrides
        FallbackPathMode                _fallbackPathMode            = FallbackPathMode::classic;
        uint32_t                        _insertedDylibCount          = 0;
    };


#if !TARGET_OS_EXCLAVEKIT
    // wrappers for macOS that causes special three libsystem dylibs to not exist if they match what is in dyld cache
    bool simulatorFileMatchesDyldCache(const char* path) const;
#endif
    bool fileExists(const char* path, FileID* fileID=nullptr, int* errNum=nullptr) const;

    // if there is a dyld cache and the supplied path is in the dyld cache at that path or a symlink, return canonical path
    const char* canonicalDylibPathInCache(const char* dylibPath) const;

    static FunctionVariantFlags   evaluatePerProcessVariantFlags(const ProcessConfig& config);
    static FunctionVariantFlags   evaluateSystemWideFunctionVariantFlags(const ProcessConfig& config);
    static FunctionVariantFlags   evaluateProcessorSpecificFunctionVariantFlags(const ProcessConfig& config);


    // all instance variables are organized into groups
    SyscallDelegate         syscall;
    Process                 process;
    Security                security;
    Logging                 log;
    DyldCache               dyldCache;
    PathOverrides           pathOverrides;
};


} // namespace dyld4



#endif /* DyldProcessConfig_h */
