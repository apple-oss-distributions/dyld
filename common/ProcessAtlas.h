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

#ifndef ProcessAtlas_h
#define ProcessAtlas_h

#include <atomic>
#include <cstdint>
#include <TargetConditionals.h>
#include "Defines.h"
#if !TARGET_OS_EXCLAVEKIT
  #include <mach/mach.h>
  #include <dispatch/dispatch.h>
#endif

#include "UUID.h"
#include "Bitmap.h"
#include "Vector.h"
#include "Allocator.h"
#include "OrderedSet.h"
#if !TARGET_OS_EXCLAVEKIT
  #include "FileManager.h"
#endif
#include "DyldRuntimeState.h"
#include "dyld_cache_format.h"

#define VIS_HIDDEN __attribute__((visibility("hidden")))

// Private notification ID used for app state changes
#define DYLD_REMOTE_EVENT_ATLAS_CHANGED (0)

class DyldSharedCache;

namespace dyld3 {
struct MachOLoaded;
}

namespace dyld4 {
struct FileManager;
struct FileRecord;
namespace Atlas {
using dyld3::MachOLoaded;
using lsl::UniquePtr;
using lsl::SharedPtr;
using lsl::UUID;
using lsl::Vector;
using lsl::OrderedSet;
using lsl::Bitmap;

// A type safe wrapper around pointers which only permits dyld to cast to pointers, and forces libdyld to use uint64_t.
// This is to support the case where arm64_32 clients of these APIs inspect arm64e processes
class VIS_HIDDEN SafePointer
{
public:
    explicit operator bool() const {
        return (pointer != 0);
    }

    explicit operator uint64_t() const {
        return pointer;
    }

    SafePointer() = default;
    SafePointer(uint32_t v) = delete;
    SafePointer(uint64_t v) : pointer(v) { }

    constexpr auto operator <=>(const SafePointer& rhs) const = default;

private:
    uint64_t pointer = 0;
};

/* The Mapper abstraction provides an interface we can use to abstract away in memory vs file layout for the cache
 *
 * All of the code is written as though the mach-o and cache files are mapped and loaded. When possible we reuse
 * dylibs from within the current process using a LocalMapper. When that is not possible we will go to disk using
 * a FileMapper. We never map remote memory.
 */

struct VIS_HIDDEN Mapper {
    // Move only smart pointer to manage mapped memory allocations
    template<typename T>
    struct Pointer {
        Pointer() = default;
        Pointer(const Pointer&) = delete;
        Pointer& operator=(const Pointer&) = delete;

        Pointer(Mapper* mapper, const SafePointer address, uint64_t size) : _mapper(mapper), _size(size) {
            auto [pointer, mmaped] = _mapper->map(address,_size);
            _pointer = pointer;
            _mmapped = mmaped;
        }
        Pointer(Pointer&& other) {
            swap(other);
        }
        Pointer& operator=(Pointer&& other) {
            swap(other);
            return *this;
        };
        ~Pointer() {
            if (_pointer && _mmapped) {
                _mapper->unmap(_pointer, _size);
            }
        }
        explicit operator bool() {
            return ((uint64_t)_pointer != 0);
        }

        T& operator*() {
            return *((T*)(uint64_t)_pointer);
        }
        T* operator->() {
            return (T*)(uint64_t)_pointer;
        }

        const T& operator*() const {
            return *((const T*)(uint64_t)_pointer);
        }
        const T* operator->() const {
            return (const T*)(uint64_t)_pointer;
        }

        friend void swap(Pointer& x, Pointer& y) {
            x.swap(y);
        }
    private:
        void swap(Pointer& other) {
            if (this == &other) { return; }
            using std::swap;
            swap(_mapper,  other._mapper);
            swap(_size,    other._size);
            swap(_pointer, other._pointer);
            swap(_mmapped, other._mmapped);
        }
        Mapper*             _mapper     = nullptr;
        uint64_t            _size       = 0;
        SafePointer         _pointer;
        bool                _mmapped    = false;
    };

    ~Mapper();
    template<typename T>
    Pointer<T>  map(const SafePointer addr, uint64_t size) {
        return Pointer<T>(this, addr, size);
    }
    struct Mapping {
        uint64_t    offset;
        uint64_t    size;
        uint64_t    address;
        int         fd; // If fd == -1 that means this is a memory mapping
    };

