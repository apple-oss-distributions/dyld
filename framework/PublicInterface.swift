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

// This file contains all the API/SPI interfaces. Most of our facade objects
// are quite thin, so this file should be mostly DocC comments

import System

//MARK: -
//MARK: Snapshot

public struct Snapshot {
    let impl:               Snapshot.Impl
    var pageSize:           Int             { return Int(impl.pageSize) }
    var pid:                pid_t           { return impl.pid }
    public var images:      [Image]         { return impl.images.map { return Image(wrapping:$0) } }
    var aotImages:          [AOTImage]?     { return impl.aotImages?.map { AOTImage(wrapping:$0) } }
    var timestamp:          UInt64          { return impl.timestamp }
    var initialImageCount:  UInt64          { return UInt64(impl.initialImageCount) }
    var state:              UInt8           { return impl.state }
    var platform:           UInt64          { return impl.platform }
    var environment:        Environment?    { return Environment(wrapping:impl.env) }
    var metrics:            [String:Int64]?   { return impl.metrics }

    public var sharedCache: SharedCache? {
        guard let sharedCache = impl.sharedCache else { return nil }
        return SharedCache(wrapping:sharedCache)
    }

    internal init(wrapping impl:Impl) {
        self.impl = impl
    }
    public init(data: Data) throws {
        let context = Snapshot.DecoderContext(snapshotBuffer:MemoryBuffer(data:data))
        guard let snapshtoImpl = context.snapshot else { throw AtlasError.missingPlist }
        self.impl = snapshtoImpl
    }
}

//MARK: -
//MARK: Image

public struct Image {
    internal    let impl:                   Image.Impl
    public      var filePath:               String?             { return impl.filePath}
    public      var installname:            String?             { return impl.installname }
    public      var address:                RebasedAddress      { return impl.address }
    public      var segments:               [Segment]           { return impl.segments.map { Segment(wrapping:$0) } }
    public      var preferredLoadAddress:   PreferredAddress    { return impl.preferredLoadAddress }
    public      var uuid:                   UUID?               { return impl.uuid }
    public      var pointerSize:            UInt64              { return impl.pointerSize }
    public      var sharedCache:            SharedCache?        { return SharedCache(wrapping:impl.sharedCache) }

    internal init(wrapping impl: Image.Impl) {
        self.impl       = impl
    }
}

public struct AOTImage {
    private let impl: Impl
    public  var x86Address:     RebasedAddress          { return impl.x86Address }
    public  var aotAddress:     RebasedAddress          { return impl.aotAddress }
    public  var aotSize:        UInt64                  { return impl.aotSize }
    public  var aotImageKey:    InlineArray<32, UInt8>  { return impl.aotImageKey }

    internal init(wrapping impl: AOTImage.Impl) {
        self.impl = impl
    }
}

public struct Segment {
    internal    let impl:                   Impl
    public      var name:                   String                  { return impl.name }
    public      var address:                RebasedAddress          { return impl.address }
    public      var preferredLoadAddress:   PreferredAddress        { return impl.preferredLoadAddress }
    public      var vmSize:                 UInt64                  { return impl.vmSize }
    public      var permissions:            UInt64                  { return impl.permissions }
    internal    var memoryBuffer:           MemoryBuffer            {
        get throws {
            return try impl.memoryBuffer
        }
    }
    public      var data:                  Data                     {
        get throws {
            return MemoryBufferNSDataBridge(memoryBuffer:try memoryBuffer) as Data
        }
    }

    internal init(wrapping impl: Impl) {
        self.impl = impl
    }
}

public struct Environment {
    private let impl: Impl
    public var rootPath:    String?      { return impl.rootPath }
    internal init?(wrapping impl: Environment.Impl?) {
        guard let impl else { return nil }
        self.impl = impl
    }
}

public struct SharedCache {
    private let impl:                   SharedCache.Impl
    public var address:                 RebasedAddress      {
        return impl.address
    }
    public var images:                 [Image]             { return impl.images.map { Image(wrapping:$0) } }
    public var preferredLoadAddress:    PreferredAddress    { return impl.preferredLoadAddress }
    public var uuid:                    UUID                { return impl.uuid }
    public var vmSize:                  UInt64              { return impl.vmSize }
    public var mappedPrivate:           Bool                { return impl.mappedPrivate }
    public var filePaths:               [String]            { return impl.filePaths}
    public var aotAddress:              RebasedAddress?     { return impl.aotAddress }
    public var aotUuid:                 UUID?               { return impl.aotUuid }
    public var localSymbolPath:         String?             { return impl.localSymbolsPath }
    public var subCaches:               [SubCache]          { return impl.subCaches.map { SubCache(wrapping:$0) } }

    public func withLocalSymbolFileBytes<ResultType>(_ body: (UnsafeRawBufferPointer) throws -> ResultType) throws -> ResultType {
        return try impl.withLocalSymbolFileBytes {
            return try body($0)
        }
    }

    func pinMappings() -> Bool {
        return impl.pinMappings()
    }
    func unpinMappings() {
        impl.unpinMappings()
    }

    init(path: FilePath, forceScavenge: Bool = false ) throws(AtlasError) {
        // Since this is directly created there is no snapshot retiaing the context, so we instead use the wrapper object to anchor it
        // in memory
        let context = Snapshot.DecoderContext(sharedCachePath:path, forceCacheScavenge:forceScavenge)
        guard let cacheImpl = context.sharedCache else { throw .missingPlist }
        self.impl = cacheImpl
        
    }

    internal init?(wrapping impl: SharedCache.Impl?) {
        guard let impl else { return nil }
        self.impl       = impl
    }
    
