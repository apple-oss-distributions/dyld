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


#ifndef LaunchCacheWriter_h
#define LaunchCacheWriter_h


#include <stdint.h>

#include <vector>
#include <list>
#include <unordered_map>
#include <map>

#include "LaunchCacheFormat.h"
#include "LaunchCache.h"
#include "MachOParser.h"
#include "shared-cache/DyldSharedCache.h"


namespace dyld3 {
namespace launch_cache {



class ContentBuffer {
private:
    std::vector<uint8_t>     _data;
public:
    std::vector<uint8_t>&    bytes()                     { return _data; }
    unsigned long            size() const                { return _data.size(); }
    void                     reserve(unsigned long l)    { _data.reserve(l); }
    const uint8_t*           start() const               { return &_data[0]; }
    const uint8_t*           end() const                 { return &_data[_data.size()]; }

    void append_uleb128(uint64_t value) {
        uint8_t byte;
        do {
            byte = value & 0x7F;
            value &= ~0x7F;
            if ( value != 0 )
                byte |= 0x80;
            _data.push_back(byte);
            value = value >> 7;
        } while( byte >= 0x80 );
    }

    void append_byte(uint8_t byte) {
        _data.push_back(byte);
    }
    
    void append_uint32(uint32_t value) {
        for (int i=0; i < 4; ++i) {
            _data.push_back(value & 0xFF);
            value = (value >> 8);
        }
    }
    
    void append_uint64(uint64_t value) {
        for (int i=0; i < 8; ++i) {
            _data.push_back(value & 0xFF);
            value = (value >> 8);
        }
    }

    void append_buffer(const ContentBuffer& value) {
        _data.insert(_data.end(), value.start(), value.end());
    }

    static unsigned int    uleb128_size(uint64_t value) {
        uint32_t result = 0;
        do {
            value = value >> 7;
            ++result;
        } while ( value != 0 );
        return result;
    }
    
    void pad_to_size(unsigned int alignment) {
        while ( (_data.size() % alignment) != 0 )
            _data.push_back(0);
    }
};

class ImageGroupWriter
{
public:
                    ImageGroupWriter(uint32_t groupNum, bool pages16KB, bool is64, bool dylibsExpectedOnDisk, bool mtimeAndInodeAreValid);

    enum class FixupType { rebase, pointerBind, pointerLazyBind, bindText, bindTextRel, rebaseText, bindImportJmpRel, ignore };
    struct FixUp {
        uint32_t             segIndex;
        uint64_t             segOffset;
        FixupType            type;
        TargetSymbolValue    target;
    };

    uint32_t        size() const;
    void            finalizeTo(Diagnostics& diag, const std::vector<const BinaryImageGroupData*>&, binary_format::ImageGroup* buffer) const;
    uint32_t        maxLoadCount(Diagnostics& diag, const std::vector<const BinaryImageGroupData*>&, binary_format::ImageGroup* buffer) const;

    bool            isInvalid(uint32_t imageIndex) const;

