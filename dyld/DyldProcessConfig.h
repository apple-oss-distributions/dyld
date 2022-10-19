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

#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
//#include <mach-o/dyld.h>
//#include <mach-o/dyld_priv.h>

#include "Defines.h"
#include "Array.h"
#include "DyldSharedCache.h"
#include "SharedCacheRuntime.h"
#include "DyldDelegates.h"
#include "Allocator.h"

#if BUILDING_CACHE_BUILDER
#include "MachOFile.h"
#else
#include "MachOAnalyzer.h"
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "Vector.h"
#endif

namespace dyld4 {

using lsl::Allocator;
using dyld3::MachOFile;
using dyld3::MachOLoaded;
using dyld3::MachOAnalyzer;
using dyld3::GradedArchs;

typedef dyld4::SyscallDelegate::DyldCommPage   DyldCommPage;

class APIs;

#if BUILDING_DYLD
void halt(const char* message)  __attribute__((noreturn));
#endif
void console(const char* format, ...) __attribute__((format(printf, 1, 2)));

struct ProgramVars
{
    const void*      mh;
    int*             NXArgcPtr;
    const char***    NXArgvPtr;
    const char***    environPtr;
    const char**     __prognamePtr;
};

// how static initializers are called
typedef void (*Initializer)(int argc, const char* const argv[], const char* const envp[], const char* const apple[], const ProgramVars* vars);



//
// This struct is how libdyld finds dyld.  At launch, dyld stuffs the first field
// in the __DATA,__dyld4 section with a pointer to a v-table in dyld.  The
// rest of the struct is pointers to the crt globals that programs might use
// from libdyld.dylib.  libsystem_c.dylib needs to know these so that putenv()
// can update "environ".  Some old programs have their own copy of these globals
// (from crt1.o), so dyld may switch these ProgramVars.
//
struct LibdyldDyld4Section {
    APIs*               apis;
    void*               allImageInfos;  // set by dyld to point to the dyld_all_image_infos struct
	dyld4::ProgramVars  defaultVars;    // set by libdyld to have addresses of default crt globals in libdyld.dylib
    dyld3::DyldLookFunc dyldLookupFuncAddr;
    void* (*tlv_get_addrAddr)(dyld3::MachOAnalyzer::TLV_Thunk*);
};

extern volatile LibdyldDyld4Section gDyld;


// how kernel pass argc,argv,envp on the stack to main executable
struct KernelArgs
{
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

#if !BUILDING_DYLD
                          KernelArgs(const MachOFile* mh, const std::vector<const char*>& argv, const std::vector<const char*>& envp, const std::vector<const char*>& apple);
#endif
};



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
        const MachOAnalyzer*        mainExecutable;
#else
        mach_o::MachOFileRef        mainExecutable = { nullptr };
#endif
        const char*                 mainExecutablePath;
        const char*                 mainUnrealPath;             // raw path used to launch
        uint64_t                    mainExecutableFSID;
        uint64_t                    mainExecutableObjID;
        uint32_t                    mainExecutableSDKVersion;
        uint32_t                    mainExecutableSDKVersionSet;
        uint32_t                    mainExecutableMinOSVersion;
        uint32_t                    mainExecutableMinOSVersionSet;
        dyld3::Platform             basePlatform;
        dyld3::Platform             platform;
        const char*                 dyldPath;
        uint64_t                    dyldFSID;
        uint64_t                    dyldObjID;
#if TARGET_OS_SIMULATOR
        uint64_t                    dyldSimFSID;
        uint64_t                    dyldSimObjID;
#endif
        int                         argc;
        const char* const*          argv;
        const char* const*          envp;
        const char* const*          apple;
        const char*                 progname;
        DyldCommPage                commPage;
        const GradedArchs*          archs;
        int                         pid;
        bool                        isTranslated;
        bool                        catalystRuntime; // Mac Catalyst app or iOS-on-mac app
        bool                        enableDataConst; // Temporarily allow disabling __DATA_CONST for bringup
        bool                        enableCompactInfo; // Temporarily allow disabling Compact Info during bringup
        bool                        proactivelyUseWeakDefMap;
        int                         pageInLinkingMode;
        const char*                 appleParam(const char* key) const;
        const char*                 environ(const char* key) const;

    private:
        friend class ProcessConfig;