    internal init(wrapping impl: SharedCache.Impl) {
        self.impl       = impl
    }

    /// Internal access to the memory map for the cache.
    /// Used by iterate_text optimizations to avoid repeatedly parsing BPList data.
    /// Returns a copy to avoid reentrancy issues with the context's lazy initialization.
    internal var memoryMap: MemoryMap { return impl.memoryMap.copy() }

    // Pre-sorted list
    static let sharedCachePaths = [
        "/private/preboot/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
        "/private/preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
        "/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
        "/private/preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/",
        "/System/Cryptexes/ExclaveOS/System/ExclaveKit/System/Library/dyld/",
        "/System/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
        "/System/Cryptexes/Incoming/OS/System/Library/dyld/",
        "/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
        "/System/Cryptexes/OS/System/Library/dyld/",
        "/System/DriverKit/System/Library/dyld/",
        "/System/ExclaveKit/System/Library/dyld/",
        "/System/Library/Caches/com.apple.dyld/",
        "/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/",
        "/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/"
    ]
    public static func systemSharedCaches(path:FilePath = "/") -> [SharedCache] {
        var result: [SharedCache] = []
        var enumeratedCaches = Set<UUID>()
        for pathString in sharedCachePaths {
            guard let children  = path.appending(pathString).realPath?.children else {
                continue
            }
            for child in children {
                guard child.isPontentialSharedCache else { continue }
                guard let sharedCache = try? SharedCache(path:child) else {
                    continue
                }
                if !enumeratedCaches.contains(sharedCache.uuid) {
                    enumeratedCaches.insert(sharedCache.uuid)
                    result.append(sharedCache)
                }
            }
        }
        
        return result
    }

    public static func installedSharedCaches() -> [SharedCache] {
        var result: [SharedCache] = []

        // First look for B&I built caches inclued in simualtor disk images
        let mountedImagePathPrefix = FilePath("/Library/Developer/CoreSimulator/Volumes/")
        for mountedImagePath in (mountedImagePathPrefix.children ?? []) {
            let runtimePathPrefix = mountedImagePath.appending("/Library/Developer/CoreSimulator/Profiles/Runtimes")
            for runtimePath in (runtimePathPrefix.children ?? []) {
                let cachePathPrefix = runtimePath.appending("/Contents/Resources/RuntimeRoot/System/Library/Caches/com.apple.dyld")
                for child in (cachePathPrefix.children ?? []) {
                    guard child.isPontentialSharedCache else { continue }
                    guard let sharedCache = try? SharedCache(path:child) else { continue }
                    result.append(sharedCache)
                }
            }
        }

        // Then look caches built at install for older simulators
        let locallyBuiltCacheBuildPrefix = FilePath("/Library/Developer/CoreSimulator/Caches/dyld")
        for locallBuiltCacheBuildPath in (locallyBuiltCacheBuildPrefix.children ?? []) {
            for locallyBuiltCacheDir in (locallBuiltCacheBuildPath.children ?? []) {
                for child in (locallyBuiltCacheDir.children ?? []) {
                    guard child.isPontentialSharedCache else { continue }
                    guard let sharedCache = try? SharedCache(path:child) else { continue }
                    result.append(sharedCache)
                }
            }
        }
        return result
    }
}

internal extension FilePath {
    var isPontentialSharedCache: Bool {
        let fileName = self.lastComponent!.stem
        let fileExtension = self.extension
        if !fileName.hasPrefix("dyld_shared_cache_") && !fileName.hasPrefix("dyld_sim_shared_cache_") { return false }
        if fileExtension != nil && fileExtension != "development" { return false }
        if fileExtension == "development" {
            // Development subcaches end in .development, filter those out here
            if (self.lastComponent!.string.components(separatedBy:".").count > 2) {
                return false;
            }
        }
        return true
    }
}

public struct SubCache {
    let impl:       SubCache.Impl
    var name:       String  { return impl.name }
    var vmOffset:   UInt64  { return impl.virtualOffset }
    var uuid:       UUID    { return impl.uuid }
    var bytes:      RawSpan {
        @_lifetime(borrow self)
        borrowing get throws {
            return unsafe _overrideLifetime(try impl.bytes, copying: self)
        }
    }
    internal init(wrapping impl: SubCache.Impl) {
        self.impl = impl
    }
}

//guard let sharedCacheRecord = context.sharedCacheRecord,
//      let sharedCacheSearchResult = SharedCache.Impl.findCacheBPlist(uuid:sharedCacheRecord.uuid, path:sharedCacheRecord.filePath) else { return nil }
//context.sharedCachePlist = sharedCacheSearchResult.bplist

public struct Process {
    private let impl: Process.Impl

    public init(task: task_read_t, queue: DispatchQueue? = nil) throws {
        self.impl = try Impl(task:task, queue:queue)
    }

    func registerForChangeNotifications( block: @escaping (_ image: Image, _ load: Bool) -> Void) throws -> UInt32 {
        return try impl.registerForChangeNotifications(block)
    }

    func register(event:UInt32, _ handler: @escaping () -> Void) throws -> UInt32 {
        return try impl.register(event: event, handler)
    }

    func unregister(handle: UInt32) {
        impl.unregister(event:handle)
    }
    public func getCurrentSnapshot() throws -> Snapshot {
        return try Snapshot(wrapping:impl.getCurrentSnapshot())
    }

    public static func forCurrentTask() -> Process {
        //FIXME: Use mach_task_self() (how do we get it into Swift? What module is it in ?)
        return try! Process(task:mach_task_self_)
    }

    internal var queue: DispatchQueue? {
        get {
            return impl.queue
        }
        set {
            impl.queue = newValue
        }
    }
}
