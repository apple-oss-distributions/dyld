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
#include <string.h>
#include <assert.h>
#include <uuid/uuid.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>

#include <string>
#include <map>
#include <list>
#include <unordered_set>
#include <unordered_map>

#include "LaunchCacheFormat.h"
#include "LaunchCacheWriter.h"
#include "shared-cache/dyld_cache_format.h"
#include "shared-cache/DyldSharedCache.h"
#include "shared-cache/FileUtils.h"

namespace std
{
  template <>
  struct hash<dyld3::launch_cache::binary_format::ImageRef>
  {
    std::size_t operator()(const dyld3::launch_cache::binary_format::ImageRef& value) const {
        return std::hash<uint16_t>()(value.value());
    }
  };
}


namespace dyld3 {
namespace launch_cache {


static uintptr_t align(uintptr_t value, uintptr_t align)
{
    return (value+align-1) & (-align);
}

////////////////////////////  ImageGroupWriter ////////////////////////////////////////

ImageGroupWriter::ImageGroupWriter(uint32_t groupNum, bool pages16KB, bool is64, bool dylibsExpectedOnDisk, bool mtimeAndInodeAreValid)
    : _isDiskImage(groupNum != 0), _is64(is64), _groupNum(groupNum), _pageSize(pages16KB ? 0x4000 : 0x1000),
      _dylibsExpectedOnDisk(dylibsExpectedOnDisk), _imageFileInfoIsCdHash(!mtimeAndInodeAreValid)
{
}


uint32_t ImageGroupWriter::size() const
{
    binary_format::ImageGroup tempGroup;
    layoutBinary(&tempGroup);
    return tempGroup.stringsPoolOffset + tempGroup.stringsPoolSize;
}

void ImageGroupWriter::layoutBinary(binary_format::ImageGroup* grp) const
{
    grp->imagesEntrySize            = _isDiskImage ? sizeof(binary_format::DiskImage) : sizeof(binary_format::CachedImage);
    grp->groupNum                   = _groupNum;
    grp->dylibsExpectedOnDisk       = _dylibsExpectedOnDisk;
    grp->imageFileInfoIsCdHash      = _imageFileInfoIsCdHash;
    grp->padding                    = 0;

    grp->imagesPoolCount            = imageCount();
    grp->imagesPoolOffset           = sizeof(binary_format::ImageGroup);
    uint32_t imagesPoolSize         = grp->imagesEntrySize * grp->imagesPoolCount;

    grp->imageAliasCount            = (uint32_t)_aliases.size();
    grp->imageAliasOffset           = grp->imagesPoolOffset + imagesPoolSize;
    uint32_t imageAliasSize         = grp->imageAliasCount * sizeof(binary_format::AliasEntry);

    grp->segmentsPoolCount          = (uint32_t)_segmentPool.size();
    grp->segmentsPoolOffset         = (uint32_t)align(grp->imageAliasOffset + imageAliasSize, 8);
    uint32_t segmentsPoolSize       = grp->segmentsPoolCount * sizeof(uint64_t);

    grp->dependentsPoolCount        = (uint32_t)_dependentsPool.size();
    grp->dependentsPoolOffset       = grp->segmentsPoolOffset + segmentsPoolSize;
    uint32_t dependentsPoolSize     = grp->dependentsPoolCount * sizeof(binary_format::ImageRef);

    grp->intializerOffsetPoolCount  = (uint32_t)_initializerOffsets.size();
    grp->intializerOffsetPoolOffset = (uint32_t)align(grp->dependentsPoolOffset + dependentsPoolSize, 4);
    uint32_t intializerOffsetSize   = grp->intializerOffsetPoolCount * sizeof(uint32_t);

    grp->intializerListPoolCount    = (uint32_t)_initializerBeforeLists.size();
    grp->intializerListPoolOffset   = grp->intializerOffsetPoolOffset  + intializerOffsetSize;
    uint32_t intializerListPoolSize = grp->intializerListPoolCount * sizeof(binary_format::ImageRef);

    grp->targetsPoolCount           = (uint32_t)_targetsPool.size();
    grp->targetsOffset              = (uint32_t)align(grp->intializerListPoolOffset + intializerListPoolSize, 8);
    uint32_t targetsSize            = grp->targetsPoolCount * sizeof(TargetSymbolValue);

    grp->fixupsPoolSize             = (uint32_t)_fixupsPool.size();
    grp->fixupsOffset               = (uint32_t)align(grp->targetsOffset + targetsSize, 4);

    grp->cachePatchTableCount       = (uint32_t)_patchPool.size();
    grp->cachePatchTableOffset      = (uint32_t)align(grp->fixupsOffset + grp->fixupsPoolSize, 4);
    uint32_t patchTableSize         = grp->cachePatchTableCount * sizeof(binary_format::PatchTable);

    grp->cachePatchOffsetsCount     = (uint32_t)_patchLocationPool.size();
    grp->cachePatchOffsetsOffset    = grp->cachePatchTableOffset + patchTableSize;
    uint32_t patchOffsetsSize       = grp->cachePatchOffsetsCount * sizeof(binary_format::PatchOffset);

    grp->symbolOverrideTableCount   = (uint32_t)_dyldCacheSymbolOverridePool.size();
    grp->symbolOverrideTableOffset  = grp->cachePatchOffsetsOffset + patchOffsetsSize;
    uint32_t symbolOverrideSize     = grp->symbolOverrideTableCount * sizeof(binary_format::DyldCacheOverride);

    grp->imageOverrideTableCount    = (uint32_t)_imageOverridePool.size();
    grp->imageOverrideTableOffset   = grp->symbolOverrideTableOffset + symbolOverrideSize;
    uint32_t imageOverrideSize      = grp->imageOverrideTableCount * sizeof(binary_format::ImageRefOverride);

    grp->dofOffsetPoolCount         = (uint32_t)_dofOffsets.size();
    grp->dofOffsetPoolOffset        = grp->imageOverrideTableOffset  + imageOverrideSize;
    uint32_t dofOffsetSize          = grp->dofOffsetPoolCount * sizeof(uint32_t);

    grp->indirectGroupNumPoolCount  = (uint32_t)_indirectGroupNumPool.size();
    grp->indirectGroupNumPoolOffset = grp->dofOffsetPoolOffset + dofOffsetSize;
    uint32_t indirectGroupNumSize   = grp->indirectGroupNumPoolCount * sizeof(uint32_t);

    grp->stringsPoolSize            = (uint32_t)_stringPool.size();
    grp->stringsPoolOffset          = grp->indirectGroupNumPoolOffset + indirectGroupNumSize;
}


void ImageGroupWriter::finalizeTo(Diagnostics& diag, const std::vector<const BinaryImageGroupData*>& curGroups, binary_format::ImageGroup* grp) const
{
    layoutBinary(grp);
    uint8_t* buffer = (uint8_t*)grp;
    if ( imageCount() > 0 ) {
        uint32_t pad1Size   = grp->segmentsPoolOffset - (grp->imageAliasOffset + grp->imageAliasCount * sizeof(binary_format::AliasEntry));
        uint32_t pad2Size   = grp->targetsOffset - (grp->intializerListPoolOffset + grp->intializerListPoolCount * sizeof(binary_format::ImageRef));
        memcpy(&buffer[grp->imagesPoolOffset],          &imageByIndex(0),                   grp->imagesEntrySize * grp->imagesPoolCount);
        memcpy(&buffer[grp->imageAliasOffset],          &_aliases[0],                       grp->imageAliasCount * sizeof(binary_format::AliasEntry));
        bzero( &buffer[grp->segmentsPoolOffset-pad1Size],                                   pad1Size);
        memcpy(&buffer[grp->segmentsPoolOffset],        &_segmentPool[0],                   grp->segmentsPoolCount * sizeof(uint64_t));
        memcpy(&buffer[grp->dependentsPoolOffset],      &_dependentsPool[0],                grp->dependentsPoolCount * sizeof(binary_format::ImageRef));
        memcpy(&buffer[grp->intializerListPoolOffset],  &_initializerBeforeLists[0],        grp->intializerListPoolCount * sizeof(binary_format::ImageRef));
        memcpy(&buffer[grp->intializerOffsetPoolOffset],&_initializerOffsets[0],            grp->intializerOffsetPoolCount * sizeof(uint32_t));
        bzero( &buffer[grp->targetsOffset-pad2Size],                                        pad2Size);
        memcpy(&buffer[grp->targetsOffset],             &_targetsPool[0],                   grp->targetsPoolCount * sizeof(TargetSymbolValue));
        memcpy(&buffer[grp->fixupsOffset],               _fixupsPool.start(),               grp->fixupsPoolSize);
        memcpy(&buffer[grp->cachePatchTableOffset],     &_patchPool[0],                     grp->cachePatchTableCount * sizeof(binary_format::PatchTable));
        memcpy(&buffer[grp->cachePatchOffsetsOffset],   &_patchLocationPool[0],             grp->cachePatchOffsetsCount * sizeof(binary_format::PatchOffset));
        memcpy(&buffer[grp->symbolOverrideTableOffset], &_dyldCacheSymbolOverridePool[0],   grp->symbolOverrideTableCount * sizeof(binary_format::DyldCacheOverride));
        memcpy(&buffer[grp->imageOverrideTableOffset],  &_imageOverridePool[0],             grp->imageOverrideTableCount * sizeof(binary_format::ImageRefOverride));
        memcpy(&buffer[grp->dofOffsetPoolOffset],       &_dofOffsets[0],                    grp->dofOffsetPoolCount * sizeof(uint32_t));
        memcpy(&buffer[grp->indirectGroupNumPoolOffset], &_indirectGroupNumPool[0],         grp->indirectGroupNumPoolCount * sizeof(uint32_t));
        memcpy(&buffer[grp->stringsPoolOffset],         &_stringPool[0],                    grp->stringsPoolSize);
    }

    // now that we have a real ImageGroup, we can analyze it to find max load counts for each image
    ImageGroup imGroup(grp);
    std::unordered_set<const BinaryImageData*> allDependents;
    STACK_ALLOC_DYNARRAY(const binary_format::ImageGroup*, curGroups.size()+1, newGroupList);
    for (int i=0; i < curGroups.size(); ++i)
        newGroupList[i] = curGroups[i];
    newGroupList[newGroupList.count()-1] = grp;
    for (uint32_t i=0; i < grp->imagesPoolCount; ++i) {
        Image image = imGroup.image(i);
        if ( image.isInvalid() )
            continue;
        allDependents.clear();
        allDependents.insert(image.binaryData());
        BinaryImageData* imageData = (BinaryImageData*)(buffer + grp->imagesPoolOffset + (i * grp->imagesEntrySize));
        if ( !image.recurseAllDependentImages(newGroupList, allDependents) ) {
            //diag.warning("%s dependents on an invalid dylib", image.path());
            imageData->isInvalid = true;
        }
        imageData->maxLoadCount = (uint32_t)allDependents.size();
    }
}

uint32_t ImageGroupWriter::maxLoadCount(Diagnostics& diag, const std::vector<const BinaryImageGroupData*>& curGroups, binary_format::ImageGroup* grp) const
{
    ImageGroup imGroup(grp);
    std::unordered_set<const BinaryImageData*> allDependents;
    std::vector<const BinaryImageGroupData*> allGroups = curGroups;
    if ( grp->groupNum == 2 )
        allGroups.push_back(grp);
    DynArray<const binary_format::ImageGroup*> groupList(allGroups);
    for (uint32_t i=0; i < grp->imagesPoolCount; ++i) {
        Image image = imGroup.image(i);
        if ( image.isInvalid() )
            continue;
        allDependents.insert(image.binaryData());
        BinaryImageData* imageData = (BinaryImageData*)((char*)grp + grp->imagesPoolOffset + (i * grp->imagesEntrySize));
        if ( !image.recurseAllDependentImages(groupList, allDependents) ) {
            //diag.warning("%s dependents on an invalid dylib", image.path());
            imageData->isInvalid = true;
        }
    }
    return (uint32_t)allDependents.size();
}

void ImageGroupWriter::setImageCount(uint32_t count)
{
    if ( _isDiskImage ) {
        _diskImages.resize(count);
        bzero(&_diskImages[0], count*sizeof(binary_format::DiskImage));
    }
    else {
        _images.resize(count);
        bzero(&_images[0], count*sizeof(binary_format::CachedImage));
    }

    int32_t offset =  0 - (int32_t)sizeof(binary_format::ImageGroup);
    for (uint32_t i=0; i < count; ++i) {
        binary_format::Image& img = imageByIndex(i);
        img.isDiskImage = _isDiskImage;
        img.has16KBpages = (_pageSize == 0x4000);
        img.groupOffset = offset;
        if ( _isDiskImage )
            offset -= sizeof(binary_format::DiskImage);
        else
            offset -= sizeof(binary_format::CachedImage);
    }
}

uint32_t ImageGroupWriter::imageCount() const
{
    if ( _isDiskImage )
        return (uint32_t)_diskImages.size();
    else
        return (uint32_t)_images.size();
}

binary_format::Image& ImageGroupWriter::imageByIndex(uint32_t imageIndex)
{
    assert(imageIndex < imageCount());
    if ( _isDiskImage )
        return _diskImages[imageIndex];
    else
        return _images[imageIndex];
}

const binary_format::Image& ImageGroupWriter::imageByIndex(uint32_t imageIndex) const
{
    assert(imageIndex < imageCount());
    if ( _isDiskImage )
        return _diskImages[imageIndex];
    else
        return _images[imageIndex];
}

bool ImageGroupWriter::isInvalid(uint32_t imageIndex) const
{
    return imageByIndex(imageIndex).isInvalid;
}

void ImageGroupWriter::setImageInvalid(uint32_t imageIndex)
{
    imageByIndex(imageIndex).isInvalid = true;
}

uint32_t ImageGroupWriter::addIndirectGroupNum(uint32_t groupNum)
{
    auto pos = _indirectGroupNumPoolExisting.find(groupNum);
    if ( pos != _indirectGroupNumPoolExisting.end() )
        return pos->second;
    uint32_t startOffset = (uint32_t)_indirectGroupNumPool.size();
    _indirectGroupNumPool.push_back(groupNum);
    _indirectGroupNumPoolExisting[startOffset] = groupNum;
    return startOffset;
}

uint32_t ImageGroupWriter::addString(const char* str)
{
    auto pos = _stringPoolExisting.find(str);
    if ( pos != _stringPoolExisting.end() )
        return pos->second;
    uint32_t startOffset = (uint32_t)_stringPool.size();
    size_t size = strlen(str) + 1;
    _stringPool.insert(_stringPool.end(), str, &str[size]);
    _stringPoolExisting[str] = startOffset;
    return startOffset;
}

void ImageGroupWriter::alignStringPool()
{
    while ( (_stringPool.size() % 4) != 0 )
        _stringPool.push_back('\0');
}

void ImageGroupWriter::setImagePath(uint32_t imageIndex, const char* path)
{
    binary_format::Image& image = imageByIndex(imageIndex);
    image.pathPoolOffset = addString(path);
    image.pathHash = ImageGroup::hashFunction(path);
}

void ImageGroupWriter::addImageAliasPath(uint32_t imageIndex, const char* anAlias)
{
    binary_format::AliasEntry entry;
    entry.aliasHash                 = ImageGroup::hashFunction(anAlias);
    entry.imageIndexInGroup         = imageIndex;
    entry.aliasOffsetInStringPool   = addString(anAlias);
    _aliases.push_back(entry);
}

void ImageGroupWriter::ImageGroupWriter::setImageUUID(uint32_t imageIndex, const uuid_t uuid)
{
    memcpy(imageByIndex(imageIndex).uuid, uuid, sizeof(uuid_t));
}

void ImageGroupWriter::setImageHasObjC(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).hasObjC = value;
}

void ImageGroupWriter::setImageIsBundle(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).isBundle = value;
}

