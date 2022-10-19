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


#ifndef DyldRuntimeState_h
#define DyldRuntimeState_h

#include <stdarg.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <os/lock_private.h>

#include "Defines.h"
#include "MachOLoaded.h"
#include "MachOAnalyzer.h"
#include "Array.h"
#include "DyldProcessConfig.h"
#include "LibSystemHelpers.h"
#include "Allocator.h"
#include "FileManager.h"
#include "Loader.h"
#include "MurmurHash.h"
#include "OptimizerSwift.h"
#include "Vector.h"
#include "Map.h"
#include "UUID.h"
#include "OrderedMap.h"


namespace dyld4 {

using lsl::UUID;
using lsl::UniquePtr;
using lsl::OrderedMap;
using lsl::Vector;
using lsl::Allocator;
using lsl::EphemeralAllocator;
using dyld3::MachOLoaded;
using dyld3::MachOAnalyzer;

namespace Atlas {
struct ProcessSnapshot;
};
using Atlas::ProcessSnapshot;

class Loader;
class Reaper;

// These replacements are done during binding, unless a replacement was found in InterposeTupleSpecific
struct InterposeTupleAll
{
    uintptr_t        replacement;
    uintptr_t        replacee;
};

// Used to support multiple dylibs interposing the same symbol.
// Each interposing impl, chains to the previous impl.
// Unlike InterposeTupleAll, these are only applied if the 'onlyImage' matches Loader the bind is in
struct InterposeTupleSpecific
{
    const Loader*    onlyImage;            // don't apply replacement to this image (allows interposer to call thru to old impl)
    uintptr_t        replacement;
    uintptr_t        replacee;
};

// Instead of patching all uses of a class, we can rewrite the class in the cache to point to the root.  This is the list
// of classes to pass to libobjc
struct ObjCClassReplacement
{
    const mach_header*  cacheMH;
    uintptr_t           cacheImpl;
    const mach_header*  rootMH;
    uintptr_t           rootImpl;
};

typedef void                (*NotifyFunc)(const mach_header* mh, intptr_t slide);
typedef void                (*LoadNotifyFunc)(const mach_header* mh, const char* path, bool unloadable);
typedef void                (*BulkLoadNotifier)(unsigned count, const mach_header* mhs[], const char* paths[]);
typedef int                 (*MainFunc)(int argc, const char* const argv[], const char* const envp[], const char* const apple[]);

#if BUILDING_DYLD
struct RuntimeLocks
{
    os_unfair_recursive_lock        loadersLock           = OS_UNFAIR_RECURSIVE_LOCK_INIT;
    os_unfair_recursive_lock        notifiersLock         = OS_UNFAIR_RECURSIVE_LOCK_INIT;
    os_unfair_recursive_lock        tlvInfosLock          = OS_UNFAIR_RECURSIVE_LOCK_INIT;
    os_unfair_recursive_lock        apiLock               = OS_UNFAIR_RECURSIVE_LOCK_INIT;
  #if !TARGET_OS_SIMULATOR
    os_lock_unfair_s                logSerializer         = OS_LOCK_UNFAIR_INIT;
  #endif
    pthread_mutex_t                 writableLock          = PTHREAD_MUTEX_INITIALIZER;
    int                             writeableCount        = 1;
};
#endif // !BUILDING_DYLD


struct WeakDefMapValue {
    const Loader*   targetLoader;
    uint64_t        targetRuntimeOffset : 62,
                    isCode              : 1,
                    isWeakDef           : 1;
};

typedef dyld3::CStringMapTo<WeakDefMapValue> WeakDefMap;

#if SUPPORT_VM_LAYOUT
struct EqualTypeConformanceLookupKey {
    static bool equal(const SwiftTypeProtocolConformanceDiskLocationKey& key, uint64_t typeDescriptor, uint64_t protocol, RuntimeState* state) {
        assert(state != nullptr);
        return (key.typeDescriptor.value(*state) == typeDescriptor) && (key.protocol.value(*state) == protocol);
    }
};

struct EqualMetadataConformanceLookupKey {
    static bool equal(const SwiftMetadataProtocolConformanceDiskLocationKey& key, uint64_t metadataDescriptor, uint64_t protocol, RuntimeState* state) {
        assert(state != nullptr);
        return (key.metadataDescriptor.value(*state) == metadataDescriptor) && (key.protocol.value(*state) == protocol);
    }
};

struct EqualForeignConformanceLookupKey {
    static bool equal(const SwiftForeignTypeProtocolConformanceDiskLocationKey& key, const char* foreignDescriptor, size_t foreignLength, uint64_t protocol, RuntimeState* state) {
        assert(state != nullptr);
        char* keyStr = (char*)key.foreignDescriptor.value(*state);
        return (strncmp(keyStr, foreignDescriptor, (size_t)key.foreignDescriptorNameLength) == 0) && (key.foreignDescriptorNameLength == foreignLength) && (key.protocol.value(*state) == protocol);
    }
};
#endif

const uint64_t twelveBitsMask = 0xFFF;

struct EqualTypeConformanceKey {
    static bool equal(const SwiftTypeProtocolConformanceDiskLocationKey& a, const SwiftTypeProtocolConformanceDiskLocationKey& b, void* state) {
        return ((a.typeDescriptor.offset() & twelveBitsMask) == (b.typeDescriptor.offset() & twelveBitsMask)) && ((a.protocol.offset()&twelveBitsMask) == (b.protocol.offset()&twelveBitsMask));
    }
};

struct HashTypeConformanceKey {
    static uint64_t hash(const SwiftTypeProtocolConformanceDiskLocationKey& v, void* state) {
        uint64_t keyA = v.typeDescriptor.offset() & twelveBitsMask;
        uint64_t keyB = v.protocol.offset() & twelveBitsMask;
        return murmurHash(&keyA, sizeof(uint64_t), 0) ^ murmurHash(&keyB, sizeof(uint64_t), 0);
    }
};

struct EqualMetadataConformanceKey {
    static bool equal(const SwiftMetadataProtocolConformanceDiskLocationKey& a, const SwiftMetadataProtocolConformanceDiskLocationKey& b, void* state) {
        return ((a.metadataDescriptor.offset()&twelveBitsMask) == (b.metadataDescriptor.offset()&twelveBitsMask)) && ((a.protocol.offset()&twelveBitsMask) == (b.protocol.offset()&twelveBitsMask));
    }
};

struct HashMetadataConformanceKey {
    static uint64_t hash(const SwiftMetadataProtocolConformanceDiskLocationKey& v, void* state) {
        uint64_t keyA = v.metadataDescriptor.offset() & twelveBitsMask;
        uint64_t keyB = v.protocol.offset() & twelveBitsMask;
        return murmurHash(&keyA, sizeof(uint64_t), 0) ^ murmurHash(&keyB, sizeof(uint64_t), 0);
    }
};

struct EqualForeignConformanceKey {
    static bool equal(const SwiftForeignTypeProtocolConformanceDiskLocationKey& a, const SwiftForeignTypeProtocolConformanceDiskLocationKey& b, void* state) {
        char* strA = nullptr;
        char* strB = nullptr;

        // State is only non-null here in dyld when calling from the APIs
#if SUPPORT_VM_LAYOUT
        if ( state != nullptr ) {
            RuntimeState* rState = (RuntimeState*)state;
            strA = (char*)a.foreignDescriptor.value(*rState);
            strB = (char*)b.foreignDescriptor.value(*rState);
        } else {
            strA = (char*)a.originalPointer;
            strB = (char*)b.originalPointer;
        }
#else
        strA = (char*)a.originalPointer;
        strB = (char*)b.originalPointer;
#endif

        return (strncmp(strA, strB, (size_t)a.foreignDescriptorNameLength) == 0) && (a.foreignDescriptorNameLength == b.foreignDescriptorNameLength) && ((a.protocol.offset()&twelveBitsMask) == (b.protocol.offset()&twelveBitsMask));
    }
};

struct HashForeignConformanceKey {
    static uint64_t hash(const SwiftForeignTypeProtocolConformanceDiskLocationKey& v, void* state) {
        char* str = nullptr;

        // State is only non-null here in dyld when calling from the APIs
#if SUPPORT_VM_LAYOUT
        if ( state != nullptr ) {
            RuntimeState* rState = (RuntimeState*)state;
            str = (char*)v.foreignDescriptor.value(*rState);
        } else {
            str = (char*)v.originalPointer;
        }
#else
        str = (char*)v.originalPointer;
#endif

        uint64_t keyPart = v.protocol.offset() & twelveBitsMask;
        return murmurHash(str, (int)v.foreignDescriptorNameLength, 0) ^ murmurHash(&keyPart, sizeof(uint64_t), 0);
    }
};

typedef dyld3::MultiMap<SwiftTypeProtocolConformanceDiskLocationKey, SwiftTypeProtocolConformanceDiskLocation, HashTypeConformanceKey, EqualTypeConformanceKey> TypeProtocolMap;
typedef dyld3::MultiMap<SwiftMetadataProtocolConformanceDiskLocationKey, SwiftMetadataProtocolConformanceDiskLocation, HashMetadataConformanceKey, EqualMetadataConformanceKey> MetadataProtocolMap;
typedef dyld3::MultiMap<SwiftForeignTypeProtocolConformanceDiskLocationKey, SwiftForeignTypeProtocolConformanceDiskLocation, HashForeignConformanceKey, EqualForeignConformanceKey> ForeignProtocolMap;


//
// Note: need to force vtable ptr auth so that libdyld.dylib from base OS and driverkit use same ABI
//
class [[clang::ptrauth_vtable_pointer(process_independent, address_discrimination, type_discrimination)]] RuntimeState
{
public:
    const ProcessConfig&            config;
    Allocator&                      persistentAllocator;
    EphemeralAllocator              ephemeralAllocator;
    const Loader*                   mainExecutableLoader = nullptr;
    Vector<ConstAuthLoader>         loaded;
    const Loader*                   libSystemLoader      = nullptr;
    const Loader*                   libdyldLoader        = nullptr;
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    const void*                     libdyldMissingSymbol = nullptr;
#endif
    uint64_t                        libdyldMissingSymbolRuntimeOffset = 0;
#if BUILDING_DYLD
    RuntimeLocks&                   _locks;
#endif
    dyld4::ProgramVars*             vars                 = nullptr;
    const LibSystemHelpers*         libSystemHelpers     = nullptr;
    Vector<InterposeTupleAll>       interposingTuplesAll;
    Vector<InterposeTupleSpecific>  interposingTuplesSpecific;
    Vector<InterposeTupleAll>       patchedObjCClasses;
    Vector<ObjCClassReplacement>    objcReplacementClasses;
    Vector<InterposeTupleAll>       patchedSingletons;
    size_t                          numSingletonObjectsPatched = 0;
    uint64_t                        weakDefResolveSymbolCount = 0;
    WeakDefMap*                     weakDefMap                = nullptr;
    TypeProtocolMap*                typeProtocolMap     = nullptr;
    MetadataProtocolMap*            metadataProtocolMap = nullptr;
    ForeignProtocolMap*             foreignProtocolMap  = nullptr;
    FileManager                     fileManager;

#if BUILDING_DYLD
                                RuntimeState(const ProcessConfig& c, Allocator& alloc, RuntimeLocks& locks)
#else
                                RuntimeState(const ProcessConfig& c, Allocator& alloc = Allocator::defaultAllocator())
#endif
    : config(c), persistentAllocator(alloc), loaded(alloc),
#if BUILDING_DYLD
                                    _locks(locks),
#endif
                                    interposingTuplesAll(alloc), interposingTuplesSpecific(alloc),
                                    patchedObjCClasses(alloc), objcReplacementClasses(alloc),
                                    patchedSingletons(alloc),
                                    fileManager(persistentAllocator, &config.syscall),
                                    _notifyAddImage(alloc), _notifyRemoveImage(alloc),
                                    _notifyLoadImage(alloc), _notifyBulkLoadImage(alloc),
                                    _tlvInfos(alloc), _loadersNeedingDOFUnregistration(alloc),
                                    _missingFlatLazySymbols(alloc), _dynamicReferences(alloc),
                                    _dlopenRefCounts(alloc), _dynamicNeverUnloads(alloc) {}

