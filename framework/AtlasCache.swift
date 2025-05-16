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

import os
import System
@_implementationOnly import Dyld_Internal

@available(macOS 13.0, *)
struct AtlasCache {
    var uuidMap = [UUID : BPList.UnsafeRawDictionary]()
    var pathMap = [String : BPList.UnsafeRawDictionary]()
    var buffers = [Data]()
    let lock = OSAllocatedUnfairLock()
    mutating func getCachePlist(uuid: UUID?, path: FilePath?, forceScavenge: Bool) -> BPList.UnsafeRawDictionary? {
        return try? lock.withLockUnchecked { () -> BPList.UnsafeRawDictionary? in
            //FIXME: Delegate support
            if let uuid, let result = uuidMap[uuid] {
                return result
            }
            guard let path else {
                return nil
            }
            if !forceScavenge, let result = pathMap[path.string] {
                return result
            }
            guard let atlasFileName = path.lastComponent?.stem.appending(".atlas") else {
                return  nil
            }
            var embeddedData = try? Data(contentsOf:URL(fileURLWithPath:path.removingLastComponent().appending(atlasFileName).string), options:.mappedIfSafe)
            var scavengedData: Data? = nil
            if forceScavenge || embeddedData == nil {
                embeddedData = nil
                var scavengedBufferSize = UInt64(0)
                if let scavegedBuffer =  scavengeCache(path.string, &scavengedBufferSize) {
                    scavengedData = Data(bytesNoCopy:scavegedBuffer, count:Int(scavengedBufferSize), deallocator:.free)
                }
            }
            let data = embeddedData ?? scavengedData
            guard let data else { return nil }
            buffers.append(data)
            var archive  = try AARDecoder(data:data)
            let archivePath = uuid?.cacheAtlasArchivePath ?? "caches/names/\(path.lastComponent!.string).plist"
            guard let plistData = try? archive.data(path: archivePath),
                  let atlasesDict = try? BPList(data:plistData).asDictionary(),
                  let byNameDict = try atlasesDict[.string("names")]?.asDictionary(),
                  let byUuidDict = try atlasesDict[.string("uuids")]?.asDictionary() else {
                return nil
            }
            var skipValidation = false
            if scavengedData == nil {
                for prefix in SharedCache.sharedCachePaths {
                    if path.string.hasPrefix(prefix) {
                        // The shared cache is coming from snapshot protected storage, skip validation
                        skipValidation = true
                        break
                    }
                }
            }
            if !skipValidation {
                do {
                    for (_,atlas) in byNameDict {
                        guard let atlasDict = try? atlas.asDictionary() else {
                            throw AtlasError.placeHolder
                        }
                        try SharedCache.Impl.validate(bplist:atlasDict)
                    }
                    for (_,atlas) in byUuidDict {
                        guard let atlasDict = try? atlas.asDictionary() else {
                            throw AtlasError.placeHolder
                        }
                        try SharedCache.Impl.validate(bplist:atlasDict)
                    }
                } catch {
                    return nil
                }
            }
            for (key,atlas) in byUuidDict {
                guard let atlasUUID = UUID(uuidString: key.asString()) else {
                    continue
                }
                if uuidMap[atlasUUID] == nil {
                    uuidMap[atlasUUID] = try! atlas.asDictionary()
                }
            }
            let dirname = path.removingLastComponent()
            for (atlasName,atlas) in byNameDict {
                let fullPath = dirname.appending(atlasName.asString()).string
                let atlasDict = try! atlas.asDictionary()
                if pathMap[fullPath] == nil {
                    pathMap[fullPath] = atlasDict
                }
            }
            if let uuid {
                return uuidMap[uuid]
            }
            return pathMap[path.string]
        }
    }
}

@available(macOS 13.0, *)
fileprivate var bufferCache = AtlasCache()

internal extension Snapshot {
    static func findCacheBPlist(uuid: UUID?, path: FilePath?, forceScavenge: Bool = false) -> BPList.UnsafeRawDictionary? {
        if #available(macOS 13.0, *) {
            guard let plist = bufferCache.getCachePlist(uuid:uuid, path:path, forceScavenge:forceScavenge) else {
                return nil
            }
            return plist
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }
}