void ImageGroupWriter::setImageHasWeakDefs(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).hasWeakDefs = value;
}

void ImageGroupWriter::setImageMayHavePlusLoads(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).mayHavePlusLoads = value;
}

void ImageGroupWriter::setImageNeverUnload(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).neverUnload = value;
}

void ImageGroupWriter::setImageMustBeThisDir(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).cwdSameAsThis = value;
}

void ImageGroupWriter::setImageIsPlatformBinary(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).isPlatformBinary = value;
}

void ImageGroupWriter::setImageOverridableDylib(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).overridableDylib = value;
}

void ImageGroupWriter::setImageFileMtimeAndInode(uint32_t imageIndex, uint64_t mTime, uint64_t inode)
{
    imageByIndex(imageIndex).fileInfo.statInfo.mtime = mTime;
    imageByIndex(imageIndex).fileInfo.statInfo.inode = inode;
    assert(!_imageFileInfoIsCdHash);
}

void ImageGroupWriter::setImageCdHash(uint32_t imageIndex, uint8_t cdHash[20])
{
    memcpy(imageByIndex(imageIndex).fileInfo.cdHash16.bytes, cdHash, 16);
    assert(_imageFileInfoIsCdHash);
}

void ImageGroupWriter::setImageIsEncrypted(uint32_t imageIndex, bool value)
{
    imageByIndex(imageIndex).isEncrypted = value;
}

