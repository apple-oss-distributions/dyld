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
#include <sys/stat.h> 
#include <sys/types.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sys/dtrace.h>
#include <sys/errno.h>
#include <unistd.h>
#include <System/sys/mman.h>
#include <System/sys/csr.h>
#include <System/machine/cpu_capabilities.h>
#include <bootstrap.h>
#include <CommonCrypto/CommonDigest.h>
#include <sandbox.h>
#include <sandbox/private.h>
#include <dispatch/dispatch.h>

#include "LaunchCache.h"
#include "LaunchCacheFormat.h"
#include "Logging.h"
#include "Loading.h"
#include "MachOParser.h"
#include "dyld.h"
#include "dyld_cache_format.h"

extern "C" {
    #include "closuredProtocol.h"
}

namespace dyld {
    void log(const char* m, ...);
}

namespace dyld3 {
namespace loader {

#if DYLD_IN_PROCESS

static bool sandboxBlocked(const char* path, const char* kind)
{
#if BUILDING_LIBDYLD || !TARGET_IPHONE_SIMULATOR
    sandbox_filter_type filter = (sandbox_filter_type)(SANDBOX_FILTER_PATH | SANDBOX_CHECK_NO_REPORT);
    return ( sandbox_check(getpid(), kind, filter, path) > 0 );
#else
    // sandbox calls not yet supported in dyld_sim
    return false;
#endif
}

static bool sandboxBlockedMmap(const char* path)
{
    return sandboxBlocked(path, "file-map-executable");
}

static bool sandboxBlockedOpen(const char* path)
{
    return sandboxBlocked(path, "file-read-data");
}

static bool sandboxBlockedStat(const char* path)
{
    return sandboxBlocked(path, "file-read-metadata");
}

#if TARGET_OS_WATCH || TARGET_OS_BRIDGE
static uint64_t pageAlign(uint64_t value)
{
  #if __arm64__
    return (value + 0x3FFF) & (-0x4000);
  #else
	return (value + 0xFFF) & (-0x1000);
  #endif
}
#endif

static void updateSliceOffset(uint64_t& sliceOffset, uint64_t codeSignEndOffset, size_t fileLen)
{
#if TARGET_OS_WATCH || TARGET_OS_BRIDGE
    if ( sliceOffset != 0 ) {
        if ( pageAlign(codeSignEndOffset) == pageAlign(fileLen) ) {
            // cache builder saw fat file, but file is now thin
            sliceOffset = 0;
            return;
        }
    }
#endif
}

static const mach_header* mapImage(const dyld3::launch_cache::Image image, Diagnostics& diag, LogFunc log_loads, LogFunc log_segments)
{
    uint64_t       sliceOffset        = image.sliceOffsetInFile();
    const uint64_t totalVMSize        = image.vmSizeToMap();
    const uint32_t codeSignFileOffset = image.asDiskImage()->codeSignFileOffset;
    const uint32_t codeSignFileSize   = image.asDiskImage()->codeSignFileSize;

    // open file
    int fd = ::open(image.path(), O_RDONLY, 0);
    if ( fd == -1 ) {
        int openErr = errno;
        if ( (openErr == EPERM) && sandboxBlockedOpen(image.path()) )
            diag.error("file system sandbox blocked open(\"%s\", O_RDONLY)", image.path());
        else
            diag.error("open(\"%s\", O_RDONLY) failed with errno=%d", image.path(), openErr);
        return nullptr;
    }

    // get file info
    struct stat statBuf;
#if TARGET_IPHONE_SIMULATOR
    if ( stat(image.path(), &statBuf) != 0 ) {
#else
    if ( fstat(fd, &statBuf) != 0 ) {
#endif
        int statErr = errno;
        if ( (statErr == EPERM) && sandboxBlockedStat(image.path()) )
            diag.error("file system sandbox blocked stat(\"%s\")", image.path());
        else
           diag.error("stat(\"%s\") failed with errno=%d", image.path(), statErr);
        close(fd);
        return nullptr;
    }

    // verify file has not changed since closure was built
    if ( image.validateUsingModTimeAndInode() ) {
        if ( (statBuf.st_mtime != image.fileModTime()) || (statBuf.st_ino != image.fileINode()) ) {
            diag.error("file mtime/inode changed since closure was built for '%s'", image.path());
            close(fd);
            return nullptr;
        }
    }

    // handle OS dylibs being thinned after closure was built
    if ( image.group().groupNum() == 1 )
        updateSliceOffset(sliceOffset, codeSignFileOffset+codeSignFileSize, (size_t)statBuf.st_size);

    // register code signature
    uint64_t coveredCodeLength  = UINT64_MAX;
    if ( codeSignFileOffset != 0 ) {
        fsignatures_t siginfo;
        siginfo.fs_file_start  = sliceOffset;                           // start of mach-o slice in fat file
        siginfo.fs_blob_start  = (void*)(long)(codeSignFileOffset);     // start of CD in mach-o file
        siginfo.fs_blob_size   = codeSignFileSize;                      // size of CD
        int result = fcntl(fd, F_ADDFILESIGS_RETURN, &siginfo);
        if ( result == -1 ) {
            int errnoCopy = errno;
            if ( (errnoCopy == EPERM) || (errnoCopy == EBADEXEC) ) {
                diag.error("code signature invalid (errno=%d) sliceOffset=0x%08llX, codeBlobOffset=0x%08X, codeBlobSize=0x%08X for '%s'",
                            errnoCopy, sliceOffset, codeSignFileOffset, codeSignFileSize, image.path());
            }
            else {
                diag.error("fcntl(fd, F_ADDFILESIGS_RETURN) failed with errno=%d, sliceOffset=0x%08llX, codeBlobOffset=0x%08X, codeBlobSize=0x%08X for '%s'",
                            errnoCopy, sliceOffset, codeSignFileOffset, codeSignFileSize, image.path());
            }
            close(fd);
            return nullptr;
        }
        coveredCodeLength = siginfo.fs_file_start;
        if ( coveredCodeLength <  image.asDiskImage()->codeSignFileOffset ) {
            diag.error("code signature does not cover entire file up to signature");
            close(fd);
            return nullptr;
        }

        // <rdar://problem/32684903> always call F_CHECK_LV to preflight
        fchecklv checkInfo;
        char  messageBuffer[512];
        messageBuffer[0] = '\0';
        checkInfo.lv_file_start = sliceOffset;
        checkInfo.lv_error_message_size = sizeof(messageBuffer);
        checkInfo.lv_error_message = messageBuffer;
        int res = fcntl(fd, F_CHECK_LV, &checkInfo);
        if ( res == -1 ) {
             diag.error("code signature in (%s) not valid for use in process: %s", image.path(), messageBuffer);
             close(fd);
             return nullptr;
        }
    }

    // reserve address range
    vm_address_t loadAddress = 0;
    kern_return_t r = vm_allocate(mach_task_self(), &loadAddress, (vm_size_t)totalVMSize, VM_FLAGS_ANYWHERE);
    if ( r != KERN_SUCCESS ) {
        diag.error("vm_allocate(size=0x%0llX) failed with result=%d", totalVMSize, r);
        close(fd);
        return nullptr;
    }

    if ( sliceOffset != 0 )
        log_segments("dyld: Mapping %s (slice offset=%llu)\n", image.path(), sliceOffset);
    else
        log_segments("dyld: Mapping %s\n", image.path());

    // map each segment
    __block bool mmapFailure = false;
    __block const uint8_t* codeSignatureStartAddress = nullptr;
    __block const uint8_t* linkeditEndAddress = nullptr;
    __block bool mappedFirstSegment = false;
    image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
        // <rdar://problem/32363581> Mapping zero filled segments fails with mmap of size 0
        if ( fileSize == 0 )
            return;
        void* segAddress = mmap((void*)(loadAddress+vmOffset), fileSize, permissions, MAP_FIXED | MAP_PRIVATE, fd, sliceOffset+fileOffset);
        int mmapErr = errno;
        if ( segAddress == MAP_FAILED ) {
            if ( mmapErr == EPERM ) {
                if ( sandboxBlockedMmap(image.path()) )
                    diag.error("file system sandbox blocked mmap() of '%s'", image.path());
                else
                    diag.error("code signing blocked mmap() of '%s'", image.path());
            }
            else {
                diag.error("mmap(addr=0x%0llX, size=0x%08X) failed with errno=%d for %s", loadAddress+vmOffset, fileSize, mmapErr, image.path());
            }
            mmapFailure = true;
            stop = true;
        }
        else if ( codeSignFileOffset > fileOffset ) {
            codeSignatureStartAddress = (uint8_t*)segAddress + (codeSignFileOffset-fileOffset);
            linkeditEndAddress = (uint8_t*)segAddress + vmSize;
        }
        // sanity check first segment is mach-o header
        if ( (segAddress != MAP_FAILED) && !mappedFirstSegment ) {
            mappedFirstSegment = true;
            if ( !MachOParser::isMachO(diag, segAddress, fileSize) ) {
                mmapFailure = true;
                stop = true;
            }
        }
        if ( !mmapFailure ) {
            MachOParser parser((mach_header*)loadAddress);
            log_segments("%14s (%c%c%c) 0x%012lX->0x%012lX \n", parser.segmentName(segIndex),
                         (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                         (long)segAddress, (long)segAddress+vmSize-1);
        }
    });
    if ( mmapFailure ) {
        vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
        close(fd);
        return nullptr;
    }

    // close file
    close(fd);

 #if BUILDING_LIBDYLD
    // verify file has not changed since closure was built by checking code signature has not changed
    if ( image.validateUsingCdHash() ) {
        if ( codeSignatureStartAddress == nullptr ) {
            diag.error("code signature missing");
        }
        else if ( codeSignatureStartAddress+codeSignFileSize > linkeditEndAddress ) {
            diag.error("code signature extends beyond end of __LINKEDIT");
        }
        else {
            uint8_t cdHash[20];
            if ( MachOParser::cdHashOfCodeSignature(codeSignatureStartAddress, codeSignFileSize, cdHash) ) {
                if ( memcmp(image.cdHash16(), cdHash, 16) != 0 )
                    diag.error("code signature changed since closure was built");
            }
            else{
                diag.error("code signature format invalid");
            }
        }
        if ( diag.hasError() ) {
            vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
            return nullptr;
        }
    }
#endif

#if __IPHONE_OS_VERSION_MIN_REQUIRED && !TARGET_IPHONE_SIMULATOR
    // tell kernel about fairplay encrypted regions
    uint32_t fpTextOffset;
    uint32_t fpSize;
    if ( image.isFairPlayEncrypted(fpTextOffset, fpSize) ) {
        const mach_header* mh = (mach_header*)loadAddress;
        int result = mremap_encrypted(((uint8_t*)mh) + fpTextOffset, fpSize, 1, mh->cputype, mh->cpusubtype);
        diag.error("could not register fairplay decryption, mremap_encrypted() => %d", result);
        vm_deallocate(mach_task_self(), loadAddress, (vm_size_t)totalVMSize);
        return nullptr;
    }
#endif

    log_loads("dyld: load %s\n", image.path());

    return (mach_header*)loadAddress;
}


void unmapImage(const launch_cache::binary_format::Image* binImage, const mach_header* loadAddress)
{
    assert(loadAddress != nullptr);
    launch_cache::Image image(binImage);
    vm_deallocate(mach_task_self(), (vm_address_t)loadAddress, (vm_size_t)(image.vmSizeToMap()));
}


static void applyFixupsToImage(Diagnostics& diag, const mach_header* imageMH, const launch_cache::binary_format::Image* imageData,
                               launch_cache::TargetSymbolValue::LoadedImages& imageResolver, LogFunc log_fixups)
{
    launch_cache::Image image(imageData);
    MachOParser imageParser(imageMH);
    // Note, these are cached here to avoid recalculating them on every loop iteration
    const launch_cache::ImageGroup& imageGroup = image.group();
    const char* leafName = image.leafName();
    intptr_t slide = imageParser.getSlide();
    image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t protections, bool& segStop) {
        if ( !image.segmentHasFixups(segIndex) )
            return;
        const launch_cache::MemoryRange segContent = { (char*)imageMH + vmOffset, vmSize };
    #if __i386__
        bool textRelocs = ((protections & VM_PROT_WRITE) == 0);
        if ( textRelocs ) {
            kern_return_t r = vm_protect(mach_task_self(), (vm_address_t)segContent.address, (vm_size_t)segContent.size, false, VM_PROT_WRITE | VM_PROT_READ);
            if ( r != KERN_SUCCESS ) {
                diag.error("vm_protect() failed trying to make text segment writable, result=%d", r);
                return;
            }
        }
    #else
        if ( (protections & VM_PROT_WRITE) == 0 ) {
            diag.error("fixups found in non-writable segment of %s", image.path());
            return;
        }
    #endif
        image.forEachFixup(segIndex, segContent, ^(uint64_t segOffset, launch_cache::Image::FixupKind kind, launch_cache::TargetSymbolValue targetValue, bool& stop) {
            if ( segOffset > segContent.size ) {
                diag.error("fixup is past end of segment. segOffset=0x%0llX, segSize=0x%0llX, segIndex=%d", segOffset, segContent.size, segIndex);
                stop = true;
                return;
            }
            uintptr_t* fixUpLoc = (uintptr_t*)((char*)(segContent.address) + segOffset);
            uintptr_t value;
        #if __i386__
            uint32_t rel32;
            uint8_t* jumpSlot;
        #endif
            //dyld::log("fixup loc=%p\n", fixUpLoc);
            switch ( kind ) {
        #if __LP64__
                case launch_cache::Image::FixupKind::rebase64:
        #else
                case launch_cache::Image::FixupKind::rebase32:
        #endif
                    *fixUpLoc += slide;
                    log_fixups("dyld: fixup: %s:%p += %p\n", leafName, fixUpLoc, (void*)slide);
                    break;
        #if __LP64__
                case launch_cache::Image::FixupKind::bind64:
        #else
                case launch_cache::Image::FixupKind::bind32:
        #endif
                    value = targetValue.resolveTarget(diag, imageGroup, imageResolver);
                    log_fixups("dyld: fixup: %s:%p = %p\n", leafName, fixUpLoc, (void*)value);
                    *fixUpLoc = value;
                    break;
        #if __i386__
            case launch_cache::Image::FixupKind::rebaseText32:
                log_fixups("dyld: text fixup: %s:%p += %p\n", leafName, fixUpLoc, (void*)slide);
                *fixUpLoc += slide;
                break;
            case launch_cache::Image::FixupKind::bindText32:
                value = targetValue.resolveTarget(diag, imageGroup, imageResolver);
                log_fixups("dyld: text fixup: %s:%p = %p\n", leafName, fixUpLoc, (void*)value);
                *fixUpLoc = value;
                break;
            case launch_cache::Image::FixupKind::bindTextRel32:
                // CALL instruction uses pc-rel value
                value = targetValue.resolveTarget(diag, imageGroup, imageResolver);
                log_fixups("dyld: CALL fixup: %s:%p = %p (pc+0x%08X)\n", leafName, fixUpLoc, (void*)value, (value - (uintptr_t)(fixUpLoc)));
                *fixUpLoc = (value - (uintptr_t)(fixUpLoc));
                break;
           case launch_cache::Image::FixupKind::bindImportJmp32:
                // JMP instruction in __IMPORT segment uses pc-rel value
                jumpSlot = (uint8_t*)fixUpLoc;
                value = targetValue.resolveTarget(diag, imageGroup, imageResolver);
                rel32 = (value - ((uintptr_t)(fixUpLoc)+5));
                log_fixups("dyld: JMP fixup: %s:%p = %p (pc+0x%08X)\n", leafName, fixUpLoc, (void*)value, rel32);
                jumpSlot[0] = 0xE9; // JMP rel32
                jumpSlot[1] = rel32 & 0xFF;
                jumpSlot[2] = (rel32 >> 8) & 0xFF;
                jumpSlot[3] = (rel32 >> 16) & 0xFF;
                jumpSlot[4] = (rel32 >> 24) & 0xFF;
                break;
        #endif
            default:
                diag.error("unknown fixup kind %d", kind);
            }
            if ( diag.hasError() )
                stop = true;
        });
    #if __i386__
        if ( textRelocs ) {
            kern_return_t r = vm_protect(mach_task_self(), (vm_address_t)segContent.address, (vm_size_t)segContent.size, false, protections);
            if ( r != KERN_SUCCESS ) {
                diag.error("vm_protect() failed trying to make text segment non-writable, result=%d", r);
                return;
            }
        }
    #endif
    });
}