        uint32_t                        findVersionSetEquivalent(dyld3::Platform versionPlatform, uint32_t version) const;
        std::pair<uint64_t, uint64_t>   fileIDFromFileHexStrings(const char* encodedFileInfo);
        const char*                     pathFromFileHexStrings(SyscallDelegate& sys, Allocator& allocator, const char* encodedFileInfo);
        std::pair<uint64_t, uint64_t>   getDyldFileID();
        const char*                     getDyldPath(SyscallDelegate& sys, Allocator& allocator);
        std::pair<uint64_t, uint64_t>   getDyldSimFileID(SyscallDelegate& syscall);
        const char*                     getMainPath(SyscallDelegate& syscall, Allocator& allocator);
        std::pair<uint64_t, uint64_t>   getMainFileID();
        const char*                     getMainUnrealPath(SyscallDelegate& syscall, Allocator& allocator);
        dyld3::Platform                 getMainPlatform();
        const GradedArchs*              getMainArchs(SyscallDelegate& osDelegate);
        bool                            usesCatalyst();
    };

    //
    // Contains security related config info
    //
    class Security
    {
    public:
                                    Security(Process& process, SyscallDelegate&);

        bool                        internalInstall;
        bool                        allowAtPaths;
        bool                        allowEnvVarsPrint;
        bool                        allowEnvVarsPath;
        bool                        allowEnvVarsSharedCache;
        bool                        allowClassicFallbackPaths;
        bool                        allowInsertFailures;
        bool                        allowInterposing;
        bool                        allowEmbeddedVars;
        bool                        skipMain;

     private:
        uint64_t                    getAMFI(const Process& process, SyscallDelegate& syscall);
        void                        pruneEnvVars(Process& process);
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

     private:
    };

    //
    // Contains dyld cache related config info
    //
    class DyldCache
    {
    public:
                                    DyldCache(Process&, const Security&, const Logging&, SyscallDelegate&, Allocator&);

        const DyldSharedCache*          addr;
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
        dyld3::Platform                 platform;
        uint32_t                        osVersion;
        uint32_t                        dylibCount;
        bool                            development;
        bool                            dylibsExpectedOnDisk;

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
        bool                        isOverridablePath(const char* dylibPath) const;
        const char*                 getCanonicalPath(const char *path) const;
        const char*                 getIndexedImagePath(uint32_t index) const;
        const dyld3::MachOFile*     getIndexedImageEntry(uint32_t index,
                                                         uint64_t& mTime, uint64_t& inode) const;

        void                        adjustDevelopmentMode() const;

    private:
        friend class ProcessConfig;
        bool                        uuidOfFileMatchesDyldCache(const Process& process, const SyscallDelegate& syscall, const char* path) const;
        void                        setPlatformOSVersion(const Process& proc);
        void                        setupDyldCommPage(Process& process, const Security& security, SyscallDelegate& syscall);
    };

    //
    // Contains path searching config info
    //
    class PathOverrides
    {
    public:
                                        PathOverrides(const Process&, const Security&, const Logging&, const DyldCache&, SyscallDelegate&, Allocator&);

        enum Type { pathDirOverride, versionedOverride, suffixOverride, catalystPrefix, simulatorPrefix, cryptexPrefix,
                    rawPathOnDisk, rawPath, rpathExpansion, loaderPathExpansion, executablePathExpansion, implictRpathExpansion,
                    customFallback, standardFallback };

        void                            forEachPathVariant(const char* requestedPath, dyld3::Platform platform, bool requestorNeedsFallbacks, bool skipFallbacks, bool& stop,
                                                           void (^handler)(const char* possiblePath, Type type, bool& stop)) const;
        void                            forEachInsertedDylib(void (^handler)(const char* dylibPath, bool &stop)) const;
        bool                            dontUsePrebuiltForApp() const;
        bool                            hasInsertedDylibs() const { return (_insertedDylibCount != 0); }
        uint32_t                        insertedDylibCount() const { return _insertedDylibCount; }
        const char*                     simRootPath() const { return _simRootPath; }
        const char*                     cryptexRootPath() const { return _cryptexRootPath; }
        const char*                     getFrameworkPartialPath(const char* path) const;

        static const char*              getLibraryLeafName(const char* path);
        static const char*              typeName(Type);

    private:
        void                            setEnvVars(const char* envp[], const char* mainExePath);
        void                            addEnvVar(const Process& process, const Security& security, Allocator&, const char* keyEqualsValue, bool isLC_DYLD_ENV, char* crashMsg);
        void                            processVersionedPaths(const Process& proc, SyscallDelegate&, const DyldCache&, dyld3::Platform, const GradedArchs&, Allocator& allocator);
        void                            forEachEnvVar(void (^handler)(const char* envVar)) const;
        void                            forEachExecutableEnvVar(void (^handler)(const char* envVar)) const;

        void                            setString(Allocator&, const char*& var, const char* value);
        static void                     forEachInColonList(const char* list1, const char* list2, bool& stop, void (^callback)(const char* path, bool& stop));
        void                            handleListEnvVar(const char* key, const char** list, void (^handler)(const char* envVar)) const;
        void                            handleEnvVar(const char* key, const char* value, void (^handler)(const char* envVar)) const;
        void                            forEachDylibFallback(dyld3::Platform platform, bool requestorNeedsFallbacks, bool& stop, void (^handler)(const char* fallbackDir, Type type, bool& stop)) const;
        void                            forEachFrameworkFallback(dyld3::Platform platform, bool requestorNeedsFallbacks, bool& stop, void (^handler)(const char* fallbackDir, Type type, bool& stop)) const;
        void                            forEachImageSuffix(const char* path, Type type, bool& stop, void (^handler)(const char* possiblePath, Type type, bool& stop)) const;
        void                            addSuffix(const char* path, const char* suffix, char* result) const;
        void                            checkVersionedPath(SyscallDelegate& delegate, const DyldCache&, Allocator&, const char* path, dyld3::Platform, const GradedArchs&);
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


    // wrappers for macOS that causes special three libsystem dylibs to not exist if they match what is in dyld cache
    bool simulatorFileMatchesDyldCache(const char* path) const;
    bool fileExists(const char* path, FileID* fileID=nullptr, int* errNum=nullptr) const;

    // if there is a dyld cache and the supplied path is in the dyld cache at that path or a symlink, return canonical path
    const char* canonicalDylibPathInCache(const char* dylibPath) const;


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