void ImageGroupWriter::setImageMaxLoadCount(uint32_t imageIndex, uint32_t count)
{
    imageByIndex(imageIndex).maxLoadCount = count;
}

void ImageGroupWriter::setImageFairPlayRange(uint32_t imageIndex, uint32_t offset, uint32_t size)
{
    assert(imageIndex < imageCount());
    assert(_isDiskImage);
    binary_format::DiskImage& image = _diskImages[imageIndex];
    if ( image.has16KBpages ) {
        assert((offset & 0x3FFF) == 0);
        assert((size & 0x3FFF) == 0);
    }
    else {
        assert((offset & 0xFFF) == 0);
        assert((size & 0xFFF) == 0);
    }
    assert(offset < (_pageSize*16));
    image.fairPlayTextStartPage = offset / _pageSize;
    image.fairPlayTextPageCount = size / _pageSize;
}

void ImageGroupWriter::setImageInitializerOffsets(uint32_t imageIndex, const std::vector<uint32_t>& offsets)
{
    binary_format::Image& image      = imageByIndex(imageIndex);
    image.initOffsetsArrayStartIndex = _initializerOffsets.size();
    image.initOffsetsArrayCount      = offsets.size();
    _initializerOffsets.insert(_initializerOffsets.end(), offsets.begin(), offsets.end());
}

void ImageGroupWriter::setImageDOFOffsets(uint32_t imageIndex, const std::vector<uint32_t>& offsets)
{
    binary_format::Image& image      = imageByIndex(imageIndex);
    image.dofOffsetsArrayStartIndex  = _dofOffsets.size();
    image.dofOffsetsArrayCount       = offsets.size();
    _dofOffsets.insert(_dofOffsets.end(), offsets.begin(), offsets.end());
}

uint32_t ImageGroupWriter::addUniqueInitList(const std::vector<binary_format::ImageRef>& initBefore)
{
    // see if this initBefore list already exists in pool
    if ( _initializerBeforeLists.size() > initBefore.size() ) {
        size_t cmpLen = initBefore.size()*sizeof(binary_format::ImageRef);
        size_t end = _initializerBeforeLists.size() - initBefore.size();
        for (uint32_t i=0; i < end; ++i) {
            if ( memcmp(&initBefore[0], &_initializerBeforeLists[i], cmpLen) == 0 ) {
                return i;
            }
        }
    }
    uint32_t result = (uint32_t)_initializerBeforeLists.size();
    _initializerBeforeLists.insert(_initializerBeforeLists.end(), initBefore.begin(), initBefore.end());
    return result;
}

void ImageGroupWriter::setImageInitBefore(uint32_t imageIndex, const std::vector<binary_format::ImageRef>& initBefore)
{
    binary_format::Image& image = imageByIndex(imageIndex);
    image.initBeforeArrayStartIndex = addUniqueInitList(initBefore);
    image.initBeforeArrayCount = initBefore.size();
}

void ImageGroupWriter::setImageSliceOffset(uint32_t imageIndex, uint64_t fileOffset)
{
    assert(imageIndex < imageCount());
    assert(_isDiskImage);
    binary_format::DiskImage& image = _diskImages[imageIndex];
    image.sliceOffsetIn4K = (uint32_t)(fileOffset / 4096);
}

void ImageGroupWriter::setImageCodeSignatureLocation(uint32_t imageIndex, uint32_t fileOffset, uint32_t size)
{
    assert(imageIndex < imageCount());
    assert(_isDiskImage);
    binary_format::DiskImage& image = _diskImages[imageIndex];
    image.codeSignFileOffset = fileOffset;
    image.codeSignFileSize = size;
}

void ImageGroupWriter::setImageDependentsCount(uint32_t imageIndex, uint32_t count)
{
    binary_format::Image& image = imageByIndex(imageIndex);
    image.dependentsArrayStartIndex = _dependentsPool.size();
    image.dependentsArrayCount = count;
    _dependentsPool.resize(_dependentsPool.size() + count);
}