    void                        setMainLoader(const Loader*);
    void                        add(const Loader*);

    MainFunc                    mainFunc()                   { return _driverKitMain; }
    void                        setMainFunc(MainFunc func)   { _driverKitMain = func; }

    void                        setDyldLoader(const Loader* ldr);

    uint8_t*                    appState(uint16_t index);
    uint8_t*                    cachedDylibState(uint16_t index);
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    const MachOFile*            appMF(uint16_t index);
    void                        setAppMF(uint16_t index, const MachOFile* mf);
    const MachOFile*            cachedDylibMF(uint16_t index);
    const mach_o::Layout*       cachedDylibLayout(uint16_t index);
#else
    const MachOLoaded*          appLoadAddress(uint16_t index);
    void                        setAppLoadAddress(uint16_t index, const MachOLoaded* ml);
    const MachOLoaded*          cachedDylibLoadAddress(uint16_t index);
#endif

    void                        log(const char* format, ...) const __attribute__((format(printf, 2, 3))) ;
    void                        vlog(const char* format, va_list list) __attribute__((format(printf, 2, 0)));

    void                        setObjCNotifiers(_dyld_objc_notify_mapped, _dyld_objc_notify_init, _dyld_objc_notify_unmapped,
                                                 _dyld_objc_notify_patch_class, _dyld_objc_notify_mapped2);
    void                        addNotifyAddFunc(const Loader* callbackLoader, NotifyFunc);
    void                        addNotifyRemoveFunc(const Loader* callbackLoader, NotifyFunc);
    void                        addNotifyLoadImage(const Loader* callbackLoader, LoadNotifyFunc);
    void                        addNotifyBulkLoadImage(const Loader* callbackLoader, BulkLoadNotifier);

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    void                        notifyObjCInit(const Loader* ldr);
    void                        buildInterposingTables();
#endif

