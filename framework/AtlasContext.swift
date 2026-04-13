/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2025 Apple Inc. All rights reserved.
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

import os
import System
import OSLog

extension Snapshot {
    internal final class DecoderContext {
        private(set) var _sharedCachePath:  FilePath? = nil
        var sharedCachePath:                FilePath? {
            if let _sharedCachePath {
                return _sharedCachePath
            }
            loadCacheInfo();
            return _sharedCachePath
        }
        private(set) var _sharedCacheUuid:  UUID? = nil
        var sharedCacheUuid:                UUID? {
            if let _sharedCacheUuid {
                return _sharedCacheUuid
            }
            loadCacheInfo();
            return _sharedCacheUuid
        }
        private(set) var _sharedCacheBitmap: Bitmap? = nil
        lazy var sharedCacheBitmap: Bitmap? = {
            if let _sharedCacheBitmap {
                return _sharedCacheBitmap
            }
            loadCacheInfo();
            return _sharedCacheBitmap
        }()
        private var cacheInfoLoaded: Bool = false
        // This function normalizes access
        private func loadCacheInfo() {
            guard !cacheInfoLoaded else { return }
            cacheInfoLoaded = true
            if _sharedCachePath == nil || _sharedCacheUuid == nil {
                if let snapshotPlist {
                    // Materialize a snapshot here to avoid dynamic memory violations when we are in the middle of Sanpshot materialization
                    let snapshot = Snapshot.Impl(unsafeBPListObject:snapshotPlist, context:self)
                    guard let record    = snapshot.sharedCacheRecord else { return }
                    _sharedCachePath    = record.filePath
                    _sharedCacheUuid    = record.uuid
                    _sharedCacheBitmap  = record.bitmap
                } else if let sharedCachePlist {
                    let sharedCache = SharedCache.Impl(unsafeBPListObject:sharedCachePlist, context:self)
                    _sharedCacheUuid    = sharedCache.uuid
                }
            }
        }
        private let snapshotAtlasBuffer:    MemoryBuffer?
        private let snapshotPreValidated:   Bool
        private var sharedCacheAtlasBuffer: MemoryBuffer?

        private var mappers:            [PreferredAddress:any Mapper] = [:]
        lazy var sharedCacheAddress:    RebasedAddress? = {
            if let address = snapshot?.sharedCacheRecord?.address {
                return address
            }
            if let preferredAddress = sharedCache?.preferredLoadAddress {
                return preferredAddress + 0
            }
            return nil
        }()
        lazy var sharedCacheSlide:      Slide? =  {
            guard let sharedCacheAddress,
                  let preferredAddress = sharedCache?.preferredLoadAddress else { return nil }
            return sharedCacheAddress - preferredAddress
        }()
        var pointerSize:                UInt64  { return snapshot?.pointerSize ?? 8 }

        private var forceCacheScavenge: Bool = false
        private weak var _sharedCache:  SharedCache.Impl? = nil
        var sharedCache:                SharedCache.Impl? {
            if let _sharedCache { return _sharedCache }
            guard let sharedCachePlist else { return nil }
            let result = SharedCache.Impl(unsafeBPListObject:sharedCachePlist, context:self)
            _sharedCache = result
            return result
        }
        private weak var _snapshot:     Snapshot.Impl? = nil
        var snapshot:                   Snapshot.Impl? {
            if let _snapshot { return _snapshot }
            guard let snapshotPlist else { return nil }
            let result = Snapshot.Impl(unsafeBPListObject:snapshotPlist, context:self)
            _snapshot = result
            return result
        }
        lazy var sharedCachePlist:      BPList.UnsafeObject? = {
            guard let result = findCacheBPlist(forceScavenge:forceCacheScavenge) else { return nil }
            sharedCacheAtlasBuffer = result.0
            return result.1
        }()