void ImageGroupWriter::setImageDependent(uint32_t imageIndex, uint32_t depIndex, binary_format::ImageRef dependent)
{
    binary_format::Image& image = imageByIndex(imageIndex);
    assert(depIndex < image.dependentsArrayCount);
    _dependentsPool[image.dependentsArrayStartIndex + depIndex] = dependent;
}

uint32_t ImageGroupWriter::imageDependentsCount(uint32_t imageIndex) const
{
    return imageByIndex(imageIndex).dependentsArrayCount;
}

binary_format::ImageRef ImageGroupWriter::imageDependent(uint32_t imageIndex, uint32_t depIndex) const
{
    const binary_format::Image& image = imageByIndex(imageIndex);
    assert(depIndex < image.dependentsArrayCount);
    return _dependentsPool[image.dependentsArrayStartIndex + depIndex];
}

void ImageGroupWriter::setImageSegments(uint32_t imageIndex, MachOParser& imageParser, uint64_t cacheUnslideBaseAddress)
{
    if ( _isDiskImage ) {
        __block uint32_t totalPageCount    = 0;
        __block uint32_t lastFileOffsetEnd = 0;
        __block uint64_t lastVmAddrEnd     = 0;
        __block std::vector<binary_format::DiskSegment> diskSegments;
        diskSegments.reserve(8);
        imageParser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
            if ( (fileOffset != 0) && (fileOffset != lastFileOffsetEnd) ) {
                binary_format::DiskSegment filePadding;
                filePadding.filePageCount   = (fileOffset - lastFileOffsetEnd)/_pageSize;
                filePadding.vmPageCount     = 0;
                filePadding.permissions     = 0;
                filePadding.paddingNotSeg   = 1;
                diskSegments.push_back(filePadding);
            }
            if ( (lastVmAddrEnd != 0) && (vmAddr != lastVmAddrEnd) ) {
                binary_format::DiskSegment vmPadding;
                vmPadding.filePageCount   = 0;
                vmPadding.vmPageCount     = (vmAddr - lastVmAddrEnd)/_pageSize;
                vmPadding.permissions     = 0;
                vmPadding.paddingNotSeg   = 1;
                diskSegments.push_back(vmPadding);
                totalPageCount += vmPadding.vmPageCount;
            }
            {
                binary_format::DiskSegment segInfo;
                segInfo.filePageCount   = (fileSize+_pageSize-1)/_pageSize;
                segInfo.vmPageCount     = (vmSize+_pageSize-1)/_pageSize;
                segInfo.permissions     = protections & 7;
                segInfo.paddingNotSeg   = 0;
                diskSegments.push_back(segInfo);
                totalPageCount   += segInfo.vmPageCount;
                if ( fileSize != 0 )
                    lastFileOffsetEnd = fileOffset + fileSize;
                if ( vmSize != 0 )
                    lastVmAddrEnd     = vmAddr + vmSize;
            }
        });
        binary_format::Image& image   = imageByIndex(imageIndex);
        image.segmentsArrayStartIndex = _segmentPool.size();
        image.segmentsArrayCount      = diskSegments.size();
        _segmentPool.insert(_segmentPool.end(), (uint64_t*)&diskSegments[0], (uint64_t*)&diskSegments[image.segmentsArrayCount]);
        _diskImages[imageIndex].totalVmPages = totalPageCount;
    }
    else {
        binary_format::Image& image   = imageByIndex(imageIndex);
        image.segmentsArrayStartIndex = _segmentPool.size();
        image.segmentsArrayCount      = imageParser.segmentCount();
        _segmentPool.resize(_segmentPool.size() + image.segmentsArrayCount);
        __block uint32_t segIndex = 0;
        imageParser.forEachSegment(^(const char* segName, uint32_t fileOffset, uint32_t fileSize, uint64_t vmAddr, uint64_t vmSize, uint8_t protections, bool& stop) {
            binary_format::DyldCacheSegment seg = { (uint32_t)(vmAddr-cacheUnslideBaseAddress), (uint32_t)vmSize, protections };
            _segmentPool[image.segmentsArrayStartIndex + segIndex] = *((uint64_t*)&seg);
            ++segIndex;
        });
    }
}

void ImageGroupWriter::setImagePatchLocations(uint32_t imageIndex, uint32_t funcVmOffset, const std::unordered_set<uint32_t>& patchLocations)
{
    assert(imageIndex < imageCount());
    binary_format::CachedImage& image = _images[imageIndex];
    if ( image.patchStartIndex == 0 ) {
        image.patchStartIndex = (uint32_t)_patchPool.size();
        image.patchCount      = 0;
    }
    else {
        assert(image.patchStartIndex + image.patchCount == _patchPool.size());
    }

    binary_format::PatchTable entry = { funcVmOffset, (uint32_t)_patchLocationPool.size() };
    for (uint32_t loc : patchLocations) {
        _patchLocationPool.push_back(*((binary_format::PatchOffset*)&loc));
    }
    _patchLocationPool.back().last = true;
    _patchPool.push_back(entry);
    _images[imageIndex].patchCount++;
}

void ImageGroupWriter::setGroupCacheOverrides(const std::vector<binary_format::DyldCacheOverride>& cacheOverrides)
{
    _dyldCacheSymbolOverridePool = cacheOverrides;
}

void ImageGroupWriter::addImageIsOverride(binary_format::ImageRef standardDylibRef, binary_format::ImageRef overrideDylibRef)
{
    _imageOverridePool.push_back({standardDylibRef, overrideDylibRef});
}


class SegmentFixUpBuilder
{
public:
                            SegmentFixUpBuilder(uint32_t segIndex, uint32_t dataSegPageCount, uint32_t pageSize, bool is64,
                                                    const std::vector<ImageGroupWriter::FixUp>& fixups,
                                                    std::vector<TargetSymbolValue>& targetsForImage, bool log);

    bool                    hasFixups() { return _hasFixups; }
    uint32_t                segIndex() { return _segIndex; }
    void                    appendSegmentFixUpMap(ContentBuffer&);

private:
    struct TmpOpcode {
        binary_format::FixUpOpcode    op;
        uint8_t                       repeatOpcodeCount;
        uint16_t                      count;

         bool operator!=(const TmpOpcode& rhs) const {
            return ((op != rhs.op) || (count != rhs.count) || (repeatOpcodeCount != rhs.repeatOpcodeCount));
        }
    };


    ContentBuffer           makeFixupOpcodesForPage(uint32_t pageStartSegmentOffset, const ImageGroupWriter::FixUp* start,
                                                    const ImageGroupWriter::FixUp* end);
    uint32_t                getOrdinalForTarget(TargetSymbolValue);
    void                    expandOpcodes(const std::vector<TmpOpcode>& opcodes, uint8_t page[0x4000],  uint32_t& offset, uint32_t& ordinal);
    void                    expandOpcodes(const std::vector<TmpOpcode>& opcodes, uint8_t page[0x4000]);
    bool                    samePageContent(const uint8_t page1[], const uint8_t page2[]);
    void                    printOpcodes(const char* prefix, const std::vector<TmpOpcode> opcodes);
    void                    printOpcodes(const char* prefix, bool printOffset, const TmpOpcode opcodes[], size_t opcodesLen, uint32_t& offset);
    uint32_t                opcodeEncodingSize(const std::vector<TmpOpcode>& opcodes);