class VIS_HIDDEN CurrentLoadImages : public launch_cache::TargetSymbolValue::LoadedImages
{
public:
                                CurrentLoadImages(launch_cache::DynArray<ImageInfo>& images, const uint8_t* cacheAddr)
                                    : _dyldCacheLoadAddress(cacheAddr), _images(images) { }

    virtual const uint8_t*      dyldCacheLoadAddressForImage();
    virtual const mach_header*  loadAddressFromGroupAndIndex(uint32_t groupNum, uint32_t indexInGroup);
    virtual void                forEachImage(void (^handler)(uint32_t anIndex, const launch_cache::binary_format::Image*, const mach_header*, bool& stop));
    virtual void                setAsNeverUnload(uint32_t anIndex) { _images[anIndex].neverUnload = true; }
private:
    const uint8_t*                      _dyldCacheLoadAddress;
    launch_cache::DynArray<ImageInfo>&  _images;
};

const uint8_t* CurrentLoadImages::dyldCacheLoadAddressForImage()
{
    return _dyldCacheLoadAddress;
}

const mach_header* CurrentLoadImages::loadAddressFromGroupAndIndex(uint32_t groupNum, uint32_t indexInGroup)
{
    __block const mach_header* result = nullptr;
    forEachImage(^(uint32_t anIndex, const launch_cache::binary_format::Image* imageData, const mach_header* mh, bool& stop) {
        launch_cache::Image         image(imageData);
        launch_cache::ImageGroup    imageGroup = image.group();
        if ( imageGroup.groupNum() != groupNum )
            return;
        if ( imageGroup.indexInGroup(imageData) == indexInGroup ) {
            result = mh;
            stop = true;
        }
    });
    return result;
}

