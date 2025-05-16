/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#include <fcntl.h>
#include <libgen.h>
#include <algorithm>
#include <libproc.h>
#include <sys/mman.h>
#include <mach/task.h>
#include <sys/malloc.h>
#include <mach/mach_vm.h>
#include <mach/mach_traps.h>

#include <TargetConditionals.h>

#include "AAREncoder.h"
#include "Allocator.h"
#include "PropertyList.h"
#include "ProcessScavenger.h"
#include "SnapshotShared.h"
#include "Header.h"
#include "DyldSharedCache.h"
#include "Vector.h"

#include <sys/fsgetpath.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/dyld_process_info.h>

#include "dyld_cache_format.h"
#include "dyld_process_info_internal.h"

using lsl::Allocator;
using lsl::UniquePtr;
using lsl::Vector;
using mach_o::Header;
using UUID          = PropertyList::UUID;
using Array         = PropertyList::Array;
using Data          = PropertyList::Data;
using Dictionary    = PropertyList::Dictionary;
using String        = PropertyList::String;
using Bitmap        = PropertyList::Bitmap;
using Integer       = PropertyList::Integer;

namespace {

struct MmappedBuffer {
    MmappedBuffer() = default;
    MmappedBuffer(const MmappedBuffer&) = delete;
    MmappedBuffer(MmappedBuffer&& other) {
        swap(other);
    }
    MmappedBuffer& operator=(const MmappedBuffer&) = delete;
    MmappedBuffer& operator=(MmappedBuffer&& other) {
        swap(other);
        return *this;
    }
    MmappedBuffer(const char* path) {
        int fd = open(path, O_RDONLY);
        if (fd < 0 ) {
            return;
        }
        struct stat statBuf;
        if (fstat(fd, &statBuf) != 0) {
            return;
        }
        _size = statBuf.st_size;
        _data =  (void*)mmap(nullptr, (size_t)_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (_data == MAP_FAILED) {
            return;
        }
        close(fd);
    }
    ~MmappedBuffer() {
        if (!_data) { return; }
        munmap(_data, (size_t)_size);
    }
    uint64_t size() const {
        return _size;
    }
    const std::span<const uint8_t> span() const {
        return std::span<const uint8_t>((const uint8_t*)_data, (size_t)_size);
    }
private:
    void swap(MmappedBuffer& other) {
        if (this == &other) { return; }
        using std::swap;
        swap(_data, other._data);
        swap(_size, other._size);
    }
    void*       _data   = nullptr;
    uint64_t    _size   = 0;
};

struct RemoteMap {
    RemoteMap(task_t task, mach_vm_address_t remote_address, vm_size_t size) : _size(size) {
        vm_prot_t cur_protection = VM_PROT_NONE;
        vm_prot_t max_protection = VM_PROT_READ;
        mach_vm_address_t localAddress = 0;
        auto kr = mach_vm_remap_new(mach_task_self(),
                       &localAddress,
                       _size,
                       0,  // mask
                       VM_FLAGS_ANYWHERE | VM_FLAGS_RESILIENT_CODESIGN | VM_FLAGS_RESILIENT_MEDIA,
                       task,
                       remote_address,
                       true,
                       &cur_protection,
                       &max_protection,
                       VM_INHERIT_NONE);
        // The call is not succesfull return
        if (kr != KERN_SUCCESS) {
            _data   = nullptr;
            _size   = 0;
            return;
        }
        // Copy into a local buffer so our results are coherent in the event the page goes way due to storage removal,
        // etc. We have to do this because even after we read the page the contents might go away of the object is paged
        // out and then the backing region is disconnected (for example, if we are copying some memory in the middle of
        // a mach-o that is on a USB drive that is disconnected after we perform the mapping). Once we copy them into a
        // local buffer the memory will be handled by the default pager instead of potentially being backed by the mmap
        // pager, and thus will be guaranteed not to mutate out from under us.
        _data = malloc(_size);
        if (_data == nullptr) {
            _size   = 0;
            (void)vm_deallocate(mach_task_self(), (vm_address_t)localAddress, _size);
            return;
        }
        memcpy(_data, (void *)localAddress, _size);
        (void)vm_deallocate(mach_task_self(), (vm_address_t)localAddress, _size);
    }
    RemoteMap(const RemoteMap&) = delete;
    RemoteMap(RemoteMap&& other) {
        swap(other);
    }
    MmappedBuffer& operator=(const MmappedBuffer&) = delete;
    RemoteMap& operator=(RemoteMap&& other) {
        swap(other);
        return *this;
    }
    ~RemoteMap() {
        if (_data) {
            free(_data);
        }
    }
    operator bool() const {
        return ((_data != nullptr) && (_size != 0));
    }
    const std::span<const uint8_t> span() const {
        return std::span<const uint8_t>((const uint8_t*)_data, (size_t)_size);
    }
    uint64_t size() const {
        return _size;
    }
private:
    void swap(RemoteMap& other) {
        if (this == &other) { return; }
        using std::swap;
        swap(_data, other._data);
        swap(_size, other._size);
    }
    void*               _data   = nullptr;
    vm_size_t           _size   = 0;
};

struct TaskSuspender {
    TaskSuspender(task_read_t task) : _task(task) {
        if (task != mach_task_self()) {
            task_suspend(_task);
        } else {
            kern_return_t kr = task_threads(_task, &_threads, &_threadCount);
            if (kr != KERN_SUCCESS) {
                return;
            }
            for (auto i = 0; i < _threadCount; ++i) {
                if (_threads[i] != mach_thread_self()) {
                    thread_suspend(_threads[i]);
                }
            }
        }
    }
    ~TaskSuspender() {
        if (_task != mach_task_self()) {
            task_resume(_task);
        } else {
            for (auto i = 0; i < _threadCount; ++i) {
                if (_threads[i] != mach_thread_self()) {
                    thread_resume(_threads[i]);
                }
                mach_port_deallocate(mach_task_self(), _threads[i]);
            }
            mach_vm_deallocate(mach_task_self(), (mach_vm_address_t) _threads, _threadCount * sizeof(*_threads));
        }
    }
private:
    task_read_t             _task           = 0;
    thread_act_array_t      _threads        = nullptr;
    mach_msg_type_number_t  _threadCount    = 0;
};

static
void addSegmentArray(PropertyList::Dictionary& image, const Header* header) {
    __block Array* segments = nullptr;
    header->forEachSegment(^(const mach_o::Header::SegmentInfo& info, bool& stop) {
        if (info.segmentName == "__PAGEZERO") {
            return;
        }
        if (!segments) {
            segments = &image.addObjectForKey<Array>(kDyldAtlasImageSegmentArrayKey);
        }
        auto segment = &segments->addObject<Dictionary>();
        segment->addObjectForKey<String>(kDyldAtlasSegmentNameKey, info.segmentName);   // Note: we use the std::string_view part of CString
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentPreferredLoadAddressKey, info.vmaddr);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentSizeKey, info.vmsize);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentFileOffsetKey, info.fileOffset);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentFileSizeKey, info.fileSize);
        segment->addObjectForKey<Integer>(kDyldAtlasSegmentPermissionsKey, info.initProt);
    });
}