    const bool                              _is64;
    const bool                              _log;
    bool                                    _hasFixups;
    const uint32_t                          _segIndex;
    const uint32_t                          _dataSegPageCount;
    const uint32_t                          _pageSize;
    std::vector<TargetSymbolValue>&         _targets;
    std::vector<ContentBuffer>              _opcodesByPage;
};




SegmentFixUpBuilder::SegmentFixUpBuilder(uint32_t segIndex, uint32_t segPageCount, uint32_t pageSize, bool is64,
                                         const std::vector<ImageGroupWriter::FixUp>& fixups,
                                         std::vector<TargetSymbolValue>& targetsForImage, bool log)
    : _is64(is64), _log(log), _hasFixups(false), _segIndex(segIndex), _dataSegPageCount(segPageCount), _pageSize(pageSize), _targets(targetsForImage)
{
    //fprintf(stderr, "SegmentFixUpBuilder(segIndex=%d, segPageCount=%d)\n", segIndex, segPageCount);
    _targets.push_back(TargetSymbolValue::makeInvalid()); // ordinal zero reserved to mean "add slide"
    _opcodesByPage.resize(segPageCount);
    size_t startFixupIndex = 0;
    for (uint32_t pageIndex=0; pageIndex < segPageCount; ++pageIndex) {
        uint32_t pageStartOffset = pageIndex*_pageSize;
        uint32_t pageEndOffset   = pageStartOffset+_pageSize;
        // find first index in this page
        while ( (startFixupIndex < fixups.size()) && ((fixups[startFixupIndex].segIndex != segIndex) || (fixups[startFixupIndex].segOffset < pageStartOffset))  )
            ++startFixupIndex;
        // find first index beyond this page
        size_t endFixupIndex = startFixupIndex;
        while ( (endFixupIndex < fixups.size()) && (fixups[endFixupIndex].segIndex == segIndex) && (fixups[endFixupIndex].segOffset < pageEndOffset) )
            ++endFixupIndex;
        // create opcodes for fixups on this pageb
        _opcodesByPage[pageIndex] = makeFixupOpcodesForPage(pageStartOffset, &fixups[startFixupIndex], &fixups[endFixupIndex]);
        startFixupIndex = endFixupIndex;
    }
}


uint32_t SegmentFixUpBuilder::getOrdinalForTarget(TargetSymbolValue target)
{
    uint32_t ordinal = 0;
    for (const TargetSymbolValue& entry : _targets) {
        if ( entry == target )
            return ordinal;
        ++ordinal;
    }
    _targets.push_back(target);
    return ordinal;
}

void SegmentFixUpBuilder::appendSegmentFixUpMap(ContentBuffer& buffer)
{
    std::vector<uint32_t> offsets;
    uint32_t curOffset = sizeof(binary_format::SegmentFixupsByPage)-4 + _dataSegPageCount*4;
    for (auto& opcodes : _opcodesByPage) {
        if ( opcodes.size() == 0 )
            offsets.push_back(0);
        else
            offsets.push_back(curOffset);
        curOffset += opcodes.size();
    }
    uint32_t totalSize = curOffset;

    // write header
    buffer.append_uint32(totalSize);                    // SegmentFixupsByPage.size
    buffer.append_uint32(_pageSize);                    // SegmentFixupsByPage.pageSize
    buffer.append_uint32(_dataSegPageCount);            // SegmentFixupsByPage.pageCount
    for (uint32_t i=0; i < _dataSegPageCount; ++i) {
        buffer.append_uint32(offsets[i]);               // SegmentFixupsByPage.pageInfoOffsets[i]
    }
    // write each page's opcode stream
    for (uint32_t i=0; i < offsets.size(); ++i) {
        buffer.append_buffer(_opcodesByPage[i]);
    }
}

void SegmentFixUpBuilder::expandOpcodes(const std::vector<TmpOpcode>& opcodes, uint8_t page[])
{
    uint32_t offset = 0;
    uint32_t ordinal = 0;
    bzero(page, _pageSize);
    expandOpcodes(opcodes, page, offset, ordinal);
}

void SegmentFixUpBuilder::expandOpcodes(const std::vector<TmpOpcode>& opcodes, uint8_t page[], uint32_t& offset, uint32_t& ordinal)
{
    for (int i=0; i < opcodes.size(); ++i) {
        assert(offset < _pageSize);
        TmpOpcode tmp = opcodes[i];
        switch ( tmp.op ) {
            case binary_format::FixUpOpcode::bind64:
                *(uint64_t*)(&page[offset]) = ordinal;
                offset += 8;
                break;
            case binary_format::FixUpOpcode::bind32:
                *(uint32_t*)(&page[offset]) = ordinal;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::rebase64:
                *(uint64_t*)(&page[offset]) = 0x1122334455667788;
                offset += 8;
                break;
            case binary_format::FixUpOpcode::rebase32:
                *(uint32_t*)(&page[offset]) = 0x23452345;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::rebaseText32:
                *(uint32_t*)(&page[offset]) = 0x56785678;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::bindText32:
                *(uint32_t*)(&page[offset]) = 0x98769876;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::bindTextRel32:
                *(uint32_t*)(&page[offset]) = 0x34563456;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::bindImportJmp32:
                *(uint32_t*)(&page[offset]) = 0x44556677;
                offset += 4;
                break;
            case binary_format::FixUpOpcode::done:
                break;
            case binary_format::FixUpOpcode::setPageOffset:
                offset = tmp.count;
                break;
            case binary_format::FixUpOpcode::incPageOffset:
                offset += (tmp.count*4);
                break;
            case binary_format::FixUpOpcode::setOrdinal:
                ordinal = tmp.count;
                break;
            case binary_format::FixUpOpcode::incOrdinal:
                ++ordinal;
                break;
            case binary_format::FixUpOpcode::repeat: {
                    std::vector<TmpOpcode> pattern;
                    for (int j=0; j < tmp.repeatOpcodeCount; ++j) {
                        pattern.push_back(opcodes[i+j+1]);
                    }
                    for (int j=0; j < tmp.count; ++j) {
                        expandOpcodes(pattern, page, offset, ordinal);
                    }
                    i += tmp.repeatOpcodeCount;
                }
                break;
        }
    }
}



uint32_t SegmentFixUpBuilder::opcodeEncodingSize(const std::vector<TmpOpcode>& opcodes)
{
    uint32_t size = 0;
    for (int i=0; i < opcodes.size(); ++i) {
        switch ( opcodes[i].op ) {
            case binary_format::FixUpOpcode::bind64:
            case binary_format::FixUpOpcode::bind32:
            case binary_format::FixUpOpcode::rebase64:
            case binary_format::FixUpOpcode::rebase32:
            case binary_format::FixUpOpcode::rebaseText32:
            case binary_format::FixUpOpcode::bindText32:
            case binary_format::FixUpOpcode::bindTextRel32:
            case binary_format::FixUpOpcode::bindImportJmp32:
            case binary_format::FixUpOpcode::done:
                ++size;
                break;
            case binary_format::FixUpOpcode::setPageOffset:
            case binary_format::FixUpOpcode::incPageOffset:
            case binary_format::FixUpOpcode::setOrdinal:
            case binary_format::FixUpOpcode::incOrdinal:
                ++size;
                if ( opcodes[i].count >= 16 )
                    size += ContentBuffer::uleb128_size(opcodes[i].count);
                break;
            case binary_format::FixUpOpcode::repeat: {
                    ++size;
                    size += ContentBuffer::uleb128_size(opcodes[i].count);
                    std::vector<TmpOpcode> pattern;
                    for (int j=0; j < opcodes[i].repeatOpcodeCount; ++j) {
                        pattern.push_back(opcodes[++i]);
                    }
                    size += opcodeEncodingSize(pattern);
                }
                break;
        }
    }
    return size;
}


