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
#include "ByteStream.h"
#include "Vector.h"
#include "Platform.h"
#include "Header.h"
#include "AAREncoder.h"


#if TARGET_OS_SIMULATOR
  #include "dyldSyscallInterface.h"
#endif

#if DYLD_FEATURE_ATLAS_GENERATION
#include "PropertyList.h"
#endif

struct dyld_all_image_infos;

#if !TARGET_OS_EXCLAVEKIT
#include "RemoteNotificationResponder.h"
#endif // !TARGET_OS_EXCLAVEKIT

struct dyld_all_image_infos;

using lsl::Allocator;

namespace dyld4 {
/*
 ExternallyViewableState encapsulates everything about the dyld state of a process that other processes may query. Generally this
 consists of two types of interfaces: Mechanisms for exposing the list of images (and some additional state) to external observers,
 as well as interfaces to notify external observers as they occur. Historically those interfaces have been distinct, which made
 synchronizing them difficult. The more recent interfaces combine both together to make them synchronized and consistent without
 the clients needing to extra work.

 The interfaces for exposing the current state of dyld consist of:
 1. dyld_all_image_info
 2. dyld_process_info (this is a wrapper interface intended to abstract away dyld_all_image_info)
 2. compact info (soon to be replaced by `Dyld.framework` atlases)

 The notification interfaces consist of:
 1. dyld_process_info_notify (which share timestampls with dyld_all_image_info and is buult on top of data carraying mach_msgs)
 2. dyld_process_t's notifiers (built ontop of dataless mach_msg)
 3. sProcessInfo->notification is a function call used to notify `LLDB` that an update has occured. The function is emty, `LLDB` sets a
    breakpoint on it and then uses other mechanisms to update its image list
 4. `kdebug_trace` these are asynhcronous notifiers that are not (yet) handled by `ExternallyViewableState`, but should be in the future
 5. (Historical) _dyld_debugger_notification: This is the original debugger interface used by `gdb`. It is not currently suppoer.

 Our longer term goal is to move all of these onto a common interface and shim or remove all legacy interfaces.

 Some details:

 * Due to the large number of historical notifier interfaces and their (loosely) coupled image list formats, sometimes clients mixed and matched
 between SPIs. This could causes problems because it made them fragile to implicit ordering dependencies between the mechanisms. To sole this
 ExternallyViewableState has been refactored so that all lists are updated, then all notifiers are called.

 * dyld_all_image_info is statically allocated in dyld's __DATA segment. When the kernel loads dyld, the kernel finds dyld_all_image_info
 in dyld and save's off the address of dyld_all_image_info in the process's task structure. It can then be found by other process
 by using task_info(TASK_DYLD_INFO).

 * When dyld switches to dyld-in-the-cache it uses a new syscall to tell the kernel the new location of dyld_all_image_info which is
 in the dyld in the dyld cache. Once control is in the dyld-in-the-cache, it vm_deallocates the original dyld loaded from disk.

 * The way lldb is able to keep track of what images are loaded, is that lldb sets a break point on address pointered to by the
 notification files of the all image info, and dyld calls that function whenever the image list changes. During that breakpoint
 processing, lldb can then requery the image list from dyld. To handle when dyld switches over to the dyld-in-the-cache, dyld calls
 sProcessInfo->notification(dyld_image_dyld_moved) which tells lldb to move the breakpoint to the the sProcessInfo->notification
 location in the new dyld. The long term goal is to make the mach_msg based notifiers handle this transition internally so clients
 can use those and not have to manually deal with the transition to dyld-in-the-cache.

 * For simulator processes, the macOS kernel just loads the host (macOS) dyld which sees the main execuable is a simulator binary and
 DYLD_ROOT_PATH is set.  The host dyld then looks for $DYLD_ROOT_PATH/usr/lib/dyld_sim.  If it exists, it is loaded and dyld jumpts into
 it.  The host dyld remains loaded because dyld_sim has to run on multiple OS versions, so it cannot use syscalls. Instead the host
 macOS dyld supplies a table (SyscallHelpers) of function pointers for dyld_sim to use.  Additionally, dyld_sim does not carry its own
 dyld_all_image_info. Instead the host dyld passes it a pointer to the host's dyld_all_image_info, which dyld_sim writes to.

 * The dyld_all_image_info format never really changed over the years (just got fields added), so it was ok for dyld_sim to directly
 update the fields of dyld_all_image_info. The issue here is that you can run multiple iOS simulator OS versions on the same macOS
 host. If the dyld_all_image_info format changed each OS release, the host might not be able to interpret the dyld_all_image_info
 data. Our long term goal is to stabalize the `Dyld.framework` atlas format which is based on binary plists. Then simulators can
 directly generate it. Until then the dyld host shim generates it on dyld sim's behalf.

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

 */
class [[clang::ptrauth_vtable_pointer(process_independent, address_discrimination, type_discrimination)]] RuntimeState;
class ExternallyViewableState
{
public:
    struct ImageInfo { uint64_t fsID=0; uint64_t fsObjID=0; const char* path=nullptr; const void* loadAddress=nullptr; bool inSharedCache=false; };
    ExternallyViewableState(Allocator& allocator);
    void        setRuntimeState(RuntimeState* state);
    void        setDyldState(uint8_t dyldState);
    void        setLibSystemInitialized();
#if TARGET_OS_SIMULATOR
    ExternallyViewableState(Allocator& allocator, const dyld::SyscallHelpers* syscalls);
    void    initSim(RuntimeState* state, const dyld::SyscallHelpers* syscalls);
#endif
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    void        addDyldSimInfo(const char* path, uint64_t loadAddress);
#endif
#if SUPPORT_ROSETTA
    void        setRosettaSharedCacheInfo(uint64_t aotCacheLoadAddress, const uuid_t aotCacheUUID);
    void        addRosettaImages(std::span<const dyld_aot_image_info>&, std::span<const dyld_image_info>&);
    void        removeRosettaImages(std::span<const mach_header*>& mhs);
#endif
    void        setDyld(const ImageInfo& dyldInfo);
    void        addImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, const std::span<ImageInfo>& images);
    void        removeImages(lsl::Allocator& persistentAllocator, lsl::Allocator& ephemeralAllocator, std::span<const mach_header*>& mhs);
    void        setSharedCacheInfo(uint64_t cacheSlide, const ImageInfo& cacheInfo, bool privateCache);
    void        setSharedCacheAddress(uintptr_t cacheSlide, uintptr_t cacheAddress);
    void        detachFromSharedRegion();
    void        disableCrashReportBacktrace();

