/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

#include <span>
#include <atomic>
#include <cstring>
#include <Block.h>
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <libproc.h>

#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fsgetpath.h>

#include <mach/mach_time.h> // mach_absolute_time()
#include <mach/mach_vm.h>
#include <mach-o/dyld_priv.h> // FIXME: We can remove this once we fully integrate into dyld4
#include "dyld_cache_format.h"
//FIXME: We should remove this header
#include "dyld_process_info_internal.h" // For dyld_all_image_infos_{32,64}

#include "Defines.h"
#include "Header.h"
#include "DyldSharedCache.h"
#include "PVLEInt64.h"
#include "MachOLoaded.h"
#include "ProcessAtlas.h"
#include "Utilities.h"

#include "CRC32c.h"
#include "UUID.h"
#include "Bitmap.h"

using lsl::CRC32c;
using lsl::Allocator;
using lsl::emitPVLEUInt64;
using lsl::readPVLEUInt64;
using lsl::Bitmap;

using dyld3::FatFile;

using mach_o::Header;
// TODO: forEach shared cache needs to filter out subcaches and skip them

// The allocations made by a snapshot need to last for the life of a spanshot. In libdyld that is under the caller control
// and thus we need to use a persistent or concurrent allocator. Inside of dyld that will be scoped to the the current
// image loading operation, and so we can use an ephtmeral allocatr

#if BUILDING_DYLD
#define _transactionalAllocator _ephemeralAllocator
#else
#define _transactionalAllocator MemoryManager::memoryManager().defaultAllocator()
#endif

#define BLEND_KERN_RETURN_LOCATION(kr, loc) (kr) = ((kr & 0x00ffffff) | loc<<24);

namespace {
static const size_t kCachePeekSize = 0x4000;

static const dyld_cache_header* cacheFilePeek(int fd, uint8_t* firstPage) {
    // sanity check header
    if ( pread(fd, firstPage, kCachePeekSize, 0) != kCachePeekSize ) {
        return nullptr;
    }
    const dyld_cache_header* cache = (dyld_cache_header*)firstPage;
    if ( strncmp(cache->magic, "dyld_v1", strlen("dyld_v1")) != 0 ) {
        return nullptr;
    }
    return cache;
}

static void getCacheInfo(const dyld_cache_header *cache, uint64_t &headerSize, bool& splitCache) {
    // If we have sub caches, then the cache header itself tells us how much space we need to cover all caches
    if ( cache->mappingOffset >= offsetof(dyld_cache_header, subCacheArrayCount) ) {
        // New style cache
        headerSize = cache->subCacheArrayOffset + (sizeof(dyld_subcache_entry)*cache->subCacheArrayCount);
        splitCache = true;
    } else {
        // Old style cache
        headerSize = cache->imagesOffsetOld + (sizeof(dyld_cache_image_info)*cache->imagesCountOld);
        splitCache = false;
    }
}

static void getBaseCachePath(const char* mainPath, char basePathBuffer[]) {
    const char* devExt = dyld4::Utils::strrstr(mainPath, DYLD_SHARED_CACHE_DEVELOPMENT_EXT);
    if ( !devExt ) {
        strcpy(basePathBuffer, mainPath);
        return;
    }
    size_t len = devExt - mainPath;
    strncpy(basePathBuffer, mainPath, len);
    basePathBuffer[len] = '\0';
}
}

namespace dyld4 {
namespace Atlas {

#pragma mark -
#pragma mark Mappers

static
void printMapping(dyld_cache_mapping_and_slide_info* mapping, uint8_t index, uint64_t slide) {
#if 0
    const char* mappingName = "*unknown*";
    if ( mapping->maxProt & VM_PROT_EXECUTE ) {
        mappingName = "__TEXT";
    } else if ( mapping->maxProt & VM_PROT_WRITE ) {
        if ( mapping->flags & DYLD_CACHE_MAPPING_AUTH_DATA ) {
            if ( mapping->flags & DYLD_CACHE_MAPPING_DIRTY_DATA )
                mappingName = "__AUTH_DIRTY";
            else if ( mapping->flags & DYLD_CACHE_MAPPING_CONST_DATA )
                mappingName = "__AUTH_CONST";
            else
                mappingName = "__AUTH";
        } else {
            if ( mapping->flags & DYLD_CACHE_MAPPING_DIRTY_DATA )
                mappingName = "__DATA_DIRTY";
            else if ( mapping->flags & DYLD_CACHE_MAPPING_CONST_DATA )
                mappingName = "__DATA_CONST";
            else
                mappingName = "__DATA";
        }
    }
    else if ( mapping->maxProt & VM_PROT_READ ) {
        mappingName = "__LINKEDIT";
    }

    fprintf(stderr, "%16s %4lluMB,  file offset: #%u/0x%08llX -> 0x%08llX,  address: 0x%08llX -> 0x%08llX\n",
            mappingName, mapping->size / (1024*1024), index, mapping->fileOffset,
            mapping->fileOffset + mapping->size, mapping->address + slide,
            mapping->address + mapping->size + slide);
#endif
}

SharedPtr<Mapper> Mapper::mapperForSharedCache(Allocator& _ephemeralAllocator, FileRecord& file, SafePointer baseAddress) {
    bool        useLocalCache   = false;
    size_t      length          = 0;
    uint64_t    slide     = 0;
    UUID        uuid;

    int fd = open(file.getPath(), O_RDONLY);
    if ( fd == -1 ) {
        return nullptr;
    }
    //TODO: Replace this with a set
    OrderedSet<int> fds(_ephemeralAllocator);
    fds.insert(fd);
    uint8_t firstPage[kCachePeekSize];
    const dyld_cache_header* onDiskCacheHeader = cacheFilePeek(fd, &firstPage[0]);
    if (!onDiskCacheHeader) {
        for (auto deadFd : fds) {
            close(deadFd);
        }
        return nullptr;
    }
    uuid = UUID(&onDiskCacheHeader->uuid[0]);
    if (!onDiskCacheHeader) {
        for (auto deadFd : fds) {
            close(deadFd);
        }
        return nullptr;
    }
    const void* localBaseAddress = _dyld_get_shared_cache_range(&length);
    if (localBaseAddress) {
        auto localCacheHeader = ((dyld_cache_header*)localBaseAddress);
        auto localUUID = UUID(&localCacheHeader->uuid[0]);
        if (localUUID == uuid) {
            useLocalCache = true;
        }
    }
    if (!baseAddress) {
        // No base address passed in, treat as unslid
        baseAddress = onDiskCacheHeader->sharedRegionStart;
    }
    slide = (uint64_t)baseAddress-(uint64_t)onDiskCacheHeader->sharedRegionStart;
    uint64_t headerSize = 0;
    bool splitCache = false;
    getCacheInfo(onDiskCacheHeader, headerSize, splitCache);
    if (splitCache && (onDiskCacheHeader->imagesCount == 0)) {
        //This is a subcache, bail
        for (auto deadFd : fds) {
            close(deadFd);
        }
        return nullptr;
    }
    void* mapping = mmap(nullptr, (size_t)headerSize, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) {
        for (auto deadFd : fds) {
            close(deadFd);
        }
        return nullptr;
    }
    auto onDiskHeaderBytes = (uint8_t*)mapping;
    auto onDiskCacheMappings = (dyld_cache_mapping_and_slide_info*)&onDiskHeaderBytes[onDiskCacheHeader->mappingWithSlideOffset];
    Vector<Mapper::Mapping> mappings(_transactionalAllocator);
    for (auto i = 0; i < onDiskCacheHeader->mappingWithSlideCount; ++i) {
        if (useLocalCache && ((onDiskCacheMappings[i].maxProt & VM_PROT_WRITE) != VM_PROT_WRITE)) {
            // This region is immutable, use in memory version
            printMapping(&onDiskCacheMappings[i], 255, slide);
            mappings.emplace_back((Mapper::Mapping){
                .address    = onDiskCacheMappings[i].address + slide,
                .size       = onDiskCacheMappings[i].size,
                // No file, just use the address
                .offset     = onDiskCacheMappings[i].address - onDiskCacheHeader->sharedRegionStart + (uint64_t)localBaseAddress,
                .fd         = -1
            });
        } else {
            printMapping(&onDiskCacheMappings[i], fd, slide);
            mappings.emplace_back((Mapper::Mapping){
                .address    = onDiskCacheMappings[i].address + slide,
                .size       = onDiskCacheMappings[i].size,
                .offset     = onDiskCacheMappings[i].fileOffset,
                .fd         = fd
            });
        }
    }
    if (splitCache) {
        auto subCaches = (dyld_subcache_entry*)&onDiskHeaderBytes[onDiskCacheHeader->subCacheArrayOffset];
        for (auto i = 0; i < onDiskCacheHeader->subCacheArrayCount; ++i) {
            char subCachePath[PATH_MAX];
            if (onDiskCacheHeader->mappingOffset <= offsetof(dyld_cache_header, cacheSubType)) {
                snprintf(&subCachePath[0], sizeof(subCachePath), "%s.%u", file.getPath(), i+1);
            } else {
                char basePath[PATH_MAX];
                getBaseCachePath(file.getPath(), basePath);
                snprintf(&subCachePath[0], sizeof(subCachePath), "%s%s", basePath, subCaches[i].fileSuffix);
            }
            fd = open(subCachePath, O_RDONLY);
            fds.insert(fd);
            if ( fd == -1 ) {
                break;
            }
            // TODO: We should check we have enough space, but for now just allocate a page
            uint8_t firstSubPage[kCachePeekSize];
            const dyld_cache_header* subCache = cacheFilePeek(fd, &firstSubPage[0]);
            if (!subCache) {
                for (auto deadFd : fds) {
                    close(deadFd);
                }
                continue;
            }
            auto subCacheheaderBytes = (uint8_t*)subCache;
            auto subCacheMappings = (dyld_cache_mapping_and_slide_info*)&subCacheheaderBytes[subCache->mappingWithSlideOffset];

            auto onDiskSubcacheUUID = UUID(subCache->uuid);
            uint8_t uuidBuf[16];
            if (onDiskCacheHeader->mappingOffset <= offsetof(dyld_cache_header, cacheSubType))  {
                auto subCacheArray = (dyld_subcache_entry_v1*)&onDiskHeaderBytes[onDiskCacheHeader->subCacheArrayOffset];
                memcpy(uuidBuf, subCacheArray[i].uuid, 16);
            } else {
                auto subCacheArray = (dyld_subcache_entry*)&onDiskHeaderBytes[onDiskCacheHeader->subCacheArrayOffset];
                memcpy(uuidBuf, subCacheArray[i].uuid, 16);
            }
            auto subcacheUUID = UUID(uuidBuf);
            if (subcacheUUID != onDiskSubcacheUUID) {
                for (auto deadFd : fds) {
                    close(deadFd);
                }
                return nullptr;
            }

            for (auto j = 0; j < subCache->mappingWithSlideCount; ++j) {
                if (useLocalCache && ((subCacheMappings[j].maxProt & VM_PROT_WRITE) != VM_PROT_WRITE)) {
                    // This region is immutable, use in memory version
                    printMapping(&subCacheMappings[j], 255, slide);;
                    mappings.emplace_back((Mapper::Mapping){
                        .address    = subCacheMappings[j].address + slide,
                        .size       = subCacheMappings[j].size,
                        // No file, just use the address
                        .offset     = subCacheMappings[j].address - onDiskCacheHeader->sharedRegionStart + (uint64_t)localBaseAddress,
                        .fd         = -1
                    });
                } else {
                    printMapping(&subCacheMappings[j], fd, slide);
                    mappings.emplace_back((Mapper::Mapping){
                        .address    = subCacheMappings[j].address + slide,
                        .size       = subCacheMappings[j].size,
                        .offset     = subCacheMappings[j].fileOffset,
                        .fd         = fd
                    });
                }
            }
        }
    }
    for (auto activeMapping : mappings) {
        fds.erase(activeMapping.fd);
    }
    for (auto deadFd : fds) {
        close(deadFd);
    }
    munmap(mapping,(size_t)headerSize);
    return _transactionalAllocator.makeShared<Mapper>(_transactionalAllocator, mappings);
}


std::pair<SharedPtr<Mapper>,uint64_t> Mapper::mapperForSharedCacheLocals(Allocator& _ephemeralAllocator, FileRecord& file) {
    int fd = file.open(O_RDONLY);
    if (fd == -1) {
        return { SharedPtr<Mapper>(), 0};
    }
    size_t fileSize = file.size();
    if (fd == 0) {
        return { SharedPtr<Mapper>(), 0};
    }
    // sanity check header
    uint8_t firstPage[kCachePeekSize];
    const dyld_cache_header* cache = cacheFilePeek(fd, &firstPage[0]);
    if (!cache) {
        file.close();
        return { SharedPtr<Mapper>(), 0};
    }
    uint64_t baseAddress = 0;

    // We want the cache header, which is at the start of the, and the locals, which are at the end.
    // Just map the whole file as a single range, as we need file offsets in the mappings anyway
    // With split caches, this is more reasonable as the locals are in their own file, so we want more or
    // less the whole file anyway, and there's no wasted space for __TEXT, __DATA, etc.
//    fprintf(stderr, "Mapping\n");
//    fprintf(stderr, "fd\tAddress\tFile Offset\tSize\n");
//    fprintf(stderr, "%u\t0x%llx\t0x%x\t%llu\n", fd, baseAddress, 0, (uint64_t)statbuf.st_size);
    Vector<Mapper::Mapping> mappings(_transactionalAllocator);
    mappings.emplace_back((Mapper::Mapping){
        .address = baseAddress,
        .size = fileSize,
        .offset = 0,
        .fd = fd
    });
    return { _transactionalAllocator.makeShared<Mapper>(_transactionalAllocator, mappings), baseAddress };
}

SharedPtr<Mapper> Mapper::mapperForMachO(Allocator& _ephemeralAllocator, FileRecord& file, const UUID& uuid, const SafePointer baseAddress) {
    const char* filePath = file.getPath();
    // open filePath
    int fd = dyld3::open(filePath, O_RDONLY, 0);
    if ( fd == -1 ) {
        ::close(fd);
        return nullptr;
    }
    // get file size of filePath
    struct stat sb;
    if ( fstat(fd, &sb) == -1 ) {
        ::close(fd);
        return nullptr;
    }

    // mmap whole file temporarily
    void* tempMapping = ::mmap(nullptr, (size_t)sb.st_size, PROT_READ, MAP_FILE | MAP_PRIVATE | MAP_RESILIENT_CODESIGN, fd, 0);
    if ( tempMapping == MAP_FAILED ) {
        ::close(fd);
        return nullptr;
    }

    // if fat file, pick matching slice
    __block const Header*    mf         = nullptr;
    const FatFile*              ff         = FatFile::isFatFile(tempMapping);
    __block uint64_t            fileOffset = 0;
    if (ff) {
        uint64_t                fileLength = sb.st_size;
        Diagnostics             diag;
        ff->forEachSlice(diag, fileLength, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void *sliceStart, uint64_t sliceSize, bool &stop) {
            auto slice = (Header*)sliceStart;
            uuid_t sliceUUIDRaw;
            slice->getUuid(sliceUUIDRaw);
            auto sliceUUID = UUID(sliceUUIDRaw);
            if (uuid == sliceUUID) {
                mf = slice;
                fileOffset = (uint64_t)sliceStart - (uint64_t)ff;
                stop = true;
                return;
            }
        });
        diag.clearError();
    }
    if (!mf) {
        auto slice =  Header::isMachO({(uint8_t*)tempMapping, (size_t)sb.st_size});
        if (slice) {
            uuid_t sliceUUID;
            slice->getUuid(sliceUUID);
            if (uuid == UUID(sliceUUID)) {
                mf = slice;
            }
        }
    }
    if (!mf) {
        ::munmap(tempMapping, (size_t)sb.st_size);
        ::close(fd);
        return nullptr;
    }
    __block Vector<Mapper::Mapping> mappings(_transactionalAllocator);
    __block uint64_t slide = 0;
    mf->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
        if ( info.segmentName == "__TEXT" ) {
            slide = (uint64_t)baseAddress - info.vmaddr;
        }
//        fprintf(stderr, "Mapping\n");
//        fprintf(stderr, "fd\tAddress\tFile Offset\tSize\n");
//        fprintf(stderr, "%u\t0x%llx\t0x%llx\t%llu\n", fd, info.vmAddr + slide, info.fileOffset + fileOffset, info.vmSize);
        mappings.emplace_back((Mapper::Mapping) {
            .address = info.vmaddr + slide,
            .size = info.vmsize,
            .offset = info.fileOffset + fileOffset,
            .fd = fd
        });
    });
    ::munmap(tempMapping, (size_t)sb.st_size);
    return _transactionalAllocator.makeShared<Mapper>(_transactionalAllocator, mappings);
}