bool scavengeProcessFromRegions(Allocator& allocator, task_read_t task, ByteStream& outputStream) {
    TaskSuspender suspender(task);
    pid_t pid;
    auto propertyListEncoder    = PropertyList(allocator);
    auto& rootDictionary        = propertyListEncoder.rootDictionary();
    auto& images                = rootDictionary.addObjectForKey<Array>(kDyldAtlasSnapshotImagesArrayKey);

    kern_return_t kr = pid_for_task(task, &pid);
    if ( kr != KERN_SUCCESS) {
        return false;
    }
    rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotPidKey, pid);

    auto snapshotFlags = rootDictionary.addObjectForKey<PropertyList::Flags<SnapshotFlags>>(kDyldAtlasSnapshotFlagsKey);
    // Set the timestamp to 1, which is earlier then any real timestamp, but not 0, since tools use 0 as a sign
    // the process is not running yet and the API call has failed.
    rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotTimestampKey, 1);
    UniquePtr<const std::byte> fullCacheHeader;
    UniquePtr<const std::byte> infoArrayBuffer;
    UniquePtr<const std::byte> aotInfoArrayBuffer;
    rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotPlatformTypeKey, 0);

#if TARGET_OS_WATCH && !TARGET_OS_SIMULATOR
    snapshotFlags.setFlag(SnapshotFlagsPointerSize4Bytes, true);
