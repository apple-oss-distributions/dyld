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

struct AtlasCache {
    struct State : Sendable {
        var uuidMap:    [UUID : (MemoryBuffer,BPList.UnsafeObject)]     = [:]
        var pathMap:    [String : (MemoryBuffer,BPList.UnsafeObject)]   = [:]

        fileprivate func lookup(uuid: UUID?, path:String) ->  (MemoryBuffer,BPList.UnsafeObject)? {
            if let uuid, let result = uuidMap[uuid] {
                return result
            }
            if let result = pathMap[path] {
                return result
            }
            return nil
        }
    }
    var state: OSAllocatedUnfairLock = .init(initialState:State())

    private func archivePath(path: FilePath?, uuid: UUID?) -> String? {
        if let uuid {
            return "caches/uuids/\(uuid.uuidString.uppercased()).plist"
        }
        if let path {
            return "caches/names/\(path.lastComponent!.string).plist"
        }

        return nil
    }

    private func atlasPath(filePath: FilePath) -> String? {
        var result = filePath.string
        if let suffix = filePath.extension {
            result.removeLast(suffix.count + 1)
        }
        return "\(result).atlas"
    }

    func getCachePlist(uuid: UUID?, path: FilePath, forceScavenge: Bool) -> (MemoryBuffer, BPList.UnsafeObject)? {
        do {
            if !forceScavenge, let earlyResult = state.withLock({ return $0.lookup(uuid:uuid, path:path.string) }) {
                return earlyResult
            }
            guard let atlasFullPath = atlasPath(filePath:path) else {
                return  nil
            }

            var skipValidation = false
            for prefix in SharedCache.sharedCachePaths {
                if atlasFullPath.hasPrefix(prefix) {
                    // The shared cache is coming from snapshot protected storage, skip validation
                    skipValidation = true
                    break
                }
            }

            var buffer = MemoryBuffer(path: atlasFullPath, decompress:true)
            if forceScavenge || buffer == nil {
                buffer = nil
                var scavengedBufferSize = UInt64(0)
                if let scavegedBuffer =  scavengeCache(path.string, &scavengedBufferSize) {
                    buffer = MemoryBuffer(malloced:scavegedBuffer, count:Int(scavengedBufferSize))
                }
                skipValidation = false
            }

            guard   let buffer,
                    var archive     = try? AppleArchive(bytes:buffer.bytes, preValidated:skipValidation),
                    let archivePath = archivePath(path:path, uuid: uuid),
                    let plistBytes  = try? archive.bytes(path:archivePath),
                    let plist       = try? BPList(bytes:plistBytes) else {
                return nil
            }
            var byNameDict: BPList.UnsafeObject?
            var byUuidDict: BPList.UnsafeObject?
            try plist.topObject.asDictionary().forEach { key, value in
                switch key {
                case "names": byNameDict = value.asUnsafeObject()
                case "uuids": byUuidDict = value.asUnsafeObject()
                default: return
                }
            }
            guard let byNameDict, let byUuidDict else { return nil }

            if !skipValidation {
                try byUuidDict.object.asDictionary().forEach { _, atlas in
                    try SharedCache.Impl.validate(bplist:atlas)
                }
            }

            return try state.withLock { state in
                try byUuidDict.object.asDictionary().forEach { key, atlas in
                    guard let atlasUUID = UUID(uuidString: key.stringValue) else {
                        return
                    }
                    if state.uuidMap[atlasUUID] == nil {
                        state.uuidMap[atlasUUID] = (buffer, atlas.asUnsafeObject())
                    }
                }
                let dirname = path.removingLastComponent()
                try byNameDict.object.asDictionary().forEach { key, atlas in
                    let fullPath = dirname.appending(key.stringValue).string
                    if state.pathMap[fullPath] == nil {
                        state.pathMap[fullPath] = (buffer, atlas.asUnsafeObject())
                    }
                }
                if let uuid {
                    return state.uuidMap[uuid]
                }
                return state.pathMap[path.string]
            }
        } catch {
            return nil
        }
    }
}