bool SegmentFixUpBuilder::samePageContent(const uint8_t page1[], const uint8_t page2[])
{
    bool result = true;
    if (memcmp(page1, page2, _pageSize) != 0) {
        if ( _is64 ) {
            const uint64_t* p1 = (uint64_t* )page1;
            const uint64_t* p2 = (uint64_t* )page2;
            for (int i=0; i < _pageSize/8; ++i) {
                if ( p1[i] != p2[i] ) {
                    fprintf(stderr, "page1[0x%03X] = 0x%016llX, page2[0x%03X] = 0x%016llX\n", i*8, p1[i], i*8, p2[i]);
                    result = false;
                }
            }
        }
        else {
            const uint32_t* p1 = (uint32_t* )page1;
            const uint32_t* p2 = (uint32_t* )page2;
            for (int i=0; i < _pageSize/4; ++i) {
                if ( p1[i] != p2[i] ) {
                    fprintf(stderr, "page1[0x%03X] = 0x%016X, page2[0x%03X] = 0x%016X\n", i*4, p1[i], i*4, p2[i]);
                    result = false;
                }
            }
        }
    }
    return result;
}

void SegmentFixUpBuilder::printOpcodes(const char* prefix, const std::vector<TmpOpcode> opcodes)
{
    uint32_t offset = 0;
    printOpcodes(prefix, true, &opcodes[0], opcodes.size(), offset);
}

void SegmentFixUpBuilder::printOpcodes(const char* prefix, bool printOffset, const TmpOpcode opcodes[], size_t opcodesLen, uint32_t& offset)
{
    for (int i=0; i < opcodesLen; ++i) {
        TmpOpcode tmp = opcodes[i];
        if ( printOffset )
            fprintf(stderr, "%s offset=0x%04X: ", prefix, offset);
        else
            fprintf(stderr, "%s               ", prefix);
        switch ( tmp.op ) {
            case binary_format::FixUpOpcode::bind64:
                fprintf(stderr, "bind64\n");
                offset += 8;
                break;
            case binary_format::FixUpOpcode::bind32:
                fprintf(stderr, "bind32\n");
                offset += 4;
                break;
            case binary_format::FixUpOpcode::rebase64:
                fprintf(stderr, "rebase64\n");
                offset += 8;
                break;
            case binary_format::FixUpOpcode::rebase32:
                fprintf(stderr, "rebase32\n");
                offset += 4;
                break;
            case binary_format::FixUpOpcode::rebaseText32:
                fprintf(stderr, "rebaseText32\n");
                offset += 4;
                break;
            case binary_format::FixUpOpcode::bindText32:
                fprintf(stderr, "bindText32\n");
                offset += 4;
                break;
            case binary_format::FixUpOpcode::bindTextRel32:
                fprintf(stderr, "bindTextRel32\n");
                offset += 4;
                break;
           case binary_format::FixUpOpcode::bindImportJmp32:
                fprintf(stderr, "bindJmpRel32\n");
                offset += 4;
                break;
            case binary_format::FixUpOpcode::done:
                fprintf(stderr, "done\n");
                break;
            case binary_format::FixUpOpcode::setPageOffset:
                fprintf(stderr, "setPageOffset(%d)\n", tmp.count);
                offset = tmp.count;
                break;
            case binary_format::FixUpOpcode::incPageOffset:
                fprintf(stderr, "incPageOffset(%d)\n", tmp.count);
                offset += (tmp.count*4);
                break;
            case binary_format::FixUpOpcode::setOrdinal:
                fprintf(stderr, "setOrdinal(%d)\n", tmp.count);
                break;
            case binary_format::FixUpOpcode::incOrdinal:
                fprintf(stderr, "incOrdinal(%d)\n", tmp.count);
                break;
            case binary_format::FixUpOpcode::repeat: {
                    char morePrefix[128];
                    strcpy(morePrefix, prefix);
                    strcat(morePrefix, "          ");
                    uint32_t prevOffset = offset;
                    fprintf(stderr, "repeat(%d times, next %d opcodes)\n", tmp.count, tmp.repeatOpcodeCount);
                    printOpcodes(morePrefix, false, &opcodes[i+1], tmp.repeatOpcodeCount, offset);
                    i += tmp.repeatOpcodeCount;
                    uint32_t repeatDelta = (offset-prevOffset)*(tmp.count-1);
                    offset += repeatDelta;
                }
                break;
        }
    }
}