    static SharedPtr<Mapper>                        mapperForSharedCache(Allocator& allocator, FileRecord& file, const SafePointer baseAddress);
    static SharedPtr<Mapper>                        mapperForMachO(Allocator& allocator, FileRecord& file, const UUID& uuid, const SafePointer baseAddress);
    static std::pair<SharedPtr<Mapper>,uint64_t>    mapperForSharedCacheLocals(Allocator& allocator, FileRecord& file);

    Mapper(Allocator& allocator);
    Mapper(Allocator& allocator, uint64_t size);
    Mapper(Allocator& allocator, const Vector<Mapping>& mapper);
    const SafePointer                               baseAddress() const;
    const uint64_t                                  size() const;
    bool                                            pin();
    void                                            unpin();
    void                                            dump() const;
private:
    std::pair<SafePointer,bool>                     map(const SafePointer addr, uint64_t size) const;
    void                                            unmap(const SafePointer addr, uint64_t size) const;
    Vector<Mapping>                                 _mappings;
    void*                                           _flatMapping;
    Allocator&                                      _allocator;
};

struct SharedCache;

    struct VIS_HIDDEN Image {
                        Image()                             = delete;
                        Image(const Image&)                 = delete;
                        Image(Image&&);
                        Image& operator=(const Image& other)    = delete;
                        Image& operator=(Image&& other);

#if BUILDING_DYLD
                        Image(RuntimeState* state, Allocator& ephemeralAllocator, SharedPtr<Mapper>& M, const Loader* ldr);
#endif
                        Image(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& M, const SafePointer mh);
                        Image(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& M, const SafePointer mh, const UUID& uuid);
                        Image(Allocator& ephemeralAllocator, SharedPtr<Mapper>& M, SafePointer baseAddress, uint64_t cacheSlide, SharedCache* sharedCache);

    std::strong_ordering    operator<=>(const Image& other) const;
    SafePointer         rebasedAddress() const;
    const UUID&         uuid() const;
    const char*         installname() const ;
    const char*         filename() const;
    const FileRecord&   file() const;
    const SharedCache*  sharedCache() const;
    uint64_t            sharedCacheVMOffset() const;
    uint32_t            pointerSize();
    bool                forEachSegment(void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize,  int perm));
    bool                forEachSection(void (^block)(const char* segmentName, const char* sectionName, uint64_t vmAddr, uint64_t
                                                     vmSize));
    bool                contentForSegment(const char* segmentName,
                                          void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize));
    bool                contentForSection(const char* segmentName, const char* sectionName,
                                          void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize));
    friend void         swap(Image& x, Image& y) { x.swap(y); }
private:
    void                swap(Image& other);
    const MachOLoaded*  ml() const;

    Allocator&                              _ephemeralAllocator;
#if !TARGET_OS_EXCLAVEKIT
    mutable FileRecord                      _file;
#endif
    mutable SharedPtr<Mapper>               _mapper             = nullptr;
    mutable UUID                            _uuid;
    mutable Mapper::Pointer<MachOLoaded>    _ml;
    std::optional<uint64_t>                 _sharedCacheSlide;
    SafePointer                             _rebasedAddress;
    SharedCache*                            _sharedCache        = nullptr;
    mutable const char*                     _installname        = nullptr;
    mutable bool                            _uuidLoaded         = false;
    mutable bool                            _installnameLoaded  = false;
    mutable bool                            _mapperFailed       = false;
};

struct VIS_HIDDEN SharedCacheLocals {
                                            SharedCacheLocals(SharedPtr<Mapper>& M, bool use64BitDylibOffsets);
    const dyld_cache_local_symbols_info*    localInfo() const;
    bool                                    use64BitDylibOffsets() const;
private:
    friend struct SharedCache;


    SharedPtr<Mapper>                   _mapper;
    Mapper::Pointer<uint8_t>            _locals;
    bool                                _use64BitDylibOffsets = false;
};