    uint64_t    imageInfoCount();
    unsigned int notifyPortValue();
    void        fork_child();
    uint64_t    lastImageListUpdateTime();
    void        setTerminationString(const char* msg);

    void        notifyMonitorOfImageListChangesSim(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[]);
    void        notifyMonitorOfMainCalled();
    void        notifyMonitorOfDyldBeforeInitializers();

    void        prepareInCacheDyldAllImageInfos(const mach_o::Header* dyldInCacheMF);
    bool        completeAllImageInfoTransition(Allocator& allocator, const dyld3::MachOFile* dyldInCacheMF);

    void        storeProcessInfoPointer(dyld_all_image_infos**); // sets value in __dyld4 section
    void        createMinimalInfo(Allocator& allocator, uint64_t dyldLoadAddress, const char* dyldPath, uint64_t mainExecutableAddress, const char* mainExecutablePath, const DyldSharedCache* cache);
    void        updateTimestamp();

    static dyld_all_image_infos* getProcessInfo();
    void        activateAtlas(Allocator& allocator, ByteStream& newAtlas);
#if DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION
    std::byte*  swapActiveAtlas(std::byte* begin, std::byte* end, struct dyld_all_image_infos* allImageInfos);
#endif /* DYLD_FEATURE_ATLAS_GENERATION || DYLD_FEATURE_COMPACT_INFO_GENERATION */
    lsl::Vector<std::byte>   generateCompactInfo(Allocator& allocator, AAREncoder& encoder);
    ByteStream          generateAtlas(Allocator& allocator);
#if DYLD_FEATURE_ATLAS_GENERATION
    PropertyList::Bitmap* gatherAtlasProcessInfo(uint64_t mainExecutableAddress, const DyldSharedCache *cache, PropertyList::Dictionary &rootDictionary);
    void atlasAddImage(PropertyList::Dictionary& image, uint64_t dyldLoadAddress, const char *dyldPath);
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    void scavengeSimulatorInfoForAtlas();
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */
#endif /* DYLD_FEATURE_ATLAS_GENERATION */
private:
    friend void setExternallyViewableStateToTerminated(const char* message);
    void addImageUUID(const dyld3::MachOFile*);
    void triggerNotifications(enum dyld_image_mode mode, uint32_t infoCount, const dyld_image_info info[]);

    Allocator*                          _persistentAllocator    = nullptr;
    RuntimeState*                       _runtimeState           = nullptr;
    uint8_t                             _dyldState              = 0;
    uint64_t                            _timestamp              = 0;
    // old style all_image_info fields
    dyld_all_image_infos*               _allImageInfo   = nullptr;
    lsl::Vector<dyld_image_info>*       _imageInfos     = nullptr;
    lsl::Vector<dyld_uuid_info>*        _imageUUIDs     = nullptr;
#if DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT
    // Stash dyld_sim info so we can add it to the atlas
    const char*                         _dyldSimPath            = nullptr;
    const char*                         _dyldSimCachePath       = nullptr;
    uint64_t                            _dyldSimLoadAddress     = 0;
#endif /* DYLD_FEATURE_SIMULATOR_NOTIFICATION_HOST_SUPPORT */
#if SUPPORT_ROSETTA
    lsl::Vector<dyld_aot_image_info>*   _aotImageInfos  = nullptr;
#endif /* SUPPORT_ROSETTA */
#if TARGET_OS_SIMULATOR
    const dyld::SyscallHelpers*         _syscallHelpers = nullptr;
#endif /* TARGET_OS_SIMULATOR */
};

void setExternallyViewableStateToTerminated(const char* message);

}; // namespace dyld4

#endif /* EXTERNALLY_VIEWABLE_STATE_H */
