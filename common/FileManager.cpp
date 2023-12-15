/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#include <fcntl.h>
#include <unistd.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <System/sys/fsgetpath.h>

#include "FileManager.h"

namespace dyld4 {

using lsl::UUID;
using lsl::UniquePtr;
using lsl::Allocator;
using lsl::OrderedMap;

FileManager::FileManager(Allocator& allocator, const SyscallDelegate* syscall)
: _syscall(syscall), _allocator(&allocator), _fsUUIDMap(_allocator->makeUnique<OrderedMap<uint64_t,UUID>>(*_allocator)) {}

void FileManager::swap(FileManager& other) {
    using std::swap;

    std::swap(_allocator,   other._allocator);
    std::swap(_fsUUIDMap,   other._fsUUIDMap);
}


FileRecord FileManager::fileRecordForPath(Allocator& allocator, const char* filePath) {
    const char* str = nullptr;
    if ( filePath )
        str = allocator.strdup(filePath);
    return FileRecord(*this, UniquePtr<const char>(str));
}

FileRecord FileManager::fileRecordForStat(const struct stat& sb) {
    return FileRecord(*this, sb);
}

FileRecord FileManager::fileRecordForVolumeUUIDAndObjID(const UUID& VID, uint64_t objectID) {
    return FileRecord(*this, VID, objectID);
}

FileRecord FileManager::fileRecordForVolumeDevIDAndObjID(uint64_t device, uint64_t objectID) {
//    auto volume = uuidForFileSystem(device);
//    char uuidString[64];
//    volume.dumpStr(&uuidString[0]);
//    fprintf(stderr, "device: %llu\n", device);
//    fprintf(stderr, "objectID: %llu\n", objectID);
//    fprintf(stderr, "VolumeUUID: %s\n", uuidString);
    return FileRecord(*this, objectID, device, 0);
}

FileRecord  FileManager::fileRecordForFileID(const FileID& fileID) {
    return FileRecord(*this, fileID.inode(), fileID.device(), fileID.mtime());
}


void FileManager::reloadFSInfos() const {
    struct VolAttrBuf {
        u_int32_t       length;
        dev_t           dev;
        fsid_t          fsid;
        vol_capabilities_attr_t volAttrs;
        uuid_t          volUUID;
    } __attribute__((aligned(4), packed));
    typedef struct VolAttrBuf VolAttrBuf;

    while (1) {
        int fsCount = getfsstat(nullptr, 0, MNT_NOWAIT);
        if (fsCount == -1) {
            // getfsstat failed, stop scanning for file systems, compact info will use full paths
            break;
        }
        int fsInfoSize = fsCount*sizeof(struct statfs);
        auto fsInfos = (struct statfs *)_allocator->malloc(fsInfoSize);
        if (this->getfsstat(fsInfos, fsInfoSize, MNT_NOWAIT) != fsCount) {
            // Retry
            _allocator->free((void*)fsInfos);
            continue;
        }
        for (auto i = 0; i < fsCount; ++i) {
            // On darwin the lower 32 bits of a fsid_t are the same as an the dev_t
            // it comes from, excluding some special circumstances related to volume
            // groups that are not relevent here.
            uint64_t f_fsid = (*((uint64_t*)&fsInfos[i].f_fsid)) & 0x00ffffffff;
            if (_fsUUIDMap->find(f_fsid) != _fsUUIDMap->end()) { continue; }

            // getattrlist() can upcall when used against a non-root volume which results in a deadlock.
            if ((fsInfos[i].f_flags & MNT_ROOTFS) == 0) {
                _fsUUIDMap->insert({f_fsid, UUID()});
                continue;
            }

            int             err;
            attrlist        attrList;
            VolAttrBuf      attrBuf;

            memset(&attrList, 0, sizeof(attrList));
            attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
            attrList.commonattr  = ATTR_CMN_FSID | ATTR_CMN_DEVID;
            attrList.volattr     =   ATTR_VOL_INFO
                                    | ATTR_VOL_CAPABILITIES
                                    | ATTR_VOL_UUID;
            err = this->getattrlist(fsInfos[i].f_mntonname, &attrList, &attrBuf, sizeof(attrBuf), 0);
            if (err == 0 && (attrBuf.volAttrs.capabilities[VOL_CAPABILITIES_FORMAT] & VOL_CAP_FMT_PERSISTENTOBJECTIDS)) {
                _fsUUIDMap->insert({f_fsid, attrBuf.volUUID});
            } else {
                _fsUUIDMap->insert({f_fsid, UUID()});
            }
        }
        _allocator->free((void*)fsInfos);
        break;
    }
}

const UUID FileManager::uuidForFileSystem(uint64_t fsid) const {
    return withFSInfoLock([&]{
        fsid &=  0x00ffffffff; // Mask off high bits that are an fs type tag
        auto i = _fsUUIDMap->find(fsid);
        if (i == _fsUUIDMap->end()) {
            // Maybe a new filesystem was loaded, try scanning
            reloadFSInfos();
            i = _fsUUIDMap->find(fsid);
        }
        if (i == _fsUUIDMap->end()) {
            // Still nothing, add a sentinel
            _fsUUIDMap->insert({fsid, UUID()});
            i = _fsUUIDMap->find(fsid);
        }
        assert (i != _fsUUIDMap->end());
        return i->second;
    });
}

uint64_t FileManager::fsidForUUID(const UUID& uuid) const {
    return withFSInfoLock([&]{
        for (const auto& i : *_fsUUIDMap) {
            if (i.second == uuid) {
                return i.first;
            }
        }
        // Maybe a new filesystem was loaded, try scanning
        // This is inefficient, but the only time it can happen is in libdyld reconsituting a compact info
        // after a volume is gone, so it is not worth the memory to make a reverse mapping table
        reloadFSInfos();
        for (const auto& i : *_fsUUIDMap) {
            if (i.second == uuid) {
                return i.first;
            }
        }
        return 0ULL;
    });
}

UniquePtr<char> FileManager::getPath(const UUID& VID, uint64_t OID) {
    if (!VID) { return nullptr; }
    uint64_t fsid = fsidForUUID(VID);
    return getPath(fsid, OID);
}

UniquePtr<char> FileManager::getPath(uint64_t fsid, uint64_t OID) {
    if ((fsid == 0) || (OID == 0)) { return nullptr; }
    char path[PATH_MAX];
    ssize_t result = this->fsgetpath(&path[0], PATH_MAX, fsid, OID);
#if !__LP64__
    //FIXME: Workaround for missing stat high bit on 32 bit platforms
    if (result == -1) {
        OID = 0x0fffffff00000000ULL | OID;
        result = this->fsgetpath(&path[0], PATH_MAX, fsid, OID);
    }
#endif
    if (result == -1) {
        return nullptr;
    }
    return UniquePtr<char>((char*)_allocator->strdup(path));
}

ssize_t FileManager::fsgetpath(char result[], size_t resultBufferSize, uint64_t fsID, uint64_t objID) const
{
#if BUILDING_DYLD
    return _syscall->fsgetpath(result, resultBufferSize, fsID, objID);
#else
    fsid_t      fsid  = *reinterpret_cast<fsid_t*>(&fsID);
    return ::fsgetpath(result, resultBufferSize, &fsid, objID);
#endif
}

int FileManager::getfsstat(struct statfs *buf, int bufsize, int flags) const {
#if BUILDING_DYLD
    return _syscall->getfsstat(buf, bufsize, flags);
#else
    return ::getfsstat(buf, bufsize, flags);
#endif
}

int FileManager::getattrlist(const char* path, struct attrlist * attrList, void * attrBuf, size_t attrBufSize, uint32_t options)
const {
#if BUILDING_DYLD
    return _syscall->getattrlist(path, attrList, attrBuf, attrBufSize, options);
#else
    return ::getattrlist(path, attrList, attrBuf, attrBufSize, options);
#endif
}

#pragma mark -
#pragma mark FileRecord

FileRecord::FileRecord(FileManager& fileManager, const UUID& VID, uint64_t objectID)
    :  _fileManager(&fileManager), _objectID(objectID), _volume(VID) {}

FileRecord::FileRecord(FileManager& fileManager, UniquePtr<const char>&& FP)
    :  _fileManager(&fileManager), _path(std::move(FP)) {}


FileRecord::FileRecord(FileManager& fileManager, uint64_t objectID, uint64_t device, uint64_t mtime)
    :   _fileManager(&fileManager), _objectID(objectID), _device(device), _volume(_fileManager->uuidForFileSystem(_device)), _mtime(mtime) {
        if (_objectID && _device && _mtime) {
            _statResult = 0;
        }
    }

FileRecord::FileRecord(FileManager& fileManager, const struct stat& sb)
    :  _fileManager(&fileManager), _objectID(sb.st_ino), _device(sb.st_dev), _volume(_fileManager->uuidForFileSystem(_device)), _mtime(sb.st_mtime), _statResult(0) {}

FileRecord::FileRecord(const FileRecord& other)
    :   _fileManager(other._fileManager), _objectID(other._objectID), _volume(other._volume),
        _path(UniquePtr<char>(_fileManager->_allocator->strdup(&*other._path))),
        _size(other._size), _mtime(other._mtime), _fd(other._fd), _statResult(other._statResult), _mode(other._mode),
        _valid(other._valid) {}

FileRecord::FileRecord(FileRecord&& other) {
    swap(other);
}

FileRecord& FileRecord::operator=(const FileRecord& other) {
    auto tmp = other;
    swap(tmp);
    return *this;
}
FileRecord& FileRecord::operator=(FileRecord&& other) {
    swap(other);
    return *this;
}

FileRecord::~FileRecord() {
    close();
}

void FileRecord::swap(FileRecord& other) {
    std::swap(_volume,      other._volume);
    std::swap(_objectID,    other._objectID);
    std::swap(_device,      other._device);
    std::swap(_path,        other._path);
    std::swap(_fileManager, other._fileManager);
    std::swap(_size,        other._size);
    std::swap(_mtime,       other._mtime);
    std::swap(_fd,          other._fd);
    std::swap(_statResult,  other._statResult);
    std::swap(_mode,        other._mode);
    std::swap(_valid,       other._valid);
}

int FileRecord::open(int flags) {
    assert(_fd == -1);
    uint64_t fsid = 0;
    if (_volume) {
        fsid = _fileManager->fsidForUUID(_volume);
    }
    if (fsid && _objectID) {
        _fd = openbyid_np((fsid_t*)&fsid, (fsobj_id_t*)_objectID, flags);
    }
    if (_fd == -1) {
        _fd = ::open(getPath(), flags);
    }
    return _fd;
}

void FileRecord::close() {
    if (_fd != -1) {
        ::close(_fd);
        _fd = -1;
    }
}

bool FileRecord::exists() const {
    stat();
    return (_statResult == 0);
}

uint64_t FileRecord::objectID() const {
    if (_objectID == 0) { stat(); }
    return _objectID;
}

uint64_t FileRecord::mtime() const {
    if (_mtime == 0) {
        stat();
    }
    return _mtime;
}

size_t FileRecord::size() const {
    if (_size == 0) { stat(); }
    return _size;
}

void FileRecord::stat() const {
    if (_statResult != 1) { return; }
    struct stat stat_buf;
    if (_fd != -1) {
        _statResult = fstat(_fd, &stat_buf);
    } else {
        _statResult = ::stat(getPath(), &stat_buf);
    }
    if (_statResult != 0) { return; }

    _size = (size_t)stat_buf.st_size;
    _mtime = stat_buf.st_mtime;
    _mode = stat_buf.st_mode;
    if ((_objectID == 0) && !_volume) {
#if __LP64__
        _objectID = stat_buf.st_ino;
#else
        _objectID = stat_buf.st_ino & 0xFFFFFFFF;  // HACK, work around inode randomly getting high bit set, making them uncomparable.
#endif
        _volume = _fileManager->uuidForFileSystem(stat_buf.st_dev);
    }
}

const char* FileRecord::getPath() const {
    if (!_path) {
        if (_device) {
            _path = _fileManager->getPath(_device, _objectID);
        } else {
            _path = _fileManager->getPath(_volume, _objectID);
        }
    }
    return _path.withUnsafe([](const char * path){
        return path;
    });
};

FileManager& FileRecord::fileManager() const {
    return *_fileManager;
}

bool FileRecord::persistent() const {
    return (_volume && _objectID);
}

const UUID& FileRecord::volume() const {
    return _volume;
}

}; /* namedpace dyld4 */

#endif // !TARGET_OS_EXCLAVEKIT
