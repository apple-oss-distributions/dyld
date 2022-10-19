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

#ifndef FileManager_h
#define FileManager_h

#include "UUID.h"
#include "Defines.h"
#include "Allocator.h"
#include "OrderedMap.h"
#include "DyldDelegates.h"

namespace dyld4 {

using lsl::UUID;
using lsl::UniquePtr;
using lsl::Allocator;
using lsl::OrderedMap;

struct FileManager;

struct VIS_HIDDEN FileRecord {
    FileRecord()                                = default;
    FileRecord(const FileRecord&other);
    FileRecord(FileRecord&& other);
    ~FileRecord();
    FileRecord& operator=(const FileRecord& other);
    FileRecord& operator=(FileRecord&& other);

    uint64_t        objectID() const;
    uint64_t        mtime() const;
    size_t          size() const;
    const UUID&     volume() const;

    int             open(int flags);
    void            close();

    bool            exists() const;
    const char*     getPath() const;
    bool            persistent() const;
    FileManager&    fileManager() const;

    friend void swap(FileRecord& x, FileRecord& y) {
        x.swap(y);
    }
private:
    friend FileManager;
    FileRecord(FileManager& fileManager, uint64_t objectID, uint64_t device, uint64_t mtime);
    FileRecord(FileManager& fileManager, const UUID& VID, uint64_t objectID);
    FileRecord(FileManager& fileManager, struct stat& sb);
    FileRecord(FileManager& fileManager, UniquePtr<const char>&& filePath);
    void swap(FileRecord& other);
    void stat() const;

    FileManager*                        _fileManager    = nullptr;
    mutable uint64_t                    _objectID       = 0;
    mutable uint64_t                    _device         = 0;
    mutable UUID                        _volume;
    mutable lsl::UniquePtr<const char>  _path;
    mutable size_t                      _size           = 0;
    mutable uint64_t                    _mtime          = 0;
    int                                 _fd             = -1;
    mutable int                         _statResult     = 1; // Tri-state: 1 not stated, 0 successful stat, -1 failed stat
    mutable mode_t                      _mode           = 0;
    mutable bool                        _valid          = true;
};

struct VIS_HIDDEN FileManager {
                    FileManager()                       = delete;
                    FileManager(const FileManager&)     = delete;
                    FileManager(FileManager&&)          = delete;
    FileManager&    operator=(const FileManager& O)     = delete;
    FileManager&    operator=(FileManager&& O)          = delete;
    FileManager(Allocator& allocator, const SyscallDelegate* syscall);

    FileRecord      fileRecordForPath(const char* filePath);
    FileRecord      fileRecordForStat(struct stat& sb);
    FileRecord      fileRecordForVolumeUUIDAndObjID(const UUID& VID, uint64_t objectID);
    FileRecord      fileRecordForVolumeDevIDAndObjID(uint64_t device, uint64_t objectID);
    FileRecord      fileRecordForFileID(const FileID& fileID);
    const UUID     uuidForFileSystem(uint64_t fsid) const;
    uint64_t        fsidForUUID(const UUID& uuid) const;
    friend void     swap(FileManager& x, FileManager& y) {
        x.swap(y);
    }
private:
    friend FileRecord;

    void    swap(FileManager& other);
    ssize_t fsgetpath(char result[], size_t resultBufferSize, uint64_t fsID, uint64_t objID) const;
    int     getfsstat(struct statfs *buf, int bufsize, int flags) const;

    int getattrlist(const char* path, struct attrlist * attrList, void * attrBuf, size_t attrBufSize, uint32_t options) const;

    void reloadFSInfos() const;
    UniquePtr<char> getPath(const lsl::UUID& VID, uint64_t OID);
    UniquePtr<char> getPath(uint64_t fsid, uint64_t OID);

    const SyscallDelegate*                          _syscall        = nullptr;
    Allocator*                                      _allocator      = nullptr;
    mutable UniquePtr<OrderedMap<uint64_t,UUID>>    _fsUUIDMap      = nullptr;
    //FIXME: We should probably have a more generic lock abstraction for locks we only need when not building dyld
    template<typename F>
    auto withFSInfoLock(F work) const
    {
        #if BUILDING_DYLD
        return work();
        #else
        os_unfair_lock_lock(&_fsUUIDMapLock);
        auto result = work();
        os_unfair_lock_unlock(&_fsUUIDMapLock);
        return result;
        #endif
    }
#if !BUILDING_DYLD
    mutable os_unfair_lock_s                        _fsUUIDMapLock  = OS_UNFAIR_LOCK_INIT;
#endif
};

}; /* namespace dyld4 */

#endif /* FileManager_h */
