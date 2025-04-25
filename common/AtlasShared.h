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

#ifndef DyldAtlasShared_h
#define DyldAtlasShared_h

// This file contains content that needs to be kept in sync between the dyld encoder and the Dyld.framework decoder.
// That primarily consists of the flags and key definitions

//FIXME: Keys are not acutally shared with Swift do to limits in C import and CodingKey conforomances. Try swift macros?

#define DYLD_ATLAS_FLAGS(_name) __attribute__((availability(swift,unavailable))) int64_t _name; enum __attribute__((flag_enum,enum_extensibility(open))) : _name

#pragma mark -
#pragma make Snapshot keys

#define kDyldAtlasSnapshotImagesArrayKey        ("imgs")
#define kDyldAtlasSnapshotAotImagesArrayKey     ("aots")
#define kDyldAtlasSnapshotSharedCacheKey        ("dsc1")
#define kDyldAtlasSnapshotPidKey                ("proc")
#define kDyldAtlasSnapshotTimestampKey          ("time")
#define kDyldAtlasSnapshotFlagsKey              ("flag")
#define kDyldAtlasSnapshotCPUTypeKey            ("cput")
#define kDyldAtlasSnapshotPlatformTypeKey       ("plat")
#define kDyldAtlasSnapshotState                 ("stat")
#define kDyldAtlasSnapshotEnvironmentVarsKey    ("envp")

#pragma mark -
#pragma make Environment keys

#define kDyldAtlasEnvironmentRootPathKey                ("root")
#define kDyldAtlasEnvironmentLibraryPathKey             ("libp")
#define kDyldAtlasEnvironmentFrameworkPathKey           ("fwkp")
#define kDyldAtlasEnvironmentFallbackLibraryPathKey     ("libf")
#define kDyldAtlasEnvironmentFallbackFrameworkPathKey   ("fwkf")
#define kDyldAtlasEnvironmentVersionedLibraryPathKey    ("libv")
#define kDyldAtlasEnvironmentVersionedFrameworkPathKey  ("fwkv")
#define kDyldAtlasEnvironmentInsertedLibrariesKey       ("istd")
#define kDyldAtlasEnvironmentImageSuffixKey             ("isuf")

// This is temporary. In the future we will set the ImageOptionsInitialLoad on the initial images and derive this, but that cannot be done in the scavenger
#define kDyldAtlasSnapshotInitialImageCount ("init")


#pragma make Snapshot options

// Enums should choose defaults such that they are represented by 0 to improve encoded size
typedef DYLD_ATLAS_FLAGS(SnapshotFlags) {
    SnapshotFlagsPageSize4k             = 1 << 0,   // 0 == 16k, 1 == 4k
    SnapshotFlagsPointerSize4Bytes      = 1 << 1,   // 0 == 64 bit pointers, 1 = 32 bit pointers
    SnapshotFlagsPrivateSharedRegion    = 1 << 2    // 0 == Normal shared region, 1 == private shared region
};

#pragma mark -
#pragma make Image keys

#define kDyldAtlasImageFlagsKey                 ("flag")
#define kDyldAtlasImageFilePathKey              ("file")
#define kDyldAtlasImageUUIDKey                  ("uuid")
#define kDyldAtlasImageLoadAddressKey           ("addr")
#define kDyldAtlasImagePreferredLoadAddressKey  ("padr")
#define kDyldAtlasImageInstallnameKey           ("name")
#define kDyldAtlasImageSegmentArrayKey          ("segs")
#define kDyldAtlasImageCPUTypeKey               ("cput")


#pragma make Image options

// Enums should choose defaults such that they are represented by 0 to improve encoded size
typedef DYLD_ATLAS_FLAGS(ImageFlags) {
    ImageFlagsInitialLoad         = 1 << 0
};

#pragma mark -
#pragma make SharedCache keys

#define kDyldAtlasSharedCacheFlagsKey                   ("flag")
#define kDyldAtlasSharedCacheFilePathKey                ("file")
#define kDyldAtlasSharedCacheUUIDKey                    ("uuid")
#define kDyldAtlasSharedCacheVMSizeKey                  ("size")
#define kDyldAtlasSharedCacheLoadAddressKey             ("addr")
#define kDyldAtlasSharedCachePreferredLoadAddressKey    ("padr")
#define kDyldAtlasSharedCacheImageArrayKey              ("imgs")
#define kDyldAtlasSharedCacheBitmapArrayKey             ("bitm")
#define kDyldAtlasSharedCacheSymbolFileUUIDKey          ("suid")
#define kDyldAtlasSharedCacheSymbolFileName             ("snme")
#define kDyldAtlasSharedCacheAotUUIDKey                 ("auid")
#define kDyldAtlasSharedCacheAotLoadAddressKey          ("aadr")
#define kDyldAtlasSharedCacheMappingArrayKey            ("maps")


#pragma mark -
#pragma make SharedCache Mapping keys

#define kDyldAtlasSharedCacheMappingsSizeKey                    ("size")
#define kDyldAtlasSharedCacheMappingsPreferredLoadAddressKey    ("padr")
#define kDyldAtlasSharedCacheMappingsFileOffsetKey              ("foff")
#define kDyldAtlasSharedCacheMappingsMaxProtKey                 ("prot")

#pragma make SharedCache options

// Enums should choose defaults such that they are represented by 0 to improve encoded size
typedef DYLD_ATLAS_FLAGS(SharedCacheFlags) {
    SharedCacheFlagsLocallyBuilt          = 1 << 0
};

#pragma mark -
#pragma make Segment keys

#define kDyldAtlasSegmentNameKey                    ("name")
#define kDyldAtlasSegmentAddressKey                 ("addr")
#define kDyldAtlasSegmentPreferredLoadAddressKey    ("padr")
#define kDyldAtlasSegmentFileOffsetKey              ("foff")
#define kDyldAtlasSegmentFileSizeKey                ("fsze")
#define kDyldAtlasSegmentSizeKey                    ("size")
#define kDyldAtlasSegmentPermissionsKey             ("perm")

#pragma mark -
#pragma make AOT Image Keys

#define kDyldAtlasAOTImageX86AddrKey    ("xadr")
#define kDyldAtlasAOTImageNativeAddrKey ("aadr")
#define kDyldAtlasAOTImageSizeKey       ("asze")
#define kDyldAtlasAOTImageImageKeyKey   ("ikey")

#define DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE    (32*1024)
#define DYLD_PROCESS_INFO_NOTIFY_LOAD_ID            0x1000
#define DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID          0x2000
#define DYLD_PROCESS_INFO_NOTIFY_MAIN_ID            0x3000
#ifndef DYLD_PROCESS_EVENT_ID_BASE
#define DYLD_PROCESS_EVENT_ID_BASE                  0x4000
#endif
#ifndef DYLD_REMOTE_EVENT_ATLAS_CHANGED
#define DYLD_REMOTE_EVENT_ATLAS_CHANGED (0)
#endif
#ifndef DYLD_REMOTE_EVENT_MAIN
#define DYLD_REMOTE_EVENT_MAIN                      (1)
#endif
#ifndef DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED
#define DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED       (2)
#endif
#ifndef DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS
#define DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS  DYLD_REMOTE_EVENT_SHARED_CACHE_MAPPED
#endif

#endif /* DyldAtlasShared_h */