void CurrentLoadImages::forEachImage(void (^handler)(uint32_t anIndex, const launch_cache::binary_format::Image*, const mach_header*, bool& stop))
{
    bool stop = false;
    for (int i=0; i < _images.count(); ++i) {
        ImageInfo& info = _images[i];
        handler(i, info.imageData, info.loadAddress, stop);
        if ( stop )
            break;
    }
}

struct DOFInfo {
    const void*            dof;
    const mach_header*     imageHeader;
    const char*            imageShortName;
};

static void registerDOFs(const DOFInfo* dofs, uint32_t dofSectionCount, LogFunc log_dofs)
{
    if ( dofSectionCount != 0 ) {
        int fd = open("/dev/" DTRACEMNR_HELPER, O_RDWR);
        if ( fd < 0 ) {
            log_dofs("can't open /dev/" DTRACEMNR_HELPER " to register dtrace DOF sections\n");
        }
        else {
            // allocate a buffer on the stack for the variable length dof_ioctl_data_t type
            uint8_t buffer[sizeof(dof_ioctl_data_t) + dofSectionCount*sizeof(dof_helper_t)];
            dof_ioctl_data_t* ioctlData = (dof_ioctl_data_t*)buffer;

            // fill in buffer with one dof_helper_t per DOF section
            ioctlData->dofiod_count = dofSectionCount;
            for (unsigned int i=0; i < dofSectionCount; ++i) {
                strlcpy(ioctlData->dofiod_helpers[i].dofhp_mod, dofs[i].imageShortName, DTRACE_MODNAMELEN);
                ioctlData->dofiod_helpers[i].dofhp_dof = (uintptr_t)(dofs[i].dof);
                ioctlData->dofiod_helpers[i].dofhp_addr = (uintptr_t)(dofs[i].dof);
            }

            // tell kernel about all DOF sections en mas
            // pass pointer to ioctlData because ioctl() only copies a fixed size amount of data into kernel
            user_addr_t val = (user_addr_t)(unsigned long)ioctlData;
            if ( ioctl(fd, DTRACEHIOC_ADDDOF, &val) != -1 ) {
                // kernel returns a unique identifier for each section in the dofiod_helpers[].dofhp_dof field.
                // Note, the closure marked the image as being never unload, so we don't need to keep the ID around
                // or support unregistering it later.
                for (unsigned int i=0; i < dofSectionCount; ++i) {
                    log_dofs("dyld: registering DOF section %p in %s with dtrace, ID=0x%08X\n",
                             dofs[i].dof, dofs[i].imageShortName, (int)(ioctlData->dofiod_helpers[i].dofhp_dof));
                }
            }
            else {
                //dyld::log( "dyld: ioctl to register dtrace DOF section failed\n");
            }
            close(fd);
        }
    }
}


