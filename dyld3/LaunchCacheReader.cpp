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


#include <stdint.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <limits.h>

#include "LaunchCacheFormat.h"
#include "LaunchCache.h"
#include "MachOParser.h"
#include "DyldCacheParser.h"

namespace dyld {
    extern void log(const char* format, ...)  __attribute__((format(printf, 1, 2)));
}

namespace dyld3 {
namespace launch_cache {

static uintptr_t read_uleb128(const uint8_t*& p, const uint8_t* end)
{
    uint64_t result = 0;
    int         bit = 0;
    do {
        if (p == end) {
            assert("malformed uleb128");
            break;
        }
        uint64_t slice = *p & 0x7f;

        if (bit > 63) {
            assert("uleb128 too big for uint64");
            break;
        }
        else {
            result |= (slice << bit);
            bit += 7;
        }
    } while (*p++ & 0x80);
    return (uintptr_t)result;
}


bool MemoryRange::contains(const MemoryRange& other) const
{
    if ( this->address > other.address )
        return false;
    const uint8_t* thisEnd = (uint8_t*)address + size;
    const uint8_t* otherEnd = (uint8_t*)other.address + other.size;
    return (thisEnd >= otherEnd);
}

bool MemoryRange::intersects(const MemoryRange& other) const
{
    const uint8_t* thisEnd = (uint8_t*)address + size;
    const uint8_t* otherEnd = (uint8_t*)other.address + other.size;
    if ( otherEnd < this->address )
        return false;
    return ( other.address < thisEnd );
}


////////////////////////////  SlowLoadSet ////////////////////////////////////////

bool SlowLoadSet::contains(const BinaryImageData* image)
{
    for (const BinaryImageData** p=_start; p < _current; ++p) {
        if ( *p == image )
            return true;
    }
    return false;
}

bool SlowLoadSet::add(const BinaryImageData* image)
{
    if ( _current < _end ) {
        *_current++ = image;
        return true;
    }
    return false;
}

void SlowLoadSet::forEach(void (^handler)(const BinaryImageData*))
{
    for (const BinaryImageData** p=_start; p < _current; ++p) {
        handler(*p);
    }
}

void SlowLoadSet::forEach(void (^handler)(const BinaryImageData*, bool& stop))
{
    bool stop = false;
    for (const BinaryImageData** p=_start; p < _current; ++p) {
        handler(*p, stop);
        if ( stop )
            break;
    }
}


long SlowLoadSet::count() const
{
    return (_current - _start);
}


////////////////////////////  TargetSymbolValue ////////////////////////////////////////
 

#if DYLD_IN_PROCESS

uintptr_t TargetSymbolValue::resolveTarget(Diagnostics& diag, const ImageGroup& inGroup, LoadedImages& images) const
{
    // this block is only used if findExportedSymbol() needs to trace re-exported dylibs to find a symbol
    MachOParser::DependentFinder reExportFollower = ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        *foundMH = nullptr;
        images.forEachImage(^(uint32_t idx, const BinaryImageData* binImage, const mach_header* mh, bool& stop) {
            Image anImage(binImage);
            if ( strcmp(depLoadPath, anImage.path()) == 0 ) {
                *foundMH = mh;
                stop = true;
            }
        });
        return (*foundMH != nullptr);
    };