Mapper::Mapper(Allocator& allocator) : _mappings(allocator), _flatMapping(nullptr), _allocator(allocator) {}
Mapper::Mapper(Allocator& allocator, const Vector<Mapping>& mapper)
    : _mappings(mapper, allocator), _flatMapping(nullptr), _allocator(allocator) {}

Mapper::~Mapper() {
    assert(_flatMapping == nullptr);
    //TODO: Replace this with a set
    Vector<int> fds(_allocator);
    for (auto& mapping : _mappings) {
        if (mapping.fd == -1) { continue; }
        if (std::find(fds.begin(), fds.end(), mapping.fd) == fds.end()) {
            fds.push_back(mapping.fd);
        }
    }
    for (auto& fd : fds) {
        close(fd);
    }
}

std::pair<SafePointer,bool> Mapper::map(const SafePointer addr, uint64_t size) const {
    if (_flatMapping) {
        uint64_t offset = (uint64_t)addr-(uint64_t)baseAddress();
        return {(uint64_t)((uintptr_t)_flatMapping+offset),false};
    }
    if (_mappings.size() == 0) {
        // No mappings means we are an identity mapper
        return { addr, false };
    }
    for (const auto& mapping : _mappings) {
        if (((uint64_t)addr >= mapping.address) && ((uint64_t)addr < (mapping.address + mapping.size))) {
            if (mapping.fd == -1) {
                return {(((uint64_t)addr-mapping.address)+mapping.offset), false};
            }
            assert(((uint64_t)addr + size) <= mapping.address + mapping.size);
            uint64_t offset = (uint64_t)addr - mapping.address + mapping.offset;
            // Handle unaligned mmap
            void* newMapping = nullptr;
            size_t extraBytes = 0;
            uint64_t roundedOffset = offset & (-1*PAGE_SIZE);
            extraBytes = (size_t)offset - (size_t)roundedOffset;
            newMapping = mmap(nullptr, (size_t)size+extraBytes, PROT_READ, MAP_FILE | MAP_PRIVATE, mapping.fd, roundedOffset);
            if (newMapping == MAP_FAILED) {
//                fprintf(stderr, "mmap failed: %s (%d)\n", strerror(errno), errno);
                return { SafePointer(), false};
            }
            return {(uint64_t)((uintptr_t)newMapping+extraBytes),true};
        }
    }
    return {SafePointer(), false};
}

void Mapper::unmap(const SafePointer addr, uint64_t size) const {
    void* roundedAddr = (void*)((intptr_t)(uint64_t)addr & (-1*PAGE_SIZE));
    size_t extraBytes = (uintptr_t)(uint64_t)addr - (uintptr_t)roundedAddr;
    munmap(roundedAddr, (size_t)size+extraBytes);
}

const SafePointer Mapper::baseAddress() const {
    return _mappings[0].address;
}

const uint64_t Mapper::size() const {
    return (_mappings.back().address - _mappings[0].address) + _mappings.back().size;
}