void mapAndFixupImages(Diagnostics& diag, launch_cache::DynArray<ImageInfo>& images, const uint8_t* cacheLoadAddress,
                       LogFunc log_loads, LogFunc log_segments, LogFunc log_fixups, LogFunc log_dofs)
{
    // scan array and map images not already loaded
    for (int i=0; i < images.count(); ++i) {
        ImageInfo& info = images[i];
        const dyld3::launch_cache::Image image(info.imageData);
        if ( info.loadAddress != nullptr ) {
            // log main executable's segments
            if ( (info.groupNum == 2) && (info.loadAddress->filetype == MH_EXECUTE) && !info.previouslyFixedUp ) {
                if ( log_segments("dyld: mapped by kernel %s\n", image.path()) ) {
                    MachOParser parser(info.loadAddress);
                    image.forEachDiskSegment(^(uint32_t segIndex, uint32_t fileOffset, uint32_t fileSize, int64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool& stop) {
                        uint64_t start = (long)info.loadAddress + vmOffset;
                        uint64_t end   = start+vmSize-1;
                        if ( (segIndex == 0) && (permissions == 0) ) {
                            start = 0;
                        }
                        log_segments("%14s (%c%c%c) 0x%012llX->0x%012llX \n", parser.segmentName(segIndex),
                                    (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                                    start, end);
                    });
                }
            }
            // skip over ones already loaded
            continue;
        }
        if ( image.isDiskImage() ) {
            //dyld::log("need to load image[%d] %s\n", i, image.path());
            info.loadAddress = mapImage(image, diag, log_loads, log_segments);
            if ( diag.hasError() ) {
                break; // out of for loop
            }
            info.justMapped = true;
        }
        else {
            bool expectedOnDisk   = image.group().dylibsExpectedOnDisk();
            bool overridableDylib = image.overridableDylib();
            if ( expectedOnDisk || overridableDylib ) {
                struct stat statBuf;
                if ( ::stat(image.path(), &statBuf) == 0 ) {
                    if ( expectedOnDisk ) {
                        // macOS case: verify dylib file info matches what it was when cache was built
                        if ( image.fileModTime() != statBuf.st_mtime ) {
                            diag.error("cached dylib mod-time has changed, dylib cache has: 0x%08llX, file has: 0x%08lX, for: %s", image.fileModTime(), (long)statBuf.st_mtime, image.path());
                            break; // out of for loop
                        }
                         if ( image.fileINode() != statBuf.st_ino ) {
                            diag.error("cached dylib inode has changed, dylib cache has: 0x%08llX, file has: 0x%08llX, for: %s", image.fileINode(), statBuf.st_ino, image.path());
                            break; // out of for loop
                        }
                   }
                    else {
                        // iOS internal: dylib override installed
                        diag.error("cached dylib overridden: %s", image.path());
                        break; // out of for loop
                    }
                }
                else {
                    if ( expectedOnDisk ) {
                        // macOS case: dylib that existed when cache built no longer exists
                        diag.error("missing cached dylib: %s", image.path());
                        break; // out of for loop
                    }
                }
            }
            info.loadAddress = (mach_header*)(cacheLoadAddress + image.cacheOffset());
            info.justUsedFromDyldCache = true;
            if ( log_segments("dyld: Using from dyld cache %s\n", image.path()) ) {
                MachOParser parser(info.loadAddress);
                image.forEachCacheSegment(^(uint32_t segIndex, uint64_t vmOffset, uint64_t vmSize, uint8_t permissions, bool &stop) {
                    log_segments("%14s (%c%c%c) 0x%012lX->0x%012lX \n", parser.segmentName(segIndex),
                                    (permissions & PROT_READ) ? 'r' : '.', (permissions & PROT_WRITE) ? 'w' : '.', (permissions & PROT_EXEC) ? 'x' : '.' ,
                                    (long)cacheLoadAddress+vmOffset, (long)cacheLoadAddress+vmOffset+vmSize-1);
                 });
            }
        }
    }
    if ( diag.hasError() )  {
        // back out and unmapped images all loaded so far
        for (uint32_t j=0; j < images.count(); ++j) {
            ImageInfo& anInfo = images[j];
            if ( anInfo.justMapped )
                 unmapImage(anInfo.imageData, anInfo.loadAddress);
            anInfo.loadAddress = nullptr;
        }
        return;
    }

    // apply fixups
    CurrentLoadImages fixupHelper(images, cacheLoadAddress);
    for (int i=0; i < images.count(); ++i) {
        ImageInfo& info = images[i];
        // images in shared cache do not need fixups applied
        launch_cache::Image image(info.imageData);
        if ( !image.isDiskImage() )
            continue;
        // previously loaded images were previously fixed up
        if ( info.previouslyFixedUp )
            continue;
        //dyld::log("apply fixups to mh=%p, path=%s\n", info.loadAddress, Image(info.imageData).path());
        dyld3::loader::applyFixupsToImage(diag, info.loadAddress, info.imageData, fixupHelper, log_fixups);
        if ( diag.hasError() )
            break;
    }

    // Record dtrace DOFs
    // if ( /* FIXME! register dofs */ )
    {
        __block uint32_t dofCount = 0;
        for (int i=0; i < images.count(); ++i) {
            ImageInfo& info = images[i];
            launch_cache::Image image(info.imageData);
            // previously loaded images were previously fixed up
            if ( info.previouslyFixedUp )
                continue;
            image.forEachDOF(nullptr, ^(const void* section) {
                // DOFs cause the image to be never-unload
                assert(image.neverUnload());
                ++dofCount;
            });
        }

        // struct RegisteredDOF { const mach_header* mh; int registrationID; };
        DOFInfo dofImages[dofCount];
        __block DOFInfo* dofImagesBase = dofImages;
        dofCount = 0;
        for (int i=0; i < images.count(); ++i) {
            ImageInfo& info = images[i];
            launch_cache::Image image(info.imageData);
            // previously loaded images were previously fixed up
            if ( info.previouslyFixedUp )
                continue;
            image.forEachDOF(info.loadAddress, ^(const void* section) {
                DOFInfo dofInfo;
                dofInfo.dof            = section;
                dofInfo.imageHeader    = info.loadAddress;
                dofInfo.imageShortName = image.leafName();
                dofImagesBase[dofCount++] = dofInfo;
            });
        }
        registerDOFs(dofImages, dofCount, log_dofs);
    }
}

#if BUILDING_DYLD
void forEachLineInFile(const char* path, void (^lineHandler)(const char* line, bool& stop))
{
    int fd = dyld::my_open(path, O_RDONLY, 0);
    if ( fd != -1 ) {
        struct stat statBuf;
        if ( fstat(fd, &statBuf) == 0 ) {
            const char* lines = (const char*)mmap(nullptr, (size_t)statBuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if ( lines != MAP_FAILED ) {
                bool stop = false;
                const char* const eof = &lines[statBuf.st_size];
                for (const char* s = lines; s < eof; ++s) {
                    char lineBuffer[MAXPATHLEN];
                    char* t = lineBuffer;
                    char* tEnd = &lineBuffer[MAXPATHLEN];
                    while ( (s < eof) && (t != tEnd) ) {
                        if ( *s == '\n' )
                            break;
                        *t++ = *s++;
                    }
                    *t = '\0';
                    lineHandler(lineBuffer, stop);
                    if ( stop )
                        break;
                }
                munmap((void*)lines, (size_t)statBuf.st_size);
            }
        }
        close(fd);
    }
}


bool internalInstall()
{
#if TARGET_IPHONE_SIMULATOR
    return false;
#elif __IPHONE_OS_VERSION_MIN_REQUIRED
    uint32_t devFlags = *((uint32_t*)_COMM_PAGE_DEV_FIRM);
    return ( (devFlags & 1) == 1 );
#else
    return ( csr_check(CSR_ALLOW_APPLE_INTERNAL) == 0 );
#endif
}

/* Checks to see if there are any args that impact dyld. These args
 * can be set sevaral ways. These will only be honored on development
 * and Apple Internal builds.
 *
 * First the existence of a file is checked for:
 *    /S/L/C/com.apple.dyld/dyld-bootargs
 * If it exists it will be mapped and scanned line by line. If the executable
 * exists in the file then the arguments on its line will be applied. "*" may
 * be used a wildcard to represent all apps. First matching line will be used,
 * the wild card must be one the last line. Additionally, lines must end with
 * a "\n"
 *
 *
 * SAMPLE FILE:

 /bin/ls:force_dyld2=1
 /usr/bin/sw_vers:force_dyld2=1
*:force_dyld3=1
EOL

 If no file exists then the kernel boot-args will be scanned.
 */
bool bootArgsContains(const char* arg)
{
    //FIXME: Use strnstr(). Unfortunately we are missing an imp libc.
#if TARGET_IPHONE_SIMULATOR
    return false;
#else
    // don't check for boot-args on customer installs
    if ( !internalInstall() )
        return false;

    char pathBuffer[MAXPATHLEN+1];
#if __IPHONE_OS_VERSION_MIN_REQUIRED
    strlcpy(pathBuffer, IPHONE_DYLD_SHARED_CACHE_DIR, sizeof(IPHONE_DYLD_SHARED_CACHE_DIR));
#else
    strlcpy(pathBuffer, MACOSX_DYLD_SHARED_CACHE_DIR, sizeof(MACOSX_DYLD_SHARED_CACHE_DIR));
#endif
    strlcat(pathBuffer, "dyld-bootargs", MAXPATHLEN+1);
    __block bool result = false;
    forEachLineInFile(pathBuffer, ^(const char* line, bool& stop) {
        const char* delim = strchr(line, ':');
        if ( delim == nullptr )
            return;
        char binary[MAXPATHLEN];
        char options[MAXPATHLEN];
        strlcpy(binary, line, MAXPATHLEN);
        binary[delim-line] = '\0';
        strlcpy(options, delim+1, MAXPATHLEN);
        if ( (strcmp(dyld::getExecutablePath(), binary) == 0) || (strcmp("*", binary) == 0) ) {
            result = (strstr(options, arg) != nullptr);
            return;
        }
    });

    // get length of full boot-args string
    size_t len;
    if ( sysctlbyname("kern.bootargs", NULL, &len, NULL, 0) != 0 )
        return false;

    // get copy of boot-args string
    char bootArgsBuffer[len];
    if ( sysctlbyname("kern.bootargs", bootArgsBuffer, &len, NULL, 0) != 0 )
        return false;

    // return true if 'arg' is a sub-string of boot-args
    return (strstr(bootArgsBuffer, arg) != nullptr);
#endif
}
#endif

#if BUILDING_LIBDYLD
// hack because libdyld.dylib should not link with libc++.dylib
extern "C" void __cxa_pure_virtual() __attribute__((visibility("hidden")));
void __cxa_pure_virtual()
{
    abort();
}
#endif


#endif // DYLD_IN_PROCESS

} // namespace loader
} // namespace dyld3





