/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef EXTERNALLY_VIEWABLE_STATE_H
#define EXTERNALLY_VIEWABLE_STATE_H

#include <stdint.h>
#include <span>
#include <mach-o/dyld_images.h>
#include <TargetConditionals.h>

#include "Array.h"
#include "Defines.h"
#include "Allocator.h"
#include "MachOFile.h"
#include "Vector.h"

#if TARGET_OS_SIMULATOR
  #include "dyldSyscallInterface.h"
#endif

struct dyld_all_image_infos;

namespace dyld4 {
#if !TARGET_OS_EXCLAVEKIT
struct FileManager;
namespace Atlas {
  struct ProcessSnapshot;
};
#endif // !TARGET_OS_EXCLAVEKIT
/*
    ExternallyViewableState encapsulates everything about the dyld state of a process that other processes may query.

    Currently we have two data structures for external consumption: the traditional dyld_all_image_info struct and the new compact info.
    Everything about those two data structures is hidden behind ExternallyViewableState.  Nothing in dyld should directly interact
    with those data structures. Eventually we can phase out dyld_all_image_info, and only ExternallyViewableState will need to be changed.

    Some details:

    * dyld_all_image_info is statically allocated in dyld's __DATA segment. When the kernel loads dyld, the kernel finds dyld_all_image_info
    in dyld and save's off the address of dyld_all_image_info in the process's task structure. It can then be found by other process
    by using task_info(TASK_DYLD_INFO).

    * When dyld switches to dyld-in-the-cache it uses a new syscall to tell the kernel the new location of dyld_all_image_info which is
    in the dyld in the dyld cache. Once control is in the dyld-in-the-cache, it vm_deallocates the original dyld loaded from disk.

    * The way lldb is able to keep track of what images are loaded, is that lldb sets a break point on _dyld_debugger_notification()
    and dyld calls that function whenever the image list changes. During that breakpoint processing, lldb can then requery the image
    list from dyld. To handle when dyld switches over to the dyld-in-the-cache, dyld calls _dyld_debugger_notification(dyld_image_dyld_moved)
    which tells lldb to move the breakpoint to the the _dyld_debugger_notification symbol in the new dyld.

    * For simulator processes, the macOS kernel just loads the host (macOS) dyld which sees the main execuable is a simulator binary and
    DYLD_ROOT_PATH is set.  The host dyld then looks for $DYLD_ROOT_PATH/usr/lib/dyld_sim.  If it exists, it is loaded and dyld jumpts into
    it.  The host dyld remains loaded because dyld_sim has to run on multiple OS versions, so it cannot use syscalls. Instead the host
    macOS dyld supplies a table (SyscallHelpers) of function pointers for dyld_sim to use.  Additionally, dyld_sim does not carry its own
    dyld_all_image_info. Instead the host dyld passes it a pointer to the host's dyld_all_image_info, which dyld_sim writes to.

    * The dyld_all_image_info format never really changed over the years (just got fields added), so it was ok for dyld_sim to directly
    update the fields of dyld_all_image_info. The issue here is that you can run multiple iOS simulator OS versions on the same macOS
    host. If the dyld_all_image_info format changed each OS release, the host might not be able to interpret the dyld_all_image_info
    data.  We have that same additional constraint on the new compact info format (host macOS need to be able to parse any old format).

    * For clients (like Instruments.app) that are monitoring a process and want to get notified if an image is loaded or unloaded, we
    have the dyld_process_info_notify SPI.  Originally there was a complex mechanism of having the monitoring process poke mach_ports
    into the monitored process. But that has been simplified recently.  Now there is one field dyld_all_image_info.notifyPorts[0] that
    when poked to a magic value means that there is some process that is monitoring the current process, and dyld should make a new
    syscall to get an array of mach_ports to send notification messages to. That multiplexing is all encapsulated in RemoteNotificationResponder.

    * The mach_port poking and msg_sending all depends on the macOS host OS version.  Therefore, in the simulator dyld_sim cannot directly
    do the mach_msg work. Instead, dyld_sim calls back into the host (via the syscall table) when notifications are needed.

    * Compact info has two forms.  While dyld is starting up, information can be added to a ProcessSnapshot object.  Once the set of images
    has stablized, the ProcessSnapshot object is serialized to the compact form which is (currently) hung off the dyld_all_image_info struct
    and the ProcessSnapshot object is freed.  Note: the ProcessSnapshot object is allocated from the emphemeralAllocator, whereas the compact
    form is allocated form the persistenAllocator.
    For simulators, compact info is generated by the host dyld, with dyld_sim calling back into the host for updates.

    * At the end of any dlopen or dlclose that changes the image list, the compact is expanded back to a ProcessSnapshot object, then image list
    modified, then re-serialized to the compact form and atomically swapped into the dyld_all_image_info struct.

    * We try to notify the debugger about images as soon as possible, so that if there is any crashes processing the image, the debugger or
    crash log can show the image.


    Some areas that still need work:

    *** The __dyld4 section in dyld still contains a pointer to the dyld_all_image_info.  This is so that libdyld.dylib can find the dyld cache
    for dyld_process_info.

    *** Figures out if ktrace image/cache stuff should move into ExternallyViewableState

    *** The notify mach_msg requires a timestamp of the last update, which ProcessSnapshot does not track, so we are using the one in dyld_all_image_info

*/
class ExternallyViewableState
{
public:
    struct ImageInfo { uint64_t fsID=0; uint64_t fsObjID=0; const char* path=nullptr; const void* loadAddress=nullptr; bool inSharedCache=false; };
#if TARGET_OS_SIMULATOR
    void initSim(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, dyld3::Platform,
                 dyld_all_image_infos* hostAllImage, const dyld::SyscallHelpers* syscalls);
#else
    void initOld(lsl::Allocator& persistentAllocator, dyld3::Platform);
#if !TARGET_OS_EXCLAVEKIT
    void init(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, FileManager& fileManager, dyld3::Platform);
    void addImageInfo(lsl::Allocator& ephemeralAllocator, const ImageInfo& imageInfo);
#endif // !TARGET_OS_EXCLAVEKIT
#endif // TARGET_OS_SIMULATOR
    void addImageInfoOld(const ImageInfo& imageInfo, uint64_t timeStamp, uintptr_t mTime);
    void setDyldOld(const ImageInfo& dyldInfo);
    void setLibSystemInitializedOld();
    void setInitialImageCountOld(uint32_t);
    void addImagesOld(lsl::Vector<dyld_image_info>& oldStyleAdditions, uint64_t timeStamp);
    void addImagesOld(lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& images);
    void removeImagesOld(dyld3::Array<const char*> &pathsBuffer, dyld3::Array<const mach_header*>& unloadedMHs, std::span<const mach_header*>& mhs, uint64_t timeStamp);
    void removeImagesOld(std::span<const mach_header*>& mhs);
#if !TARGET_OS_EXCLAVEKIT
    void setDyld(lsl::Allocator& ephemeralAllocator, const ImageInfo& dyldInfo);
    void setLibSystemInitialized();
    void setInitialImageCount(uint32_t);
    void addImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& images);
    void removeImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, std::span<const mach_header*>& mhs);
    void setSharedCacheInfo(lsl::Allocator& ephemeralAllocator, uint64_t cacheSlide, const ImageInfo& cacheInfo, bool privateCache);
    void detachFromSharedRegion();
    void commit(Atlas::ProcessSnapshot* processSnapshot, lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator);
    void commit(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator);
    void release(lsl::Allocator& ephemeralAllocator);
    void disableCrashReportBacktrace();