    void                        withNotifiersReadLock(void (^work)());

    template<typename F>
    ALWAYS_INLINE void          withNotifiersWriteLock(F work)
    {
    #if BUILDING_DYLD
        if ( this->libSystemHelpers != nullptr ) {
            this->libSystemHelpers->os_unfair_recursive_lock_lock_with_options(&_locks.notifiersLock, OS_UNFAIR_LOCK_NONE);
              this->incWritable();
                 work();
              this->decWritable();
            this->libSystemHelpers->os_unfair_recursive_lock_unlock(&_locks.notifiersLock);
        }
        else
    #endif
        {
            work();
        }
    }

    void                        addPermanentRanges(const Array<const Loader*>& neverUnloadLoaders);
    bool                        inPermanentRange(uintptr_t start, uintptr_t end, uint8_t* perms, const Loader** loader);

    void                        notifyLoad(const std::span<const Loader*>& newLoaders);
    void                        notifyUnload(const std::span<const Loader*>& removeLoaders);
    void                        doSingletonPatching();
    void                        notifyObjCPatching();
    void                        notifyDebuggerLoad(const Loader* oneLoader);
    void                        notifyDebuggerLoad(const std::span<const Loader*>& newLoaders);
    void                        notifyDebuggerUnload(const std::span<const Loader*>& removingLoaders);
    void                        notifyDtrace(const std::span<const Loader*>& newLoaders);

