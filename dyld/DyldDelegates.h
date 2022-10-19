/*
 * Copyright (c) 2019-2020 Apple Inc. All rights reserved.
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


#ifndef DyldDelegates_h
#define DyldDelegates_h

#include <stdint.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/syslog.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/dtrace.h>

#if !BUILDING_DYLD
  #include <vector>
  #include <map>
#endif

#include "Defines.h"
#include "Array.h"
#include "UUID.h"
#include "MachOAnalyzer.h"
#include "SharedCacheRuntime.h"

class DyldSharedCache;

namespace dyld4 {

using lsl::UUID;
using dyld3::MachOAnalyzer;
using dyld3::GradedArchs;
using dyld3::Array;

// for detecting symlinks and hard links, so dyld does not load same file twice
struct FileID
{
    FileID(uint64_t inode, uint64_t device, uint64_t mtime, bool valid) : _inode(inode), _device(device), _modTime(mtime), _isValid(valid)  { }

    bool            valid() const   { return _isValid; }
    uint64_t        inode() const   { return _inode; }
    uint64_t        device() const   { return _device; }
    uint64_t        mtime() const   { return _modTime; }
    static FileID   none() { return FileID(); }

    bool            operator==(const FileID& other) const {
        // Note, if either this or other is invalid, the result is false
        return (_device == other._device) && (_isValid && other._isValid) && (_inode==other._inode) && (_modTime==other._modTime);
    }
    bool            operator!=(const FileID& other) const { return !(*this == other); }

private:
    FileID() = default; // We have this here so none() can default construct an empty UUID
    uint64_t        _inode      = 0;
    uint64_t        _device     = 0;
    uint64_t        _modTime    = 0;
    bool            _isValid    = false;

};

#define ENOTAFILE_NP  666 // magic errno returned by SyscallDelegate::fileExists() if S_ISREG() is false

// all dyld syscalls goes through this delegate, which enables cache building and off line testing
class SyscallDelegate
{
public:
    uint64_t            amfiFlags(bool restricted, bool fairPlayEncryted) const;
    bool                internalInstall() const;
    bool                isTranslated() const;
    bool                getCWD(char path[MAXPATHLEN]) const;
    const GradedArchs&  getGradedArchs(const char* archName, bool keysOff, bool osBinariesOnly) const;
    int                 openLogFile(const char* path) const;
    bool                hasExistingDyldCache(uint64_t& cacheBaseAddress, uint64_t& fsid, uint64_t& fsobjid) const;
    void                disablePageInLinking() const;
    void                getDyldCache(const dyld3::SharedCacheOptions& opts, dyld3::SharedCacheLoadInfo& loadInfo) const;
    void                forEachInDirectory(const char* dir, bool dirs, void (^handler)(const char* pathInDir, const char* leafName)) const;
    bool                getDylibInfo(const char* dylibPath, dyld3::Platform platform, const GradedArchs& archs, uint32_t& version, char installName[PATH_MAX]) const;
    bool                isContainerized(const char* homeDir) const;
    bool                isMaybeContainerized(const char* homeDir) const;
    bool                fileExists(const char* path, FileID* fileID=nullptr, int* errNum=nullptr) const;
    bool                dirExists(const char* path) const;
    bool                mkdirs(const char* path) const;
    bool                realpath(const char* filePath, char output[1024]) const;
    bool                realpathdir(const char* dirPath, char output[1024]) const;
    const void*         mapFileReadOnly(Diagnostics& diag, const char* path, size_t* size=nullptr, FileID* fileID=nullptr, bool* isOSBinary=nullptr, char* betterPath=nullptr) const;
    void                unmapFile(const void* buffer, size_t size) const;
    void                withReadOnlyMappedFile(Diagnostics& diag, const char* path, bool checkIfOSBinary, void (^handler)(const void* mapping, size_t mappedSize, bool isOSBinary, const FileID& fileID, const char* realPath)) const;
    bool                getFileAttribute(const char* path, const char* attrName, Array<uint8_t>& attributeBytes) const;
    bool                setFileAttribute(const char* path, const char* attrName, const Array<uint8_t>& attributeBytes) const;
    bool                saveFileWithAttribute(Diagnostics& diag, const char* path, const void* buffer, size_t size, const char* attrName, const Array<uint8_t>& attributeBytes) const;
    bool                sandboxBlockedMmap(const char* path) const;
    bool                sandboxBlockedOpen(const char* path) const;
    bool                sandboxBlockedStat(const char* path) const;
    bool                sandboxBlocked(const char* path, const char* kind) const;
    bool                sandboxBlockedSyscall(int syscallNum) const;
    bool                sandboxBlockedPageInLinking() const;
    bool                getpath(int fd, char* realerPath) const;
    bool                onHaswell() const;
    bool                dtraceUserProbesEnabled() const;
    void                dtraceRegisterUserProbes(dof_ioctl_data_t* probes) const;
    void                dtraceUnregisterUserProbe(int registeredID) const;
    bool                kernelDyldImageInfoAddress(void*& infoAddress, size_t& infoSize) const;

    struct DyldCommPage
    {
        uint64_t  forceCustomerCache        :  1,     // dyld_flags=0x00000001
                  testMode                  :  1,     // dyld_flags=0x00000002
                  forceDevCache             :  1,     // dyld_flags=0x00000004
                  skipIgnition              :  1,     // dyld_flags=0x00000008
                  useSystemCache            :  1,     // dyld_flags=0x00000010
                  useSystemDriverKitCache   :  1,     // dyld_flags=0x00000020
                  unusedFlagsLow            : 12,
                  enableCompactInfo         :  1,     // dyld_flags=0x00040000
                  disableCompactInfo        :  1,     // dyld_flags=0x00080000
                  forceRODataConst          :  1,     // dyld_flags=0x00100000
                  forceRWDataConst          :  1,     // dyld_flags=0x00200000
                  unusedFlagsHigh           :  10,
                  libPlatformRoot           :  1,
                  libPthreadRoot            :  1,
                  libKernelRoot             :  1,
                  bootVolumeWritable        :  1,
                  foundRoot                 :  1,
                  unused3                   : 27;

        // The comm page flags are divided in two.  The low 32-bits are owned by
        // the boot-arg.  The high 32-bit should be set by launchd (pid 1).  Due to
        // macOS pivoting, there are 2 pid 1's at boot, so we need to reset these bits
        // to avoid the first pid 1's decision leaking in to the second
        enum Masks : uint64_t {
            bootArgsMask = 0x00000000FFFFFFFFULL,
        };

                  DyldCommPage();
    };
    static_assert(sizeof(DyldCommPage) == sizeof(uint64_t));



    DyldCommPage        dyldCommPageFlags() const;
    void                setDyldCommPageFlags(DyldCommPage value) const;
    bool                bootVolumeWritable() const;

    // posix level
    int                 getpid() const;
    int                 open(const char* path, int flag, int other) const;
    int                 close(int) const;
    ssize_t             pread(int fd, void* bufer, size_t len, size_t offset) const;
    ssize_t             pwrite(int fd, const void* bufer, size_t len, size_t offset) const;
    int                 mprotect(void* start, size_t, int prot) const;
    int                 unlink(const char* path) const;
    int                 fcntl(int fd, int cmd, void*) const;
    int                 fstat(int fd, struct stat* buf) const;
    int                 stat(const char* path, struct stat* buf) const;
    void*               mmap(void* addr, size_t len, int prot, int flags, int fd, size_t offset) const;
    int                 munmap(void* addr, size_t len) const;
    int                 socket(int domain, int type, int protocol) const;
    int                 connect(int socket, const struct sockaddr* address, socklen_t address_len) const;
    kern_return_t       vm_protect(task_port_t, vm_address_t, vm_size_t, bool which, uint32_t perms) const;
    int                 mremap_encrypted(void*, size_t len, uint32_t, uint32_t cpuType, uint32_t cpuSubtype) const;
    ssize_t             fsgetpath(char result[], size_t resultBufferSize, uint64_t fsid, uint64_t objid) const;
    int                 getfsstat(struct statfs *buf, int bufsize, int flags) const;
    int                 getattrlist(const char* path, struct attrlist * attrList, void * attrBuf, size_t attrBufSize, uint32_t options)
    const ;

#if !BUILDING_DYLD
    typedef std::map<std::string, std::vector<const char*>> PathToPathList;
    struct VersionAndInstallName { uint32_t version; const char* installName; };
    typedef std::map<std::string, VersionAndInstallName> PathToDylibInfo;
    typedef std::map<uint64_t, std::string> FileIDsToPath;

    static uint64_t     makeFsIdPair(uint64_t fsid, uint64_t objid) { return (fsid << 32) |  objid; }

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    struct MappingInfo { const void* mappingStart; size_t mappingSize; };
    typedef std::map<std::string_view, MappingInfo> PathToMapping;
#endif

    uint64_t                _amfiFlags       = -1;
    DyldCommPage            _commPageFlags;
    bool                    _internalInstall = false;
    int                     _pid             = 100;
    const char*             _cwd             = nullptr;
    PathToPathList          _dirMap;
    const DyldSharedCache*  _dyldCache       = nullptr;
    PathToDylibInfo         _dylibInfoMap;
    FileIDsToPath           _fileIDsToPath;
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    PathToMapping           _mappedOtherDylibs;
    const GradedArchs*      _gradedArchs    = nullptr;
#endif

#if BUILDING_CLOSURE_UTIL || BUILDING_SHARED_CACHE_UTIL
    // An alternative root path to use.  This will not fall back to /
    // Note this must be a real path
    const char*             _rootPath       = nullptr;
    // An overlay to layer on top of the root path.  It must be a real path
    const char*             _overlayPath    = nullptr;
#endif

#if BUILDING_UNIT_TESTS
    bool                    _bypassMockFS   = false;
#endif

#endif // !BUILDING_DYLD
};


} // namespace



#endif /* DyldDelegates_h */