    void            setImageCount(uint32_t);
    void            setImageInvalid(uint32_t imageIndex);
    void            setImagePath(uint32_t imageIndex, const char* path);
    void            setImageUUID(uint32_t imageIndex, const uuid_t uuid);
    void            setImageHasObjC(uint32_t imageIndex, bool value);
    void            setImageIsBundle(uint32_t imageIndex, bool value);
    void            setImageHasWeakDefs(uint32_t imageIndex, bool value);
    void            setImageMayHavePlusLoads(uint32_t imageIndex, bool value);
    void            setImageNeverUnload(uint32_t imageIndex, bool);
    void            setImageMustBeThisDir(uint32_t imageIndex, bool value);
    void            setImageIsPlatformBinary(uint32_t imageIndex, bool value);
    void            setImageOverridableDylib(uint32_t imageIndex, bool value);
    void            setImageIsEncrypted(uint32_t imageIndex, bool value);
    void            setImageMaxLoadCount(uint32_t imageIndex, uint32_t count);
    void            setImageFairPlayRange(uint32_t imageIndex, uint32_t offset, uint32_t size);
    void            setImageInitializerOffsets(uint32_t imageIndex, const std::vector<uint32_t>& offsets);
    void            setImageDOFOffsets(uint32_t imageIndex, const std::vector<uint32_t>& offsets);
    void            setImageInitBefore(uint32_t imageIndex, const std::vector<binary_format::ImageRef>&);
    void            setImageSliceOffset(uint32_t imageIndex, uint64_t fileOffset);
    void            setImageFileMtimeAndInode(uint32_t imageIndex, uint64_t mTime, uint64_t inode);
    void            setImageCdHash(uint32_t imageIndex, uint8_t cdHash[20]);
    void            setImageCodeSignatureLocation(uint32_t imageIndex, uint32_t fileOffset, uint32_t size);
    void            setImageDependentsCount(uint32_t imageIndex, uint32_t count);
    void            setImageDependent(uint32_t imageIndex, uint32_t depIndex, binary_format::ImageRef dependent);
    void            setImageSegments(uint32_t imageIndex, MachOParser& imageParser, uint64_t cacheUnslideBaseAddress);
    void            setImageFixups(Diagnostics& diag, uint32_t imageIndex, std::vector<FixUp>& fixups, bool hasTextRelocs);
    void            addImageAliasPath(uint32_t imageIndex, const char* anAlias);
    void            setImagePatchLocations(uint32_t imageIndex, uint32_t funcOffset, const std::unordered_set<uint32_t>& patchLocations);
    void            setGroupCacheOverrides(const std::vector<binary_format::DyldCacheOverride>& cacheOverrides);
    void            addImageIsOverride(binary_format::ImageRef replacer, binary_format::ImageRef replacee);

    uint32_t        addIndirectGroupNum(uint32_t groupNum);

    uint32_t        addString(const char* str);
    void            alignStringPool();

    uint32_t                 imageDependentsCount(uint32_t imageIndex) const;
    binary_format::ImageRef  imageDependent(uint32_t imageIndex, uint32_t depIndex) const;

private:
    struct InitializerInfo {
        std::vector<uint32_t>                   offsetsInImage;
        std::vector<binary_format::ImageRef>    initBeforeImages;
    };

    uint32_t                                    imageCount() const;
    binary_format::Image&                       imageByIndex(uint32_t);
    const binary_format::Image&                 imageByIndex(uint32_t) const;
    std::vector<uint8_t>                        makeFixupOpcodes(const FixUp* start, const FixUp* end, uint32_t pageStartSegmentOffset, std::map<uint32_t, TargetSymbolValue>&);
    void                                        makeDataFixupMapAndOrdinalTable(std::vector<uint8_t>& fixupMap, std::vector<TargetSymbolValue>& ordinalTable);
    void                                        computeInitializerOrdering(uint32_t imageIndex);
    uint32_t                                    addUniqueInitList(const std::vector<binary_format::ImageRef>& initBefore);
    void                                        layoutBinary(binary_format::ImageGroup* grp) const;

    const bool                                   _isDiskImage;
    const bool                                   _is64;
    const uint16_t                               _groupNum;
    const uint32_t                               _pageSize;
    bool                                         _dylibsExpectedOnDisk;
    bool                                         _imageFileInfoIsCdHash;
    std::vector<binary_format::CachedImage>      _images;
    std::vector<binary_format::DiskImage>        _diskImages;
    std::vector<binary_format::AliasEntry>       _aliases;
    std::vector<uint64_t>                        _segmentPool;
    std::vector<binary_format::ImageRef>         _dependentsPool;
    std::vector<uint32_t>                        _initializerOffsets;
    std::vector<binary_format::ImageRef>         _initializerBeforeLists;
    std::vector<uint32_t>                        _dofOffsets;
    std::vector<TargetSymbolValue>               _targetsPool;
    ContentBuffer                                _fixupsPool;
    std::vector<binary_format::PatchTable>       _patchPool;
    std::vector<binary_format::PatchOffset>      _patchLocationPool;
    std::vector<binary_format::DyldCacheOverride>_dyldCacheSymbolOverridePool;
    std::vector<binary_format::ImageRefOverride> _imageOverridePool;
    std::vector<uint32_t>                        _indirectGroupNumPool;
    std::unordered_map<uint32_t, uint32_t>       _indirectGroupNumPoolExisting;
    std::vector<char>                            _stringPool;
    std::unordered_map<std::string, uint32_t>    _stringPoolExisting;
};



} //  namespace launch_cache
} //  namespace dyld3


#endif // LaunchCacheWriter_h