    void                        incDlRefCount(const Loader* ldr);  // used by dlopen
    void                        decDlRefCount(const Loader* ldr);  // used by dlclose

    void                        setLaunchMissingDylib(const char* missingDylibPath, const char* clientUsingDylib);
    void                        setLaunchMissingSymbol(const char* missingSymbolName, const char* dylibThatShouldHaveSymbol, const char* clientUsingSymbol);

    void                        addMissingFlatLazySymbol(const Loader* ldr, const char* symbolName, uintptr_t* bindLoc);
    void                        rebindMissingFlatLazySymbols(const std::span<const Loader*>& newLoaders);
    void                        removeMissingFlatLazySymbols(const std::span<const Loader*>& removingLoaders);
    bool                        hasMissingFlatLazySymbols() const;

    void                        addDynamicReference(const Loader* from, const Loader* to);
    void                        removeDynamicDependencies(const Loader* removee);

    void                        setSavedPrebuiltLoaderSet()      { _wrotePrebuiltLoaderSet = true; }
    bool                        didSavePrebuiltLoaderSet() const { return _wrotePrebuiltLoaderSet; }

    void                        setVMAccountingSuspending(bool mode);
    bool                        hasOverriddenCachedDylib() { return _hasOverriddenCachedDylib; }
    void                        setHasOverriddenCachedDylib() { _hasOverriddenCachedDylib = true; }