bool Mapper::pin() {
    assert(_flatMapping == nullptr);
    //TODO: Move onto dyld allocators once we merge the large allocations support
    if (vm_allocate(mach_task_self(), (vm_address_t*)&_flatMapping, (vm_size_t)size(), VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        return false;
    }
    for (const auto& mapping : _mappings) {
        uint64_t destAddr = (mapping.address - _mappings[0].address) + (uint64_t)_flatMapping;
        if (mapping.fd == -1) {
            if (vm_copy(mach_task_self(), (vm_address_t)mapping.address, (vm_size_t)mapping.size, (vm_address_t)destAddr) != KERN_SUCCESS) {
                unpin();
                return false;
            }
        } else {
            if (mmap((void*)destAddr, (vm_size_t)mapping.size, PROT_READ, MAP_FILE | MAP_PRIVATE | MAP_FIXED, mapping.fd, mapping.offset) == MAP_FAILED) {
                unpin();
                return false;
            }
        }
    }
    return true;
}
void Mapper::unpin() {
    assert(_flatMapping != nullptr);
    vm_deallocate(mach_task_self(), (vm_address_t)_flatMapping, (vm_size_t)size());
    _flatMapping = nullptr;
}

void Mapper::dump() const {
    fprintf(stderr, "fd\tAddress\tSize\n");
    
    for (const auto& mapping : _mappings) {
        fprintf(stderr, "%d\t0x%llx\t%llu\n", mapping.fd, mapping.address, mapping.size);
    }
}

#pragma mark -
#pragma mark Image

#if BUILDING_DYLD
Image::Image(RuntimeState* state, Allocator& ephemeralAllocator, SharedPtr<Mapper>& mapper, const Loader* ldr)
    :   _ephemeralAllocator(ephemeralAllocator), _mapper(mapper), _rebasedAddress((uint64_t)ldr->loadAddress(*state)) {
        auto fileID = ldr->fileID(*state);
        if (fileID.inode() &&  fileID.device()) {
            _file = state->fileManager.fileRecordForFileID(ldr->fileID(*state));
            if ( _file.volume().empty() ) {
                _file = state->fileManager.fileRecordForPath(ephemeralAllocator, ldr->path(*state));
            }
        } else {
            _file = state->fileManager.fileRecordForPath(ephemeralAllocator, ldr->path(*state));
        }
    }
#endif
Image::Image(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& mapper, const SafePointer mh)
    : _ephemeralAllocator(ephemeralAllocator), _file(std::move(file)), _mapper(mapper), _rebasedAddress(mh) {}
Image::Image(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& mapper, const SafePointer mh, const UUID& uuid)
    :  _ephemeralAllocator(ephemeralAllocator), _file(std::move(file)), _mapper(mapper), _uuid(uuid), _rebasedAddress(mh) {}

Image::Image(Allocator& ephemeralAllocator, SharedPtr<Mapper>& mapper, SafePointer baseAddress, uint64_t cacheSlide, SharedCache* sharedCache)
    : _ephemeralAllocator(ephemeralAllocator), _mapper(mapper), _sharedCacheSlide(cacheSlide), _rebasedAddress(((uint64_t)baseAddress+cacheSlide)), _sharedCache(sharedCache) {}

Image::Image(Image&& other) : _ephemeralAllocator(other._ephemeralAllocator) {
    swap(other);
}

Image& Image::operator=(Image&& other) {
    swap(other);
    return *this;
}

std::strong_ordering Image::operator<=>(const Image& other) const {
    return (rebasedAddress() <=> other.rebasedAddress());
}

void Image::swap(Image& other) {
    using std::swap;

    if (this == &other) { return; }
    swap(_uuid,                 other._uuid);
    swap(_ml,                   other._ml);
    swap(_sharedCacheSlide,     other._sharedCacheSlide);
    swap(_rebasedAddress,       other._rebasedAddress);
    swap(_mapper,               other._mapper);
    swap(_sharedCache,          other._sharedCache);
    swap(_installname,          other._installname);
    swap(_file,                 other._file);
    swap(_uuidLoaded,           other._uuidLoaded);
    swap(_installnameLoaded,    other._installnameLoaded);
    swap(_mapperFailed,         other._mapperFailed);
}

const MachOLoaded* Image::ml() const {
    if (_mapperFailed) {
        return nullptr;
    }
    if (!_ml) {
        SafePointer slidML = rebasedAddress();
        // Note, using 4k here as we might be an arm64e process inspecting an x86_64 image, which uses 4k pages
        if (!_mapper && !_mapperFailed) {
            _mapper = Mapper::mapperForMachO(_transactionalAllocator, _file, _uuid, _rebasedAddress);
        }
        if (!_mapper) {
            _mapperFailed = true;
            return nullptr;
        }
        _ml = _mapper->map<MachOLoaded>(slidML, 4096);
        if (!_ml) {
           _mapperFailed = true;
           return nullptr;
        }
        size_t size = _ml->sizeofcmds;
        if ( _ml->magic == MH_MAGIC_64 ) {
            size += sizeof(mach_header_64);
        } else {
            size += sizeof(mach_header);
        }
        if (size > 4096) {
            _ml = _mapper->map<MachOLoaded>(slidML, size);
            if (!_ml) {
               _mapperFailed = true;
               return nullptr;
            }
        }
    }
    // This is a bit of a mess. With compact info this will be unified, but for now we use a lot of hacky abstactions here to deal with
    // in process / vs out of process / vs shared cache.
    return &*_ml;
}

const UUID& Image::uuid() const {
    if (!_uuidLoaded) {
        uuid_t fileUUID;
        const Header* mh = (Header*)ml();
        if (mh && mh->hasMachOMagic()) {
            if (mh->getUuid(fileUUID))
                _uuid = UUID(fileUUID);
        }
        _uuidLoaded = true;
    }
    return _uuid;
}

SafePointer Image::rebasedAddress() const {
    return (uint64_t)_rebasedAddress;
}


const char* Image::installname() const {
    if (!_installnameLoaded) {
        if (ml()) {
            _installname = ((const Header*)ml())->installName();
        }
        _installnameLoaded = true;
    }
    return _installname;
}
const char* Image::filename() const {
    if (_sharedCache) { return nullptr; }
    return _file.getPath();
}

const FileRecord& Image::file() const {
    return _file;
}

const SharedCache* Image::sharedCache() const {
    return _sharedCache;
}

uint64_t Image::sharedCacheVMOffset() const {
    return (uint64_t)_rebasedAddress - (uint64_t)sharedCache()->rebasedAddress();
}

uint32_t Image::pointerSize() {
    if (!ml()) { return 0; }
    return ml()->pointerSize();
}

bool Image::forEachSegment(void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm)) {
    if (!ml()) { return false; }
    __block uint64_t slide = (uint64_t)_rebasedAddress - ((const Header*)ml())->preferredLoadAddress();
    ((const Header*)ml())->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
        uint64_t vmAddr = 0x0;
        if ( _sharedCacheSlide.has_value() ) {
            vmAddr = info.vmaddr + _sharedCacheSlide.value();
        } else {
            if ( ml()->isMainExecutable() ) {
                if ( info.segmentName.starts_with("__PAGEZERO") )
                    return;
            }
            vmAddr = info.vmaddr + slide;
        }
        block(info.segmentName.data(), vmAddr, info.vmsize, info.initProt);
    });
    return true;
}

bool Image::forEachSection(void (^block)(const char* segmentName, const char* sectionName, uint64_t vmAddr, uint64_t vmSize)) {
    if (!ml()) { return false; }
    __block uint64_t slide = (uint64_t)_rebasedAddress - ((const Header*)ml())->preferredLoadAddress();
    ((const Header*)ml())->forEachSection(^(const Header::SectionInfo &info, bool &stop) {
        uint64_t sectAddr = 0x0;
        if ( _sharedCacheSlide.has_value() ) {
            sectAddr = info.address + _sharedCacheSlide.value();
        } else {
            sectAddr = info.address + slide;
        }
        block(info.segmentName.data(), info.sectionName.data(), sectAddr, info.size);
    });
    return true;
}

bool Image::contentForSegment(const char* segmentName, void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
    if (!ml()) { return false; }
    __block bool result = false;
    __block uint64_t slide = (uint64_t)_rebasedAddress - ((const Header*)ml())->preferredLoadAddress();
    ((const Header*)ml())->forEachSegment(^(const Header::SegmentInfo &info, bool &stop) {
        if ( segmentName != info.segmentName ) { return; }
        uint64_t vmAddr = 0;
        if ( _sharedCacheSlide.has_value() ) {
            vmAddr = info.vmaddr + _sharedCacheSlide.value();
        } else {
            if ( ml()->isMainExecutable() ) {
                if ( info.segmentName.starts_with("__PAGEZERO") )
                    return;
            }
            vmAddr = info.vmaddr + slide;
        }

        if (info.vmsize) {
            auto content = _mapper->map<uint8_t>(vmAddr, info.vmsize);
            contentReader((void*)&*content, vmAddr, info.vmsize);
        } else {
            contentReader(nullptr, vmAddr, 0);
        }
        result = true;
        stop = true;
    });
    return result;
}

bool Image::contentForSection(const char* segmentName, const char* sectionName,
                              void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize)) {
    __block bool result = false;
    __block uint64_t slide = (uint64_t)_rebasedAddress - ((const Header*)ml())->preferredLoadAddress();
    ((const Header*)ml())->forEachSection(^(const Header::SectionInfo &info, bool &stop) {
        if ( segmentName != info.segmentName ) { return; }
        if ( sectionName != info.sectionName ) { return; }
        uint64_t sectAddr = 0;
        if ( _sharedCacheSlide.has_value() ) {
            sectAddr = info.address + _sharedCacheSlide.value();
        } else {
            sectAddr = info.address + slide;
        }
        if (info.size) {
            auto content = _mapper->map<uint8_t>(sectAddr, info.size);
            contentReader((void*)&*content, sectAddr, info.size);
        } else {
            contentReader(nullptr, sectAddr, 0);
        }
        result = true;
        stop = true;
    });
    return result;
}

#pragma mark -
#pragma mark Shared Cache Locals

SharedCacheLocals::SharedCacheLocals(SharedPtr<Mapper>& M, bool use64BitDylibOffsets)
    : _mapper(M), _use64BitDylibOffsets(use64BitDylibOffsets) {
    auto header = _mapper->map<dyld_cache_header>(0ULL, sizeof(dyld_cache_header));

    // Map in the whole locals buffer.
    // TODO: Once we have the symbols in their own file, simplify this to just map the whole file
    // and not do the header and locals separately
    _locals = _mapper->map<uint8_t>(header->localSymbolsOffset, header->localSymbolsSize);
}

const dyld_cache_local_symbols_info* SharedCacheLocals::localInfo() const {
    return (const dyld_cache_local_symbols_info*)(&*_locals);
}

bool SharedCacheLocals::use64BitDylibOffsets() const {
    return _use64BitDylibOffsets;
}

#pragma mark -
#pragma mark Shared Cache

// Copied from DyldSharedCache::mappedSize()
static uint64_t cacheMappedSize(Mapper::Pointer<dyld_cache_header>& header, SharedPtr<Mapper>& mapper,
                                uint64_t rebasedAddress, bool splitCache)
{
    // If we have sub caches, then the cache header itself tells us how much space we need to cover all caches
    if (header->mappingOffset >= offsetof(dyld_cache_header, subCacheArrayCount) ) {
        return header->sharedRegionSize;
    } else {
        auto headerBytes = (uint8_t*)&*header;
        auto mappings = (dyld_cache_mapping_and_slide_info*)&headerBytes[header->mappingWithSlideOffset];
        uint64_t endAddress = 0;
        for (auto i = 0; i < header->mappingWithSlideCount; ++i) {
            if (endAddress < mappings[i].address + mappings[i].size) {
                endAddress = mappings[i].address + mappings[i].size;
            }
        }
        if (splitCache) {
            for (auto i = 0; i < header->subCacheArrayCount; ++i) {
                uint64_t subCacheOffset = 0;
                if (header->mappingOffset <= offsetof(dyld_cache_header, cacheSubType) ) {
                    auto subCaches = (dyld_subcache_entry_v1*)&headerBytes[header->subCacheArrayOffset];
                    subCacheOffset = subCaches[i].cacheVMOffset;
                } else {
                    auto subCaches = (dyld_subcache_entry*)&headerBytes[header->subCacheArrayOffset];
                    subCacheOffset = subCaches[i].cacheVMOffset;
                }
                auto subCacheHeader = mapper->map<dyld_cache_header>(subCacheOffset + rebasedAddress, PAGE_SIZE);
                uint64_t subCacheHeaderSize = 0;
                bool splitCacheUnused;
                getCacheInfo(&*subCacheHeader, subCacheHeaderSize, splitCacheUnused);
                if (subCacheHeaderSize > PAGE_SIZE) {
                    subCacheHeader = mapper->map<dyld_cache_header>(subCacheOffset + rebasedAddress, subCacheHeaderSize);
                }
                auto subCacheHeaderBytes = (uint8_t*)&*subCacheHeader;
                auto subCacheMappings = (dyld_cache_mapping_and_slide_info*)&subCacheHeaderBytes[subCacheHeader->mappingWithSlideOffset];
                for (auto j = 0; j < subCacheHeader->mappingWithSlideCount; ++j) {
                    if (endAddress < subCacheMappings[j].address + subCacheMappings[j].size) {
                        endAddress = subCacheMappings[j].address + subCacheMappings[j].size;
                    }
                }
            }
        }
        return endAddress - header->sharedRegionStart;
    }
}

SharedCache::SharedCache(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& mapper, SafePointer rebasedAddress, bool P)
    : _ephemeralAllocator(ephemeralAllocator), _file(std::move(file)), _mapper(mapper),  _rebasedAddress(rebasedAddress), _private(P)
{
    assert(_mapper);
    _header = _mapper->map<dyld_cache_header>(_rebasedAddress, PAGE_SIZE);
    uint64_t headerSize = 0;
    bool splitCache = false;
    getCacheInfo(&*_header, headerSize, splitCache);
    if (headerSize > PAGE_SIZE) {
        _header = _mapper->map<dyld_cache_header>(_rebasedAddress, headerSize);
    }
    _uuid = UUID(&_header->uuid[0]);
    _slide = (uint64_t)_rebasedAddress -  _header->sharedRegionStart;
    _size = cacheMappedSize(_header, _mapper, (uint64_t)rebasedAddress, splitCache);
}