struct VIS_HIDDEN SharedCache {
                                    SharedCache(Allocator& ephemeralAllocator, FileRecord&& file, SharedPtr<Mapper>& M, SafePointer rebasedAddress, bool P);
    const UUID&                     uuid() const;
    SafePointer                     rebasedAddress() const;
    uint64_t                        size() const;
    void                            forEachFilePath(void (^block)(const char* file_path)) const;
    bool                            isPrivateMapped() const;
    void                            forEachImage(void (^block)(Image* image));
    void                            withImageForIndex(uint32_t idx, void (^block)(Image* image));
    size_t                          imageCount() const;
    UniquePtr<SharedCacheLocals>    localSymbols() const;
    const FileRecord&               file() const;
    bool                            pin();
    void                            unpin();

#ifdef TARGET_OS_OSX
    bool                            mapSubCacheAndInvokeBlock(const dyld_cache_header* subCacheHeader, void (^block)(const void* cacheBuffer, size_t size));
    bool                            forEachSubcache4Rosetta(void (^block)(const void* cacheBuffer, size_t size));
#endif

    static UniquePtr<SharedCache>   createForFileRecord(Allocator& ephemeralAllocator, FileRecord&& fileRecord);
    static void                     forEachInstalledCacheWithSystemPath(Allocator& ephemeralAllocator, FileManager& _fileManager, const char* systemPath, void (^block)(SharedCache* cache));
private:
    friend struct ProcessSnapshot;

    Allocator&                          _ephemeralAllocator;
#if !TARGET_OS_EXCLAVEKIT
    FileRecord                          _file;
#endif
    UUID                                _uuid;
    uint64_t                            _size;
    Mapper::Pointer<dyld_cache_header>  _header;
    SharedPtr<Mapper>                   _mapper;
    uint64_t                            _slide      = 0;
    SafePointer                         _rebasedAddress;
    bool                                _private;
};

struct VIS_HIDDEN ProcessSnapshot {
                                        ProcessSnapshot(Allocator& ephemeralAllocator, FileManager& fileManager, bool useIdentityMapper);
                                        ProcessSnapshot(Allocator& ephemeralAllocator, FileManager& fileManager, bool useIdentityMapper, const std::span<std::byte> data);
    Vector<std::byte>                   serialize();
    UniquePtr<SharedCache>&             sharedCache();
    void                                forEachImage(void (^block)(Image* image));
    void                                forEachImageNotIn(const ProcessSnapshot& other, void (^block)(Image* image));
#if BUILDING_DYLD
    void                                addImages(RuntimeState* state, Vector<ConstAuthLoader>& loaders);
    void                                removeImages(RuntimeState* state, const std::span<const Loader*>& loaders);
#endif /* BUILDING_DYLD */
    void                                addImage(Image&& images);
#if BUILDING_DYLD || BUILDING_UNIT_TESTS
    void                                addSharedCache(SharedCache&& sharedCache);
    void                                addSharedCacheImage(const struct mach_header* mh);
    void                                removeImageAtAddress(uint64_t address);

    void                                setInitialImageCount(uint64_t imageCount);
    void                                setDyldState(uint64_t state);
    void                                setPlatform(uint64_t platform);
#endif /* BUILDING_DYLD || BUILDING_UNIT_TESTS */
    SharedPtr<Mapper>&                  identityMapper();
    bool                                valid() const;
    void                                dump();

private:
    struct Serializer {
        static const uint32_t kMagic                        = 0xa71a5166;
        static const uint64_t kProcessFlagsHasSharedCache   = 0x01;
        static const uint64_t kProcessFlagsHasPrivateCache  = 0x02;
        static const uint64_t kProcessFlagsHas16kPages      = 0x04;
        static const uint64_t kMappedFileFlagsHasFileID     = 0x01;
        static const uint64_t kMappedFileFlagsHasFilePath   = 0x02;
        static const uint64_t kMappedFileFlagsHasUUID       = 0x04;

        Serializer(ProcessSnapshot& processSnapshot);
        Vector<std::byte>  serialize();
        bool deserialize(const std::span<std::byte> data);
    private:
        template <typename T>
        void emit(T t, Vector<std::byte>& data) {
            auto newData = (std::byte*)&t;
            for(auto i = 0; i < sizeof(T); ++i) {
                data.push_back(newData[i]);
            }
        }
        template<>
        void emit(std::span<std::byte> t, Vector<std::byte>& data) {
            std::copy(t.begin(), t.end(), std::back_inserter(data));
        }
        template <typename T>
        auto read(std::span<std::byte>& data) {
            contract(sizeof(T) <= data.size());
            T result;
            std::copy(&data[0], &data[sizeof(T)], (std::byte*)&result);
            data = data.last(data.size()-sizeof(T));
            return result;
        }
        void emitMappedFileInfo(uint64_t rebasedAddress, const UUID& uuid, const FileRecord& file, Vector<std::byte>& data);
        bool readMappedFileInfo(std::span<std::byte>& data, uint64_t& rebasedAddress, UUID& uuid, FileRecord& file);