    pthread_key_t               dlerrorPthreadKey() { return _dlerrorPthreadKey; }

    typedef void  (*TLV_TermFunc)(void* objAddr);

    void                        initialize();
    void                        setUpTLVs(const MachOAnalyzer* ma);
    void                        addTLVTerminationFunc(TLV_TermFunc func, void* objAddr);
    void                        exitTLV();

    void                        withLoadersReadLock(void (^work)());

    template<typename F>
    ALWAYS_INLINE void          withLoadersWriteLock(F work)
    {
        #if BUILDING_DYLD
        if ( this->libSystemHelpers != nullptr ) {
            this->libSystemHelpers->os_unfair_recursive_lock_lock_with_options(&_locks.loadersLock, OS_UNFAIR_LOCK_NONE);
            this->incWritable();
            work();
            this->decWritable();
            this->libSystemHelpers->os_unfair_recursive_lock_unlock(&_locks.loadersLock);
        }
        else
        #endif
        {
            work();
        }
    }

    ALWAYS_INLINE void          incWritable()
    {
        #if BUILDING_DYLD
        // FIXME: move inc/decWritable() into Allocator to replace writeProtect()
        pthread_mutex_lock(&_locks.writableLock);
        _locks.writeableCount += 1;
        if ( _locks.writeableCount == 1 ) {
            {
                persistentAllocator.writeProtect(false);
            }
        }
        pthread_mutex_unlock(&_locks.writableLock);
        #endif
    }

    ALWAYS_INLINE void          decWritable()
    {
        #if BUILDING_DYLD
        pthread_mutex_lock(&_locks.writableLock);
        _locks.writeableCount -= 1;
        if ( _locks.writeableCount == 0 ) {
            // We are done all write operations, so we can tear down the ephemeral allocator and write protect the persistent one
#if !TARGET_OS_SIMULATOR
            freeProcessSnapshot();
#endif
            ephemeralAllocator.reset();
            {
                persistentAllocator.writeProtect(true);
            }
        }
        pthread_mutex_unlock(&_locks.writableLock);
        #endif
    }

#if BUILDING_DYLD
    void                        initializeClosureMode();
#endif
    const PrebuiltLoaderSet*    processPrebuiltLoaderSet() const { return _processPrebuiltLoaderSet; }
    const PrebuiltLoaderSet*    cachedDylibsPrebuiltLoaderSet() const { return _cachedDylibsPrebuiltLoaderSet; }
    uint8_t*                    prebuiltStateArray(bool app) const { return (app ? _processDylibStateArray : _cachedDylibsStateArray); }
    void                        setInitialImageCount(uint32_t count) { _initialImageCount = count; }
    uint32_t                    initialImageCount() const { return _initialImageCount; }

