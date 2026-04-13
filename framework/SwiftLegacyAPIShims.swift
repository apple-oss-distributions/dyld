/*
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

import Foundation
import os
import System
@_implementationOnly import Dyld_Internal
@_implementationOnly import MachO.dyld_images
@_implementationOnly import MachO_Private.dyld
@_implementationOnly import MachO_Private.dyld_cache_format

/// Callback type for iterate_text functions
/// Note: This is internal because these functions are only called from C via @_cdecl
internal typealias IterateTextCallback = @convention(block) (UnsafePointer<dyld_shared_cache_dylib_text_info>) -> Void

/// Compare two uuid_t tuples for equality using uuid_compare.
private func == (_ a: uuid_t, _ b: uuid_t) -> Bool {
    var aCopy = a
    var bCopy = b
    return withUnsafePointer(to: &aCopy) { aPtr in
        withUnsafePointer(to: &bCopy) { bPtr in
            uuid_compare(
                aPtr.withMemoryRebound(to: UInt8.self, capacity: 16) { $0 },
                bPtr.withMemoryRebound(to: UInt8.self, capacity: 16) { $0 }
            ) == 0
        }
    }
}

/// Convert a fixed-size C array to uuid_t tuple.
@inline(__always)
private func uuidArrayToTuple(_ arr: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                                       UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8)) -> uuid_t {
    return arr
}

// MARK: - Slow Path Caching

/// Cached information for iterating cache text segments.
/// Stores only header offsets - actual data is read from memoryMap on demand.
private struct CachedCacheTextInfo: Sendable {
    let memoryMap: MemoryMap
    let sharedRegionStart: UInt64
    let imagesTextOffset: UInt64
    let imagesTextCount: UInt64
}

/// Global cache for iterate text info, keyed by UUID.
/// This avoids repeatedly parsing BPList data for the same cache.
private let sCacheTextInfoCache = OSAllocatedUnfairLock(initialState: [UUID: CachedCacheTextInfo]())

/// Get or create cached text info for a SharedCache.
/// Returns the cached info on success, nil on failure to read header.
private func getOrCreateCachedTextInfo(for cache: SharedCache, uuidKey: UUID) -> CachedCacheTextInfo? {
    // Check cache first
    if let cached = sCacheTextInfoCache.withLock({ $0[uuidKey] }) {
        return cached
    }

    // Cache miss - extract info from cache header
    let sharedRegionStart = cache.preferredLoadAddress.value
    let map = cache.memoryMap
    guard let headerBuffer = try? map.memoryBuffer(range: sharedRegionStart..<(sharedRegionStart + UInt64(MemoryLayout<dyld_cache_header>.size))) else {
        return nil
    }
    let header = headerBuffer.bytes.withUnsafeBytes { $0.load(as: dyld_cache_header.self) }

    let cachedInfo = CachedCacheTextInfo(
        memoryMap: map,
        sharedRegionStart: sharedRegionStart,
        imagesTextOffset: header.imagesTextOffset,
        imagesTextCount: header.imagesTextCount
    )

    // Store in cache
    sCacheTextInfoCache.withLock { $0[uuidKey] = cachedInfo }

    return cachedInfo
}

// MARK: - Public API Implementations

/// Pure Swift implementation of shared cache text iteration for the current process.
/// This avoids ObjC bridging overhead by working directly with Swift types.
///
/// Returns 0 on success, -1 if cache not found or UUID doesn't match.
@_cdecl("dyld_shared_cache_iterate_text_swift")
internal func dyld_shared_cache_iterate_text_swift(
    _ cacheUuidPtr: UnsafePointer<UInt8>,
    _ callback: IterateTextCallback
) -> Int32 {
    let requestedUuid = cacheUuidPtr.withMemoryRebound(to: uuid_t.self, capacity: 1) { $0.pointee }
    let requestedUuidKey = UUID(uuid: requestedUuid)

    // Fast path: use _dyld_get_shared_cache_range to get cache base address directly
    // This avoids the overhead of Process.forCurrentTask() and getCurrentSnapshot()
    var cacheLength: Int = 0
    if let cachePtr = _dyld_get_shared_cache_range(&cacheLength) {
        let cacheAddress = UInt64(UInt(bitPattern: cachePtr))
        if iterateCacheTextsFastPath(cacheBaseAddress: cacheAddress, requestedUuid: requestedUuid, callback: callback) {
            return 0
        }
    }

    // Check cache first, before creating any SharedCache objects
    if let cached = sCacheTextInfoCache.withLock({ $0[requestedUuidKey] }) {
        iterateCacheTextsFromMemoryMap(cached: cached, callback: callback)
        return 0
    }

    // Cache miss - find the matching SharedCache in system caches
    var foundCache: SharedCache? = nil
    for systemCache in SharedCache.systemSharedCaches() {
        if requestedUuid == systemCache.uuid.uuid {
            foundCache = systemCache
            break
        }
    }
    guard let cache = foundCache else { return -1 }

    // Get or create cached info and iterate
    guard let cachedInfo = getOrCreateCachedTextInfo(for: cache, uuidKey: requestedUuidKey) else {
        return -1
    }
    iterateCacheTextsFromMemoryMap(cached: cachedInfo, callback: callback)
    return 0
}

/// Pure Swift implementation of shared cache text iteration with extra search directories.
/// Returns 0 on success, -1 if cache not found.
@_cdecl("dyld_shared_cache_find_iterate_text_swift")
internal func dyld_shared_cache_find_iterate_text_swift(
    _ cacheUuidPtr: UnsafePointer<UInt8>,
    _ extraSearchDirs: UnsafePointer<UnsafePointer<CChar>?>?,
    _ callback: IterateTextCallback
) -> Int32 {
    // First try the standard locations (uses caching internally)
    let result = dyld_shared_cache_iterate_text_swift(cacheUuidPtr, callback)
    if result == 0 { return 0 }

    // Try extra search directories if provided
    guard let extraSearchDirs = extraSearchDirs else { return -1 }

    let requestedUuid = cacheUuidPtr.withMemoryRebound(to: uuid_t.self, capacity: 1) { $0.pointee }
    let requestedUuidKey = UUID(uuid: requestedUuid)

    // Check cache first for extra search dirs case too
    if let cached = sCacheTextInfoCache.withLock({ $0[requestedUuidKey] }) {
        iterateCacheTextsFromMemoryMap(cached: cached, callback: callback)
        return 0
    }

    var p = extraSearchDirs
    while let dirPtr = p.pointee {
        let path = String(cString: dirPtr)
        if let cache = try? SharedCache(path: FilePath(path)),
           requestedUuid == cache.uuid.uuid {
            // Get or create cached info and iterate
            if let cachedInfo = getOrCreateCachedTextInfo(for: cache, uuidKey: requestedUuidKey) {
                iterateCacheTextsFromMemoryMap(cached: cachedInfo, callback: callback)
                return 0
            }
        }
        p = p.advanced(by: 1)
    }

    return -1
}

// MARK: - Internal Helpers

/// Convenience overload that takes CachedCacheTextInfo.
@inline(__always)
private func iterateCacheTextsFromMemoryMap(cached: CachedCacheTextInfo, callback: IterateTextCallback) {
    iterateCacheTextsFromMemoryMap(
        memoryMap: cached.memoryMap,
        sharedRegionStart: cached.sharedRegionStart,
        imagesTextOffset: cached.imagesTextOffset,
        imagesTextCount: cached.imagesTextCount,
        callback: callback
    )
}

/// Iterate cache text segments using cached header info and memoryMap.
/// This is the common iteration path for both cache hits and misses.
@inline(__always)
internal func iterateCacheTextsFromMemoryMap(
    memoryMap: MemoryMap,
    sharedRegionStart: UInt64,
    imagesTextOffset: UInt64,
    imagesTextCount: UInt64,
    callback: IterateTextCallback
) {
    let imageInfoSize = UInt64(MemoryLayout<dyld_cache_image_text_info>.stride)
    guard let imageInfoBuffer = try? memoryMap.memoryBuffer(
        range: sharedRegionStart + imagesTextOffset..<(sharedRegionStart + imagesTextOffset + imagesTextCount * imageInfoSize)) else {
        return
    }

    for i in 0..<Int(imagesTextCount) {
        let imageInfo = imageInfoBuffer.bytes.withUnsafeBytes {
            $0.load(fromByteOffset: i * Int(imageInfoSize), as: dyld_cache_image_text_info.self)
        }

        // Resolve path from pathOffset - read enough bytes for a reasonable path
        let pathRange = (sharedRegionStart + UInt64(imageInfo.pathOffset))..<(sharedRegionStart + UInt64(imageInfo.pathOffset + 1024))
        guard let pathBuffer = try? memoryMap.memoryBuffer(
            range: pathRange) else {
            continue
        }

        pathBuffer.bytes.withUnsafeBytes { pathBytes in
            let pathPtr = pathBytes.baseAddress!.assumingMemoryBound(to: CChar.self)

            var info = dyld_shared_cache_dylib_text_info()
            info.version = 2
            info.loadAddressUnslid = imageInfo.loadAddress
            info.textSegmentSize = UInt64(imageInfo.textSegmentSize)
            info.textSegmentOffset = imageInfo.loadAddress - sharedRegionStart
            info.path = pathPtr
            info.dylibUuid = uuidArrayToTuple(imageInfo.uuid)

            withUnsafePointer(to: &info) { callback($0) }
        }
    }
}

/// Fast path: iterate text segments directly from mapped cache header.
/// This bypasses the BPList entirely by reading the dyld_cache_image_text_info array directly.
/// Returns true if fast path succeeded, false if caller should fall back to slow path.
@inline(__always)
internal func iterateCacheTextsFastPath(cacheBaseAddress: UInt64, requestedUuid: uuid_t, callback: IterateTextCallback) -> Bool {
    let cachePtr = UnsafeRawPointer(bitPattern: UInt(cacheBaseAddress))
    guard let cachePtr else { return false }

    // Read the header directly
    let header = cachePtr.assumingMemoryBound(to: dyld_cache_header.self).pointee

    // Check that mappingOffset indicates imagesTextOffset/imagesTextCount are valid
    // The imagesTextCount field must be within the header (before the first mapping)
    guard header.mappingOffset >= MemoryLayout<dyld_cache_header>.offset(of: \dyld_cache_header.imagesTextCount)! + MemoryLayout<UInt64>.size else { return false }

    // Check imagesTextCount is reasonable
    guard header.imagesTextCount > 0 && header.imagesTextCount < 100000 else { return false }

    // Verify UUID matches
    let cacheUuid = uuidArrayToTuple(header.uuid)
    guard cacheUuid == requestedUuid else { return false }

    // Get pointer to text image info array
    let imagesTextPtr = cachePtr.advanced(by: Int(header.imagesTextOffset))
        .assumingMemoryBound(to: dyld_cache_image_text_info.self)

    // Iterate directly through the array
    for i in 0..<Int(header.imagesTextCount) {
        let imageInfo = imagesTextPtr[i]

        // Get path string pointer
        let pathPtr = cachePtr.advanced(by: Int(imageInfo.pathOffset))
            .assumingMemoryBound(to: CChar.self)

        var info = dyld_shared_cache_dylib_text_info()
        info.version = 2
        info.loadAddressUnslid = imageInfo.loadAddress
        info.textSegmentSize = UInt64(imageInfo.textSegmentSize)
        info.textSegmentOffset = imageInfo.loadAddress - header.sharedRegionStart
        info.path = pathPtr
        info.dylibUuid = uuidArrayToTuple(imageInfo.uuid)

        autoreleasepool {
            withUnsafePointer(to: &info) { infoPtr in
                callback(infoPtr)
            }
        }
    }

    return true
}
