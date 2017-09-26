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



#ifndef ImageProxy_h
#define ImageProxy_h

#include <stdint.h>

#include <string>
#include <vector>
#include <set>
#include <unordered_map>

#include "DyldSharedCache.h"
#include "Diagnostics.h"
#include "LaunchCache.h"
#include "LaunchCacheWriter.h"
#include "PathOverrides.h"
#include "ClosureBuffer.h"
#include "DyldCacheParser.h"


namespace dyld3 {

typedef launch_cache::binary_format::Image              BinaryImageData;
typedef launch_cache::binary_format::ImageGroup         BinaryImageGroupData;
typedef launch_cache::binary_format::Closure            BinaryClosureData;
typedef launch_cache::binary_format::ImageRef           ImageRef;
typedef launch_cache::Image::LinkKind                   LinkKind;
typedef launch_cache::ImageGroupWriter::FixUp           FixUp;
typedef launch_cache::binary_format::DyldCacheOverride  DyldCacheOverride;
typedef launch_cache::ImageGroupList                    ImageGroupList;




class ImageProxyGroup;

class ImageProxy
{
public:
                            ImageProxy(const mach_header* mh, const BinaryImageData* image, uint32_t indexInGroup, bool dyldCacheIsRaw);
                            ImageProxy(const DyldSharedCache::MappedMachO& mapping, uint32_t groupNum, uint32_t indexInGroup, bool dyldCacheIsRaw);

    struct RPathChain {
        ImageProxy*                         inProxy;
        const RPathChain*                   prev;
        const std::vector<std::string>&     rpaths;
    };

    struct InitOrderInfo {
        bool                     beforeHas(ImageRef);
        bool                     upwardHas(ImageProxy*);
        void                     removeRedundantUpwards();
        std::vector<ImageRef>    initBefore;
        std::vector<ImageProxy*> danglingUpward;
    };

    struct FixupInfo {
        std::vector<FixUp>  fixups;
        bool                hasTextRelocs = false;
    };

    void                    recursiveBuildInitBeforeInfo(ImageProxyGroup& owningGroup);
    void                    addDependentsShallow(ImageProxyGroup& owningGroup, RPathChain* chain=nullptr);
    void                    addDependentsDeep(ImageProxyGroup& owningGroup, RPathChain* chain, bool staticallyReferenced);
    void                    markInvalid() { _invalid = true; }

    uint32_t                groupNum() const                { return _groupNum; }
    uint32_t                indexInGroup() const            { return _indexInGroup; }
    const mach_header*      mh() const                      { return _mh; }
    const std::string&      runtimePath() const             { return _runtimePath; }
    uint64_t                sliceFileOffset() const         { return _sliceFileOffset; }
    uint64_t                fileModTime() const             { return _modTime; }
    uint64_t                fileInode() const               { return _inode; }
    bool                    isSetUID() const                { return _isSetUID; }
    bool                    invalid() const                 { return _invalid; }
    bool                    staticallyReferenced() const    { return _staticallyReferenced; }
    bool                    cwdMustBeThisDir() const        { return _cwdMustBeThisDir; }
    bool                    isPlatformBinary() const        { return _platformBinary; }
    bool                    isProxyForCachedDylib() const   { return _imageBinaryData != nullptr; }
    const Diagnostics&      diagnostics() const             { return _diag; }
    ImageRef                overrideOf() const              { return _overrideOf; }
    bool                    inLibSystem() const;
    void                    setCwdMustBeThisDir()           { _cwdMustBeThisDir = true; }
    void                    setPlatformBinary()             { _platformBinary = true; }
    void                    setOverrideOf(uint32_t groupNum, uint32_t indexInGroup);
    void                    checkIfImageOverride(const std::string& runtimeLoadPath);
    void                    forEachDependent(void (^handler)(ImageProxy* dep, LinkKind)) const;
    FixupInfo               buildFixups(Diagnostics& diag, uint64_t cacheUnslideBaseAddress, launch_cache::ImageGroupWriter& groupWriter) const;
    bool                    findExportedSymbol(Diagnostics& diag, const char* symbolName, MachOParser::FoundSymbol& foundInfo) const;
    void                    convertInitBeforeInfoToArray(ImageProxyGroup& owningGroup);
    void                    addToFlatLookup(std::vector<ImageProxy*>& imageList);
    const std::vector<ImageRef>&    getInitBeforeList(ImageProxyGroup& owningGroup);
    const std::vector<std::string>& rpaths() { return _rpaths; }

private:
    void                    processRPaths(ImageProxyGroup& owningGroup);