    const PrebuiltLoader*       findPrebuiltLoader(const char* loadPath) const;
    bool                        saveAppClosureFile() const { return _saveAppClosureFile; }
    bool                        failIfCouldBuildAppClosureFile() const { return _failIfCouldBuildAppClosureFile; }
    bool                        saveAppPrebuiltLoaderSet(const PrebuiltLoaderSet* pblset) const;
    bool                        inPrebuiltLoader(const void* p, size_t len) const;
    const UUID&                 uuidForFileSystem(uint64_t fsid);
    uint64_t                    fsidForUUID(const UUID& uuid);
#if !BUILDING_DYLD
    void                        resetCachedDylibs(const PrebuiltLoaderSet* dylibs, uint8_t* stateArray);
    void                        setProcessPrebuiltLoaderSet(const PrebuiltLoaderSet* appPBLS);
    void                        resetCachedDylibsArrays(const PrebuiltLoaderSet* cachedDylibsPBLS);
#endif

    // this need to be virtual to be callable from libdyld.dylb
    virtual void                _finalizeListTLV(void* l);
    virtual void*               _instantiateTLVs(pthread_key_t key);

#if !TARGET_OS_SIMULATOR
    ProcessSnapshot*            getCurrentProcessSnapshot();
    void                        commitProcessSnapshot();
    void                        freeProcessSnapshot();
#endif

protected:

    // Helpers to reset locks across fork()
    void                        takeLockBeforeFork();
    void                        releaseLockInForkParent();
    void                        resetLockInForkChild();
    void                        takeDlopenLockBeforeFork();
    void                        releaseDlopenLockInForkParent();
    void                        resetDlopenLockInForkChild();

private:
    //
    // The PermanentRanges structure is used to make dyld_is_memory_immutable()
    // fast and lock free. The table contains just ranges of memory that are in
    // images that will never be unloaded.  Dylibs in the dyld shared cache are
    // never in this table. A PermanentRanges struct is allocated at launch for
    // app and its non-cached dylibs, because they can never be unloaded. Later
    // if a dlopen() brings in non-cached dylibs which can never be unloaded,
    // another PermanentRanges is allocated with the ranges brought in by that
    // dlopen.  The PermanentRanges struct are chained together in a linked list
    // with state._permanentRanges pointing to the start of the list.
    // Because these structs never change, they can be read without taking a lock.
    // That makes finding immutable ranges lock-less.
    //
    class PermanentRanges
    {
    public:
        static PermanentRanges* make(RuntimeState& state, const Array<const Loader*>& neverUnloadLoaders);
        bool                    contains(uintptr_t start, uintptr_t end, uint8_t* perms, const Loader** loader) const;
        PermanentRanges*        next() const;
        void                    append(PermanentRanges*);

    private:
        void                addPermanentRange(uintptr_t start, uintptr_t end, bool immutable, const Loader* loader);
        void                add(const Loader*);

        struct Range
        {
            uintptr_t      start;
            uintptr_t      end;
            const Loader*  loader;
            uintptr_t      permissions;
        };

        // FIXME: we could pack this structure better to reduce memory usage
        std::atomic<PermanentRanges*>   _next       = nullptr;
        uintptr_t                       _rangeCount = 0;
        Range                           _ranges[1];
    };

    // keep dlopen counts in a side table because it is rarely used, so it would waste space for each Loader object to have its own count field
    friend class Reaper;
    friend class RecursiveAutoLock;
    struct DlopenCount {
        const Loader*  loader;
        uintptr_t      refCount;
    };

    // when a thread_local is first accessed on a thread, the thunk calls into dyld
    // to allocate the variables.  The pthread_key is the index used to find the
    // TLV_Info which then describes how much to allocate and how to initalize that memory.
    struct TLV_Info
    {
        const MachOAnalyzer*    ma;
        pthread_key_t           key;
        uint32_t                initialContentOffset;
        uint32_t                initialContentSize;
    };

    // used to record _tlv_atexit() entries to clean up on thread exit
    struct TLV_Terminator
    {
        TLV_TermFunc  termFunc;
        void*         objAddr;
    };

    struct TLV_TerminatorList {
        TLV_TerminatorList* next  = nullptr;
        uintptr_t           count = 0;
        TLV_Terminator      elements[7];

        void                reverseWalkChain(void (^work)(TLV_TerminatorList*));
    };