        void emitStringRef(const char* string, Vector<std::byte>& data);
    private:
        static const size_t kByteSize      = 8;
        static const size_t kPageSize4K    = 4096;
        static const size_t kPageSize16K   = 16384;

        ProcessSnapshot&                _processSnapshot;
        Allocator&                      _ephemeralAllocator;
        FileManager&                    _fileManager;
        OrderedSet<UniquePtr<Image>>&   _images;
        UniquePtr<SharedCache>&         _sharedCache;
        UniquePtr<Bitmap>&              _bitmap;
        Vector<UUID>                    _volumeUUIDs;
        Vector<const char *>            _strings;
        Vector<char>                    _stringTableBuffer;
        Vector<uint32_t>                _stringTableOffsets;
        uint32_t                        _magic              = kMagic;
        uint32_t                        _version            = 0;
        uint64_t                        _systemInfoAddress  = 0;
        uint32_t                        _systemInfoSize     = 0;
        uint64_t                        _timestamp          = 0;
        uint32_t                        _genCount           = 0;
        uint32_t                        _crc32c             = 0;
        uint64_t                        _processFlags       = 0;
        uint64_t&                       _platform;
        uint64_t&                       _initialImageCount;
        uint64_t&                       _dyldState;
    };
    Allocator&                      _ephemeralAllocator;
    FileManager&                    _fileManager;
    OrderedSet<UniquePtr<Image>>    _images;
    UniquePtr<Bitmap>               _bitmap;
    UniquePtr<SharedCache>          _sharedCache;
    SharedPtr<Mapper>               _identityMapper;
    uint64_t                        _platform           = 0;
    uint64_t                        _initialImageCount  = 0;
    uint64_t                        _dyldState          = 0;
    bool                            _useIdentityMapper;
    bool                            _valid              = true;
};

#if !BUILDING_DYLD
struct VIS_HIDDEN Process {
                                Process(Allocator& ephemeralAllocator, FileManager& _fileManager, task_read_t task, kern_return_t *kr);
                                ~Process();

    uint32_t                    registerEventHandler(kern_return_t *kr, uint32_t event, dispatch_queue_t queue, void (^block)());
    uint32_t                    registerAtlasChangedEventHandler(kern_return_t *kr, dispatch_queue_t queue, void (^block)(Image* image, bool load));
    void                        unregisterEventHandler(uint32_t handle);
    UniquePtr<ProcessSnapshot>  synthesizeSnapshot(kern_return_t *kr);
    UniquePtr<ProcessSnapshot>  getSnapshot(kern_return_t *kr);
private:
    kern_return_t       task_dyld_process_info_notify_register(task_read_t target_task, mach_port_t notify);
    kern_return_t       task_dyld_process_info_notify_deregister(task_read_t target_task, mach_port_t notify);
    struct ProcessNotifierRecord {
        dispatch_queue_t    queue;
        void                (^block)();
        uint32_t            notifierID;
    };
    struct ProcessUpdateRecord {
        dispatch_queue_t    queue;
        void                (^block)(Image*,bool);
    };
    enum ProcessNotifierState {
        Disconnected = 0,
        Connected,
        Disconnecting
    };
    void                        setupNotifications(kern_return_t *kr);
    void                        teardownNotifications();
    void                        handleNotifications();
    Allocator&                                      _ephemeralAllocator;
    FileManager&                                    _fileManager;
    task_read_t                                     _task               = TASK_NULL;
    mach_port_t                                     _port               = MACH_PORT_NULL;
    dispatch_queue_t                                _queue              = NULL;
    dispatch_source_t                               _machSource         = NULL;
    ProcessNotifierState                            _state              = Disconnected;
    OrderedMap<uint32_t,ProcessNotifierRecord>      _registeredNotifiers;
    OrderedMap<uint32_t,ProcessUpdateRecord>        _registeredUpdaters;
    UniquePtr<ProcessSnapshot>                      _snapshot;
    uint32_t                                        _handleIdx          = 1; // Start at 1 since we return 0 on error
};
#endif
};
};

#endif /* ProcessAtlas_h */