    uintptr_t offset;
    switch ( _data.sharedCache.kind ) {
    
        case TargetSymbolValue::kindSharedCache:
            assert(_data.sharedCache.offsetIntoCache != 0);
            return (uintptr_t)(images.dyldCacheLoadAddressForImage() + _data.sharedCache.offsetIntoCache);
            
        case TargetSymbolValue::kindAbsolute:
            offset = (uintptr_t)_data.absolute.value;
            // sign extend 42 bit value
            if ( offset & 0x2000000000000000ULL )
                offset |= 0xC000000000000000ULL;
            return offset;
            
        case TargetSymbolValue::kindGroup: {
            uint32_t groupNum = _data.group.isIndirectGroup ? inGroup.indirectGroupNum(_data.group.groupNum) : _data.group.groupNum;
            uintptr_t targetImageLoadAddress = (uintptr_t)(images.loadAddressFromGroupAndIndex(groupNum, _data.group.indexInGroup));
            if ( targetImageLoadAddress == 0 )
                diag.error("image for groupNum=%d, indexInGroup=%d not found", groupNum, _data.group.indexInGroup);
            offset = (uintptr_t)_data.group.offsetInImage;
            // sign extend 42 bit offset
            if ( offset & 0x0000020000000000ULL )
                offset |= 0xFFFFFC0000000000ULL;
            return targetImageLoadAddress + offset;
         }

        case TargetSymbolValue::kindDynamicGroup: {
            const char* imagePath =  inGroup.stringFromPool(_data.dynamicGroup.imagePathOffset);
            const char* symbolName = inGroup.stringFromPool(_data.dynamicGroup.symbolNameOffset);
            __block uintptr_t result = 0;
            __block bool      found  = false;
            if ( strcmp(imagePath, "@flat") == 0 ) {
                // search all images in load order
                images.forEachImage(^(uint32_t idx, const BinaryImageData* binImage, const mach_header* mh, bool& stop) {
                    Diagnostics findSymbolDiag;
                    dyld3::MachOParser parser(mh);
                    dyld3::MachOParser::FoundSymbol foundInfo;
                    if ( parser.findExportedSymbol(findSymbolDiag, symbolName, nullptr, foundInfo, ^(uint32_t, const char* depLoadPath, void*, const mach_header** foundMH, void**) {
                            // <rdar://problem/31921090> need to follow re-exported symbols to support libc renamed and reexported symbols
                           *foundMH = nullptr;
                            images.forEachImage(^(uint32_t innerIndex, const BinaryImageData* innerBinImage, const mach_header* innerMH, bool& innerStop) {
                                Image innerImage(innerBinImage);
                                if ( strcmp(depLoadPath, innerImage.path()) == 0 ) {
                                    *foundMH = innerMH;
                                    innerStop = true;
                                }
                            });
                            return (*foundMH != nullptr);
                        }) ) {
                        switch (foundInfo.kind) {
                            case MachOParser::FoundSymbol::Kind::headerOffset:
                            case MachOParser::FoundSymbol::Kind::resolverOffset:
                                result = ((uintptr_t)(foundInfo.foundInDylib) + (uintptr_t)foundInfo.value);
                                break;
                            case MachOParser::FoundSymbol::Kind::absolute:
                                result = (uintptr_t)foundInfo.value;
                                break;
                        }
                        images.setAsNeverUnload(idx);
                        found  = true;
                        stop   = true;
                    }
                });
                // <rdar://problem/31944092> bind unfound flat symbols to NULL to support lazy binding semantics
                if ( !found ) {
                    result = 0;
                    found = true;
                }
            }
            else if ( strcmp(imagePath, "@main") == 0 ) {
                // search only main executable
                images.forEachImage(^(uint32_t idx, const BinaryImageData* binImage, const mach_header* mh, bool& stop) {
                    if ( mh->filetype == MH_EXECUTE ) {
                        Diagnostics findSymbolDiag;
                        dyld3::MachOParser parser(mh);
                        dyld3::MachOParser::FoundSymbol foundInfo;
                        if ( parser.findExportedSymbol(findSymbolDiag, symbolName, nullptr, foundInfo, nullptr) ) {
                            switch (foundInfo.kind) {
                                case MachOParser::FoundSymbol::Kind::headerOffset:
                                case MachOParser::FoundSymbol::Kind::resolverOffset:
                                    result = ((uintptr_t)(foundInfo.foundInDylib) + (uintptr_t)foundInfo.value);
                                    break;
                                case MachOParser::FoundSymbol::Kind::absolute:
                                    result = (uintptr_t)foundInfo.value;
                                    break;
                            }
                            found  = true;
                            stop   = true;
                        }
                    }
                });
            }
            else if ( strcmp(imagePath, "@weak_def") == 0 ) {
                // search images with weak definitions in load order
                images.forEachImage(^(uint32_t idx, const BinaryImageData* binImage, const mach_header* mh, bool& stop) {
                    Image anImage(binImage);
                    if ( anImage.hasWeakDefs() ) {
                        Diagnostics findSymbolDiag;
                        dyld3::MachOParser parser(mh);
                        dyld3::MachOParser::FoundSymbol foundInfo;
                        if ( parser.findExportedSymbol(findSymbolDiag, symbolName, nullptr, foundInfo, nullptr) ) {
                            switch (foundInfo.kind) {
                                case MachOParser::FoundSymbol::Kind::headerOffset:
                                case MachOParser::FoundSymbol::Kind::resolverOffset:
                                    result = ((uintptr_t)(foundInfo.foundInDylib) + (uintptr_t)foundInfo.value);
                                    break;
                                case MachOParser::FoundSymbol::Kind::absolute:
                                    result = (uintptr_t)foundInfo.value;
                                    break;
                            }
                            found  = true;
                            images.setAsNeverUnload(idx);
                            stop   = true;
                        }
                    }
                });
            }
            else {
                // search only image the matches supplied path
                images.forEachImage(^(uint32_t idx, const BinaryImageData* binImage, const mach_header* mh, bool& stop) {
                    Image anImage(binImage);
                    if ( strcmp(anImage.path(), imagePath) == 0 ) {
                        Diagnostics findSymbolDiag;
                        dyld3::MachOParser parser(mh);
                        dyld3::MachOParser::FoundSymbol foundInfo;
                        if ( parser.findExportedSymbol(findSymbolDiag, symbolName, nullptr, foundInfo, reExportFollower) ) {
                            switch (foundInfo.kind) {
                                case MachOParser::FoundSymbol::Kind::headerOffset:
                                case MachOParser::FoundSymbol::Kind::resolverOffset:
                                    result = ((uintptr_t)(foundInfo.foundInDylib) + (uintptr_t)foundInfo.value);
                                    break;
                                case MachOParser::FoundSymbol::Kind::absolute:
                                    result = (uintptr_t)foundInfo.value;
                                    break;
                            }
                            found  = true;
                            stop = true;
                        }
                    }
                });
            }
            if ( found )
                return result;
            if ( _data.dynamicGroup.weakImport )
                return 0;
            diag.error("dynamic symbol '%s' not found for %s", symbolName, imagePath);
            return 0;
        }
    }
    assert(0 && "resolveTarget() not reachable");
}

#else

TargetSymbolValue::TargetSymbolValue()
{
    _data.raw = 0;
}

TargetSymbolValue TargetSymbolValue::makeInvalid()
{
    return TargetSymbolValue();
}

TargetSymbolValue TargetSymbolValue::makeSharedCacheOffset(uint32_t offset)
{
    TargetSymbolValue t;
    t._data.sharedCache.kind                = kindSharedCache;
    t._data.sharedCache.offsetIntoCache     = offset;
    return t;
}

TargetSymbolValue TargetSymbolValue::makeAbsolute(uint64_t value)
{
    TargetSymbolValue t;
    t._data.absolute.kind                   = kindAbsolute;
    t._data.absolute.value                  = value;
    return t;
}

TargetSymbolValue TargetSymbolValue::makeGroupValue(uint32_t groupIndex, uint32_t imageIndexInGroup, uint64_t offsetInImage, bool isIndirectGroupNum)
{
    assert(groupIndex != 0 || isIndirectGroupNum);
    assert(groupIndex < 128);
    assert(imageIndexInGroup < 4096);
    TargetSymbolValue t;
    t._data.group.kind                      = kindGroup;
    t._data.group.isIndirectGroup           = isIndirectGroupNum;
    t._data.group.groupNum                  = groupIndex;
    t._data.group.indexInGroup              = imageIndexInGroup;
    t._data.group.offsetInImage             = offsetInImage;
    return t;
}

TargetSymbolValue TargetSymbolValue::makeDynamicGroupValue(uint32_t imagePathPoolOffset, uint32_t imageSymbolPoolOffset, bool weakImport)
{
    TargetSymbolValue t;
    t._data.dynamicGroup.kind               = kindDynamicGroup;
    t._data.dynamicGroup.weakImport         = weakImport;
    t._data.dynamicGroup.imagePathOffset    = imagePathPoolOffset;
    t._data.dynamicGroup.symbolNameOffset   = imageSymbolPoolOffset;
    return t;
}

bool TargetSymbolValue::isSharedCacheTarget(uint64_t& offsetInCache) const
{
    if ( _data.sharedCache.kind != kindSharedCache )
        return false;
    offsetInCache = _data.sharedCache.offsetIntoCache;
    return true;
}

bool TargetSymbolValue::isGroupImageTarget(uint32_t& groupNum, uint32_t& indexInGroup, uint64_t& offsetInImage) const
{
    if ( _data.sharedCache.kind != kindGroup )
        return false;
    // This is only used for interposing, so refuse to allow indirect for group 2
    assert(!_data.group.isIndirectGroup);
    groupNum      = _data.group.groupNum;
    indexInGroup  = _data.group.indexInGroup;
    offsetInImage = _data.group.offsetInImage;
    return true;
}

bool TargetSymbolValue::isInvalid() const
{
    return (_data.raw == 0);
}

static std::string hex8(uint64_t value) {
    char buff[64];
    sprintf(buff, "0x%08llX", value);
    return buff;
}

static std::string decimal(uint64_t value) {
    char buff[64];
    sprintf(buff, "%llu", value);
    return buff;
}

std::string TargetSymbolValue::asString(ImageGroup group) const
{
    int64_t offset;
    switch ( _data.sharedCache.kind ) {
        case kindSharedCache:
            if ( _data.sharedCache.offsetIntoCache == 0 )
                return "{invalid target}";
            else
                return "{cache+" + hex8(_data.sharedCache.offsetIntoCache) + "}";
        case kindAbsolute:
            offset = (uintptr_t)_data.absolute.value;
            // sign extend 42 bit value
            if ( offset & 0x2000000000000000ULL )
                offset |= 0xC000000000000000ULL;
            return "{absolute:" + hex8(offset) + "}";
        case kindGroup:
            offset = _data.group.offsetInImage;
            // sign extend 42 bit offset
            if ( offset & 0x0000020000000000ULL )
                offset |= 0xFFFFFC0000000000ULL;
            if ( _data.group.groupNum == 1 )
                return "{otherDylib[" + decimal(_data.group.indexInGroup) +"]+" + hex8(offset) + "}";
            if ( _data.group.groupNum == 2 )
                return "{closure[" + decimal(_data.group.indexInGroup) +"]+" + hex8(offset) + "}";
            else {
                uint32_t groupNum = _data.group.isIndirectGroup ? group.indirectGroupNum(_data.group.groupNum) : _data.group.groupNum;
                return "{dlopen-group-" + decimal(groupNum-2) + "[" + decimal(_data.group.indexInGroup) +"]+" + hex8(offset) + "}";
            }
        case kindDynamicGroup:
            return "{dynamic image='" + std::string(group.stringFromPool(_data.dynamicGroup.imagePathOffset))
                 + "' symbol='" + std::string(group.stringFromPool(_data.dynamicGroup.symbolNameOffset)) + "'}";
    }
    assert(0 && "unreachable");
    return "xx";
}

#endif

////////////////////////////  ImageRef ////////////////////////////////////////

binary_format::ImageRef binary_format::ImageRef::weakImportMissing()
{
    ImageRef missing(0xFFFFFFFF);
    return missing;
}
 


////////////////////////////  Closure ////////////////////////////////////////

Closure::Closure(const binary_format::Closure* closure)
 : _binaryData(closure)
{
    assert(closure->magic == binary_format::Closure::magicV1);
}

size_t Closure::size() const
{
    return _binaryData->stringPoolOffset + _binaryData->stringPoolSize;
}

const ImageGroup Closure::group() const
{
    return ImageGroup(&_binaryData->group);
}

void Closure::forEachEnvVar(void (^handler)(const char* keyEqualValue, bool& stop)) const
{
    const uint32_t* envVarStringOffsets = (uint32_t*)((uint8_t*)_binaryData + _binaryData->dyldEnvVarsOffset);
    const char*     stringPool          = (char*)_binaryData + _binaryData->stringPoolOffset;
    bool            stop                = false;
    for (uint32_t i=0; i < _binaryData->dyldEnvVarsCount; ++i) {
        handler(&stringPool[envVarStringOffsets[i]], stop);
        if ( stop )
            break;
    }
}
 
void Closure::forEachMustBeMissingFile(void (^handler)(const char* path, bool& stop)) const
{
    const uint16_t*     offsets     = (uint16_t*)((uint8_t*)_binaryData + _binaryData->missingFileComponentsOffset);
    if ( *offsets == 0 )
        return;
    const char*         stringPool  = (char*)_binaryData + _binaryData->stringPoolOffset;
    bool                stop        = false;
    while ( !stop ) {
        char path[PATH_MAX];
        path[0] = '\0';
        while ( *offsets != 0 ) {
            const char* component = &stringPool[*offsets++];
            strlcat(path, "/", PATH_MAX);
            strlcat(path, component, PATH_MAX);
        }
        handler(path, stop);
        ++offsets;  // move to next path
        if ( *offsets == 0 )  // if no next path, then end of list of strings
            stop = true;
    }
}

const uuid_t* Closure::dyldCacheUUID() const
{
    return &(_binaryData->dyldCacheUUID);
}


const uint8_t* Closure::cdHash() const
{
    return _binaryData->mainExecutableCdHash;
}


uint32_t Closure::initialImageCount() const
{
    return _binaryData->initialImageCount;
}


uint32_t Closure::mainExecutableImageIndex() const
{
    return _binaryData->mainExecutableIndexInGroup;
}


uint32_t Closure::mainExecutableEntryOffset() const
{
    return _binaryData->mainExecutableEntryOffset;
}

bool Closure::mainExecutableUsesCRT() const
{
    return _binaryData->usesCRT;
}

bool Closure::isRestricted() const
{
    return _binaryData->isRestricted;
}

bool Closure::usesLibraryValidation() const
{
    return _binaryData->usesLibraryValidation;
}

uint32_t Closure::libdyldVectorOffset() const
{
    return _binaryData->libdyldVectorOffset;
}

const BinaryImageData* Closure::libSystem(const ImageGroupList& groups)
{
    return Image::resolveImageRef(groups, _binaryData->libSystemRef).binaryData();
}

const BinaryImageData* Closure::libDyld(const ImageGroupList& groups)
{
    return Image::resolveImageRef(groups, _binaryData->libDyldRef).binaryData();
}


////////////////////////////  ImageGroup ////////////////////////////////////////

size_t ImageGroup::size() const
{
    return (_binaryData->stringsPoolOffset + _binaryData->stringsPoolSize + 3) & (-4);
}

uint32_t ImageGroup::groupNum() const
{
    return _binaryData->groupNum;
}

bool ImageGroup::dylibsExpectedOnDisk() const
{
    return _binaryData->dylibsExpectedOnDisk;
}

uint32_t ImageGroup::imageCount() const
{
    return _binaryData->imagesPoolCount;
}

const binary_format::Image* ImageGroup::imageBinary(uint32_t index) const
{
    assert(index <_binaryData->imagesPoolCount);
    return (binary_format::Image*)((char*)_binaryData + _binaryData->imagesPoolOffset + (index * _binaryData->imagesEntrySize));
}


const Image ImageGroup::image(uint32_t index) const
{
    return Image(imageBinary(index));
}

uint32_t ImageGroup::indexInGroup(const binary_format::Image* img) const
{
    long delta = (char*)img - ((char*)_binaryData + _binaryData->imagesPoolOffset);
    uint32_t index = (uint32_t)(delta  /_binaryData->imagesEntrySize);
    assert(image(index)._binaryData == img);
    return index;
}

const binary_format::Image* ImageGroup::findImageByPath(const char* path, uint32_t& foundIndex) const
{
    // check path of each image in group
    uint32_t targetHash = hashFunction(path);
    const uint8_t* p = (uint8_t*)_binaryData + _binaryData->imagesPoolOffset;
    for (uint32_t i=0; i < _binaryData->imagesPoolCount; ++i) {
        const binary_format::Image* binImage = (binary_format::Image*)p;
        if ( binImage->pathHash == targetHash ) {
            Image img(binImage);
            if ( !img.isInvalid() && (strcmp(img.path(), path) == 0) ) {
                foundIndex = i;
                return binImage;
            }
        }
        p += _binaryData->imagesEntrySize;
    }
    // check each alias
    const binary_format::AliasEntry* aliasEntries =  (binary_format::AliasEntry*)((uint8_t*)_binaryData + _binaryData->imageAliasOffset);
    for (uint32_t i=0; i < _binaryData->imageAliasCount; ++i) {
        const char* aliasPath = stringFromPool(aliasEntries[i].aliasOffsetInStringPool);
        if ( aliasEntries[i].aliasHash == targetHash ) {
            if ( strcmp(aliasPath, path) == 0 ) {
                Image img = image(aliasEntries[i].imageIndexInGroup);
                if ( !img.isInvalid() ) {
                    foundIndex = aliasEntries[i].imageIndexInGroup;
                    return img.binaryData();
                }
            }
        }
    }
    return nullptr;
}

const binary_format::Image* ImageGroup::findImageByCacheOffset(size_t cacheVmOffset, uint32_t& mhCacheOffset, uint8_t& foundPermissions) const
{
    assert(groupNum() == 0);

    const binary_format::DyldCacheSegment* cacheSegs = (binary_format::DyldCacheSegment*)segmentPool(0);
    const binary_format::Image* image = (binary_format::Image*)((char*)_binaryData + _binaryData->imagesPoolOffset);
    // most address lookups are in TEXT, so just search first segment in first pass
    for (uint32_t imageIndex=0; imageIndex < _binaryData->imagesPoolCount; ++imageIndex) {
        const binary_format::DyldCacheSegment* segInfo = &cacheSegs[image->segmentsArrayStartIndex];
        if ( (cacheVmOffset >= segInfo->cacheOffset) && (cacheVmOffset < (segInfo->cacheOffset + segInfo->size)) ) {
            mhCacheOffset    = segInfo->cacheOffset;
            foundPermissions = segInfo->permissions;
            return image;
        }
        image = (binary_format::Image*)((char*)image + _binaryData->imagesEntrySize);
    }
    // second pass, skip TEXT segment
    image = (binary_format::Image*)((char*)_binaryData + _binaryData->imagesPoolOffset);
    for (uint32_t imageIndex=0; imageIndex < _binaryData->imagesPoolCount; ++imageIndex) {
        for (uint32_t segIndex=1; segIndex < image->segmentsArrayCount; ++segIndex) {
            const binary_format::DyldCacheSegment* segInfo = &cacheSegs[image->segmentsArrayStartIndex+segIndex];
            if ( (cacheVmOffset >= segInfo->cacheOffset) && (cacheVmOffset < (segInfo->cacheOffset + segInfo->size)) ) {
                mhCacheOffset    = cacheSegs[image->segmentsArrayStartIndex].cacheOffset;
                foundPermissions = segInfo->permissions;
                return image;
            }
        }
        image = (binary_format::Image*)((char*)image + _binaryData->imagesEntrySize);
    }
    return nullptr;
}

void ImageGroup::forEachAliasOf(uint32_t imageIndex, void (^handler)(const char* aliasPath, uint32_t aliasPathHash, bool& stop)) const
{
    bool stop = false;
    const binary_format::AliasEntry* aliasEntries =  (binary_format::AliasEntry*)((uint8_t*)_binaryData + _binaryData->imageAliasOffset);
    for (uint32_t i=0; i < _binaryData->imageAliasCount; ++i) {
        if ( aliasEntries[i].imageIndexInGroup ==  imageIndex ) {
            const char* aliasPath = stringFromPool(aliasEntries[i].aliasOffsetInStringPool);
            handler(aliasPath, aliasEntries[i].aliasHash, stop);
            if ( stop )
                break;
        }
    }
}

const char* ImageGroup::stringPool() const
{
    return (char*)_binaryData + _binaryData->stringsPoolOffset;
}

const char* ImageGroup::stringFromPool(uint32_t offset) const
{
    assert(offset < _binaryData->stringsPoolSize);
    return (char*)_binaryData + _binaryData->stringsPoolOffset + offset;
}

uint32_t ImageGroup::stringPoolSize() const
{
    return _binaryData->stringsPoolSize;;
}

binary_format::ImageRef ImageGroup::dependentPool(uint32_t index) const
{
    assert(index < _binaryData->dependentsPoolCount);
    const binary_format::ImageRef* depArray = (binary_format::ImageRef*)((char*)_binaryData + _binaryData->dependentsPoolOffset);
    return depArray[index];
}

const uint64_t* ImageGroup::segmentPool(uint32_t index) const
{
    assert(index < _binaryData->segmentsPoolCount);
    const uint64_t* segArray = (uint64_t*)((char*)_binaryData + _binaryData->segmentsPoolOffset);
    return &segArray[index];
}


const uint32_t* ImageGroup::initializerOffsetsPool() const
{
    return (uint32_t*)((char*)_binaryData + _binaryData->intializerOffsetPoolOffset);
}

const uint32_t ImageGroup::initializerOffsetsCount() const
{
    return _binaryData->intializerOffsetPoolCount;
}

const binary_format::ImageRef* ImageGroup::intializerListPool() const
{
    return (binary_format::ImageRef*)((char*)_binaryData + _binaryData->intializerListPoolOffset);
}

const uint32_t ImageGroup::intializerListPoolCount() const
{
    return _binaryData->intializerListPoolCount;
}

const binary_format::AllFixupsBySegment* ImageGroup::fixUps(uint32_t offset) const
{
    return (binary_format::AllFixupsBySegment*)((char*)_binaryData + _binaryData->fixupsOffset + offset);
}

const TargetSymbolValue* ImageGroup::targetValuesArray() const
{
    return (TargetSymbolValue*)((char*)_binaryData + _binaryData->targetsOffset);
}

uint32_t ImageGroup::targetValuesCount() const
{
    return _binaryData->targetsPoolCount;
}


const uint32_t* ImageGroup::dofOffsetsPool() const
{
    return (uint32_t*)((char*)_binaryData + _binaryData->dofOffsetPoolOffset);
}

const uint32_t ImageGroup::dofOffsetsCount() const
{
    return _binaryData->dofOffsetPoolCount;
}


const uint32_t* ImageGroup::indirectGroupNumsPool() const
{
    return (uint32_t*)((char*)_binaryData + _binaryData->indirectGroupNumPoolOffset);
}

const uint32_t ImageGroup::indirectGroupNumsCount() const
{
    return _binaryData->indirectGroupNumPoolCount;
}

uint32_t ImageGroup::indirectGroupNum(uint32_t offset) const
{
    assert(offset < _binaryData->indirectGroupNumPoolCount);
    return indirectGroupNumsPool()[offset];
}

uint32_t ImageGroup::hashFunction(const char* str)
{
    uint32_t h = 0;
    for (const char* s=str; *s != '\0'; ++s)
        h = h*5 + *s;
    return h;
}


void ImageGroup::forEachDyldCachePatch(uint32_t patchTargetIndex, uint32_t cacheDataVmOffset, void (^handler)(uint32_t targetCacheOffset, uint32_t usePointersCacheOffset, bool hasAddend, bool& stop)) const
{
    assert(_binaryData->imagesEntrySize == sizeof(binary_format::CachedImage) && "only callable on group-0 in shared cache");
    assert(patchTargetIndex < _binaryData->cachePatchTableCount);
    const binary_format::PatchTable* patches = (binary_format::PatchTable*)((char*)_binaryData + _binaryData->cachePatchTableOffset);
    uint32_t offsetsIndex      = patches[patchTargetIndex].offsetsStartIndex;
    uint32_t targetCacheOffset = patches[patchTargetIndex].targetCacheOffset;
    const binary_format::PatchOffset* patchLocationOffsets = (binary_format::PatchOffset*)((char*)_binaryData + _binaryData->cachePatchOffsetsOffset);
    bool stop = false;
    while ( !stop ) {
        assert(offsetsIndex < _binaryData->cachePatchOffsetsCount);
        binary_format::PatchOffset entry = patchLocationOffsets[offsetsIndex];
        ++offsetsIndex;
        handler(targetCacheOffset, cacheDataVmOffset+entry.dataRegionOffset, entry.hasAddend, stop);
        if ( entry.last )
            stop = true;
    }
}

void ImageGroup::forEachImageRefOverride(void (^handler)(binary_format::ImageRef standardDylibRef, binary_format::ImageRef overrideDylibRef, bool& stop)) const
{
    bool stop = false;
    const binary_format::ImageRefOverride* entries = (binary_format::ImageRefOverride*)((char*)_binaryData + _binaryData->imageOverrideTableOffset);
    for (uint32_t i=0; (i < _binaryData->imageOverrideTableCount) && !stop; ++i) {
        handler(entries[i].standardDylib, entries[i].overrideDylib, stop);
    }
}

void ImageGroup::forEachImageRefOverride(const ImageGroupList& groupList, void (^handler)(Image standardDylib, Image overrideDylib, bool& stop)) const
{
    forEachImageRefOverride(^(binary_format::ImageRef standardDylibRef, binary_format::ImageRef overrideDylibRef, bool& stop) {
        Image standardDylib = Image::resolveImageRef(groupList, standardDylibRef, false);
        Image overrideDylib = Image::resolveImageRef(groupList, overrideDylibRef, false);
        handler(standardDylib, overrideDylib, stop);
    });
}


#if DYLD_IN_PROCESS

void ImageGroup::forEachDyldCachePatchLocation(const void* dyldCacheLoadAddress, uint32_t patchTargetIndex, void (^handler)(uintptr_t* locationToPatch, uintptr_t addend, bool&)) const
{
    DyldCacheParser cacheParser((DyldSharedCache*)dyldCacheLoadAddress, false);
    uint32_t cacheDataVmOffset = (uint32_t)cacheParser.dataRegionRuntimeVmOffset();
    forEachDyldCachePatch(patchTargetIndex, cacheDataVmOffset, ^(uint32_t targetCacheOffset, uint32_t usePointersCacheOffset, bool hasAddend, bool& stop) {
        uintptr_t addend = 0;
        uintptr_t* fixupLoc = (uintptr_t*)((char*)dyldCacheLoadAddress + usePointersCacheOffset);
        if ( hasAddend ) {
            uintptr_t currentValue  = *fixupLoc;
            uintptr_t expectedValue = (uintptr_t)dyldCacheLoadAddress + targetCacheOffset;
            uintptr_t delta         = currentValue - expectedValue;
            assert(delta < 32);
            addend = delta;
        }
        handler(fixupLoc, addend, stop);
    });
}

void ImageGroup::forEachDyldCacheSymbolOverride(void (^handler)(uint32_t patchTableIndex, const BinaryImageData* image, uint32_t imageOffset, bool& stop)) const
{
    bool stop = false;
    const binary_format::DyldCacheOverride* entries = (binary_format::DyldCacheOverride*)((char*)_binaryData + _binaryData->symbolOverrideTableOffset);
    for (uint32_t i=0; (i < _binaryData->symbolOverrideTableCount) && !stop; ++i) {
        handler(entries[i].patchTableIndex, imageBinary(entries[i].imageIndex), entries[i].imageOffset, stop);
    }
}

#else

void ImageGroup::forEachDyldCacheSymbolOverride(void (^handler)(uint32_t patchTableIndex, uint32_t imageIndexInClosure, uint32_t imageOffset, bool& stop)) const
{
    bool stop = false;
    const binary_format::DyldCacheOverride* entries = (binary_format::DyldCacheOverride*)((char*)_binaryData + _binaryData->symbolOverrideTableOffset);
    for (uint32_t i=0; (i < _binaryData->symbolOverrideTableCount) && !stop; ++i) {
        handler(entries[i].patchTableIndex, entries[i].imageIndex, entries[i].imageOffset, stop);
    }
}

void ImageGroup::forEachDyldCachePatchLocation(const DyldCacheParser& cacheParser, void (^handler)(uint32_t targetCacheOffset, const std::vector<uint32_t>& usesPointersCacheOffsets, bool& stop)) const
{
    uint32_t cacheDataVmOffset = (uint32_t)cacheParser.dataRegionRuntimeVmOffset();
    __block std::vector<uint32_t> pointerCacheOffsets;
    bool stop = false;
    for (uint32_t patchIndex=0; patchIndex < _binaryData->cachePatchTableCount; ++patchIndex) {
        pointerCacheOffsets.clear();
        __block uint32_t targetCacheOffset = 0;
        forEachDyldCachePatch(patchIndex, cacheDataVmOffset, ^(uint32_t targetCacheOff, uint32_t usePointersCacheOffset, bool hasAddend, bool&) {
            targetCacheOffset = targetCacheOff;
            pointerCacheOffsets.push_back(usePointersCacheOffset);
        });
        std::sort(pointerCacheOffsets.begin(), pointerCacheOffsets.end(), [&](uint32_t a, uint32_t b) { return a < b; });
        handler(targetCacheOffset, pointerCacheOffsets, stop);
        if ( stop )
            break;
    }
}

bool ImageGroup::hasPatchTableIndex(uint32_t targetCacheOffset, uint32_t& foundIndex) const
{
    const binary_format::PatchTable* patches = (binary_format::PatchTable*)((char*)_binaryData + _binaryData->cachePatchTableOffset);
    for (uint32_t i=0; i < _binaryData->cachePatchTableCount; ++i) {
        if ( patches[i].targetCacheOffset == targetCacheOffset ) {
            foundIndex = i;
            return true;
        }
    }
    return false;
}

#endif


////////////////////////////  Image ////////////////////////////////////////



const ImageGroup Image::group() const
{
    return ImageGroup((binary_format::ImageGroup*)(((char*)_binaryData) + (_binaryData->groupOffset)));
}

uint32_t Image::maxLoadCount() const
{
    return _binaryData->maxLoadCount;
}

const char* Image::path() const
{
    return group().stringFromPool(_binaryData->pathPoolOffset);
}

uint32_t Image::pathHash() const
{
    return _binaryData->pathHash;
}

const char* Image::leafName() const
{
    const char* path = group().stringFromPool(_binaryData->pathPoolOffset);
    const char* lastSlash = strrchr(path, '/');
    if ( lastSlash != nullptr )
        return lastSlash+1;
    else
        return path;
}

const uuid_t* Image::uuid() const
{
    return &(_binaryData->uuid);
}

bool Image::isInvalid() const
{
    return (_binaryData == nullptr) || _binaryData->isInvalid;
}

bool Image::hasObjC() const
{
    return _binaryData->hasObjC;
}

bool Image::isBundle() const
{
    return _binaryData->isBundle;
}

bool Image::hasWeakDefs() const
{
    return _binaryData->hasWeakDefs;
}

bool Image::mayHavePlusLoads() const
{
    return _binaryData->mayHavePlusLoads;
}

bool Image::hasTextRelocs() const
{
    return _binaryData->hasTextRelocs;
}

bool Image::neverUnload() const
{
    return _binaryData->neverUnload;
}

bool Image::cwdMustBeThisDir() const
{
    return _binaryData->cwdSameAsThis;
}

bool Image::isPlatformBinary() const
{
    return _binaryData->isPlatformBinary;
}

bool Image::overridableDylib() const
{
    return _binaryData->overridableDylib;
}

void Image::forEachDependentImage(const ImageGroupList& groups, void (^handler)(uint32_t depIndex, Image depImage, LinkKind kind, bool& stop)) const
{
    assert(!_binaryData->isInvalid);
    binary_format::ImageRef missingRef = binary_format::ImageRef::weakImportMissing();
    __block bool stop = false;
    for (uint32_t depIndex=0; (depIndex < _binaryData->dependentsArrayCount) && !stop; ++depIndex) {
        binary_format::ImageRef ref = group().dependentPool(_binaryData->dependentsArrayStartIndex + depIndex);
        if ( ref != missingRef ) {
            Image depImage(resolveImageRef(groups, ref));
            handler(depIndex, depImage, (LinkKind)ref.kind(), stop);
        }
    }
}
 

#if !DYLD_IN_PROCESS
bool Image::recurseAllDependentImages(const ImageGroupList& groups, std::unordered_set<const BinaryImageData*>& allDependents) const
{
    if ( isInvalid() )
        return false;
    __block bool result = true;
    forEachDependentImage(groups, ^(uint32_t depIndex, Image depImage, LinkKind kind, bool& stop) {
        if ( allDependents.count(depImage.binaryData()) == 0 ) {
            allDependents.insert(depImage.binaryData());
            if ( !depImage.recurseAllDependentImages(groups, allDependents) ) {
                result = false;
                stop = true;
            }
        }
    });
    return result;
}
#endif

bool Image::recurseAllDependentImages(const ImageGroupList& groups, SlowLoadSet& allDependents, bool& stopped,
                                      void (^handler)(const dyld3::launch_cache::binary_format::Image* aBinImage, bool& stop)) const
{
    __block bool result = true;
    // breadth first, add all directly dependent images
    const dyld3::launch_cache::binary_format::Image* needToProcessArray[_binaryData->dependentsArrayCount];
    memset((void*)needToProcessArray, 0, _binaryData->dependentsArrayCount * sizeof(*needToProcessArray));
    const dyld3::launch_cache::binary_format::Image** const needToProcess = needToProcessArray;
    forEachDependentImage(groups, ^(uint32_t depIndex, Image depImage, LinkKind kind, bool& stop) {
        const dyld3::launch_cache::binary_format::Image* depImageData = depImage.binaryData();
        if ( allDependents.contains(depImageData) ) {
            needToProcess[depIndex] = nullptr;
        }
        else {
            needToProcess[depIndex] = depImageData;
            if ( !allDependents.add(depImageData) ) {
                result = false;
                stop = true;
                return;
            }
            if (handler) {
                handler(depImageData, stop);
                if ( stop )
                    stopped = true;
            }
        }
    });

    // recurse on each dependent image
    for (int i=0; !stopped && (i < _binaryData->dependentsArrayCount); ++i) {
        if ( const dyld3::launch_cache::binary_format::Image* depImageData = needToProcess[i] ) {
            Image depImage(depImageData);
            if ( !depImage.recurseAllDependentImages(groups, allDependents, stopped, handler) ) {
                return false;
            }
        }
    }

    return result;
}

bool Image::recurseAllDependentImages(const ImageGroupList& groups, SlowLoadSet& allDependents,
                                      void (^handler)(const dyld3::launch_cache::binary_format::Image* aBinImage, bool& stop)) const
{
    bool stopped = false;
    return recurseAllDependentImages(groups, allDependents, stopped, handler);
}

void Image::forEachDiskSegment(void (^handler)(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const
{
    assert(isDiskImage());
    const uint32_t                      pageSize     = (_binaryData->has16KBpages ?  0x4000 : 0x1000);
    const uint64_t*                     rawSegs      = group().segmentPool(_binaryData->segmentsArrayStartIndex);
    const binary_format::DiskSegment*   diskSegs     = (binary_format::DiskSegment*)rawSegs;
    uint32_t                            segIndex     = 0;
    uint32_t                            fileOffset   = 0;
    int64_t                             vmOffset     = 0;
    // decrement vmOffset by all segments before TEXT (e.g. PAGEZERO)
    for (uint32_t i=0; i < _binaryData->segmentsArrayCount; ++i) {
        const binary_format::DiskSegment* seg = &diskSegs[i];
        if ( seg->filePageCount != 0 ) {
            break;
        }
        vmOffset -= (uint64_t)seg->vmPageCount * pageSize;
    }
    // walk each segment and call handler
    for (uint32_t i=0; i < _binaryData->segmentsArrayCount; ++i) {
        const binary_format::DiskSegment* seg = &diskSegs[i];
        uint64_t vmSize   = (uint64_t)seg->vmPageCount * pageSize;
        uint32_t fileSize = seg->filePageCount * pageSize;
        if ( !seg->paddingNotSeg ) {
            bool     stop     = false;
            handler(segIndex, ( fileSize == 0) ? 0 : fileOffset, fileSize, vmOffset, vmSize, seg->permissions, stop);
            ++segIndex;
            if ( stop )
                break;
        }
        vmOffset   += vmSize;
        fileOffset += fileSize;
    }
}

void Image::forEachCacheSegment(void (^handler)(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop)) const
{
    assert(!isDiskImage());
    const uint64_t* rawSegs = group().segmentPool(_binaryData->segmentsArrayStartIndex);
    const binary_format::DyldCacheSegment* cacheSegs = (binary_format::DyldCacheSegment*)rawSegs;
    bool stop = false;
    for (uint32_t i=0; i < _binaryData->segmentsArrayCount; ++i) {
        uint64_t vmOffset    = cacheSegs[i].cacheOffset - cacheSegs[0].cacheOffset;
        uint64_t vmSize      = cacheSegs[i].size;
        uint8_t  permissions = cacheSegs[i].permissions;
        handler(i, vmOffset, vmSize, permissions, stop);
        if ( stop )
            break;
    }
}

bool Image::segmentHasFixups(uint32_t segIndex) const
{
    return (segmentFixups(segIndex) != nullptr);
}

bool Image::containsAddress(const void* addr, const void* imageLoadAddress, uint8_t* permissions) const
{
    if ( addr < imageLoadAddress )
        return false;

    __block bool found = false;
    uint64_t offsetInImage = (char*)addr - (char*)imageLoadAddress;
    if ( _binaryData->isDiskImage ) {
        forEachDiskSegment(^(uint32_t segIterIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t segPerms, bool& stop) {
            if ( (offsetInImage >= vmOffset) && (offsetInImage < vmOffset+vmSize) ) {
                if ( permissions != nullptr )
                    *permissions = segPerms;
                found = true;
                stop = true;
            }
        });
    }
    else {
        forEachCacheSegment(^(uint32_t segIterIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t segPerms, bool& stop) {
            if ( (offsetInImage >= vmOffset) && (offsetInImage < vmOffset+vmSize) ) {
                if ( permissions != nullptr )
                    *permissions = segPerms;
                found = true;
                stop = true;
            }
        });
    }
    return found;
}

void Image::forEachInitializer(const void* imageLoadAddress, void (^handler)(const void* initializer)) const
{
    const uint32_t   initCount   = _binaryData->initOffsetsArrayCount;
    const uint32_t   startIndex  = _binaryData->initOffsetsArrayStartIndex;
    const uint32_t*  initOffsets = group().initializerOffsetsPool();
    assert(startIndex + initCount <= group().initializerOffsetsCount());
    for (uint32_t i=0; i < initCount; ++i) {
        uint32_t anOffset = initOffsets[startIndex+i];
        const void* func = (char*)imageLoadAddress + anOffset;
        handler(func);
    }
}

void Image::forEachInitBefore(void (^handler)(binary_format::ImageRef imageToInit)) const
{
    const uint32_t                  initCount   = _binaryData->initBeforeArrayCount;
    const uint32_t                  startIndex  = _binaryData->initBeforeArrayStartIndex;
    const uint32_t                  endIndex    = group().intializerListPoolCount();
    const binary_format::ImageRef*  initRefs    = group().intializerListPool();
    assert(startIndex + initCount <= endIndex);
    for (uint32_t i=0; i < initCount; ++i) {
        binary_format::ImageRef ref = initRefs[startIndex+i];
        handler(ref);
    }
}

void Image::forEachDOF(const void* imageLoadAddress, void (^handler)(const void* section)) const
{
    const uint32_t   dofCount   = _binaryData->dofOffsetsArrayCount;
    const uint32_t   startIndex  = _binaryData->dofOffsetsArrayStartIndex;
    const uint32_t*  dofOffsets = group().dofOffsetsPool();
    assert(startIndex + dofCount <= group().dofOffsetsCount());
    for (uint32_t i=0; i < dofCount; ++i) {
        uint32_t anOffset = dofOffsets[startIndex+i];
        const void* section = (char*)imageLoadAddress + anOffset;
        handler(section);
    }
}

Image Image::resolveImageRef(const ImageGroupList& groups, binary_format::ImageRef ref, bool applyOverrides)
{
    // first look if ref image is overridden in closure
    __block binary_format::ImageRef targetRef = ref;
    if ( applyOverrides ) {
        binary_format::ImageRef refToMatch = ref;
        refToMatch.clearKind();
        for (int i=0; i < groups.count(); ++i) {
            ImageGroup aGroup(groups[i]);
            if ( aGroup.groupNum() >= 2 ) {
                aGroup.forEachImageRefOverride(^(binary_format::ImageRef standardDylibRef, binary_format::ImageRef overrideDylibRef, bool &stop) {
                    if ( refToMatch == standardDylibRef ) {
                        targetRef = overrideDylibRef;
                        stop = true;
                    }
                });
            }
        }
    }
    // create Image object from targetRef
    for (int i=0; i < groups.count(); ++i) {
        ImageGroup aGroup(groups[i]);
        if ( aGroup.groupNum() == targetRef.groupNum() ) {
            return aGroup.image(targetRef.indexInGroup());
        }
    }
    //assert(0 && "invalid ImageRef");
    return Image(nullptr);
}

void Image::forEachInitBefore(const ImageGroupList& groups, void (^handler)(Image imageToInit)) const
{
    forEachInitBefore(^(binary_format::ImageRef ref) {
        handler(resolveImageRef(groups, ref));
    });
}

bool Image::validateUsingModTimeAndInode() const
{
    return !group().binaryData()->imageFileInfoIsCdHash;
}

bool Image::validateUsingCdHash() const
{
    // don't have cdHash info if union has modtime info in it
    if ( !group().binaryData()->imageFileInfoIsCdHash )
        return false;

    // don't have codesign blob in dyld cache
    if ( !_binaryData->isDiskImage )
        return false;

    // return true if image is code signed and cdHash16 is non-zero
    const binary_format::DiskImage* diskImage = asDiskImage();
    if ( diskImage->codeSignFileOffset == 0 )
        return false;

    uint8_t zeros[16];
    bzero(zeros, 16);
    return (memcmp(cdHash16(), zeros, 16) != 0);
}

const uint8_t* Image::cdHash16() const
{
    return _binaryData->fileInfo.cdHash16.bytes;
}

uint64_t Image::fileModTime() const
{
    return _binaryData->fileInfo.statInfo.mtime;
}

uint64_t Image::fileINode() const
{
    return _binaryData->fileInfo.statInfo.inode;
}


bool Image::isDiskImage() const
{
    return _binaryData->isDiskImage;
}

const binary_format::DiskImage* Image::asDiskImage() const
{
    assert(_binaryData->isDiskImage);
    return (binary_format::DiskImage*)_binaryData;
}

const binary_format::CachedImage* Image::asCachedImage() const
{
    assert(!_binaryData->isDiskImage);
    return (binary_format::CachedImage*)_binaryData;
}

uint32_t Image::pageSize() const
{
    return (_binaryData->has16KBpages ?  0x4000 : 0x1000);
}

uint32_t Image::cacheOffset() const
{
    assert(!_binaryData->isDiskImage);
    const uint64_t* rawSegs = group().segmentPool(_binaryData->segmentsArrayStartIndex);
    const binary_format::DyldCacheSegment* cacheSegs = (binary_format::DyldCacheSegment*)rawSegs;
    return cacheSegs[0].cacheOffset;
}

uint32_t Image::patchStartIndex() const
{
    return asCachedImage()->patchStartIndex;
}

uint32_t Image::patchCount() const
{
    return asCachedImage()->patchCount;
}

uint64_t Image::sliceOffsetInFile() const
{
    return asDiskImage()->sliceOffsetIn4K * 4096;
}

bool Image::hasCodeSignature(uint32_t& fileOffset, uint32_t& size) const
{
    const binary_format::DiskImage* diskImage = asDiskImage();
    if ( diskImage->codeSignFileOffset != 0 ) {
        fileOffset = diskImage->codeSignFileOffset;
        size       = diskImage->codeSignFileSize;
        return true;
    }
    return false;
}

bool Image::isFairPlayEncrypted(uint32_t& textOffset, uint32_t& size) const
{
    const binary_format::DiskImage* diskImage = asDiskImage();
    if ( diskImage->fairPlayTextPageCount != 0 ) {
        textOffset = diskImage->fairPlayTextStartPage * pageSize();
        size       = diskImage->fairPlayTextPageCount * pageSize();
        return true;
    }
    return false;
}

uint64_t Image::vmSizeToMap() const
{
    return asDiskImage()->totalVmPages * pageSize();
}

void Image::forEachFixup(const uint8_t* pageFixups, const void* segContent, uint32_t& offset, uint32_t& ordinal,
                         void (^handler)(uint32_t pageOffset, FixupKind kind, uint32_t ordinal, bool& stop))
{
    bool stop = false;
    for (const uint8_t* p = pageFixups; (*p != 0) && !stop;) {
        binary_format::FixUpOpcode fullOp = (binary_format::FixUpOpcode)(*p);
        binary_format::FixUpOpcode majorOp = (binary_format::FixUpOpcode)(*p & 0xF0);
        uint8_t low4 = (*p & 0x0F);
        switch ( majorOp ) {
            case binary_format::FixUpOpcode::done:
                return;
            case binary_format::FixUpOpcode::rebase32: // apply
                switch ( fullOp ) {
                    case binary_format::FixUpOpcode::bind64:
                        handler(offset, FixupKind::bind64, ordinal, stop);
                        offset += 8;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::bind32:
                        handler(offset, FixupKind::bind32, ordinal, stop);
                        offset += 4;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::rebase64:
                        handler(offset, FixupKind::rebase64, 0, stop);
                        offset += 8;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::rebase32:
                        handler(offset, FixupKind::rebase32, 0, stop);
                        offset += 4;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::rebaseText32:
                        handler(offset, FixupKind::rebaseText32, 0, stop);
                        offset += 4;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::bindText32:
                        handler(offset, FixupKind::bindText32, ordinal, stop);
                        offset += 4;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::bindTextRel32:
                        handler(offset, FixupKind::bindTextRel32, ordinal, stop);
                        offset += 4;
                        ++p;
                        break;
                    case binary_format::FixUpOpcode::bindImportJmp32:
                        handler(offset, FixupKind::bindImportJmp32, ordinal, stop);
                        offset += 5;
                        ++p;
                        break;
                    //case binary_format::FixUpOpcode::fixupChain64:
                    //    assert(0 && "rebase/bind chain support not implemented yet");
                    //    break;
                    default:
                        assert(0 && "bad opcode");
                        break;
                }
                break;
            case binary_format::FixUpOpcode::incPageOffset:
                if ( low4 == 0 ) {
                    ++p;
                    offset += read_uleb128(p, p+8)*4;
                }
                else {
                    offset += (low4*4);
                    ++p;
                }
                break;
            case binary_format::FixUpOpcode::setPageOffset:
                if ( low4 == 0 ) {
                    ++p;
                    offset = (uint32_t)read_uleb128(p, p+8);
                }
                else {
                    offset = low4;
                    ++p;
                }
                break;
            case binary_format::FixUpOpcode::incOrdinal:
                if ( low4 == 0 ) {
                    ++p;
                    ordinal += read_uleb128(p, p+8);
                }
                else {
                    ordinal += low4;
                    ++p;
                }
                break;
            case binary_format::FixUpOpcode::setOrdinal:
                if ( low4 == 0 ) {
                    ++p;
                    ordinal = (uint32_t)read_uleb128(p, p+8);
                }
                else {
                    ordinal = low4;
                    ++p;
                }
                break;
            case binary_format::FixUpOpcode::repeat: {
                    ++p;
                    uint32_t count = (uint32_t)read_uleb128(p, p+8);
                    uint8_t pattern[32];
                    for (int j=0; j < low4; ++j) {
                        pattern[j] = *p++;
                    }
                    pattern[low4] = (uint8_t)binary_format::FixUpOpcode::done;
                    for (int j=0; j < count; ++j) {
                        forEachFixup(&pattern[0], segContent, offset, ordinal, handler);
                        if ( stop )
                            break;
                    }
                }
                break;
            default:
                assert(0 && "bad opcode");
                break;
        }
    }
}

const binary_format::SegmentFixupsByPage* Image::segmentFixups(uint32_t segIndex) const
{
    const binary_format::DiskImage* diskImage = asDiskImage();
    //const BinaryImageGroupData* g =  group().binaryData();
    uint32_t segCountWithFixups = diskImage->fixupsPoolSegCount;
    //fprintf(stderr,"segmentFixups(binImage=%p, segIndex=%d), group=%p, segCountWithFixup=%d\n", _binaryData, segIndex, g, segCountWithFixups);
    const binary_format::AllFixupsBySegment* allFixups = group().fixUps(diskImage->fixupsPoolOffset);
    for (uint32_t i=0; i < segCountWithFixups; ++i) {
        if ( allFixups[i].segIndex == segIndex ) {
            //fprintf(stderr,"segmentFixups(binImage=%p, segIndex=%d) allFixups=%p, allFixups[%d].segIndex=%d, allFixups[%d].offset=%d\n", _binaryData, segIndex, allFixups, i, allFixups[i].segIndex, i, allFixups[i].offset);
            return (binary_format::SegmentFixupsByPage*)((char*)allFixups + allFixups[i].offset);
        }
    }
    //fprintf(stderr,"segmentFixups(binImage=%p, segIndex=%d) => nullptr\n", _binaryData, segIndex);
    return nullptr;
}

void Image::forEachFixup(uint32_t segIndex, MemoryRange segContent, void (^handler)(uint64_t segOffset, FixupKind, TargetSymbolValue, bool& stop)) const
{
    const binary_format::SegmentFixupsByPage* segFixups = segmentFixups(segIndex);
    if ( segFixups == nullptr )
        return;

    assert(segFixups->pageCount*segFixups->pageSize <= segContent.size);

    const uint32_t ordinalsIndexInGroupPool = asDiskImage()->targetsArrayStartIndex;
    const uint32_t maxOrdinal = asDiskImage()->targetsArrayCount;
    const TargetSymbolValue* groupArray = group().targetValuesArray();
    assert(ordinalsIndexInGroupPool < group().targetValuesCount());
    const TargetSymbolValue* targetOrdinalArray = &groupArray[ordinalsIndexInGroupPool];

    for (uint32_t pageIndex=0; pageIndex < segFixups->pageCount; ++pageIndex) {
        const uint8_t* opcodes = (uint8_t*)(segFixups) + segFixups->pageInfoOffsets[pageIndex];
        uint64_t pageStartOffet = pageIndex * segFixups->pageSize;
        uint32_t curOffset = 0;
        uint32_t curOrdinal = 0;
        forEachFixup(opcodes, segContent.address, curOffset, curOrdinal, ^(uint32_t pageOffset, FixupKind kind, uint32_t targetOrdinal, bool& stop) {
            assert(targetOrdinal < maxOrdinal);
            handler(pageStartOffet + pageOffset, kind, targetOrdinalArray[targetOrdinal], stop);
        });
    }
}


} // namespace launch_cache
} // namespace dyld3