    const mach_header*     const _mh;
    uint64_t               const _sliceFileOffset;
    uint64_t               const _modTime;
    uint64_t               const _inode;
    const BinaryImageData* const _imageBinaryData;    // only used if proxy is for image in shared cache
    std::string            const _runtimePath;
    bool                   const _isSetUID;
    bool                   const _dyldCacheIsRaw;
    uint32_t               const _groupNum;
    uint32_t               const _indexInGroup;
    bool                         _platformBinary;
    Diagnostics                  _diag;
    std::vector<ImageProxy*>     _dependents;
    std::vector<LinkKind>        _dependentsKind;
    std::vector<std::string>     _rpaths;
    InitOrderInfo                _initBeforesInfo;
    std::vector<ImageRef>        _initBeforesArray;
    ImageRef                     _overrideOf;
    bool                         _directDependentsSet;
    bool                         _deepDependentsSet;
    bool                         _initBeforesArraySet;
    bool                         _initBeforesComputed;
    bool                         _invalid;
    bool                         _staticallyReferenced;
    bool                         _cwdMustBeThisDir;
};


class ImageProxyGroup
{
public:
                                    ~ImageProxyGroup();


    typedef std::unordered_map<const mach_header*, std::unordered_map<uint32_t, std::unordered_set<uint32_t>>> PatchTable;


    // used when building dyld shared cache
    static ImageProxyGroup*         makeDyldCacheDylibsGroup(Diagnostics& diag, const DyldCacheParser& dyldCache, const std::vector<DyldSharedCache::MappedMachO>& cachedDylibs,
                                                             const std::vector<std::string>& buildTimePrefixes, const PatchTable& patchTable,
                                                             bool stubEliminated, bool dylibsExpectedOnDisk);

    // used when building dyld shared cache
    static ImageProxyGroup*         makeOtherOsGroup(Diagnostics& diag, const DyldCacheParser& dyldCache, ImageProxyGroup* cachedDylibsGroup,
                                                     const std::vector<DyldSharedCache::MappedMachO>& otherDylibsAndBundles,
                                                     bool inodesAreSameAsRuntime, const std::vector<std::string>& buildTimePrefixes);

     const BinaryImageGroupData*    makeImageGroupBinary(Diagnostics& diag, const char* const neverEliminateStubs[]=nullptr);

    // used when building dyld shared cache
    static BinaryClosureData*       makeClosure(Diagnostics& diag, const DyldCacheParser& dyldCache, ImageProxyGroup* cachedDylibsGroup,
                                                ImageProxyGroup* otherOsDylibs, const DyldSharedCache::MappedMachO& mainProg,
                                                bool inodesAreSameAsRuntime, const std::vector<std::string>& buildTimePrefixes);

    // used by closured for dlopen of unknown dylibs
    static const BinaryImageGroupData* makeDlopenGroup(Diagnostics& diag, const DyldCacheParser& dyldCache, uint32_t groupNum,
                                                       const std::vector<const BinaryImageGroupData*>& existingGroups,
                                                       const std::string& imagePath, const std::vector<std::string>& envVars);

    static const BinaryImageGroupData* makeDlopenGroup(Diagnostics& diag, const ClosureBuffer& buffer, task_t requestor, const std::vector<std::string>& buildTimePrefixes={});

    static BinaryClosureData*          makeClosure(Diagnostics& diag, const ClosureBuffer& buffer, task_t requestor, const std::vector<std::string>& buildTimePrefixes={});