void SharedCache::forEachInstalledCacheWithSystemPath(Allocator& _ephemeralAllocator, FileManager& fileManager, const char* systemPath, void (^block)(SharedCache* cache)) {
    // TODO: We can make this more resilient by encoding all the paths in a special section /usr/lib/dyld, and then parsing them out
    // Search all paths we might find shared caches at for any OS in the last 2+ years
    static const char* cacheDirPaths[] = {
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/",
        "/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
        "/private/preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/",
        "/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
        "/System/Cryptexes/OS/System/Library/dyld/",
        "/System/Cryptexes/ExclaveOS/System/ExclaveKit/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
        "/private/preboot/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
        "/private/preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
        "/System/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
        "/System/Cryptexes/Incoming/OS/System/Library/dyld/",
        "/System/Library/Caches/com.apple.dyld/",
        "/System/DriverKit/System/Library/dyld/",
        "/System/ExclaveKit/System/Library/dyld/",
        "/System/Library/dyld/"
    };

    OrderedSet<const char*, lsl::ConstCharStarCompare> realPaths(_ephemeralAllocator);
    for ( int i = 0; i < sizeof(cacheDirPaths)/sizeof(char*); i++ ) {
        char systemCacheDirPath[PATH_MAX];
        systemCacheDirPath[0] = 0;
        if ( systemPath != nullptr ) {
            if ( Utils::concatenatePaths(systemCacheDirPath, systemPath, PATH_MAX) >= PATH_MAX )
                continue;
        }
        if ( Utils::concatenatePaths(systemCacheDirPath, cacheDirPaths[i], PATH_MAX) >= PATH_MAX )
            continue;

        char systemCacheDirRealPath[PATH_MAX];
        systemCacheDirRealPath[0] = 0;
        if ( realpath(systemCacheDirPath, systemCacheDirRealPath) == nullptr )
            continue;
        if ( Utils::concatenatePaths(systemCacheDirRealPath, "/", PATH_MAX) >= PATH_MAX )
            continue;

        const char* systemDirDup = _ephemeralAllocator.strdup(systemCacheDirRealPath);
        auto insertRes = realPaths.insert(systemDirDup);
        if ( !insertRes.second ) {
            _ephemeralAllocator.free((void*)systemDirDup);
            continue;
        }

        DIR* dirp = ::opendir(systemCacheDirRealPath);
        if ( dirp != NULL) {
            dirent entry;
            dirent* entp = NULL;
            char cachePath[PATH_MAX];
            cachePath[0] = 0;
            while ( ::readdir_r(dirp, &entry, &entp) == 0 ) {
                if ( entp == NULL )
                    break;
                if ( entp->d_type != DT_REG )
                    continue;
                const char* leafName = entp->d_name;
                if ( DyldSharedCache::isSubCachePath(leafName) )
                    continue;
                if ( strlcpy(cachePath, systemCacheDirRealPath, PATH_MAX) >= PATH_MAX )
                    continue;
                if ( Utils::concatenatePaths(cachePath, entp->d_name, PATH_MAX) >= PATH_MAX )
                    continue;

                // FIXME: The memory managemnt here is awful, fix with allocators
                auto cacheFile = fileManager.fileRecordForPath(_ephemeralAllocator, cachePath);
                auto cache = Atlas::SharedCache::createForFileRecord(_ephemeralAllocator, std::move(cacheFile));
                if (cache) {
                    cache.withUnsafe([&](auto cachePtr){
                        block(cachePtr);
                    });
                }
            }
            closedir(dirp);
        }
    }
    for (auto path : realPaths) {
        _ephemeralAllocator.free((void*)path);
    }
}

UniquePtr<SharedCache> SharedCache::createForFileRecord(Allocator& _ephemeralAllocator, FileRecord&& file) {
    auto uuid = UUID();
    auto fileMapper = Mapper::mapperForSharedCache(_ephemeralAllocator, file, 0ULL);
    if (!fileMapper) { return nullptr; }
    return _transactionalAllocator.makeUnique<SharedCache>(_ephemeralAllocator, std::move(file), fileMapper, (uint64_t)fileMapper->baseAddress(), true);
}


const UUID& SharedCache::uuid() const {
    return _uuid;
}

SafePointer SharedCache::rebasedAddress() const {
    return _rebasedAddress;
}

uint64_t SharedCache::size() const {
    return _size;
}

const FileRecord& SharedCache::file() const {
    return _file;
}


void SharedCache::forEachFilePath(void (^block)(const char* file_path)) const {
    block(_file.getPath());

    uint64_t headerSize = 0;
    bool splitCache = false;
    getCacheInfo(&*_header, headerSize, splitCache);

    if (splitCache) {
        auto headerBytes = (std::byte*)&*_header;
        char subCachePath[PATH_MAX];
        if (_header->mappingOffset <= offsetof(dyld_cache_header, cacheSubType)) {
            for (auto i = 0; i < _header->subCacheArrayCount; ++i) {
                snprintf(&subCachePath[0], sizeof(subCachePath), "%s.%u", _file.getPath(), i+1);
                block(subCachePath);
            }
        } else {
            auto subCacheArray = (dyld_subcache_entry*)&headerBytes[_header->subCacheArrayOffset];
            for (auto i = 0; i < _header->subCacheArrayCount; ++i) {
                snprintf(&subCachePath[0], sizeof(subCachePath), "%s%s", _file.getPath(), subCacheArray[i].fileSuffix);
                block(subCachePath);
            }
        }
        if ( (_header->mappingOffset >= offsetof(dyld_cache_header, symbolFileUUID)) && !uuid_is_null(_header->symbolFileUUID) ) {
            strlcpy(&subCachePath[0], _file.getPath(), PATH_MAX);
            // On new caches, the locals come from a new subCache file
            if (strstr(subCachePath, ".development") != nullptr) {
                subCachePath[strlen(subCachePath)-(strlen(".development"))] = 0;
            }
            strlcat(subCachePath, ".symbols", PATH_MAX);
            block(subCachePath);
        }
    }
}

bool SharedCache::isPrivateMapped() const {
    return _private;
}

size_t SharedCache::imageCount() const {
    return (size_t)_header->imagesTextCount;
}

void SharedCache::forEachImage(void (^block)(Image* image)) {
    auto headerBytes = (uint8_t*)&*_header;
    auto images = std::span((const dyld_cache_image_text_info*)&headerBytes[_header->imagesTextOffset], (size_t)_header->imagesTextCount);
    for (auto i : images) {
        auto image = Image(_ephemeralAllocator, _mapper, i.loadAddress, _slide, this);
        block(&image);
    }
}

void SharedCache::withImageForIndex(uint32_t idx, void (^block)(Image* image)) {
    auto headerBytes = (uint8_t*)&*_header;
    auto images = std::span((const dyld_cache_image_text_info*)&headerBytes[_header->imagesTextOffset], (size_t)_header->imagesTextCount);
    auto image = Image(_ephemeralAllocator, _mapper, images[idx].loadAddress, _slide, this);
    block(&image);
}

// Maps the local symbols for this shared cache.
// Locals are in an unmapped part of the file, so we have to map then in separately
UniquePtr<SharedCacheLocals> SharedCache::localSymbols() const {
    // The locals might be in their own locals file, or in the main cache file.
    // Where it is depends on the cache header
    char localSymbolsCachePath[PATH_MAX];
    strlcpy(&localSymbolsCachePath[0], _file.getPath(), PATH_MAX);
    bool useSymbolsFile = (_header->mappingOffset >= offsetof(dyld_cache_header, symbolFileUUID));
    if ( useSymbolsFile ) {
        if ( uuid_is_null(_header->symbolFileUUID) )
            return nullptr;

        // On new caches, the locals come from a new subCache file
        if (strstr(localSymbolsCachePath, DYLD_SHARED_CACHE_DEVELOPMENT_EXT ) != nullptr) {
            localSymbolsCachePath[strlen(localSymbolsCachePath)-(strlen(DYLD_SHARED_CACHE_DEVELOPMENT_EXT ))] = 0;
        }
        strlcat(localSymbolsCachePath, ".symbols", PATH_MAX);
    } else {
        if ( (_header->localSymbolsSize == 0) || (_header->localSymbolsOffset == 0) )
            return nullptr;
    }
    // TODO: Create Path extension helpers for FileRecord
    auto localSymbolsCacheFile = _file.fileManager().fileRecordForPath(_ephemeralAllocator, localSymbolsCachePath);
    auto [fileMapper, baseAddress] = Mapper::mapperForSharedCacheLocals(_ephemeralAllocator, localSymbolsCacheFile);
    if (!fileMapper) { return nullptr; }
    // Use placement new since operator new is not available
    return _transactionalAllocator.makeUnique<SharedCacheLocals>(fileMapper, useSymbolsFile);
}

bool SharedCache::pin() {
    return _mapper->pin();
}

void SharedCache::unpin() {
    return _mapper->unpin();
}