ContentBuffer SegmentFixUpBuilder::makeFixupOpcodesForPage(uint32_t pageStartSegmentOffset, const ImageGroupWriter::FixUp* start, const ImageGroupWriter::FixUp* end)
{
    //fprintf(stderr, "  makeFixupOpcodesForPage(segOffset=0x%06X, startFixup=%p, endFixup=%p)\n", pageStartSegmentOffset, start, end);
    std::vector<TmpOpcode> tmpOpcodes;
    const uint32_t pointerSize = (_is64 ? 8 : 4);
    uint32_t offset = pageStartSegmentOffset;
    uint32_t ordinal = 0;
    const ImageGroupWriter::FixUp* lastFixup = nullptr;
    for (const ImageGroupWriter::FixUp* f=start; f < end; ++f) {
        // ignore double bind at same address (ld64 bug)
        if ( lastFixup && (lastFixup->segOffset == f->segOffset) )
            continue;
        // add opcode to adjust current offset if needed
        if ( f->segOffset != offset ) {
            if ( ((f->segOffset % 4) != 0) || ((offset % 4) != 0) ) {
                // mis aligned pointers use bigger set opcode
                tmpOpcodes.push_back({binary_format::FixUpOpcode::setPageOffset, 0, (uint16_t)(f->segOffset-pageStartSegmentOffset)});
            }
            else {
                uint32_t delta4 = (uint32_t)(f->segOffset - offset)/4;
                assert(delta4*4 < _pageSize);
                tmpOpcodes.push_back({binary_format::FixUpOpcode::incPageOffset, 0, (uint16_t)delta4});
            }
            offset = (uint32_t)f->segOffset;
        }
        uint32_t nextOrd = 0;
        switch ( f->type ) {
            case ImageGroupWriter::FixupType::rebase:
                tmpOpcodes.push_back({_is64 ? binary_format::FixUpOpcode::rebase64 : binary_format::FixUpOpcode::rebase32, 0, 0});
                offset += pointerSize;
                _hasFixups = true;
                break;
            case ImageGroupWriter::FixupType::pointerLazyBind:
            case ImageGroupWriter::FixupType::pointerBind:
                //assert(f->target.imageIndex == binary_format::OrdinalEntry::kImageIndexDyldSharedCache);
                nextOrd = getOrdinalForTarget(f->target);
                if ( nextOrd != ordinal ) {
                    if ( (nextOrd > ordinal) && (nextOrd < (ordinal+31)) ) {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::incOrdinal, 0, (uint16_t)(nextOrd-ordinal)});
                    }
                    else {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::setOrdinal, 0, (uint16_t)nextOrd});
                    }
                    ordinal = nextOrd;
                }
                tmpOpcodes.push_back({_is64 ? binary_format::FixUpOpcode::bind64 : binary_format::FixUpOpcode::bind32, 0, 0});
                offset += pointerSize;
                 _hasFixups = true;
               break;
            case ImageGroupWriter::FixupType::rebaseText:
                assert(!_is64);
                tmpOpcodes.push_back({binary_format::FixUpOpcode::rebaseText32, 0, 0});
                offset += pointerSize;
                _hasFixups = true;
                break;
            case ImageGroupWriter::FixupType::bindText:
                assert(!_is64);
                 nextOrd = getOrdinalForTarget(f->target);
                if ( nextOrd != ordinal ) {
                    if ( (nextOrd > ordinal) && (nextOrd < (ordinal+31)) ) {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::incOrdinal, 0, (uint16_t)(nextOrd-ordinal)});
                    }
                    else {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::setOrdinal, 0, (uint16_t)nextOrd});
                    }
                    ordinal = nextOrd;
                }
                tmpOpcodes.push_back({binary_format::FixUpOpcode::bindText32, 0, 0});
                offset += pointerSize;
                _hasFixups = true;
                break;
            case ImageGroupWriter::FixupType::bindTextRel:
                assert(!_is64);
                nextOrd = getOrdinalForTarget(f->target);
                if ( nextOrd != ordinal ) {
                    if ( (nextOrd > ordinal) && (nextOrd < (ordinal+31)) ) {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::incOrdinal, 0, (uint16_t)(nextOrd-ordinal)});
                    }
                    else {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::setOrdinal, 0, (uint16_t)nextOrd});
                    }
                    ordinal = nextOrd;
                }
                tmpOpcodes.push_back({binary_format::FixUpOpcode::bindTextRel32, 0, 0});
                offset += pointerSize;
                _hasFixups = true;
                break;
            case ImageGroupWriter::FixupType::bindImportJmpRel:
                assert(!_is64);
                nextOrd = getOrdinalForTarget(f->target);
                if ( nextOrd != ordinal ) {
                    if ( (nextOrd > ordinal) && (nextOrd < (ordinal+31)) ) {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::incOrdinal, 0, (uint16_t)(nextOrd-ordinal)});
                    }
                    else {
                        tmpOpcodes.push_back({binary_format::FixUpOpcode::setOrdinal, 0, (uint16_t)nextOrd});
                    }
                    ordinal = nextOrd;
                }
                tmpOpcodes.push_back({binary_format::FixUpOpcode::bindImportJmp32, 0, 0});
                offset += pointerSize;
                _hasFixups = true;
                break;
            case ImageGroupWriter::FixupType::ignore:
                assert(0 && "ignore fixup types should have been removed");
                break;
        }
        lastFixup = f;
    }

    uint8_t firstExpansion[0x4010]; // larger than 16KB to handle unaligned pointers
    expandOpcodes(tmpOpcodes, firstExpansion);

    if (_log) printOpcodes("start", tmpOpcodes);


    for (int stride=1; stride < 6; ++stride) {
        for (int i=0; i < tmpOpcodes.size(); ++i) {
            int j;
            for (j=i+stride; j < tmpOpcodes.size(); j += stride) {
                bool strideMatch = true;
                for (int k=0; k < stride; ++k) {
                    if ( (j+k >= tmpOpcodes.size()) || (tmpOpcodes[j+k] != tmpOpcodes[i+k]) ) {
                        strideMatch = false;
                        break;
                    }
                    if ( (tmpOpcodes[j+k].op == binary_format::FixUpOpcode::repeat) && (tmpOpcodes[j+k].repeatOpcodeCount+k >= stride) ) {
                        strideMatch = false;
                        break;
                    }
                }
                if ( !strideMatch )
                    break;
            }
            // see if same opcode repeated three or more times
            int repeats = (j-i)/stride;
            if ( repeats > 3 ) {
                // replace run with repeat opcode
                tmpOpcodes[i].op                = binary_format::FixUpOpcode::repeat;
                tmpOpcodes[i].repeatOpcodeCount = stride;
                tmpOpcodes[i].count             = repeats;
                tmpOpcodes.erase(tmpOpcodes.begin()+i+1, tmpOpcodes.begin()+j-stride);
                i += stride;
            }
            else {
                // don't look for matches inside a repeat loop
                if ( tmpOpcodes[i].op == binary_format::FixUpOpcode::repeat )
                    i += tmpOpcodes[i].repeatOpcodeCount;
            }
        }
        if (_log) {
            char tmp[32];
            sprintf(tmp, "stride %d", stride);
            printOpcodes(tmp, tmpOpcodes);
        }
        uint8_t secondExpansion[0x4010];
        expandOpcodes(tmpOpcodes, secondExpansion);
        if ( !samePageContent(firstExpansion, secondExpansion) )
            printOpcodes("opt", tmpOpcodes);
    }

    // convert temp opcodes to real opcodes
    bool wroteDone = false;
    ContentBuffer opcodes;
    for (const TmpOpcode& tmp : tmpOpcodes) {
        switch ( tmp.op ) {
            case binary_format::FixUpOpcode::bind64:
            case binary_format::FixUpOpcode::bind32:
            case binary_format::FixUpOpcode::rebase64:
            case binary_format::FixUpOpcode::rebase32:
            case binary_format::FixUpOpcode::rebaseText32:
            case binary_format::FixUpOpcode::bindText32:
            case binary_format::FixUpOpcode::bindTextRel32:
            case binary_format::FixUpOpcode::bindImportJmp32:
                opcodes.append_byte((uint8_t)tmp.op);
                break;
            case binary_format::FixUpOpcode::done:
                opcodes.append_byte((uint8_t)tmp.op);
                wroteDone = true;
                break;
            case binary_format::FixUpOpcode::setPageOffset:
            case binary_format::FixUpOpcode::incPageOffset:
            case binary_format::FixUpOpcode::setOrdinal:
            case binary_format::FixUpOpcode::incOrdinal:
                if ( (tmp.count > 0) && (tmp.count < 16) ) {
                    opcodes.append_byte((uint8_t)tmp.op | tmp.count);
                }
                else {
                    opcodes.append_byte((uint8_t)tmp.op);
                    opcodes.append_uleb128(tmp.count);
                }
                break;
            case binary_format::FixUpOpcode::repeat: {
                    const TmpOpcode* nextOpcodes = &tmp;
                    ++nextOpcodes;
                    std::vector<TmpOpcode> pattern;
                    for (int i=0; i < tmp.repeatOpcodeCount; ++i) {
                        pattern.push_back(nextOpcodes[i]);
                    }
                    uint32_t repeatBytes = opcodeEncodingSize(pattern);
                    assert(repeatBytes < 15);
                    opcodes.append_byte((uint8_t)tmp.op | repeatBytes);
                    opcodes.append_uleb128(tmp.count);
                }
                break;
        }
    }

    if ( (opcodes.size() == 0) || !wroteDone )
        opcodes.append_byte((uint8_t)binary_format::FixUpOpcode::done);

    // make opcodes streams 4-byte aligned
    opcodes.pad_to_size(4);

    //fprintf(stderr, "  makeFixupOpcodesForPage(pageStartSegmentOffset=0x%0X) result=%lu bytes\n", pageStartSegmentOffset, opcodes.size());

    return opcodes;
}