#if SUPPORT_ROSETTA
    void setRosettaSharedCacheInfo(uint64_t aotCacheLoadAddress, const uuid_t aotCacheUUID);
    void addRosettaImages(std::span<const dyld_aot_image_info>&, std::span<const dyld_image_info>&);
    void removeRosettaImages(std::span<const mach_header*>& mhs);
#endif
    uint64_t     imageInfoCount();
    unsigned int notifyPortValue();
    void        fork_child();
    uint64_t    lastImageListUpdateTime();
    void        setTerminationString(const char* msg);

    void        notifyMonitorOfImageListChangesSim(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[]);
    void        coresymbolication_load_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh);
    void        coresymbolication_unload_notifier(void* connection, uint64_t timestamp, const char* path, const struct mach_header* mh);

    bool        notifyMonitorNeeded();
    void        notifyMonitorOfImageListChanges(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[]);
    void        notifyMonitorOfMainCalled();
    void        notifyMonitorOfDyldBeforeInitializers();

    static void notifyMonitoringDyld(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[]);
    static void switchDyldLoadAddress(const dyld3::MachOFile* dyldInCacheMF);
    static bool switchToDyldInDyldCache(const dyld3::MachOFile* dyldInCacheMF);
#endif // !TARGET_OS_EXCLAVEKIT

    static dyld_all_image_infos* getProcessInfo();
    void storeProcessInfoPointer(dyld_all_image_infos**); // sets value in __dyld4 section

private:
    void addImageUUID(const dyld3::MachOFile*);
    void ensureSnapshot(lsl::Allocator& ephemeralAllocator);

#if !TARGET_OS_EXCLAVEKIT
    // compact info fields
    Atlas::ProcessSnapshot*             _snapshot       = nullptr;
    FileManager*                        _fileManager    = nullptr;
#if !TARGET_OS_SIMULATOR
    os_unfair_lock_s                    _processSnapshotLock    = OS_UNFAIR_LOCK_INIT;
#endif // !TARGET_OS_SIMULATOR
#endif // !TARGET_OS_EXCLAVEKIT

    // old style all_image_info fields
    dyld_all_image_infos*               _allImageInfo   = nullptr;
    lsl::Vector<dyld_image_info>*       _imageInfos     = nullptr;
    lsl::Vector<dyld_uuid_info>*        _imageUUIDs     = nullptr;
#if SUPPORT_ROSETTA
    lsl::Vector<dyld_aot_image_info>*   _aotImageInfos  = nullptr;
#endif
#if TARGET_OS_SIMULATOR
    const dyld::SyscallHelpers*         _syscallHelpers = nullptr;
#endif
};

} // namespace dyld4

#endif /* EXTERNALLY_VIEWABLE_STATE_H */