#ifdef TARGET_OS_OSX
bool SharedCache::mapSubCacheAndInvokeBlock(const dyld_cache_header* subCacheHeader,
                                            void (^block)(const void* cacheBuffer, size_t size)) {
    bool result = true;
    auto subCacheHeaderBytes = (uint8_t*)subCacheHeader;
    uint64_t fileSize = 0;
    for(auto i = 0; i < subCacheHeader->mappingCount; ++i) {
        auto mapping = (dyld_cache_mapping_info*)&subCacheHeaderBytes[subCacheHeader->mappingOffset+(i*sizeof(dyld_cache_mapping_info))];
        uint64_t regionEndSize = mapping->fileOffset + mapping->size;
        if (fileSize < regionEndSize) {
            fileSize = regionEndSize;
        }
    }
    vm_address_t mappedSubCache = 0;
    if (vm_allocate(mach_task_self(), &mappedSubCache, (size_t)fileSize, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        result = false;
        return result;
    }
    for(auto i = 0; i < _header->mappingCount; ++i) {
        //    _slide = _baseAddress -  _header->sharedRegionStart;
        auto mapping = (dyld_cache_mapping_info*)&subCacheHeaderBytes[subCacheHeader->mappingOffset+(i*sizeof(dyld_cache_mapping_info))];
        auto mappingBytes = _mapper->map<uint8_t>(mapping->address - _slide, mapping->size);
        kern_return_t r = vm_copy(mach_task_self(), (vm_address_t)&*mappingBytes, (vm_size_t)mapping->size, (vm_address_t)(mappedSubCache+mapping->fileOffset));
        if ( r != KERN_SUCCESS ) {
            result = false;
            break;
        }
    }
    if ( result )
        block((void*)mappedSubCache, (size_t)fileSize);

    assert(vm_deallocate(mach_task_self(), (vm_address_t)mappedSubCache, (vm_size_t)fileSize) == KERN_SUCCESS);
    return result;
}

bool SharedCache::forEachSubcache4Rosetta(void (^block)(const void* cacheBuffer, size_t size)) {
    if (strcmp(_header->magic, "dyld_v1  x86_64") != 0) {
        return false;
    }
    uint64_t headerSize;
    bool splitCache = false;
    getCacheInfo(&*_header, headerSize, splitCache);
    mapSubCacheAndInvokeBlock(&*_header, block);
    auto headerBytes = (uint8_t*)&*_header;
    if (splitCache) {
        for (auto i = 0; i < _header->subCacheArrayCount; ++i) {
            uint64_t subCacheOffset = 0;
            if (_header->mappingOffset <= offsetof(dyld_cache_header, cacheSubType) ) {
                auto subCaches = (dyld_subcache_entry_v1*)&headerBytes[_header->subCacheArrayOffset];
                subCacheOffset = subCaches[i].cacheVMOffset;
            } else {
                auto subCaches = (dyld_subcache_entry*)&headerBytes[_header->subCacheArrayOffset];
                subCacheOffset = subCaches[i].cacheVMOffset;
            }
            auto subCacheHeader = _mapper->map<dyld_cache_header>((uint64_t)rebasedAddress() + subCacheOffset, PAGE_SIZE);
            uint64_t subCacheHeaderSize = subCacheHeader->mappingOffset+subCacheHeader->mappingCount*sizeof(dyld_cache_mapping_info);
            getCacheInfo(&*_header, headerSize, splitCache);
            if (subCacheHeaderSize > PAGE_SIZE) {
                subCacheHeader = _mapper->map<dyld_cache_header>((uint64_t)rebasedAddress() + subCacheOffset, subCacheHeaderSize);
            }
//            printf("Subcache Offset: %lx\n", (uintptr_t)&headerBytes[subCacheOffset]);
//            printf("subCacheHeader: %lx\n", (uintptr_t)&*subCacheHeader);
//            printf("Subcache magic: %s\n", subCacheHeader->magic);
            mapSubCacheAndInvokeBlock(&*subCacheHeader, block);
        }
    }
    return true;
}
#endif

#if BUILDING_LIBDYLD
#pragma mark -
#pragma mark Process

Process::Process(Allocator& ephemeralAllocator, FileManager& fileManager, task_read_t task, kern_return_t *kr)
    :   _ephemeralAllocator(ephemeralAllocator), _fileManager(fileManager), _task(task), _queue(dispatch_queue_create("com.apple.dyld.introspection", NULL)),
        _registeredNotifiers(_transactionalAllocator), _registeredUpdaters(_transactionalAllocator)  {
    _snapshot = getSnapshot(kr);
}

Process::~Process() {
    dispatch_async_and_wait(_queue, ^{
        if (_state == Connected) {
            teardownNotifications();
        }
    });
    dispatch_release(_queue);
}

kern_return_t Process::task_dyld_process_info_notify_register(task_t target_task, mach_port_t notify) {
#if TARGET_OS_SIMULATOR
    static dispatch_once_t onceToken;
    static kern_return_t (*tdpinr)(task_t, mach_port_t) = nullptr;
    dispatch_once(&onceToken, ^{
        tdpinr = (kern_return_t (*)(task_t, mach_port_t))dlsym(RTLD_DEFAULT, "task_dyld_process_info_notify_register");
    });
    if (tdpinr) {
        return tdpinr(target_task, notify);
    }
    return KERN_FAILURE;
#else
    return ::task_dyld_process_info_notify_register(target_task, notify);
#endif
}

kern_return_t Process::task_dyld_process_info_notify_deregister(task_t target_task, mach_port_t notify) {
#if TARGET_OS_SIMULATOR
    static dispatch_once_t onceToken;
    static kern_return_t (*tdpind)(task_t, mach_port_t) = nullptr;
    dispatch_once(&onceToken, ^{
        tdpind = (kern_return_t (*)(task_t, mach_port_t))dlsym(RTLD_DEFAULT, "task_dyld_process_info_notify_deregister");
    });
    if (tdpind) {
        return tdpind(target_task, notify);
    }
    return KERN_FAILURE;
#else
    // Our libsystem does not have task_dyld_process_info_notify_deregister, emulate
    return ::task_dyld_process_info_notify_deregister(target_task, notify);
#endif
}

// Some day the kernel will setup a compact info for us, so there will always be one, but for now synthesize one for processes
// that launch suspended and have not run long enough to have one
UniquePtr<ProcessSnapshot> Process::synthesizeSnapshot(kern_return_t *kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }

    auto result = _transactionalAllocator.makeUnique<ProcessSnapshot>(_ephemeralAllocator, _fileManager, false);
    pid_t   pid;
    *kr = pid_for_task(_task, &pid);
    if ( *kr != KERN_SUCCESS ) {
        BLEND_KERN_RETURN_LOCATION(*kr, 0xea);
        *kr |= 0xeb000000UL;
        return nullptr;
    }

    mach_task_basic_info ti;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    *kr = task_info(_task, MACH_TASK_BASIC_INFO, (task_info_t)&ti, &count);
    if ( *kr != KERN_SUCCESS ) {
        BLEND_KERN_RETURN_LOCATION(*kr, 0xe9);
        return nullptr;
    }

    bool foundDyld              = false;
    bool foundMainExecutable    = false;
    mach_vm_size_t      size;
    for (mach_vm_address_t address = 0; ; address += size) {
        vm_region_basic_info_data_64_t  info;
        mach_port_t                     objectName;
        unsigned int                    infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        if ( mach_vm_region(_task, &address, &size, VM_REGION_BASIC_INFO,
                            (vm_region_info_t)&info, &infoCount, &objectName) != KERN_SUCCESS ) {
            break;
        }
        if ( info.protection != (VM_PROT_READ|VM_PROT_EXECUTE) ) {
            continue;
        }
        char executablePath[PATH_MAX+1];
        auto bytes = UniquePtr<std::byte>((std::byte*)_ephemeralAllocator.malloc((size_t)size));
        bytes.withUnsafe([&](std::byte* unsafeBytes) {
            mach_vm_size_t readSize = 0;
            *kr = mach_vm_read_overwrite(_task, address, size, (mach_vm_address_t)&unsafeBytes[0], &readSize);
            if ( *kr != KERN_SUCCESS ) {
                BLEND_KERN_RETURN_LOCATION(*kr, 0xe8);
                return;
            }
            auto mh = Header::isMachO({(const uint8_t*)unsafeBytes, (size_t)readSize});
            if (!mh) {
                return;
            }
            if ( mh->isMainExecutable() ) {
                int len = proc_regionfilename(pid, address, executablePath, PATH_MAX);
                if ( len != 0 ) {
                    executablePath[len] = '\0';
                }
                SharedPtr<Mapper> mapper = nullptr;
                auto file = _fileManager.fileRecordForPath(_transactionalAllocator, executablePath);
                uuid_t rawUUID;
                mh->getUuid(rawUUID);
                auto uuid = UUID(rawUUID);
                result->addImage(Image(_transactionalAllocator, std::move(file), mapper, (uint64_t)address, uuid));
                foundMainExecutable = true;
            } else if ( mh->isDylinker() ) {
                int len = proc_regionfilename(pid, address, executablePath, PATH_MAX);
                if ( len != 0 ) {
                    executablePath[len] = '\0';
                }
                SharedPtr<Mapper> mapper = nullptr;
                auto file = _fileManager.fileRecordForPath(_transactionalAllocator, executablePath);
                uuid_t rawUUID;
                mh->getUuid(rawUUID);
                auto uuid = UUID(rawUUID);
                result->addImage(Image(_transactionalAllocator, std::move(file), mapper, (uint64_t)address, uuid));
                foundDyld = true;
            }
        });
        if (foundDyld && foundMainExecutable) {
            return result;
        }
    }

    if ( *kr != KERN_SUCCESS ) {
        return nullptr;
    }

    if ( foundMainExecutable ) {
        // return result even if no dyld was found. It could be present in the shared cache
        return result;
    }

    // Something failed, we don't know what
    *kr = KERN_FAILURE;
    return nullptr;
}