    //
    // Creates a binary launch closure for the specified main executable.
    // Used by closured and dyld_closure_util
    //
    // The closure is allocated with malloc().  Use free() to release when done.
    // The size of the closure can be determined using Closure::size().
    // If the closure cannot be built (e.g. app needs a symbol not exported by a framework),
    // the reason for the failure is returned as a string in the diag parameter.
    // The mainProgRuntimePath path is the path the program will be at runtime.
    // The buildTimePrefixes is a list of prefixes to add to each path during closure
    // creation to find the files at buildtime.
    //
   static BinaryClosureData*       makeClosure(Diagnostics& diag, const DyldCacheParser& dyldCache,
                                               const std::string& mainProgRuntimePath, bool includeDylibsInDir,
                                               const std::vector<std::string>& buildTimePrefixes={},
                                               const std::vector<std::string>& envVars={});


private:
    friend class ImageProxy;

                                    ImageProxyGroup(uint32_t groupNum, const DyldCacheParser& dyldCache, const BinaryImageGroupData* basedOn,
                                                    ImageProxyGroup* next, const std::string& mainProgRuntimePath,
                                                    const std::vector<const BinaryImageGroupData*>& knownGroups,
                                                    const std::vector<std::string>& buildTimePrefixes,
                                                    const std::vector<std::string>& envVars,
                                                    bool stubsEliminated=false, bool dylibsExpectedOnDisk=true, bool inodesAreSameAsRuntime=true);

    ImageProxy*                     findImage(Diagnostics& diag, const std::string& runtimePath, bool canBeMissing, ImageProxy::RPathChain*);
    ImageProxy*                     findAbsoluteImage(Diagnostics& diag, const std::string& runtimePath, bool canBeMissing, bool makeErrorMessage, bool pathIsReal=false);
    bool                            builtImageStillValid(const launch_cache::Image& image);
    const std::string&              mainProgRuntimePath() { return _mainProgRuntimePath; }
    DyldSharedCache::MappedMachO*   addMappingIfValidMachO(Diagnostics& diag, const std::string& runtimePath, bool ignoreMainExecutables=false);
    BinaryClosureData*              makeClosureBinary(Diagnostics& diag, ImageProxy* mainProg, bool includeDylibsInDir);
    void                            findLibdyldEntry(Diagnostics& diag, ImageRef& ref, uint32_t& offset);
    void                            findLibSystem(Diagnostics& diag, bool sim, ImageRef& ref);
    void                            populateGroupWriter(Diagnostics& diag, launch_cache::ImageGroupWriter& groupWriter, const char* const neverEliminateStubs[]=nullptr);
    std::string                     normalizedPath(const std::string& path);
    void                            addExtraMachOsInBundle(const std::string& appDir);
    bool                            addInsertedDylibs(Diagnostics& diag);
    std::vector<ImageProxy*>        flatLookupOrder();

    PathOverrides                                   _pathOverrides;
    const BinaryImageGroupData*                     _basedOn;   // if not null, then lazily populate _images
    const PatchTable*                               _patchTable;
    ImageProxyGroup*  const                         _nextSearchGroup;
    const DyldCacheParser                           _dyldCache;
    uint32_t const                                  _groupNum;
    bool const                                      _stubEliminated;
    bool const                                      _dylibsExpectedOnDisk;
    bool const                                      _inodesAreSameAsRuntime;
    uint32_t                                        _mainExecutableIndex;
    std::vector<const BinaryImageGroupData*>        _knownGroups;
    std::vector<ImageProxy*>                        _images;
    std::unordered_map<std::string, ImageProxy*>    _pathToProxy;
    std::vector<DyldSharedCache::MappedMachO>       _ownedMappings;
    std::vector<std::string>                        _buildTimePrefixes;
    std::vector<DyldCacheOverride>                  _cacheOverrides;
    std::string                                     _mainProgRuntimePath;
    std::string                                     _archName;
    Platform                                        _platform;
    std::set<std::string>                           _mustBeMissingFiles;
};





}

#endif // ImageProxy_h