    struct RegisteredDOF
    {
        const Loader*   ldr;
        int             registrationID;
    };

    struct MissingFlatSymbol
    {
        const Loader*   ldr;
        const char*     symbolName;
        uintptr_t*      bindLoc;
    };

    struct DynamicReference
    {
        const Loader*   from;
        const Loader*   to;
    };

    struct HiddenCacheAddr { const void* cacheAddr; const void* replacementAddr; };

    enum { kMaxBootTokenSize = 128 };

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    void                        appendInterposingTuples(const Loader* ldr, const uint8_t* dylibTuples, uint32_t tupleCount);
#endif
    void                        garbageCollectImages();
    void                        garbageCollectInner();
    void                        removeLoaders(const std::span<const Loader*>& loadersToRemove);
    void                        withTLVLock(void (^work)());
    void                        setUpLogging();
    void                        buildAppPrebuiltLoaderSetPath(bool createDirs);
    bool                        fileAlreadyHasBootToken(const char* path, const Array<uint8_t>& bootToken) const;
    bool                        buildBootToken(dyld3::Array<uint8_t>& bootToken) const;
    void                        loadAppPrebuiltLoaderSet();
    bool                        allowOsProgramsToSaveUpdatedClosures() const;
    bool                        allowNonOsProgramsToSaveUpdatedClosures() const;
#if BUILDING_DYLD || BUILDING_CLOSURE_UTIL
    void                        allocateProcessArrays(uintptr_t count);
#endif
    void                        checkHiddenCacheAddr(const Loader* t, const void* targetAddr, const char* symbolName, dyld3::OverflowSafeArray<HiddenCacheAddr>& hiddenCacheAddrs) const;
    void                        setDyldPatchedObjCClasses() const;
    void                        reloadFSInfos();

    _dyld_objc_notify_mapped        _notifyObjCMapped       = nullptr;
    _dyld_objc_notify_init          _notifyObjCInit         = nullptr;
    _dyld_objc_notify_unmapped      _notifyObjCUnmapped     = nullptr;
    _dyld_objc_notify_patch_class   _notifyObjCPatchClass   = nullptr;
    _dyld_objc_notify_mapped2       _notifyObjCMapped2       = nullptr;
    Vector<NotifyFunc>              _notifyAddImage;
    Vector<NotifyFunc>              _notifyRemoveImage;
    Vector<LoadNotifyFunc>          _notifyLoadImage;
    Vector<BulkLoadNotifier>        _notifyBulkLoadImage;
    Vector<TLV_Info>                _tlvInfos;
    Vector<RegisteredDOF>           _loadersNeedingDOFUnregistration;
    Vector<MissingFlatSymbol>       _missingFlatLazySymbols;
    Vector<DynamicReference>        _dynamicReferences;
    const PrebuiltLoaderSet*        _cachedDylibsPrebuiltLoaderSet  = nullptr;
    uint8_t*                        _cachedDylibsStateArray         = nullptr;
    const char*                     _processPrebuiltLoaderSetPath   = nullptr;
    const PrebuiltLoaderSet*        _processPrebuiltLoaderSet       = nullptr;
    uint8_t*                        _processDylibStateArray         = nullptr;
#if SUPPORT_VM_LAYOUT
    const MachOLoaded**             _processLoadedAddressArray      = nullptr;
#else
    const MachOFile**               _processLoadedMachOArray        = nullptr;
#endif
    bool                            _saveAppClosureFile;
    bool                            _failIfCouldBuildAppClosureFile;
    uint32_t                        _initialImageCount        = 256;
    PermanentRanges*                _permanentRanges          = nullptr;
    MainFunc                        _driverKitMain            = nullptr;
    Vector<DlopenCount>             _dlopenRefCounts;
    Vector<const Loader*>           _dynamicNeverUnloads;
    std::atomic<int32_t>            _gcCount                  = 0;
    pthread_key_t                   _tlvTerminatorsKey        = 0;
    pthread_key_t                   _dlerrorPthreadKey        = 0;
    int                             _logDescriptor            = -1;
    bool                            _logToSyslog              = false;
    bool                            _logSetUp                 = false;
    bool                            _hasOverriddenCachedDylib = false;
    bool                            _wrotePrebuiltLoaderSet   = false;
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    bool                            _vmAccountingSuspended    = false;
#endif // TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    UniquePtr<OrderedMap<uint64_t,UUID>>    _fsUUIDMap;
    ProcessSnapshot*                        _processSnapshot    = nullptr;
};



// Helper class for temp change of permissions on __DATA_CONST of shared cache
class DyldCacheDataConstLazyScopedWriter
{
public:
    DyldCacheDataConstLazyScopedWriter(RuntimeState&);
    ~DyldCacheDataConstLazyScopedWriter();