UniquePtr<ProcessSnapshot> Process::getSnapshot(kern_return_t *kr) {
    kern_return_t krSink = KERN_SUCCESS;
    if (kr == nullptr) {
        kr = &krSink;
    }
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    task_dyld_info_data_t task_dyld_info;
    *kr = task_info(_task, TASK_DYLD_INFO, (task_info_t)&task_dyld_info, &count);
    if ( *kr != KERN_SUCCESS ) {
        BLEND_KERN_RETURN_LOCATION(*kr, 0xef);
        return nullptr;
    }
    //The kernel will return MACH_VM_MIN_ADDRESS for an executable that has not had dyld loaded
    if (task_dyld_info.all_image_info_addr == MACH_VM_MIN_ADDRESS) {
        BLEND_KERN_RETURN_LOCATION(*kr, 0xee);
        return nullptr;
    }
    uint8_t remoteBuffer[16*1024];
    mach_vm_size_t readSize = 0;
    uint64_t failedAddress = 0;
    while (1) {
        // Using mach_vm_read_overwrite because this is part of dyld. If the file is removed or the codesignature is invalid
        // then the system is broken beyond recovery anyway
        *kr = mach_vm_read_overwrite(_task, task_dyld_info.all_image_info_addr, task_dyld_info.all_image_info_size,
                                     (mach_vm_address_t)&remoteBuffer[0], &readSize);
        if (*kr != KERN_SUCCESS) {
            BLEND_KERN_RETURN_LOCATION(*kr, 0xed);
            // If we cannot read the all image info this is game over
            return nullptr;
        }
        uint64_t compactInfoAddress;
        uint64_t compactInfoSize;
        if (task_dyld_info.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_32 ) {
            const dyld_all_image_infos_32* info = (const dyld_all_image_infos_32*)&remoteBuffer[0];
            compactInfoAddress              = info->compact_dyld_image_info_addr;
            compactInfoSize                 = info->compact_dyld_image_info_size;
        } else {
            const dyld_all_image_infos_64* info = (const dyld_all_image_infos_64*)&remoteBuffer[0];
            // Mask of TBI bits
            compactInfoAddress              = (info->compact_dyld_image_info_addr & 0x00ff'ffff'ffff'ffff);
            compactInfoSize                 = info->compact_dyld_image_info_size;
        }
        if (compactInfoSize == 0) {
            return synthesizeSnapshot(kr);
        }
        auto compactInfo = UniquePtr<std::byte>((std::byte*)_transactionalAllocator.malloc((size_t)compactInfoSize));
        *kr = mach_vm_read_overwrite(_task, compactInfoAddress, compactInfoSize, (mach_vm_address_t)&*compactInfo, &readSize);
        if (*kr != KERN_SUCCESS) {
            BLEND_KERN_RETURN_LOCATION(*kr, 0xec);
            if (compactInfoAddress == failedAddress) {
                // We tried the same address twice and it failed both times, this is not a simple mutation issue, give up and reutrn an error
                return nullptr;
            }
            failedAddress = compactInfoAddress;
            // The read failed, chances are the process mutated the compact info, retry
            continue;
        }
        std::span<std::byte> data = std::span<std::byte>(&*compactInfo, (size_t)compactInfoSize);
        UniquePtr<ProcessSnapshot> result = _transactionalAllocator.makeUnique<ProcessSnapshot>(_ephemeralAllocator, _fileManager, false, data);
        if (!result->valid()) {
            // Something blew up we don't know what
            *kr = KERN_FAILURE;
            BLEND_KERN_RETURN_LOCATION(*kr, 0xeb);
            return nullptr;
        }
        return result;
    }
}

void Process::setupNotifications(kern_return_t *kr) {
    assert(dispatch_get_current_queue() == _queue);
    assert(kr != NULL);
    assert(_state == Disconnected);
    // Allocate a port to listen on in this monitoring task
    mach_port_options_t options = { .flags = MPO_IMPORTANCE_RECEIVER | MPO_CONTEXT_AS_GUARD | MPO_STRICT, .mpl = { MACH_PORT_QLIMIT_DEFAULT }};
    *kr = mach_port_construct(mach_task_self(), &options, (mach_port_context_t)this, &_port);
    if (*kr != KERN_SUCCESS) {
        return;
    }
    // Setup notifications in case the send goes away
    mach_port_t previous = MACH_PORT_NULL;
    *kr = mach_port_request_notification(mach_task_self(), _port, MACH_NOTIFY_NO_SENDERS, 1, _port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
    if ((*kr != KERN_SUCCESS) || previous != MACH_PORT_NULL) {
        (void)mach_port_destruct(mach_task_self(), _port, 0, (mach_port_context_t)this);
        return;
    }
    *kr = task_dyld_process_info_notify_register(_task, _port);
    if (*kr != KERN_SUCCESS) {
        (void)mach_port_destruct(mach_task_self(), _port, 0, (mach_port_context_t)this);
        return;
    }
    _machSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, _port, 0, _queue);
    if (_machSource == nullptr) {
        (void)mach_port_destruct(mach_task_self(), _port, 0, (mach_port_context_t)this);
        return;
    }
    dispatch_source_set_event_handler(_machSource, ^{ handleNotifications(); });
    // Copy these into locals so the block captures them as const instead of implicitly referring to the members via this
    task_read_t blockTask = _task;
    mach_port_t blockPort = _port;
    dispatch_source_t blockSource = _machSource;
    dispatch_source_set_cancel_handler(_machSource, ^{
        (void)task_dyld_process_info_notify_deregister(blockTask, blockPort);
        (void)mach_port_destruct(mach_task_self(), blockPort, 0, (mach_port_context_t)this);
        dispatch_release(blockSource);
    });
    dispatch_activate(_machSource);
    _state = Connected;
}

void Process::teardownNotifications() {
    assert(dispatch_get_current_queue() == _queue);
    assert(_state == Connected);
    if (_machSource) {
        dispatch_source_cancel(_machSource);
        _port = 0;
        _machSource = NULL;
        _state = Disconnected;
        // We leave the handle records so that we can correctly process release, but we release
        // the resources
        for (auto& [handle,updaterRecord] : _registeredUpdaters) {
            assert(handle != 0);
            if (updaterRecord.queue) {
                dispatch_release(updaterRecord.queue);
                updaterRecord.queue = nullptr;
            }
            if (updaterRecord.block) {
                Block_release(updaterRecord.block);
                updaterRecord.block = nullptr;
            }
        }
        for (auto& [handle,notifierRecord] : _registeredNotifiers) {
            assert(handle != 0);
            if (notifierRecord.queue) {
                dispatch_release(notifierRecord.queue);
                notifierRecord.queue = nullptr;
            }
            if (notifierRecord.block) {
                Block_release(notifierRecord.block);
                notifierRecord.block = nullptr;
            }
        }
    }
}

void Process::handleNotifications() {
    if (_state != Connected) { return; }
    // This event handler block has an implicit reference to "this"
    // if incrementing the count goes to one, that means the object may have already been destroyed
    uint8_t messageBuffer[DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE] = {};
    mach_msg_header_t* h = (mach_msg_header_t*)messageBuffer;

    kern_return_t r = mach_msg(h, MACH_RCV_MSG | MACH_RCV_VOUCHER| MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT) | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0), 0, sizeof(messageBuffer)-sizeof(mach_msg_audit_trailer_t), _port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if ( r == KERN_SUCCESS && !(h->msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
        //fprintf(stderr, "received message id=0x%X, size=%d\n", h->msgh_id, h->msgh_size);
        if ( (h->msgh_id & 0xFFFFF000) == DYLD_PROCESS_EVENT_ID_BASE ) {
            if (h->msgh_size != sizeof(mach_msg_header_t)) {
                teardownNotifications();
            } else if ((h->msgh_id & ~0xFFFFF000) == DYLD_REMOTE_EVENT_ATLAS_CHANGED) {
                kern_return_t kr;
                auto newSnapshot = getSnapshot(&kr);
                if (kr == KERN_SUCCESS) {
                    newSnapshot.withUnsafe([&](ProcessSnapshot* newSanpshotPtr) {
                        _snapshot->forEachImageNotIn(*newSanpshotPtr, ^(Image *image) {
                            for(auto& updaterRecord : _registeredUpdaters) {
                                dispatch_async_and_wait(updaterRecord.second.queue, ^{
                                    updaterRecord.second.block(image, false);
                                });
                            }
                        });
                    });
                    _snapshot.withUnsafe([&](ProcessSnapshot* oldSnapshot) {
                        newSnapshot->forEachImageNotIn(*oldSnapshot, ^(Image *image) {
                            for(auto& updaterRecord : _registeredUpdaters) {
                                dispatch_async_and_wait(updaterRecord.second.queue, ^{
                                    updaterRecord.second.block(image, true);
                                });
                            }
                        });
                    });
                }
                _snapshot = std::move(newSnapshot);
                //FIXME: Should we do something on failure here?
            } else {
                for (auto& [handle,notifier] : _registeredNotifiers) {
                    if ((h->msgh_id & ~0xFFFFF000) == notifier.notifierID) {
                        dispatch_async_and_wait(notifier.queue, notifier.block);
                    }
                }
            }
            mach_msg_header_t replyHeader;
            replyHeader.msgh_bits        = MACH_MSGH_BITS_SET(MACH_MSGH_BITS_REMOTE(h->msgh_bits), 0, 0, 0);
            replyHeader.msgh_id          = 0;
            replyHeader.msgh_local_port  = MACH_PORT_NULL;
            replyHeader.msgh_remote_port  = h->msgh_remote_port;
            replyHeader.msgh_reserved    = 0;
            replyHeader.msgh_size        = sizeof(replyHeader);
            r = mach_msg(&replyHeader, MACH_SEND_MSG, replyHeader.msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
            if (r == KERN_SUCCESS) {
                h->msgh_remote_port = MACH_PORT_NULL;
            } else {
                teardownNotifications();
            }
        } else if ( h->msgh_id == MACH_NOTIFY_NO_SENDERS ) {
            // Validate this notification came from the kernel
            const mach_msg_audit_trailer_t *audit_tlr = (mach_msg_audit_trailer_t *)((uint8_t *)h + round_msg(h->msgh_size));
            if (audit_tlr->msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0
                && audit_tlr->msgh_trailer_size >= sizeof(mach_msg_audit_trailer_t)
                // We cannot link to libbsm, so we are hardcoding the audit token offset (5)
                // And the value the represents the kernel (0)
                && audit_tlr->msgh_audit.val[5] == 0) {
                teardownNotifications();
            }
        } else if ( h->msgh_id != DYLD_PROCESS_INFO_NOTIFY_LOAD_ID
                   && h->msgh_id != DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID
                   && h->msgh_id != DYLD_PROCESS_INFO_NOTIFY_MAIN_ID) {
            fprintf(stderr, "dyld: received unknown message id=0x%X, size=%d\n", h->msgh_id, h->msgh_size);
        }
    } else {
        fprintf(stderr, "dyld: received unknown message id=0x%X, size=%d\n", h->msgh_id, h->msgh_size);
    }
    mach_msg_destroy(h);
}

uint32_t Process::registerAtlasChangedEventHandler(kern_return_t *kr, dispatch_queue_t queue, void (^block)(Image* image, bool load)) {
    __block uint32_t result = 0;
    dispatch_async_and_wait(_queue, ^{
        if (_state == Disconnected) {
            setupNotifications(kr);
            if (*kr != KERN_SUCCESS) {
                return;
            }
        }

        // Connection is setup, which means remote process will now block whenever updates occur
        if (*kr != KERN_SUCCESS) {
            teardownNotifications();
            return;
        }
        // Call for every image already in snapshot
        _snapshot->forEachImage(^(Image *image) {
            block(image, true);
        });
        assert(_state == Connected);
        dispatch_retain(queue);
        result = _handleIdx++;
        _registeredUpdaters.insert({result,(ProcessUpdateRecord){queue, Block_copy(block)}});
    });
    return result;
}

uint32_t Process::registerEventHandler(kern_return_t *kr, uint32_t event, dispatch_queue_t queue, void (^block)()) {
    __block uint32_t result = 0;
    dispatch_async_and_wait(_queue, ^{
        if (_state == Disconnected) {
            setupNotifications(kr);
            if (*kr != KERN_SUCCESS) {
                return;
            }
        }
        assert(_state == Connected);
        dispatch_retain(queue);
        result = _handleIdx++;
        _registeredNotifiers.insert({result,(ProcessNotifierRecord){queue, Block_copy(block), event}});
    });
    return result;
}

void Process::unregisterEventHandler(uint32_t handle) {
    dispatch_async_and_wait(_queue, ^{
        if (auto i = _registeredUpdaters.find(handle); i != _registeredUpdaters.end()) {
            assert(i->second.block != NULL);
            if (i->second.queue) {
                dispatch_release(i->second.queue);
            }
            if (i->second.block) {
                Block_release(i->second.block);
            }
            _registeredUpdaters.erase(i);
        } else if (auto j = _registeredNotifiers.find(handle); j != _registeredNotifiers.end()) {
            if (j->second.queue) {
                dispatch_release(j->second.queue);
            }
            if (j->second.block) {
                Block_release(j->second.block);
            }
            _registeredNotifiers.erase(j);
        }
    });
}

#endif /* BUILDING_LIBDYLD */

#pragma mark -
#pragma mark Process Snapshot

ProcessSnapshot::ProcessSnapshot(Allocator& ephemeralAllocator, FileManager& fileManager, bool useIdentityMapper)
    :   _ephemeralAllocator(ephemeralAllocator), _fileManager(fileManager), _images(_transactionalAllocator),
        _identityMapper(_transactionalAllocator.makeShared<Mapper>(_transactionalAllocator)), _useIdentityMapper(useIdentityMapper) {}

#if BUILDING_LIBDYLD && !TARGET_OS_DRIVERKIT && !TARGET_OS_EXCLAVEKIT
// This function is a private interface between libdyld and Dyld.framework and implemented in Swift
// there is no header
extern "C" const bool unwrapCompactInfo(void* _Nonnull buffer, uint64_t* _Nonnull size);
#endif /* BUILDING_LIBDYLD && !TARGET_OS_DRIVERKIT && !TARGET_OS_EXCLAVEKIT */

ProcessSnapshot::ProcessSnapshot(Allocator& ephemeralAllocator, FileManager& fileManager, bool useIdentityMapper, const std::span<std::byte> data)
    :   _ephemeralAllocator(ephemeralAllocator), _fileManager(fileManager), _images(_transactionalAllocator),
        _identityMapper(_transactionalAllocator.makeShared<Mapper>(_transactionalAllocator)), _useIdentityMapper(useIdentityMapper) {
        Serializer serializer(*this);
        bool deserializedSucceeed = serializer.deserialize(data);
#if BUILDING_LIBDYLD && !TARGET_OS_DRIVERKIT && !TARGET_OS_EXCLAVEKIT
        static dispatch_once_t onceToken;
        static __typeof__(unwrapCompactInfo) *unwrapCompactInfoPtr = nullptr;
        if (!deserializedSucceeed) {
            // If we failed we try to load the unwrap function
            dispatch_once(&onceToken, ^{
                // We attempt a dlopen() here since the unwrapCompactInfo is not in the build yet, and this is a temporary compatibility hack
                void* dyldFrameworkHandle = dlopen("/System/Library/PrivateFrameworks/Dyld.framework/Dyld", RTLD_NOW);
                unwrapCompactInfoPtr = (__typeof__(unwrapCompactInfo)*)dlsym(dyldFrameworkHandle, "unwrapCompactInfo");
            });
        }
        // Only try the fallback if we managed to load the unwrap function
        if (unwrapCompactInfoPtr && !deserializedSucceeed) {
            std::byte* unwrappedData = (std::byte*)_transactionalAllocator.malloc(data.size());
            std::copy(data.begin(), data.end(), unwrappedData);
            uint64_t unwrappedSize = data.size();
            if (unwrapCompactInfoPtr((void*)unwrappedData, &unwrappedSize)) {
                std::span<std::byte> unwrappedSpan = std::span<std::byte>(unwrappedData, (size_t)unwrappedSize);
                deserializedSucceeed = serializer.deserialize(unwrappedSpan);
            }
            free((void*)unwrappedData);
        }
#endif /* BUILDING_LIBDYLD && !TARGET_OS_DRIVERKIT && !TARGET_OS_EXCLAVEKIT */
        if (!deserializedSucceeed) {
            // Deerialization failed, reset the snapshot and mark invalid
            _images.clear();
            if (_bitmap) {
                _bitmap->clear();
            }
            _sharedCache        = nullptr;
            _platform           = 0;
            _initialImageCount  = 0;
            _dyldState          = 0;
            _valid              = false;
        }
}

SharedPtr<Mapper>& ProcessSnapshot::identityMapper() {
    return _identityMapper;
}

bool ProcessSnapshot::valid() const {
    return _valid;
}

void ProcessSnapshot::forEachImage(void (^block)(Image* image)) {
    bool processedCacheImages = false;
    auto processSharedCacheImages = [&] {
        if (processedCacheImages) { return; }
        processedCacheImages = true;
        for (auto i = 0; i < _sharedCache->imageCount(); ++i) {
            if (!_bitmap->checkBit(i)) { continue; }
            _sharedCache->withImageForIndex(i, ^(Image *image) {
                block(image);
            });
        }
    };
    for (auto& image : _images) {
        if (_sharedCache && (image->rebasedAddress() >= _sharedCache->rebasedAddress())) {
            processSharedCacheImages();
        }
        block(&*image);
    }
    if (_sharedCache) {
        processSharedCacheImages();
    }
}

void ProcessSnapshot::forEachImageNotIn(const ProcessSnapshot& other, void (^block)(Image* image)) {
    bool processedCacheImages = false;
    auto processSharedCacheImages = [&] {
        if (processedCacheImages) { return; }
        if (!_sharedCache) { return; }
        for (auto i = 0; i < _sharedCache->imageCount(); ++i) {
            if (!_bitmap->checkBit(i)) { continue; }
            if (other._sharedCache && other._bitmap->checkBit(i)) { continue; }
            _sharedCache->withImageForIndex(i, ^(Image *image) {
                block(image);
            });
        }
    };

    uint64_t address    = ~0ULL;
    auto i              = other._images.begin();
    if (i != other._images.end()) {
        address = (uint64_t)(*i)->rebasedAddress();
    }

    for (auto& image : _images) {
        if (_sharedCache && (image->rebasedAddress() >= _sharedCache->rebasedAddress())) {
            processSharedCacheImages();
        }
        for ( ; image->rebasedAddress() > address; ++i) {
            if (i == other._images.end()) {
                address = ~0ULL;
                break;
            }
            address = (uint64_t)(*i)->rebasedAddress();
        }
        if (image->rebasedAddress() != address) {
            block(&*image);
        }
    }
    if (_sharedCache) {
        processSharedCacheImages();
    }
}


UniquePtr<SharedCache>& ProcessSnapshot::sharedCache() {
    return _sharedCache;
}

#if BUILDING_DYLD
void ProcessSnapshot::addImages(RuntimeState* state, Vector<ConstAuthLoader>& loaders) {
    for (auto& ldr : loaders) {
        if (_sharedCache && ldr->dylibInDyldCache) {
            _bitmap->setBit(ldr->ref.index);
        } else {
            _images.insert(_transactionalAllocator.makeUnique<Image>(state, _ephemeralAllocator, identityMapper(), ldr));
        }
    }
}

void ProcessSnapshot::removeImages(RuntimeState* state, const std::span<const Loader*>& loaders) {
    for (auto ldr: loaders) {
        removeImageAtAddress((uint64_t)ldr->loadAddress(*state));
    }
}
#endif


void ProcessSnapshot::addImage(Image&& image) {
    _images.insert(_transactionalAllocator.makeUnique<Image>(std::move(image)));
}

#if BUILDING_DYLD || BUILDING_UNIT_TESTS
void ProcessSnapshot::addSharedCache(SharedCache&& sharedCache) {
    _sharedCache = _transactionalAllocator.makeUnique<SharedCache>(std::move(sharedCache));
    _bitmap = _transactionalAllocator.makeUnique<Bitmap>(_transactionalAllocator, _sharedCache->imageCount());
}

void ProcessSnapshot::addSharedCacheImage(const struct mach_header* mh) {
    assert(mh->flags & MH_DYLIB_IN_CACHE);
    auto header = (dyld_cache_header*)(uint64_t)_sharedCache->rebasedAddress();
    auto headerBytes = (uint8_t*)header;
    auto slide = (uint64_t)header - header->sharedRegionStart;
    auto images = std::span((const dyld_cache_image_text_info*)&headerBytes[header->imagesTextOffset], (size_t)header->imagesTextCount);
    auto i = std::find_if(images.begin(), images.end(), [&](const dyld_cache_image_text_info& other) {
        return (other.loadAddress == ((uint64_t)mh-slide));
    });
    assert(i != images.end());
    _bitmap->setBit(i-images.begin());
}



void ProcessSnapshot::removeImageAtAddress(uint64_t address) {
    //FIXME: Perf improvements by bisection. More generally remove UniquePtrs from sets needs a better solution
    for (auto i = _images.begin(); i != _images.end(); ++i) {
        if ((*i)->rebasedAddress() == address) {
            _images.erase(i);
            return;
        }
    }
}

Vector<std::byte> ProcessSnapshot::serialize() {
    Serializer serializer(*this);
    return serializer.serialize();
}

void ProcessSnapshot::setInitialImageCount(uint64_t imageCount) {
    _initialImageCount = imageCount;
}

void ProcessSnapshot::setDyldState(uint64_t state) {
    _dyldState = state;
}

void ProcessSnapshot::setPlatform(uint64_t platform) {
    _platform = platform;
}

#endif /* BUILDING_DYLD || BUILDING_UNIT_TESTS */


void ProcessSnapshot::dump() {
    forEachImage(^(Image *image) {
        char uuidStr[64];
        image->uuid().dumpStr(uuidStr);
        const char* name = image->installname();
        if (!name) {
            name = image->filename();
        }
        if (!name) {
            name = "<unknown>";
        }
        fprintf(stderr, "0x%llx %s %s\n",  (uint64_t)image->rebasedAddress(), uuidStr, name);
    });
}

#pragma mark -
#pragma mark Process Snapshot Serializer

//
//           ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
//           │Compact Info                                                                                                                            │
//           │   ┌────────────────────────────┐                                                                                                       │
//           │   │uint32_t magic              │           ┌────────────────────────────┐                                                              │
//           │   ├────────────────────────────┤     ┌────▶│PVLEUInt64 count            │                                                              │
//           │   │uint32_t version            │     │     ├────────────────────────────┘                                                              │
//           │   ├────────────────────────────┤     │      UUID elt[0]                 │◀───────────────┐                                             │
//           │   │uint64_t systemInfoAddress  │     │     ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │                                             │
//           │   ├────────────────────────────┤     │      ...                         │                │                                             │
//           │   │uint32_t systemInfoSize     │     │     ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │                                             │
//           │   ├────────────────────────────┤     │      UUID elt[count - 1]         │                │                                             │
//           │   │uint32_t genCount           │     │     └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │                                             │
//           │   ├────────────────────────────┤     │                                                   │                                             │
//           │   │uint64_t timeStamp          │     │                                                   │                                             │
//           │   ├────────────────────────────┤     │                                                   │                                             │
//           │   │uint32_t crc32              │     │             ┌────────────────────────────┐        │                                             │
//           │   ├────────────────────────────┤     │   ┌────────▶│PVLEUInt64 count            │        │                                             │
//           │   │PVLEUInt64 processFlags     │     │   │         ├────────────────────────────┘        │                                             │
//           │   ├────────────────────────────┤     │   │          char data[count - 1]        │◀───────┼─────┐                                       │
//           │   │PVLEUInt64 platform         │     │   │         └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─         │     │                                       │
//           │   ├────────────────────────────┤     │   │                                               │     │                                       │
//           │   │PVLEUInt64 initialImageCount│     │   │                                               │     │                                       │
//           │   ├────────────────────────────┤     │   │               ┌────────────────────────────┐  │     │                                       │
//           │   │PVLEUInt64 dyldState        │     │   │   ─ ─ ─ ─ ─ ─▶│PVLEUInt64 flags            │  │     │                                       │
//           │   ├────────────────────────────┤     │   │  │            ├────────────────────────────┤  │     │                                       │
//           │   │VolumeUUIDs                 │─────┘   │               │PVLEUInt64 baseAddress      │  │     │                                       │
//           │   ├────────────────────────────┤         │  │            ├────────────────────────────┘  │     │                                       │
//           │   │StringTableBuffer           │─────────┘                UUID                        │  │     │                                       │
//           │   ├────────────────────────────┤            │            ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   │     │                                       │
//           │   │sharedCache                 │── ─ ─ ─ ─ ─              PVLEUInt64 volumeUUID       │──┘     │                                       │
//           │   ├────────────────────────────┤                         ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─         │                                       │
//           │   │images                      │───────┐                  PVLEUInt64 objectID         │        │                                       │
//           │   ├────────────────────────────┤       │                 ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─         │                                       │
//           │   │Zero padding (% 16)         │       │                  PVLEUInt64 path             │────────┘                                       │
//           │   └────────────────────────────┘       │                 ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                ┌────────────────────────────┐   │
//           │                                        │                  Bitmap bitmap               │──────────────▶│PVLEUInt64 count            │   │
//           │                                        │                 └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                ├────────────────────────────┘   │
//           │                                        │                                                               byte data[count - 1]        │   │
//           │                                        │                                                              └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─    │
//           │                                        │                                                                                               │
//           │                                        │        ┌────────────────────────────┐           ┌────────────────────────────┐                │
//           │                                        └───────▶│PVLEUInt64 count            │     ┌────▶│PVLEUInt64 flags            │                │
//           │                                                 ├────────────────────────────┘     │     ├────────────────────────────┤                │
//           │                                                  MappedFileInfo elt[0]       │─────┘     │PVLEUInt64 baseAddress      │                │
//           │                                                 ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─            ├────────────────────────────┘                │
//           │                                                  ...                         │            UUID                        │                │
//           │                                                 ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─            ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │
//           │                                                  MappedFileInfo elt[count -  │            PVLEUInt64 volumeUUID       │                │
//           │                                                 └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─            ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │
//           │                                                                                           PVLEUInt64 objectID         │                │
//           │                                                                                          ├ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │
//           │                                                                                           PVLEUInt64 path             │                │
//           │                                                                                          └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─                 │
//           │                                                                                                                                        │
//           └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

ProcessSnapshot::Serializer::Serializer(ProcessSnapshot& processSnapshot)
    :   _processSnapshot(processSnapshot), _ephemeralAllocator(_processSnapshot._ephemeralAllocator),
        _fileManager(_processSnapshot._fileManager), _images(_processSnapshot._images),
        _sharedCache(_processSnapshot._sharedCache), _bitmap(_processSnapshot._bitmap), _volumeUUIDs(_ephemeralAllocator),
        _strings(_ephemeralAllocator), _stringTableBuffer(_ephemeralAllocator),
        _stringTableOffsets(_ephemeralAllocator), _platform(_processSnapshot._platform),
        _initialImageCount(_processSnapshot._initialImageCount), _dyldState(_processSnapshot._dyldState)
{}

void ProcessSnapshot::Serializer::emitStringRef(const char* string, Vector<std::byte>& data) {
    auto i = std::lower_bound(_strings.begin(), _strings.end(), string, lsl::ConstCharStarCompare());
    if (i == _strings.end()) {
        i = std::lower_bound(_strings.begin(), _strings.end(), "???", lsl::ConstCharStarCompare());
    }
    contract(i != _strings.end());
    contract(strcmp(*i, string) == 0);
    uint32_t index = (uint32_t)((uintptr_t)(*i) - (uintptr_t)&_stringTableBuffer[0]);
    contract(strcmp(*i, &_stringTableBuffer[index]) == 0);
    emitPVLEUInt64(index, data);
}

void ProcessSnapshot::Serializer::emitMappedFileInfo(uint64_t rebasedAddress, const UUID& uuid, const FileRecord& file, Vector<std::byte>& data) {
    uint64_t flags = 0;
    if (uuid) {
        flags |= kMappedFileFlagsHasUUID;
    }
    if (file.persistent()) {
        flags |= kMappedFileFlagsHasFileID;
    } else if (file.getPath()) {
        flags |= kMappedFileFlagsHasFilePath;
    }
    emitPVLEUInt64(flags,           data);
    emitPVLEUInt64(rebasedAddress,  data);
    if (flags & kMappedFileFlagsHasUUID) {
        std::copy(uuid.begin(), uuid.end(), std::back_inserter(data));
    }
    if (flags & kMappedFileFlagsHasFileID) {
        auto i = std::lower_bound(_volumeUUIDs.begin(), _volumeUUIDs.end(), file.volume());
        assert(i != _volumeUUIDs.end());
        assert(*i == file.volume());
        uint16_t index = i - _volumeUUIDs.begin();
        emitPVLEUInt64(index, data);
        emitPVLEUInt64(file.objectID(), data);
    }
    if (flags & kMappedFileFlagsHasFilePath) {
        emitStringRef(file.getPath(), data);
    }
}

bool ProcessSnapshot::Serializer::readMappedFileInfo(std::span<std::byte>& data, uint64_t& rebasedAddress, UUID& uuid, FileRecord& file) {
    uint64_t flags = 0;
    if (!readPVLEUInt64(data, flags)
        || !readPVLEUInt64(data, rebasedAddress)) {
        return false;
    }
    if (flags & kMappedFileFlagsHasUUID) {
        if (data.size() < 16) {
            return false;
        }
        uuid = UUID(&data[0]);
        data = data.last(data.size()-16);
    }
    if (flags & kMappedFileFlagsHasFileID) {
        uint64_t volumeIndex = 0;
        uint64_t objectID = 0;
        if (!readPVLEUInt64(data, volumeIndex)
            || !readPVLEUInt64(data, objectID)
            || volumeIndex >= _volumeUUIDs.size()) {
            return false;
        }
        file = _fileManager.fileRecordForVolumeUUIDAndObjID(_volumeUUIDs[(size_t)volumeIndex], objectID);
    }
    if (flags & kMappedFileFlagsHasFilePath) {
        uint64_t pathOffset = 0;
        if (!readPVLEUInt64(data, pathOffset) || pathOffset >= _stringTableBuffer.size()) {
            return false;
        }
        file = _fileManager.fileRecordForPath(_ephemeralAllocator, &_stringTableBuffer[(size_t)pathOffset]);
    }
    return true;
}

Vector<std::byte> ProcessSnapshot::Serializer::serialize() {
    _timestamp = mach_absolute_time();
    _genCount++;
    auto result = Vector<std::byte>(_ephemeralAllocator);
    // We need unique all the strings and UUIDs and place them in sorted tables
    // FIXME: We should use vectors and sort them since it faster in pathological cases, but we need a non-allocating sort
    OrderedSet<const char*, lsl::ConstCharStarCompare>   stringSet(_ephemeralAllocator);
    OrderedSet<UUID>                                     volumeUUIDSet(_ephemeralAllocator);
    if (PAGE_SIZE == 16384) {
        _processFlags |= kProcessFlagsHas16kPages;
    }
    if (_sharedCache) {
        _processFlags |= kProcessFlagsHasSharedCache;
        auto& file = _sharedCache->file();
        if (file.persistent()) {
            volumeUUIDSet.insert(file.volume());
        } else if (auto filePath = file.getPath()) {
            stringSet.insert(filePath);
        } else {
            stringSet.insert("???");
        }
        //FIXME record private cache info
    }
    for (const auto& image : _images) {
        auto& file = image->file();
        if (file.persistent()) {
            volumeUUIDSet.insert(file.volume());
        } else if (auto filePath = file.getPath()) {
            stringSet.insert(filePath);
        } else {
            stringSet.insert("???");
        }
    }
    // Insert them into vectors so we can get offsets cheaply
    _volumeUUIDs = Vector<UUID>(volumeUUIDSet.begin(), volumeUUIDSet.end(), _ephemeralAllocator);

    for (const auto& string : stringSet) {
        _stringTableOffsets.push_back((uint32_t)_stringTableBuffer.size());
        for(auto i = 0; string[i] != 0; ++i) {
            _stringTableBuffer.push_back(string[i]);
        }
        _stringTableBuffer.push_back(0);
    }
    for (const auto& offset : _stringTableOffsets) {
        _strings.push_back(&_stringTableBuffer[offset]);
    }

    // First, serializer the various pieces of metadata using fixed with ints
    emit<uint32_t>(_magic,              result);
    emit<uint32_t>(_version,            result);
    emit<uint64_t>(_systemInfoAddress,  result);
    emit<uint32_t>(_systemInfoSize,     result);
    emit<uint32_t>(_genCount,           result);
    emit<uint64_t>(_timestamp,          result);
    emit<uint32_t>(_crc32c,             result);

    // Switch over to variable width now that we are past pieces of the header the kernel may want to parse
    emitPVLEUInt64(_processFlags,       result);
    emitPVLEUInt64(_platform,           result);
    emitPVLEUInt64(_initialImageCount,  result);
    emitPVLEUInt64(_dyldState,           result);

    emitPVLEUInt64(_volumeUUIDs.size(), result);
    for (const auto& uuid : _volumeUUIDs) {
        std::copy(uuid.begin(), uuid.end(), std::back_inserter(result));
    }
    emitPVLEUInt64(_stringTableBuffer.size(), result);
    std::copy((std::byte*)_stringTableBuffer.begin(), (std::byte*)_stringTableBuffer.end(), std::back_inserter(result));
    if (_processFlags & kProcessFlagsHasSharedCache) {
        uint64_t address = (uint64_t)_sharedCache->rebasedAddress()/((_processFlags & kProcessFlagsHas16kPages) ? 16384 : 4096);
        emitMappedFileInfo(address, _sharedCache->uuid(), _sharedCache->file(), result);
        emitPVLEUInt64(_bitmap->size(), result);
        if (_bitmap->size() > 0)
            emit(_bitmap->bytes(), result);
    }

    emitPVLEUInt64(_images.size(), result);
    uint64_t lastAddress = 0;
    for (const auto& image : _images) {
        uint64_t address = ((uint64_t)image->rebasedAddress()-lastAddress)/((_processFlags & kProcessFlagsHas16kPages) ? 16384 : 4096);
        lastAddress = (uint64_t)image->rebasedAddress();
        emitMappedFileInfo(address, image->uuid(), image->file(), result);
    }
    while(result.size()%16 != 0) {
        emit<uint8_t>(0, result);
    }
    CRC32c checksumer;
    checksumer(result);
    *((uint32_t*)&result[32]) = checksumer;
    return result;
}

bool ProcessSnapshot::Serializer::deserialize(const std::span<std::byte> data) {
    auto i = data;
    if (i.size() < 36) {
        // Ensure data is at least large enough to read the header
        return false;
    }
    // Confirm magic
    _magic              = read<uint32_t>(i);
    _version            = read<uint32_t>(i);
    _systemInfoAddress  = read<uint64_t>(i);
    _systemInfoSize     = read<uint32_t>(i);
    _genCount           = read<uint32_t>(i);
    _timestamp          = read<uint64_t>(i);
    _crc32c             = read<uint32_t>(i);
    if (_magic != kMagic) {
        return false;
    }
    if (_version != 0) {
        return false;
    }
    CRC32c checksumer;
    checksumer(std::span(&data[0], 32));
    checksumer((uint32_t)0); // Zero out the actual checksum
    checksumer(std::span(&data[36], data.size() - 36));
    if (_crc32c != checksumer) {
        return false;
    }
    uint64_t volumeUUIDCount = 0;
    if (!readPVLEUInt64(i, _processFlags)
        || !readPVLEUInt64(i, _platform)
        || !readPVLEUInt64(i, _initialImageCount)
        || !readPVLEUInt64(i, _dyldState)
        || !readPVLEUInt64(i, volumeUUIDCount)) {
        return false;
    }
    if (i.size() < volumeUUIDCount*16) {
        return false;
    }
    for (auto j = 0; j < volumeUUIDCount; ++j) {
        UUID volumeUUID(&i[j*16]);
        _volumeUUIDs.push_back(volumeUUID);
    }
    i = i.last((size_t)(i.size()-(16*volumeUUIDCount)));
    uint64_t stringTableSize = 0;
    if (!readPVLEUInt64(i, stringTableSize)
        || i.size() < stringTableSize) {
        return false;
    }
    _stringTableBuffer.reserve((size_t)stringTableSize);
    std::copy((uint8_t*)&i[0], (uint8_t*)&i[(size_t)stringTableSize], std::back_inserter(_stringTableBuffer));
    i = i.last((size_t)(i.size()-stringTableSize));

    if (_processFlags & kProcessFlagsHasSharedCache) {
        uint64_t rebasedAddress;
        UUID uuid;
        FileRecord file;
        if ( !readMappedFileInfo(i, rebasedAddress, uuid, file) )
            return false;
        rebasedAddress = rebasedAddress * ((_processFlags & kProcessFlagsHas16kPages) ? kPageSize16K : kPageSize4K);
        SharedPtr<Mapper> mapper = nullptr;
        if (_processSnapshot._useIdentityMapper) {
            mapper = _processSnapshot.identityMapper();
        } else {
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
            mapper = _transactionalAllocator.makeShared<Mapper>(_transactionalAllocator);
#else
            mapper = Mapper::mapperForSharedCache(_transactionalAllocator, file, rebasedAddress);
#endif
        }
        if (!mapper) {
            return false;
        }
        _sharedCache = _transactionalAllocator.makeUnique<SharedCache>(_transactionalAllocator, std::move(file), mapper,
                                                                       rebasedAddress, _processFlags & kProcessFlagsHasPrivateCache);
        uint64_t encodedSize = 0;
        if (!readPVLEUInt64(i, encodedSize)) {
            return false;
        }
        _bitmap = _transactionalAllocator.makeUnique<Bitmap>(_transactionalAllocator, (size_t)encodedSize, i);
        if (_bitmap->size() == 0) {
            return false;
        }
    }
    uint64_t imageCount = 0;
    if (!readPVLEUInt64(i, imageCount)) {
        return false;
    }
    uint64_t lastAddress = 0;
    for (auto j = 0; j < imageCount; ++j) {
        uint64_t rebasedAddress;
        UUID uuid;
        FileRecord file;
        if ( !readMappedFileInfo(i, rebasedAddress, uuid, file) )
            return false;
        rebasedAddress = (rebasedAddress * ((_processFlags & kProcessFlagsHas16kPages) ? 16384 : 4096)) + lastAddress;
        lastAddress = rebasedAddress;
        SharedPtr<Mapper> mapper = nullptr;
        if (_processSnapshot._useIdentityMapper) {
            mapper = _processSnapshot.identityMapper();
        }
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
        else {
            mapper = _transactionalAllocator.makeShared<Mapper>(_transactionalAllocator);
        }
#endif
        auto image = Image(_transactionalAllocator, std::move(file), mapper, (uint64_t)rebasedAddress, uuid);
        _images.insert(_transactionalAllocator.makeUnique<Image>(std::move(image)));
    }
    return true;
}

};
};
#endif // !TARGET_OS_EXCLAVEKIT