#endif
    mach_vm_size_t      size;
    bool dyldFound           = false;
    bool mainExecutableFound = false;
    for (mach_vm_address_t address = 0; ; address += size) {
        vm_region_basic_info_data_64_t  info;
        mach_port_t                     objectName;
        unsigned int                    infoCount = VM_REGION_BASIC_INFO_COUNT_64;
        if ( mach_vm_region(task, &address, &size, VM_REGION_BASIC_INFO,
                            (vm_region_info_t)&info, &infoCount, &objectName) != KERN_SUCCESS ) {
            break;
        }
        if ( info.protection != (VM_PROT_READ|VM_PROT_EXECUTE) ) {
            continue;
        }
        RemoteMap map(task, address, std::min((size_t)size, (size_t)PAGE_SIZE));
        if (!map) {
            continue;
        }
        auto mf = Header::isMachO(map.span());
        if (!mf) {
            continue;
        }
        if (mf->machHeaderSize() > PAGE_SIZE) {
            size_t newSize =  (size_t)lsl::roundToNextAligned(PAGE_SIZE, mf->machHeaderSize());
            auto newMap = RemoteMap(task, address, newSize);
            map = std::move(newMap);
            if (!map) {
                continue;
            }
            mf = Header::isMachO(map.span());
            if (!mf) {
                continue;
            }
        }
        if (mf->isDylinker()) {
            dyldFound = true;
        }
        if (mf->isMainExecutable()) {
            mainExecutableFound = true;
        }
        // If this is not dyld or a main executable we don't need to scan the region
        if (!mf->isDylinker() && !mf->isMainExecutable()) { continue; }
        auto& image = images.addObject<Dictionary>();
        uint64_t preferredLoadAddress = mf->preferredLoadAddress();
        if (preferredLoadAddress) {
            image.addObjectForKey<Integer>(kDyldAtlasImagePreferredLoadAddressKey, preferredLoadAddress);
        }
        image.addObjectForKey<Integer>(kDyldAtlasImageLoadAddressKey, address);
        const char* installname = mf->installName();
        if (installname) {
            image.addObjectForKey<String>(kDyldAtlasImageInstallnameKey, installname);
        }
        uuid_t uuid;
        if (mf->getUuid(uuid)) {
            image.addObjectForKey<UUID>(kDyldAtlasImageUUIDKey, uuid);
        }
        char executablePath[PATH_MAX+1];
        int len = proc_regionfilename(pid, address, executablePath, PATH_MAX);
        if ( len != 0 ) {
            executablePath[len] = '\0';
            image.addObjectForKey<String>(kDyldAtlasImageFilePathKey, executablePath);
        }
        addSegmentArray(image, mf);
        // If we have found dyld and the main executable we are done, exit early
        if (dyldFound && mainExecutableFound) { break; }
    }
    rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotInitialImageCount, 1);
    rootDictionary.addObjectForKey<Integer>(kDyldAtlasSnapshotState, dyld_process_state_not_started);

    ByteStream fileStream(allocator);
    propertyListEncoder.encode(fileStream);
    AAREncoder aarEncoder(allocator);
    aarEncoder.addFile("process.plist", fileStream);
    aarEncoder.encode(outputStream);
    return true;
}
};

// via bufferSize. If the size is larger than was passed in then the return value is false. Otherwise it is true.
bool scavengeProcess(task_read_t task, void** buffer, uint64_t* bufferSize) {
    STACK_ALLOCATOR(allocator, 0);
    ByteStream outputStream(allocator);
    if (!scavengeProcessFromRegions(allocator, task, outputStream)) {
        return false;
    }
    *bufferSize = outputStream.size();
    *buffer = malloc((size_t)(*bufferSize));
    std::copy(outputStream.begin(), outputStream.end(), (std::byte*)*buffer);
    return true;
}