    void makeWriteable() const;

protected:
    // Delete all other kinds of constructors to make sure we don't accidentally copy these around
                                        DyldCacheDataConstLazyScopedWriter() = delete;
                                        DyldCacheDataConstLazyScopedWriter(const DyldCacheDataConstLazyScopedWriter&) = delete;
                                        DyldCacheDataConstLazyScopedWriter(DyldCacheDataConstLazyScopedWriter&&) = delete;
    DyldCacheDataConstLazyScopedWriter& operator=(const DyldCacheDataConstLazyScopedWriter&) = delete;
    DyldCacheDataConstLazyScopedWriter& operator=(DyldCacheDataConstLazyScopedWriter&&) = delete;


private:
    RuntimeState&  _state;
    mutable bool   _wasMadeWritable;
};

class DyldCacheDataConstScopedWriter : public DyldCacheDataConstLazyScopedWriter
{
public:
    DyldCacheDataConstScopedWriter(RuntimeState&);
    ~DyldCacheDataConstScopedWriter() = default;

protected:
    // Delete all other kinds of constructors to make sure we don't accidentally copy these around
                                    DyldCacheDataConstScopedWriter() = delete;
                                    DyldCacheDataConstScopedWriter(const DyldCacheDataConstScopedWriter&) = delete;
                                    DyldCacheDataConstScopedWriter(DyldCacheDataConstScopedWriter&&) = delete;
    DyldCacheDataConstScopedWriter& operator=(const DyldCacheDataConstScopedWriter&) = delete;
    DyldCacheDataConstScopedWriter& operator=(DyldCacheDataConstScopedWriter&&) = delete;
};



void notifyMonitoringDyld(bool unloading, unsigned imageCount, const mach_header* loadAddresses[], const char* imagePaths[]);
void notifyMonitoringDyldMain();
void notifyMonitoringDyldBeforeInitializers();
#if BUILDING_DYLD && !TARGET_OS_SIMULATOR
void notifyMonitoringDyldSimSupport(bool unloading, unsigned imageCount, const mach_header* loadAddresses[], const char* imagePaths[]);
#endif
void coresymbolication_load_notifier(void* connection, uint64_t timestamp, const char* path, const mach_header* mh);
void coresymbolication_unload_notifier(void* connection, uint64_t timestamp, const char* path, const mach_header* mh);
kern_return_t mach_msg_sim_interposed(mach_msg_header_t* msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                                      mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify);

//
// The implementation of all dyld load/unload API's must hold a global lock
// so that the next load/unload does start until the current is complete.
// This lock is recursive so that initializers can call dlopen().
// This is done using the macros DYLD_LOCK_THIS_BLOCK.
// Example:
//
//  void dyld_load_api()
//  {
//      RecursiveAutoLock apiLock;
//      // free to do stuff here
//      // that accesses dyld internal data structures
//  }
//
class VIS_HIDDEN RecursiveAutoLock
{
public:
    RecursiveAutoLock(RuntimeState& state, bool skip=false);
    ~RecursiveAutoLock();
private:
    const LibSystemHelpers*     _libSystemHelpers = nullptr;
#if BUILDING_DYLD
    os_unfair_recursive_lock&   _lock;
    bool                        _skip;
#endif
};


} // namespace



#endif /* DyldRuntimeState_h */