void ImageGroupWriter::setImageFixups(Diagnostics& diag, uint32_t imageIndex, std::vector<FixUp>& fixups, bool hasTextRelocs)
{
    // only applicable for ImageGroup in a closure (not group of images in dyld cache)
    assert(_isDiskImage);

    // sort all rebases and binds by address
    std::sort(fixups.begin(), fixups.end(), [](FixUp& lhs, FixUp& rhs) -> bool {
        if ( &lhs == &rhs )
            return false;
        // sort by segIndex
        if ( lhs.segIndex < rhs.segIndex )
            return true;
        if ( lhs.segIndex > rhs.segIndex )
            return false;
        // then sort by segOffset
        if ( lhs.segOffset < rhs.segOffset )
            return true;
        if ( lhs.segOffset > rhs.segOffset )
            return false;
        // two fixups at same location

        // if the same (linker bug), ignore one
        if ( lhs.type == rhs.type ) {
            rhs.type = FixupType::ignore;
        }
        // if one is rebase for lazy pointer, ignore rebase because dyld3 does not lazy bind
        else if ( (lhs.type == FixupType::pointerLazyBind) && (rhs.type == FixupType::rebase) ) {
            // lazy pointers have rebase and (lazy) bind at same location.  since dyld3 does not do lazy binding, we mark the rebase to be ignored later
            rhs.type = FixupType::ignore;
        }
        else if ( (rhs.type == FixupType::pointerLazyBind) && (lhs.type == FixupType::rebase) ) {
            // lazy pointers have rebase and (lazy) bind at same location.  since dyld3 does not do lazy binding, we mark the rebase to be ignored later
            lhs.type = FixupType::ignore;
        }
        return (lhs.type < rhs.type);
    });

    // remove ignoreable fixups
    fixups.erase(std::remove_if(fixups.begin(), fixups.end(),
                                [&](const FixUp& a) {
                                    return (a.type == FixupType::ignore);
                                }), fixups.end());

    // look for overlapping fixups
    const uint32_t pointerSize = (_is64 ? 8 : 4);
    const FixUp* lastFixup = nullptr;
    for (const FixUp& fixup : fixups) {
        if ( lastFixup != nullptr ) {
            if ( lastFixup->segIndex == fixup.segIndex ) {
                uint64_t increment = fixup.segOffset - lastFixup->segOffset;
                if ( increment < pointerSize ) {
                    if ( (increment == 0) && ((lastFixup->type == FixupType::ignore) || (fixup.type == FixupType::ignore))  ) {
                        // allow rebase to local lazy helper and lazy bind to same location
                    }
                    else {
                        diag.error("segment %d has overlapping fixups at offset 0x%0llX and 0x%0llX", fixup.segIndex, lastFixup->segOffset, fixup.segOffset);
                        setImageInvalid(imageIndex);
                        return;
                    }
                }
            }
        }
        lastFixup = &fixup;
    }

    if ( hasTextRelocs )
        _diskImages[imageIndex].hasTextRelocs = true;

    // there is one ordinal table per image, shared by all segments with fixups in that image
    std::vector<TargetSymbolValue>  targetsForImage;

    const bool opcodeLogging = false;
    // calculate SegmentFixupsByPage for each segment
    std::vector<SegmentFixUpBuilder*> builders;
    for (uint32_t segIndex=0, onDiskSegIndex=0; segIndex < _diskImages[imageIndex].segmentsArrayCount; ++segIndex) {
        const binary_format::DiskSegment* diskSeg = (const binary_format::DiskSegment*)&(_segmentPool[_diskImages[imageIndex].segmentsArrayStartIndex+segIndex]);
        SegmentFixUpBuilder* builder = nullptr;
        if ( diskSeg->paddingNotSeg )
            continue;
        if ( diskSeg->filePageCount == 0 ) {
            ++onDiskSegIndex;
            continue;
        }
        if ( diskSeg->permissions & VM_PROT_WRITE ) {
            builder = new SegmentFixUpBuilder(onDiskSegIndex, diskSeg->filePageCount, _pageSize, _is64, fixups, targetsForImage, opcodeLogging);
        }
        else if ( hasTextRelocs && (diskSeg->permissions == (VM_PROT_READ|VM_PROT_EXECUTE)) ) {
            builder = new SegmentFixUpBuilder(onDiskSegIndex, diskSeg->filePageCount, _pageSize, _is64, fixups, targetsForImage, opcodeLogging);
        }
        if ( builder != nullptr ) {
            if ( builder->hasFixups() )
                builders.push_back(builder);
            else
                delete builder;
        }
        ++onDiskSegIndex;
    }

    // build AllFixupsBySegment for image
    _fixupsPool.pad_to_size(4);
    uint32_t startOfFixupsOffset = (uint32_t)_fixupsPool.size();
    size_t headerSize = builders.size() * sizeof(binary_format::AllFixupsBySegment);
    size_t offsetOfSegmentHeaderInBuffer = _fixupsPool.size();
    for (int i=0; i < headerSize; ++i) {
        _fixupsPool.append_byte(0);
    }
    uint32_t entryIndex = 0;
    for (SegmentFixUpBuilder* builder : builders) {
        binary_format::AllFixupsBySegment* entries = (binary_format::AllFixupsBySegment*)(_fixupsPool.start()+offsetOfSegmentHeaderInBuffer);
        entries[entryIndex].segIndex = builder->segIndex();
        entries[entryIndex].offset   = (uint32_t)_fixupsPool.size() - startOfFixupsOffset;
        builder->appendSegmentFixUpMap(_fixupsPool);
        delete builder;
        ++entryIndex;
    }
    _diskImages[imageIndex].fixupsPoolOffset   = (uint32_t)offsetOfSegmentHeaderInBuffer;
    _diskImages[imageIndex].fixupsPoolSegCount = entryIndex;

    // append targetsForImage into group
    size_t start = _targetsPool.size();
    size_t count = targetsForImage.size();
    _diskImages[imageIndex].targetsArrayStartIndex = (uint32_t)start;
    _diskImages[imageIndex].targetsArrayCount      = (uint32_t)count;
    assert(_diskImages[imageIndex].targetsArrayStartIndex == start);
    assert(_diskImages[imageIndex].targetsArrayCount      == count);
    _targetsPool.insert(_targetsPool.end(), targetsForImage.begin(), targetsForImage.end());
}


}
}