        lazy var snapshotPlist:         BPList.UnsafeObject? = {
            guard   let snapshotAtlasBuffer,
                    var archive = try? AppleArchive(bytes:snapshotAtlasBuffer.bytes, preValidated:snapshotPreValidated),
                    let bplistBytes = try? archive.bytes(path: "process.plist"),
                    let bplist = try? BPList(bytes:bplistBytes) else { return nil }
            do {
                try Snapshot.Impl.validate(bplist:bplist.topObject)
                return bplist.topObject.asUnsafeObject()
            } catch {
                return nil
            }
        }()
        init(snapshotBuffer:MemoryBuffer? = nil, snapshotPreValidated: Bool = false, sharedCachePath: FilePath? = nil, forceCacheScavenge: Bool = false) {
            self._sharedCachePath        = sharedCachePath
            self.snapshotAtlasBuffer    = snapshotBuffer
            self.snapshotPreValidated   = snapshotPreValidated
            self.forceCacheScavenge     = forceCacheScavenge
        }
        lazy var memoryMap: MemoryMap = {
                var mappers: [any Mapper] = (snapshotPlist != nil ? MachOMapper.mappersForProcessMachOs(unsafeProcessInfoBPList:snapshotPlist!) : [])
                if let pathPrefix = sharedCachePath?.removingLastComponent().string,
                   let sharedCachePlist {
                    // Get the address from the snapshot's sharedCacheRecord if available.
                    // Otherwise pass nil, which SharedCacheMapper handles as "no slide" for path-only cases.
                    // This avoids accessing context.sharedCache which would cause reentrancy.
                    let address = snapshot?.sharedCacheRecord?.address.value
                    mappers.append(SharedCacheMapper(unsafeImageBPList:sharedCachePlist, address:address, prefixPath:pathPrefix))
                }
                return MemoryMap(mappers:mappers)
        }()

        @inline(__always)
        func memoryBuffer(range: Range<UInt64>) throws(MemoryMapError) -> MemoryBuffer {
            if let pinnedCacheMemoryMap {
                do throws(MemoryMapError) {
                    return try pinnedCacheMemoryMap.memoryBuffer(range:range)
                } catch {
                    return try memoryMap.memoryBuffer(range:range)
                }
            }
            return try memoryMap.memoryBuffer(range:range)
        }

        // This is used to implement C APIs that do not return an object, so we need to manually refcount how many times this particular users has pinnd the
        // cache, which is sperate from the shared usage managed by the pinned mapping cache. We will need to clean this up for senabdable support
        var pinnedMappingRefCount:   Int            = 0
        var pinnedMapMemoryBuffer:  MemoryBuffer?   = nil
        var pinnedCacheMemoryMap:   MemoryMap?      = nil
        func incrementPinnaedMappingRefCount() {
            pinnedMappingRefCount += 1
        }
        func decrementPinnaedMappingRefCount() {
            pinnedMappingRefCount -= 1
            if pinnedMappingRefCount == -1 {
                pinnedMapMemoryBuffer   = nil
                pinnedCacheMemoryMap    = nil
            }
        }


        func pinSharedCacheMappings() -> Bool {
            // Pinned mappings are rare. We cache them mainly because they are huge and we do not want two for the same cache ever. So unlike something
            // like the file buffer cache where we drop the lock between a failed lookup and wehn we insert the new one create (and risk two threads
            // creating one) here we hold the lock around the creation and block everyone else so we never map it twice).
            if pinnedCacheMemoryMap != nil {
                incrementPinnaedMappingRefCount()
                return true
            }
            if let pathPrefix = sharedCachePath?.removingLastComponent().string,
               let sharedCachePlist,
               let sharedCache {
                let cacheMapping = SharedCacheMapper(unsafeImageBPList:sharedCachePlist, address:sharedCache.address.value, prefixPath:pathPrefix)
                var memoryMap = cacheMapping.memoryMap
                guard let buffer = MemoryBuffer(memoryMap:&memoryMap) else {
                    return false
                }
                pinnedCacheMemoryMap = memoryMap
                pinnedMapMemoryBuffer = buffer
                pinnedMappingRefCount = 0
                return true
            }
            return false
        }
        func unpinSharedCacheMappings() {
            guard pinnedCacheMemoryMap != nil else { fatalError("Unpinning shared cache mapping that was not pinned") }
            decrementPinnaedMappingRefCount()
        }

        static var bufferCache = AtlasCache()
        private func findCacheBPlist(forceScavenge: Bool = false) -> (MemoryBuffer,BPList.UnsafeObject)? {
            guard let sharedCachePath else { return nil }
            return Snapshot.DecoderContext.bufferCache.getCachePlist(uuid:sharedCacheUuid, path:sharedCachePath, forceScavenge:forceScavenge)
        }
    }
}

