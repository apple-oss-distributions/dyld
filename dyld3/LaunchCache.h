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


#ifndef LaunchCache_h
#define LaunchCache_h


#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <mach-o/loader.h>

#if !DYLD_IN_PROCESS
  #include <vector>
  #include <unordered_set>
  #include <string>
  #include "shared-cache/DyldSharedCache.h"
#endif

#include "Diagnostics.h"

#define VIS_HIDDEN __attribute__((visibility("hidden")))


namespace dyld3 {

class DyldCacheParser;

namespace launch_cache {


namespace binary_format {
    struct Image;
    struct ImageGroup;
    union  ImageRef;
    struct Closure;
    struct DiskImage;
    struct CachedImage;
    struct AllFixupsBySegment;
    struct SegmentFixupsByPage;
}

typedef binary_format::Image                BinaryImageData;
typedef binary_format::ImageGroup           BinaryImageGroupData;
typedef binary_format::Closure              BinaryClosureData;


struct VIS_HIDDEN MemoryRange
{
    bool        contains(const MemoryRange& other) const;
    bool        intersects(const MemoryRange& other) const;

    const void* address;
    uint64_t    size;
};


class VIS_HIDDEN SlowLoadSet
{
public:
            SlowLoadSet(const BinaryImageData** start, const BinaryImageData** end) : _start(start), _end(end), _current(start) { }
    bool    contains(const BinaryImageData*);
    bool    add(const BinaryImageData*);
    void    forEach(void (^handler)(const BinaryImageData*));
    void    forEach(void (^handler)(const BinaryImageData*, bool& stop));
    long    count() const;
private:
    const BinaryImageData** const  _start;
    const BinaryImageData** const  _end;
    const BinaryImageData**        _current;
};

struct ImageGroup;


template <typename T>
class VIS_HIDDEN DynArray
{
public:
                DynArray(uintptr_t count, T* storage) : _count(count), _elements(storage) { }
#if !DYLD_IN_PROCESS
                DynArray(const std::vector<T>& vec) : _count(vec.size()), _elements((T*)&vec[0]) { }
#endif

    T&          operator[](size_t idx)        { assert(idx < _count); return _elements[idx]; }
    const T&    operator[](size_t idx) const  { assert(idx < _count); return _elements[idx]; }
    uintptr_t   count() const                      { return _count; }
private:
    uintptr_t  _count;
    T*         _elements;
};


//  STACK_ALLOC_DYNARRAY(foo, 10, myarray);
#define STACK_ALLOC_DYNARRAY(_type, _count, _name)  \
    uintptr_t __##_name##_array_alloc[1 + ((sizeof(_type)*(_count))/sizeof(uintptr_t))]; \
    dyld3::launch_cache::DynArray<_type> _name(_count, (_type*)__##_name##_array_alloc);


typedef DynArray<const BinaryImageGroupData*> ImageGroupList;


// In the pre-computed fixups for an Image, each fixup location is set to a TargetSymbolValue
// which is an abstract encoding of a resolved symbol in an image that can be turned into a
// real address once all ASLR slides are known.
struct VIS_HIDDEN TargetSymbolValue
{
#if DYLD_IN_PROCESS
    class LoadedImages
    {
    public:
        virtual const uint8_t*      dyldCacheLoadAddressForImage() = 0;
        virtual const mach_header*  loadAddressFromGroupAndIndex(uint32_t groupNum, uint32_t indexInGroup) = 0;
        virtual void                forEachImage(void (^handler)(uint32_t anIndex, const BinaryImageData*, const mach_header*, bool& stop)) = 0;
        virtual void                setAsNeverUnload(uint32_t anIndex) = 0;
    };

    uintptr_t                resolveTarget(Diagnostics& diag, const ImageGroup& inGroup, LoadedImages& images) const;
#else
    static TargetSymbolValue makeInvalid();
    static TargetSymbolValue makeAbsolute(uint64_t value);
    static TargetSymbolValue makeSharedCacheOffset(uint32_t offset);
    static TargetSymbolValue makeGroupValue(uint32_t groupIndex, uint32_t imageIndexInGroup, uint64_t offsetInImage, bool isIndirectGroupNum);
    static TargetSymbolValue makeDynamicGroupValue(uint32_t imagePathPoolOffset, uint32_t imageSymbolPoolOffset, bool weakImport);
    std::string              asString(ImageGroup group) const;
    bool                     operator==(const TargetSymbolValue& other) const { return (_data.raw == other._data.raw); }
    bool                     isSharedCacheTarget(uint64_t& offsetInCache) const;
    bool                     isGroupImageTarget(uint32_t& groupNum, uint32_t& indexInGroup, uint64_t& offsetInImage) const;
    bool                     isInvalid() const;
#endif
private:
                             TargetSymbolValue();
    
    enum Kinds { kindSharedCache, kindAbsolute, kindGroup, kindDynamicGroup };


    struct SharedCacheOffsetTarget {
        uint64_t    kind            :  2,       // kindSharedCache
                    offsetIntoCache : 62;
    };
    struct AbsoluteTarget {
        uint64_t    kind            :  2,       // kindAbsolute
                    value           : 62;
    };
    struct GroupImageTarget {
        uint64_t    kind            :  2,       // kindGroup
                    isIndirectGroup :  1,       // 0 => use groupNum directly.  1 => index indirect side table
                    groupNum        :  7,       // 0 not used, 1 => other dylibs, 2 => main closure, 3 => first dlopen group
                    indexInGroup    : 12,
                    offsetInImage   : 42;
    };
    struct DynamicGroupImageTarget {
        uint64_t    kind            :  2,       // kindDynamicGroup
                    weakImport      :  1,
                    imagePathOffset : 30,
                    symbolNameOffset: 31;
    };
    union {
        SharedCacheOffsetTarget sharedCache;
        AbsoluteTarget          absolute;
        GroupImageTarget        group;
        DynamicGroupImageTarget dynamicGroup;
        uint64_t                raw;
    } _data;

    static_assert(sizeof(_data) == 8, "Overflow in size of TargetSymbolValue");
};


struct VIS_HIDDEN Image
{
    enum class LinkKind { regular=0, weak=1, upward=2, reExport=3 };
    enum class FixupKind { rebase32, rebase64, bind32, bind64, rebaseText32, bindText32, bindTextRel32, bindImportJmp32 };

                                        Image(const BinaryImageData* binaryData) : _binaryData(binaryData) { }

    bool                                valid() const { return (_binaryData != nullptr); }
    const BinaryImageData*              binaryData() const { return _binaryData; }
    const ImageGroup                    group() const;
    uint32_t                            maxLoadCount() const;
    const char*                         path() const;
    const char*                         leafName() const;
    uint32_t                            pathHash() const;
    const uuid_t*                       uuid() const;
    bool                                isInvalid() const;
    bool                                hasObjC() const;
    bool                                isBundle() const;
    bool                                hasWeakDefs() const;
    bool                                mayHavePlusLoads() const;
    bool                                hasTextRelocs() const;
    bool                                neverUnload() const;
    bool                                cwdMustBeThisDir() const;
    bool                                isPlatformBinary() const;
    bool                                overridableDylib() const;
    bool                                validateUsingModTimeAndInode() const;
    bool                                validateUsingCdHash() const;
    uint64_t                            fileModTime() const;
    uint64_t                            fileINode() const;
    const uint8_t*                      cdHash16() const;
    void                                forEachDependentImage(const ImageGroupList& groups, void (^handler)(uint32_t depIndex, Image depImage, LinkKind kind, bool& stop)) const;
#if !DYLD_IN_PROCESS
    bool                                recurseAllDependentImages(const ImageGroupList& groups, std::unordered_set<const BinaryImageData*>& allDependents) const;
#endif
    bool                                recurseAllDependentImages(const ImageGroupList& groups, SlowLoadSet& allDependents,
                                                                  void (^handler)(const dyld3::launch_cache::binary_format::Image* aBinImage, bool& stop)) const;
    bool                                containsAddress(const void* addr, const void* imageLoadAddress, uint8_t* permissions) const;
    bool                                segmentHasFixups(uint32_t segIndex) const;
    void                                forEachInitializer(const void* imageLoadAddress, void (^handler)(const void* initializer)) const;
    void                                forEachInitBefore(const ImageGroupList& groups, void (^handler)(Image imageToInit)) const;
    void                                forEachInitBefore(void (^handler)(binary_format::ImageRef imageToInit)) const;
    void                                forEachDOF(const void* imageLoadAddress, void (^handler)(const void* initializer)) const;

    bool                                isDiskImage() const;

    // the following are only valid if isDiskImage() returns false
    const binary_format::CachedImage*   asCachedImage() const;
    uint32_t                            cacheOffset() const;
    uint32_t                            patchStartIndex() const;
    uint32_t                            patchCount() const;


    // the following are only valid if isDiskImage() returns true
    const binary_format::DiskImage*     asDiskImage() const;
    uint64_t                            sliceOffsetInFile() const;
    bool                                hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const;
    bool                                isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const;
    uint64_t                            vmSizeToMap() const;
    void                                forEachDiskSegment(void (^handler)(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const;
    void                                forEachCacheSegment(void (^handler)(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const;
    void                                forEachFixup(uint32_t segIndex, MemoryRange segContent,
                                                     void (^handler)(uint64_t segOffset, FixupKind kind, TargetSymbolValue value, bool& stop)) const;

#if !DYLD_IN_PROCESS
    void                                 printAsJSON(const ImageGroupList& groupList, bool printFixups=false, bool printDependentsDetails=false, FILE* out=stdout) const;
#endif

// todo: fairPlayTextPages

private:
    friend struct ImageGroup;
    friend struct Closure;

    bool                                        recurseAllDependentImages(const ImageGroupList& groups, SlowLoadSet& allDependents, bool& stopped,
                                                                          void (^handler)(const dyld3::launch_cache::binary_format::Image* aBinImage, bool& stop)) const;
    uint32_t                                    pageSize() const;
    const binary_format::SegmentFixupsByPage*   segmentFixups(uint32_t segIndex) const;
    static void                                 forEachFixup(const uint8_t* pageFixups, const void* segContent, uint32_t& offset, uint32_t& ordinal,
                                                             void (^handler)(uint32_t pageOffset, FixupKind kind, uint32_t targetOrdinal, bool& stop));
    static Image                                resolveImageRef(const ImageGroupList& groups, binary_format::ImageRef ref, bool applyOverrides=true);


    const BinaryImageData*              _binaryData;
};


struct VIS_HIDDEN ImageGroup
{
                                    ImageGroup(const BinaryImageGroupData* binaryData) : _binaryData(binaryData) { }

    size_t                          size() const;
    uint32_t                        imageCount() const;
    uint32_t                        groupNum() const;
    bool                            dylibsExpectedOnDisk() const;
    const Image                     image(uint32_t index) const;
    uint32_t                        indexInGroup(const BinaryImageData* image) const;
    const BinaryImageData*          findImageByPath(const char* path, uint32_t& foundIndex) const;
    const BinaryImageData*          findImageByCacheOffset(size_t cacheVmOffset, uint32_t& mhCacheOffset, uint8_t& foundPermissions) const;
    const BinaryImageData*          imageBinary(uint32_t index) const;
    binary_format::ImageRef         dependentPool(uint32_t index) const;
    const BinaryImageGroupData*     binaryData() const { return _binaryData; }
    const char*                     stringFromPool(uint32_t offset) const;
    uint32_t                        indirectGroupNum(uint32_t index) const;
    void                            forEachImageRefOverride(void (^handler)(binary_format::ImageRef standardDylibRef, binary_format::ImageRef overrideDyilbRef, bool& stop)) const;
    void                            forEachImageRefOverride(const ImageGroupList& groupList, void (^handler)(Image standardDylib, Image overrideDyilb, bool& stop)) const;
    void                            forEachAliasOf(uint32_t imageIndex, void (^handler)(const char* aliasPath, uint32_t aliasPathHash, bool& stop)) const;
#if DYLD_IN_PROCESS
    void                            forEachDyldCacheSymbolOverride(void (^handler)(uint32_t patchTableIndex, const BinaryImageData* image, uint32_t imageOffset, bool& stop)) const;
    void                            forEachDyldCachePatchLocation(const void* dyldCacheLoadAddress, uint32_t patchTargetIndex,
                                                                  void (^handler)(uintptr_t* locationToPatch, uintptr_t addend, bool& stop)) const;
#else
    void                            forEachDyldCacheSymbolOverride(void (^handler)(uint32_t patchTableIndex, uint32_t imageIndexInClosure, uint32_t imageOffset, bool& stop)) const;
    void                            forEachDyldCachePatchLocation(const DyldCacheParser& cacheParser, void (^handler)(uint32_t targetCacheOffset, const std::vector<uint32_t>& usesPointersCacheOffsets, bool& stop)) const;
    bool                            hasPatchTableIndex(uint32_t targetCacheOffset, uint32_t& index) const;
#endif

    static uint32_t                 hashFunction(const char* s);
#if !DYLD_IN_PROCESS
    void                            printAsJSON(const ImageGroupList& groupList, bool printFixups=false, bool printDependentsDetails=false, FILE* out=stdout) const;
    void                            printStatistics(FILE* out=stderr) const;
#endif

private:
    friend struct Image;

    const char*                                 stringPool() const;
    uint32_t                                    stringPoolSize() const;
    const uint64_t*                             segmentPool(uint32_t index) const;
    const binary_format::AllFixupsBySegment*    fixUps(uint32_t offset) const;
    const TargetSymbolValue*                    targetValuesArray() const;
    uint32_t                                    targetValuesCount() const;
    uint32_t                                    initializersPoolCount() const;
    const uint32_t*                             initializerOffsetsPool() const;
    const uint32_t                              initializerOffsetsCount() const;
    const binary_format::ImageRef*              intializerListPool() const;
    const uint32_t                              intializerListPoolCount() const;
    const uint32_t*                             dofOffsetsPool() const;
    const uint32_t                              dofOffsetsCount() const;
    const uint32_t*                             indirectGroupNumsPool() const;
    const uint32_t                              indirectGroupNumsCount() const;
    void                                        forEachDyldCachePatch(uint32_t patchTargetIndex, uint32_t cacheDataVmOffset,
                                                                      void (^handler)(uint32_t targetCacheOffset, uint32_t usePointersCacheOffset, bool hasAddend, bool& stop)) const;

    const BinaryImageGroupData*        _binaryData;
};



struct VIS_HIDDEN Closure
{
                                        Closure(const BinaryClosureData* binaryData);

    size_t                              size() const;
    const uuid_t*                       dyldCacheUUID() const;
    const uint8_t*                      cdHash() const;
    uint32_t                            initialImageCount() const;
    uint32_t                            mainExecutableImageIndex() const;
    uint32_t                            mainExecutableEntryOffset() const;
    bool                                mainExecutableUsesCRT() const;
    bool                                isRestricted() const;
    bool                                usesLibraryValidation() const;
    const BinaryImageData*              libSystem(const ImageGroupList& groups);
    const BinaryImageData*              libDyld(const ImageGroupList& groups);
    uint32_t                            libdyldVectorOffset() const;
    const ImageGroup                    group() const;
    const BinaryClosureData*            binaryData() const { return _binaryData; }
    void                                forEachMustBeMissingFile(void (^handler)(const char* path, bool& stop)) const;
    void                                forEachEnvVar(void (^handler)(const char* keyEqualValue, bool& stop)) const;

#if !DYLD_IN_PROCESS
    void                                printAsJSON(const ImageGroupList& groupList, bool printFixups=true, bool printDependentsDetails=false, FILE* out=stdout) const;
    void                                printStatistics(FILE* out=stderr) const;
#endif

private:
    const BinaryClosureData*            _binaryData;
};






} //  namespace launch_cache
} //  namespace dyld3


#endif // LaunchCache_h


