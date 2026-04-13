/*
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
import Foundation
@_implementationOnly import Dyld_Internal

// Basic design:
//
// We want to expose a struct based interface, without any additional protocol conformances. We also want to use Codable in order
// to parse the image data, which requires public conformances on the structs/classes we are using. In order to do this, we
// implement facade structs with the names we want (like `Image`), which are thing shims on non-public classes (like `Image.Impl`).
// which then hose ~Copyable structs we can lazily evalute (aka `Image.Info`). This seperatation has several nice properties:
//
// * It reduces our ABI surface
// * It allows to us use classes for the internal implementations. This allows us to copy a single Object around while exposing value semantics
// * It allows to be completely lazy on resolving the BPList, but also lets memoize the work and only do it once
//
// There is a shared context object which all the `Impl`'s for a given snapshot point to it. It owns the memory backing the objects, so as long as it is
// alive everything can maintain unsafe references to memory. That also means it cannot hold strong reference to anything or we will have a retain loop.
// In order to avoid leaking memory and spurious object creation we maintain the following invariants:
//
// * All Impl's (and some unresolve Infos's) have reference to the context
// * The context has a weak reference to the SharedCache.Impl and the Snapshot.Impl (if they exist). That allows it to return the shared instance instead
//   of creating new ones. If the last reference disappears we may need to do work to deserialize again, but otherwise we don't and we avoid extra copys
// * For anything where the context needs to refer to data that would be in objects (segment mappers, for example) it works directly on the BPList without
//   materializing any objects.
//
// The above ensures no loops form, while avoiding spurious BPList processing and minimizing memory usage to objects that are actually accessed by the user.



//TODO: Figure out what kind of errors we want to hand to public API users

enum AtlasError: Error, BPListErrorWrapper {
    case machError(kern_return_t)
    case bplistError(BPListError)
    case machOError(MachOError)
    case motMachOError
    case aarDecoderError(AARDecoderError)
    case schemaValidationError
    case truncatedVMRead
    case truncatedUuid
    case truncatedData
    case missingPlist
    case missingRequiredPlistField
    case missingAddressField
    case missingFileError
    case memoryUpdateError
    case vmReadTooSmall(UInt64, UInt64)
    case taskInfoFormatUnknown(Int32)
    case scavangerFailed
    case stateMachineError
    init(_ plistError: BPListError) {
        self = .bplistError(plistError)
    }
}

extension AtlasError: CustomNSError {
    public static var errorDomain: String = "com.apple.dyld.snapshot"
    public var errorCode: Int {
        switch self {
        case .machError:
            return 1
        default:
            return 0
        }
    }

    var userInfo: [String : Any] {
        switch self {
        case let .machError(kr):
            return ["kern_return_t" : kr]
            //specify what you want to return
        default:
           return [String : Any]()
        }
    }
}

internal protocol AtlasImpl: AnyObject {
    init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext)
}

internal protocol AtlasInfo: ~Copyable {
    associatedtype Context
    init(unsafeBPListObject: BPList.UnsafeObject, context:Context)
}

enum Deferrable<T: AtlasInfo & ~Copyable>: ~Copyable {
    case unresolved(BPList.UnsafeObject, T.Context)
    case finalized(T)
    var resolved: T {
        mutating _read {
            var result: T? = nil
            switch self {
            case .unresolved(let unsafeObject, let context):
                result = T(unsafeBPListObject:unsafeObject, context:context)
                yield result!
            case .finalized(let finalizedObject): yield finalizedObject
            }
            if let result {
                self = .finalized(result)
            }
        }
    }
    init(unsafeBPListObject: BPList.UnsafeObject, context:T.Context) {
        self = .unresolved(unsafeBPListObject, context)
    }
}

//MARK: -
//MARK: Image

internal extension Image {
    struct Info: AtlasInfo, ~Copyable {
        let context:                Snapshot.DecoderContext
        let uuid:                   UUID?
        var filePath:               BPList.FastString?
        var installname:            BPList.FastString?
        let preferredAddress:       PreferredAddress?
        let address:                RebasedAddress
        let sharedCache:            Bool
        let segments:               BPList.UnsafeObject // ArrayGuts
        init(unsafeBPListObject: BPList.UnsafeObject, context:(Snapshot.DecoderContext,Slide?)) {
            self.context = context.0
            var uuid:                   UUID?
            var filePath:               BPList.FastString?
            var installname:            BPList.FastString?
            var preferredAddress:       PreferredAddress?
            var address:                RebasedAddress?
            var segments:               BPList.UnsafeObject!

            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue {
                case "name": installname        = try! value.asFastString()
                case "file": filePath           = try! value.asFastString()
                case "addr": address            = RebasedAddress(try! value.asInt64())
                case "padr": preferredAddress   = PreferredAddress(try! value.asInt64())
                case "segs": segments           = value.asUnsafeObject()
                case "uuid": uuid               = UUID(bytes:try! value.asData())
                default: return
                }
            }
            // Images will either:
            // 1. Just have a preferred load address. These are image records in the shared cache atlas records, and need to fixed up by adding the cache
            //    slide, which we will do here.
            // 2. Have just an address, in which case we should use it
            // 3. Both a preferred load address, and a load address. These are in memory atlas records generated by dyld, and we should use the
            //    address. They can only happen on platforms which support preferred load addresses (`x86_64`)
            // 4. Have none. these are malformed, but we should return nil in that case
            if let preferredAddress, let slide = context.1, address == nil {
                // This handles case 1
                self.address     = preferredAddress + slide
                self.sharedCache = true
            } else if let address, context.1 == nil {
                self.address = address
                self.sharedCache = false
            } else {
                fatalError("Invalid image info")
            }
            self.uuid = uuid
            self.filePath = filePath
            self.installname = installname
            self.preferredAddress = preferredAddress
            self.segments = segments
        }
    }
}

extension Image.Impl: Hashable {
    static func == (lhs: Image.Impl, rhs: Image.Impl) -> Bool {
        if lhs.address == rhs.address {
            // The address must match for the library objects to be equivalent
            if lhs.uuid != nil {
                if lhs.uuid == rhs.uuid {
                    // If there are uuids and they match return true
                    return true
                }
                // uuids are not nil and they don't match, return false
                return false
            } else {
                // Fallback to potential string compares if there are no uuids
                return (lhs.filePath == rhs.filePath) && (lhs.installname == rhs.installname)
            }
        }
        // Addresses did not match, return false
        return false
    }
    func hash(into hasher: inout Hasher) {
        hasher.combine(address)
        hasher.combine(uuid)
        if uuid == nil {
            // Only hash int the strings if we don't have a uuid
            hasher.combine(installname)
            hasher.combine(filePath)
        }
    }
}

extension Image {
    internal final class Impl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext, slide: Slide? = nil) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:(context,slide))
        }

        lazy var segments: [Segment.Impl] = {
            let slide = address - preferredLoadAddress
            let context = info.resolved.context
            let segmentArrayObject = info.resolved.segments
            return try! segmentArrayObject.object.asArray().map { plistObject in
                return Segment.Impl(unsafeBPListObject:plistObject.asUnsafeObject(), context:context, slide: slide)
            }
        }()
        var slide:                  Slide               { return address - preferredLoadAddress }
        var uuid:                   UUID?               { return info.resolved.uuid }
        var filePath:               String?             { return info.resolved.filePath?.stringValue }
        var installname:            String?             { return info.resolved.installname?.stringValue }
        var preferredLoadAddress:   PreferredAddress    { return info.resolved.preferredAddress ?? PreferredAddress(UInt64(0)) }
        var address:                RebasedAddress      { return info.resolved.address }
        var pointerSize:            UInt64              { return info.resolved.context.pointerSize }
        var sharedCache:            SharedCache.Impl?   { return info.resolved.sharedCache ? info.resolved.context.sharedCache : nil }
    }
}


extension AOTImage {
    struct Info: AtlasInfo, ~Copyable {
        var x86Address:     RebasedAddress
        var aotAddress:     RebasedAddress
        var aotSize:        UInt64
        var aotImageKey =   InlineArray<32, UInt8>(repeating:0)
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            var x86Address:         RebasedAddress!
            var aotAddress:         RebasedAddress!
            var aotSize:            UInt64!
            var aotImageKey       = InlineArray<32, UInt8>(repeating:0)
            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue {
                case "xadr": x86Address = RebasedAddress(try! value.asInt64())
                case "aadr": aotAddress = RebasedAddress(try! value.asInt64())
                case "asze": aotSize = UInt64(try! value.asInt64())
                case "ikey":
                    try! value.asData().withUnsafeBytes { bytes in
                        var tempKey =  InlineArray<32, UInt8>(repeating:0)
                        var mutableSpan = tempKey.mutableSpan
                        var mutableBytes = mutableSpan.mutableBytes
                        mutableBytes.withUnsafeMutableBytes {
                            $0.copyBytes(from:bytes)
                        }
                        aotImageKey = tempKey
                    }
                    return
                default: return
                }
            }
            self.x86Address = x86Address
            self.aotAddress = aotAddress
            self.aotSize = aotSize
            self.aotImageKey = aotImageKey
        }
    }
}

extension AOTImage {
    internal final class Impl: AtlasImpl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
        }
        var x86Address:     RebasedAddress          { return info.resolved.x86Address }
        var aotAddress:     RebasedAddress          { return info.resolved.aotAddress }
        var aotSize:        UInt64                  { return info.resolved.aotSize }
        var aotImageKey:    InlineArray<32, UInt8>  { return info.resolved.aotImageKey }
    }
}

internal extension Segment {
    struct Info: AtlasInfo, ~Copyable {
        let context:            Snapshot.DecoderContext
        let name:               BPList.FastString
        let vmSize:             UInt64
        let fileSize:           UInt64
        let preferredAddress:   PreferredAddress
        let address:            RebasedAddress
        let permissions:        UInt64
        init(unsafeBPListObject:BPList.UnsafeObject, context:(Snapshot.DecoderContext,Slide)) {
            self.context = context.0
            var name:               BPList.FastString!
            var vmSize:             UInt64!
            var fileSize:           UInt64!
            var preferredAddress:   PreferredAddress!
            var address:            RebasedAddress!
            var permissions:        UInt64!
            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue  {
                case "name": name = try! value.asFastString()
                case "size": vmSize = UInt64(try! value.asInt64())
                case "fsze": fileSize = UInt64(try! value.asInt64())
                case "perm": permissions = UInt64(try! value.asInt64())
                case "padr":
                    preferredAddress = PreferredAddress(try! value.asInt64())
                    address = preferredAddress + context.1
                default: return
                }
            }
            self.name = name
            self.vmSize = vmSize
            self.fileSize = fileSize
            self.preferredAddress = preferredAddress
            self.address = address
            self.permissions = permissions
        }
    }
}

 extension Segment {
     final class Impl {
         private var info:       Deferrable<Info>
         init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext, slide: Slide) {
             self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:(context,slide))
         }
         var name:                  String              { info.resolved.name.stringValue }
         var vmSize:                UInt64              { info.resolved.vmSize }
         var fileSize:              UInt64              { info.resolved.fileSize }
         var preferredLoadAddress:  PreferredAddress    { info.resolved.preferredAddress }
         var address:               RebasedAddress      { info.resolved.address }
         var permissions:           UInt64              { info.resolved.permissions }

         @inline(__always)
         var memoryBuffer:           MemoryBuffer {
             get throws(MemoryMapError) {
                 return try info.resolved.context.memoryBuffer(range:address.value..<(address.value + vmSize))
             }
         }

    }
}

extension Environment {
    struct Info: AtlasInfo, ~Copyable {
        let rootPath:   BPList.FastString?
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            var rootPath:               BPList.FastString?
            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue  {
                case "root": rootPath = try! value.asFastString()
                default: return
                }
            }
            self.rootPath = rootPath
        }
    }
}

extension Environment {
    internal final class Impl: AtlasImpl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
        }

        var rootPath: String?  { info.resolved.rootPath?.stringValue }
    }
}

//MARK: -
//MARK: Shared Cache

// So this is a bit more complex than any of the other types. The reason is the shared cache exists in the API (and the
// underlying data) in two different forms. The immutable info about the on disk shared cache, and the specific data about
// the shared cache in the process (such as the slide and bitmap of all the in use file.
//
// We handle that by having seperate Impls for each, and synethesizing the data together in the public facade. This keeps handling
// of all data sources clear and consistent.

internal extension SharedCache {
    struct Info: AtlasInfo, ~Copyable {
        let context:                Snapshot.DecoderContext
        let preferredLoadAddress:   PreferredAddress
        let uuid:                   UUID
        let pointerSize:            UInt64
        let vmSize:                 UInt64
        let localSymbolsUuid:       UUID?
        let localSymbolsFileName:   BPList.FastString?
        let images:                 BPList.UnsafeObject /* BPList.ArrayGuts */
        let subCaches:              BPList.UnsafeObject /* BPList.ArrayGuts */
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.context = context
            var preferredLoadAddress:   PreferredAddress!
            var uuid:                   UUID!
            var pointerSize:            UInt64!
            var vmSize:                 UInt64!
            var localSymbolsUuid:       UUID?
            var localSymbolsFileName:   BPList.FastString?
            var images:                 BPList.UnsafeObject!
            var subCaches:              BPList.UnsafeObject!

            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue  {
                case "padr": preferredLoadAddress = PreferredAddress(try! value.asInt64())
                case "uuid": uuid = UUID(bytes:try! value.asData())
                case "psze": pointerSize = UInt64(try! value.asInt64())
                case "size": vmSize = UInt64(try! value.asInt64())
                case "suid": localSymbolsUuid = UUID(bytes:try! value.asData())
                case "snme":
                    // FIXME: Hack to deal with malformed caches that with two snme keys
                    if localSymbolsFileName == nil {
                        localSymbolsFileName = try! value.asFastString()
                    }
                case "imgs": images = value.asUnsafeObject()
                case "dscs": subCaches = value.asUnsafeObject()
                default: return
                }
            }

            self.preferredLoadAddress = preferredLoadAddress
            self.uuid = uuid
            self.pointerSize = pointerSize
            self.vmSize = vmSize
            self.localSymbolsUuid = localSymbolsUuid
            self.localSymbolsFileName = localSymbolsFileName
            self.images = images
            self.subCaches = subCaches
        }
    }
}

extension SharedCache {
    internal final class Impl: AtlasImpl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
        }

        var mappedPrivate:          Bool                { return info.resolved.context.snapshot?.privateSharedRegion ?? false }
        var aotAddress:             RebasedAddress?     { return info.resolved.context.snapshot?.sharedCacheRecord?.aotAddress }
        var aotUuid:                UUID?               { return info.resolved.context.snapshot?.sharedCacheRecord?.aotUuid }
        var address:                RebasedAddress      { return info.resolved.context.snapshot?.sharedCacheRecord?.address ?? info.resolved.preferredLoadAddress + 0 }
        var preferredLoadAddress:   PreferredAddress    { return info.resolved.preferredLoadAddress }
        var uuid:                   UUID                { return info.resolved.uuid }
        var pointerSize:            UInt64              { return info.resolved.pointerSize }
        var vmSize:            UInt64                   { return info.resolved.vmSize }
        lazy var images: [Image.Impl] = {
            let context = info.resolved.context
            let imageArrayObject = info.resolved.images
            return try! imageArrayObject.object.asArray().map { Image.Impl(unsafeBPListObject:$0.asUnsafeObject(), context:context, slide:context.sharedCacheSlide) }
        }()
        lazy var subCaches: [SubCache.Impl] = {
            let context = info.resolved.context
            let subCacheArrayObject = info.resolved.subCaches
            return try! subCacheArrayObject.object.asArray().map { SubCache.Impl(unsafeBPListObject:$0.asUnsafeObject(), context:context) }
        }()
        lazy var filePaths: [String] = {
            guard let sharedCachePath = info.resolved.context.sharedCachePath?.removingLastComponent() else {
                return subCaches.map { subCache in
                    return subCache.name
                }
            }
            return subCaches.map { subCache in
                return sharedCachePath.appending(subCache.name).string
            }
        }()
        lazy var localSymbolsFilePath: FilePath? = {
            guard let localSymbolsFileName = info.resolved.localSymbolsFileName?.stringValue else { return nil }
            guard let sharedCachePath = info.resolved.context.sharedCachePath else { return FilePath(localSymbolsFileName) }
            return sharedCachePath.removingLastComponent().appending(localSymbolsFileName)
        }()
        lazy var localSymbolsPath: String? = {
            return localSymbolsFilePath?.string
        }()
        var localSymbolsUuid: UUID? { return info.resolved.localSymbolsUuid }

        var memoryMap: MemoryMap { return info.resolved.context.memoryMap }

        func pinMappings() -> Bool {
            return info.resolved.context.pinSharedCacheMappings()
        }
        func unpinMappings() {
            info.resolved.context.unpinSharedCacheMappings()
        }

        // TODO: Convert to RawSpan
        func withLocalSymbolFileBytes<T,E>(_ body: (UnsafeRawBufferPointer) throws(E) -> T) throws(E) -> T {
            guard let localSymbolsFilePath,
                  let buffer = mappedFileCache.fileBuffer(path:localSymbolsFilePath.string, offset:0, zerofill:0) else {
                let buffer = UnsafeRawBufferPointer(start:nil, count:0)
                return try body(buffer)
            }
            return try buffer.bytes.withUnsafeBytes { (buffer) throws(E) -> T in
                return try body(buffer)
            }
        }
    }
}

extension SharedCache {
    // There is never an instantiated ProcessRecord because it is not public, just ProcessRecord.Info ProcessRecord.Impl
    struct ProcessRecord {
        struct Info: AtlasInfo, ~Copyable {
            let context:        Snapshot.DecoderContext
            let uuid:           UUID
            let address:        RebasedAddress
            let bitmap:         BPList.UnsafeObject /* Bitmap */
            let filePathString: BPList.FastString
            let aotUuid:        UUID?
            let aotAddress:     RebasedAddress?

            init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
                self.context = context
                var uuid:           UUID!
                var address:        RebasedAddress!
                var bitmap:         BPList.UnsafeObject!
                var filePathString: BPList.FastString!
                var aotUuid:        UUID?
                var aotAddress:     RebasedAddress?

                try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                    switch key.stringValue  {
                    case "uuid": uuid = UUID(bytes:try! value.asData())
                    case "addr": address = RebasedAddress(try! value.asInt64())
                    case "bitm": bitmap = value.asUnsafeObject()
                    case "file": filePathString = try! value.asFastString()
                    case "auid": aotUuid = UUID(bytes:try! value.asData())
                    case "aadr": aotAddress = RebasedAddress(try! value.asInt64())
                    default: return
                    }
                }

                self.uuid = uuid
                self.address = address
                self.bitmap = bitmap
                self.filePathString = filePathString
                self.aotUuid = aotUuid
                self.aotAddress = aotAddress
            }
        }
        internal final class Impl: AtlasImpl {
            private var info:       Deferrable<Info>
            init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
                self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
            }

            var uuid:       UUID            { return info.resolved.uuid }
            var address:    RebasedAddress  { return info.resolved.address }
            var filePath:   FilePath        { return FilePath(info.resolved.filePathString.stringValue) }
            var aotUuid:    UUID?           { return info.resolved.aotUuid }
            var aotAddress: RebasedAddress? { return info.resolved.aotAddress }

            lazy var bitmap: Bitmap? = {
                let bitmapOject = info.resolved.bitmap
                return try! Bitmap(bytes:bitmapOject.object.asData())
            }()
        }
    }
}

extension SubCache {
    struct Info: AtlasInfo, ~Copyable {
        let context:                Snapshot.DecoderContext
        let name:                   BPList.FastString
        let uuid:                   UUID
        let vmSize:                 UInt64
        let fileSize:               UInt64
        let virtualOffset:          UInt64
        let preferredLoadAddress:   PreferredAddress

        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.context = context
            var name:                   BPList.FastString!
            var uuid:                   UUID!
            var vmSize:                 UInt64!
            var fileSize:               UInt64!
            var virtualOffset:          UInt64!
            var preferredLoadAddress:   PreferredAddress!

            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue  {
                case "name": name = try! value.asFastString()
                case "uuid": uuid = UUID(bytes:try! value.asData())
                case "size": vmSize = UInt64(try! value.asInt64())
                case "fsze": fileSize = UInt64(try! value.asInt64())
                case "voff": virtualOffset = UInt64(try! value.asInt64())
                case "padr": preferredLoadAddress = PreferredAddress(try! value.asInt64())
                default: return
                }
            }

            self.name = name
            self.uuid = uuid
            self.vmSize = vmSize
            self.fileSize = fileSize
            self.virtualOffset = virtualOffset
            self.preferredLoadAddress = preferredLoadAddress
        }
    }
}

extension SubCache {
    internal final class Impl: AtlasImpl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
        }
        var name:                   String              { return info.resolved.name.stringValue }
        var uuid:                   UUID                { return info.resolved.uuid}
        var vmSize:                 UInt64              { return info.resolved.vmSize}
        var fileSize:               UInt64              { return info.resolved.fileSize}
        var virtualOffset:          UInt64              { return info.resolved.virtualOffset }
        var preferredLoadAddress:   PreferredAddress    { return info.resolved.preferredLoadAddress }
        var address:                RebasedAddress      { return info.resolved.preferredLoadAddress + (info.resolved.context.sharedCacheSlide ?? 0) }
        var bytes:      RawSpan {
            @_lifetime(borrow self)
            borrowing get throws {
                assert(info.resolved.context.pinnedCacheMemoryMap != nil)
                let memoryBuffer = try info.resolved.context.memoryBuffer(range:address.value..<(address.value + vmSize))
                return unsafe _overrideLifetime(memoryBuffer.bytes, copying: self)
            }
        }
    }
}

//MARK: -
//MARK: Snapshot

extension Snapshot {
    struct Info: AtlasInfo, ~Copyable {
        let context:                    Snapshot.DecoderContext
        let pid:                        pid_t
        let platform:                   UInt64
        let timestamp:                  UInt64
        let state:                      UInt8
        let initialImageCount:          Int64
        let flags:                      SnapshotFlags
        let images:                     BPList.UnsafeObject /* BPList.ArrayGuts */
        let aotImages:                  BPList.UnsafeObject?   /* BPList.ArrayGuts */
        let sharedCacheProcessRecord:   BPList.UnsafeObject?    /* BPList.DictionaryGuts */
        let environment:                BPList.UnsafeObject?    /* BPList.DictionaryGuts */
        let metrics:                    BPList.UnsafeObject?    /* BPList.DictionaryGuts */
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.context = context
            var pid:                        pid_t!
            var platform:                   UInt64!
            var timestamp:                  UInt64!
            var state:                      UInt8!
            var initialImageCount:          Int64!
            var flags:                      SnapshotFlags?
            var images:                     BPList.UnsafeObject!
            var aotImages:                  BPList.UnsafeObject?
            var sharedCacheProcessRecord:   BPList.UnsafeObject?
            var environment:                BPList.UnsafeObject?
            var metrics:                    BPList.UnsafeObject?
            try! unsafeBPListObject.object.asDictionary().forEach { key, value in
                switch key.stringValue  {
                case "proc":    pid = pid_t(try! value.asInt64())
                case "plat":    platform = UInt64(try! value.asInt64())
                case "time":    timestamp = UInt64(try! value.asInt64())
                case "stat":    state = UInt8(try! value.asInt64())
                case "init":    initialImageCount = try! value.asInt64()
                case "flags":   flags = SnapshotFlags(rawValue: try! value.asInt64())
                case "imgs":    images = value.asUnsafeObject()
                case "aots":    aotImages = value.asUnsafeObject()
                case "dsc1":    sharedCacheProcessRecord = value.asUnsafeObject()
                case "envp":    environment = value.asUnsafeObject()
                case "metr":    metrics = value.asUnsafeObject()
                default: return
                }
            }
            self.pid = pid
            self.platform = platform
            self.timestamp = timestamp
            self.state = state
            self.initialImageCount = initialImageCount
            self.flags = flags ?? SnapshotFlags(rawValue: 0)
            self.images = images
            self.aotImages = aotImages
            self.sharedCacheProcessRecord = sharedCacheProcessRecord
            self.environment = environment
            self.metrics = metrics
        }
    }
}

extension Snapshot {
    // Internal implemenation class. We do this so that the `Decodable` conformance does not
    // leak into the ABI
    internal final class Impl: AtlasImpl {
        private var info:       Deferrable<Info>
        init(unsafeBPListObject:BPList.UnsafeObject, context:Snapshot.DecoderContext) {
            self.info = Deferrable<Info>(unsafeBPListObject:unsafeBPListObject, context:context)
        }

        private var flags:          SnapshotFlags   { return info.resolved.flags }
        var pid:                    pid_t           { return info.resolved.pid }
        var platform:               UInt64          { return info.resolved.platform }
        var timestamp:              UInt64          { return info.resolved.timestamp }
        var state:                  UInt8           { return info.resolved.state }
        var initialImageCount:      Int64           { return info.resolved.initialImageCount }
        var pageSize:               UInt64          { return info.resolved.flags.contains(.pageSize4k) ? 4096 : 16384 }
        var pointerSize:            UInt64          { return info.resolved.flags.contains(.pointerSize4Bytes) ? 4 : 8 }
        var privateSharedRegion:    Bool            { return info.resolved.flags.contains(.privateSharedRegion) }
        lazy var sharedCacheRecord:      SharedCache.ProcessRecord.Impl? = {
            guard let sharedCacheProcessRecord = info.resolved.sharedCacheProcessRecord else { return nil }
            return SharedCache.ProcessRecord.Impl(unsafeBPListObject:sharedCacheProcessRecord, context:info.resolved.context)
        }()
        lazy var env: Environment.Impl? = {
            guard let envObject = info.resolved.environment else { return nil}
            return Environment.Impl(unsafeBPListObject:envObject, context:info.resolved.context)
        }()

        lazy var aotImages: [AOTImage.Impl]? = {
            let context = info.resolved.context
            let imageArrayObject = info.resolved.aotImages
            return try! imageArrayObject?.object.asArray().map {
                return AOTImage.Impl(unsafeBPListObject:$0.asUnsafeObject(), context:context)
            }
        }()

        lazy var images: [Image.Impl] = {
            let context = info.resolved.context
            let imageArrayObject = info.resolved.images
            var result = try! imageArrayObject.object.asArray().map {
                return Image.Impl(unsafeBPListObject:$0.asUnsafeObject(), context:context)
            }
            if  let sharedCache = context.sharedCache,
                let bitmap = context.sharedCacheBitmap {
                let cacheImages = zip(sharedCache.images, bitmap.entries).filter { $0.1 }.map { $0.0 }
                result = Array.mergeArraySorted(result, cacheImages) {
                    $0.address < $1.address
                }
            }
            return result
        }()

        lazy var sharedCache: SharedCache.Impl? = {
            return info.resolved.context.sharedCache
        }()

        lazy var metrics: [String:Int64]? = {
            guard let metrics = info.resolved.metrics else { return nil }
            var result = [String:Int64]()
            try! metrics.object.asDictionary().forEach { key, value in
                result[key.stringValue] = try! value.asInt64()
            }
            return result
        }()
    }
}

extension Array {
    static func mergeArraySorted<E>(_ x:[Self.Element], _ y:[Self.Element], by areInIncreasingOrder: (Self.Element, Self.Element) throws(E) -> Bool) throws(E) -> [Self.Element] {
        var result = [Self.Element]()
        result.reserveCapacity(x.count + y.count)
        var x = x
        var y = y
        while x.count > 0 && y.count > 0 {
            if try areInIncreasingOrder(x.first!, y.first!) {
                result.append(x.removeFirst())
            } else {
                result.append(y.removeFirst())
            }
        }
        return result + x + y
    }
}



//FIXME: This is so unsafe I can't even begin to dsecribe it.
// The only reason we can do it is because we have examined the code of all clients, and given the existing
// SPIs and design issues this is safe for those clients and the only way to make this work. As soon as we
// expose new SPIs we should transition existing clients off of the existing ones and obsolete the existing SPIs


extension DispatchQueue {
    static let notifierQueueKey: DispatchSpecificKey<Bool> = DispatchSpecificKey<Bool>()
    func asyncAndWaitRecursiveHACK<T>(_ block: () throws -> T) rethrows -> T {
        if isOnHACKQueue {
            return try block()
        } else {
            return try self.asyncAndWait {
                return try block()
            }
        }
    }
    func markAsNotifierQueue() {
        self.setSpecific(key:DispatchQueue.notifierQueueKey, value: true)
    }
    var isOnHACKQueue: Bool {
        if let onQueue = self.getSpecific(key: DispatchQueue.notifierQueueKey), onQueue == true {
            return true;
        }
        return false
    }
}
extension Process {
    internal final class Impl {
        var notifierClients = [UInt32:ProcessNotifierRecord]()
        var updateClients = [UInt32:ProcessUpdateRecord]()
        enum NotifierState {
            case connected(DispatchSourceMachReceive, mach_port_context_t)
            // This is the naked mach_port because the existing source has been cancelled, but the underlying port may be reused
            case disconnecting(mach_port_t, mach_port_context_t)
            case disconnected
            case failing(kern_return_t, mach_port_t, mach_port_context_t)
            case failed(kern_return_t)
            var isConnected: Bool {
                switch self {
                case .connected:
                    return true
                default:
                    return false
                }
            }
            var isFailed: Bool {
                switch self {
                case .failed:
                    return true
                default:
                    return false
                }
            }
        }

        private let task: MachTask
        private var notifierState: NotifierState = .disconnected
        // FIXME: This should be read only, but for bincompat with existing clients we need a hack to set this here.
        var queue: DispatchQueue? {
            get {
                return _queue
            }
            set {
                // The dyld_process_introspection notifiers take per event queues, but those can cause dead locks. It turns out no one
                // uses that functionality, the only client (OrderFiles) sets up a single process wide global queue. All other clients
                // use the older SPIs that setup a queue for the process. Because of that know this should only ever be called once,
                // but just in case lets swallow the event here... there is no "correct" answer here, but presumably no one is using
                // the existing interface incorrectly or they would deadlock
                guard _queue == nil else {
                    return
                }
                _queue = newValue
                _queue!.markAsNotifierQueue()

            }
        }
        private var _queue: DispatchQueue?

        var currentId: UInt32

        class ProcessNotifierRecord {
            let block:  () -> Void
            let event:  UInt32
            var active: Bool = true
            init(event: UInt32, block: @escaping () -> Void) {
                self.event  = event
                self.block  = block
            }
            func run() {
                guard active else { return }
                block()
            }
        };
        class ProcessUpdateRecord {
            let block: (Image,Bool) -> Void
            var active: Bool = true
            init(block: @escaping (Image,Bool) -> Void) {
                self.block  = block
            }
            func run(_ image: Image, _ loading: Bool) {
                guard active else { return }
                block(image,loading)
            }
        };

        private var currentSnapshot: Snapshot.Impl
        init(task: task_read_t, queue: DispatchQueue? = nil) throws {
            self.task               = try MachTask(task)
            self._queue              = queue
            self.currentId          = 1
            self.currentSnapshot    = try Process.Impl.getNewSnapshot(self.task)
            if let _queue {
                _queue.markAsNotifierQueue()
            }
        }
        deinit {
            // For the moment while we have no clients except the shim lets enforce that clients need to
            // unregister prior to object destruction. We can consider doing it implicitly later, but it is
            // difficult with respect to some of the queue invaraiants
            if let queue {
                queue.asyncAndWaitRecursiveHACK {
                    teardownNotifications(&notifierState)
                }
            }
        }
        func getCurrentSnapshot() throws -> Snapshot.Impl {
            return try Process.Impl.getNewSnapshot(self.task, currentSnapshot:currentSnapshot)
        }
        func handleNotifications(_ state: inout NotifierState, _ notification: UInt32) {
            guard state.isConnected else { return }
            if notification == DYLD_REMOTE_EVENT_ATLAS_CHANGED {
                guard let newSnapshot = try? Process.Impl.getNewSnapshot(self.task) else {
                    // TODO: better error handling and tear down
                    return
                }
                if newSnapshot.timestamp <= (currentSnapshot.timestamp) {
                    // The snapshot is not newer than what we have. That can happen due to a notification being queued due to a dylib load while the
                    // notifier was being setup, ignore it.
                    return
                }
                // We need to update currentSnapshot before we call the notifiers incase they choose
                // to query it
                let oldSnapshot = currentSnapshot
                currentSnapshot = newSnapshot

                let changes = currentSnapshot.images.difference(from:oldSnapshot.images)
                // FIXME: Re-entrancy issue
                // So technically this in that calling unregister (or register for that matter) inside of a handler will recursively enter
                // the unfair lock and assert. It appears the old code had some issue around this as well, (though it was on queue using asnc and wait).
                // Fortunately that means no extant clients should do that, but we should fix it. We can do that by mocing to use .async here when
                // we go onto the client provided queue, which will get us off the internal queue so asyncAndWait to the internal queue wll not trigger
                // a re-enter. Having said that, doing that requies a rework of the locking AND fixing up strict concurrency warnings, so not for today.
                for (_, updateClient) in updateClients {
                    for change in changes {
                        switch change {
                        case .remove(_, let element, _):
                            updateClient.run(Image(wrapping: element), false)
                        case .insert(_, let element, _):
                            updateClient.run(Image(wrapping: element), true)
                        }
                    }
                }
            } else {
                for (_,notifierClient) in notifierClients where notifierClient.event == notification {
                    notifierClient.run()
                }
            }
        }
        private func setupAndGetNextNotifierId(_ state: inout NotifierState) throws -> UInt32 {
            switch state {
            case .connected:
                break
            case .disconnected, .disconnecting:
                let kr = setupNotifications(&state)
                guard kr == KERN_SUCCESS else { throw AtlasError.machError(kr) }
            case .failed, .failing:
                // TODO: We should distinguish between internal failures and process level failures
                throw AtlasError.stateMachineError
            }
            let result = currentId
            currentId += 1
            return result
        }
        func registerForChangeNotifications(_ block: @escaping (_ image: Image, _ load: Bool) -> Void) throws -> UInt32 {
            guard let queue else {
                fatalError("Setting up notifications requires a queue")
            }
            return try queue.asyncAndWaitRecursiveHACK {
                let result = try setupAndGetNextNotifierId(&notifierState);
                guard notifierState.isConnected else {
                    throw AtlasError.stateMachineError
                }
                updateClients[result] = ProcessUpdateRecord(block: block)
                // We forcibly update the the snapshot here, after we registered a notifier. The reason is if we lose a notification the
                // the delta weill be wrong. This way if a notification is firing simultaneously it will be on this queue behind the update,
                // and since it synchrnous it will be the only one, ensuring the delta is correct.
                self.currentSnapshot = try Process.Impl.getNewSnapshot(self.task)
                for image in currentSnapshot.images {
                    block(Image(wrapping:image), true)
                }
                return result
            }
        }
        func register(event:UInt32, _ handler: @escaping () -> Void) throws -> UInt32 {
            guard let queue else {
                fatalError("Setting up notifications requires a queue")
            }
            return try queue.asyncAndWaitRecursiveHACK {
                let result = try setupAndGetNextNotifierId(&notifierState);
                guard notifierState.isConnected else {
                    throw AtlasError.stateMachineError
                }
                notifierClients[result] = ProcessNotifierRecord(event:event, block:handler)
                return result
            }
        }
        func unregister(event: UInt32) {
            guard let queue else {
                fatalError("Tearing down notifications requires a queue")
            }
            queue.asyncAndWaitRecursiveHACK {
                guard notifierState.isConnected else {
                    return
                }
                if let updateClient = updateClients[event] {
                    updateClient.active = false
                    updateClients.removeValue(forKey:event)
                }
                if let notifierClient = notifierClients[event] {
                    notifierClient.active = false
                    notifierClients.removeValue(forKey:event)
                }
                // FIXME: There is a race somewhere in the reconnect logic.
                // disconnecting it just an optimization that in practice no one uses, so disable it for now
//                if updateClients.isEmpty && notifierClients.isEmpty {
//                    teardownNotifications(&notifierState)
//                }
            }
        }
        static func getNewSnapshot(_ task: borrowing MachTask, currentSnapshot:Snapshot.Impl? = nil) throws(AtlasError) -> Snapshot.Impl {
            // The atlas update is atomic, but our read of the atlas info and subsequent read of the atlas are not, so it is possible that:
            // 1. We read the atlas info
            // 2. The remote process updates the atlas info (which points to a new valid atlas)
            // 3. It frees the old atlas
            // 4. We read the old location as it is being torn down
            //
            // Generally that is not an issue for most use cases, for example in a notifer the remote process is synchornously blocked, and because it
            // takes a lot more work to update an atlas then to copy it if we immediately retry we will almost always succeed. In some pathological
            // cases if the reader is on a e-core and the writer is a p-core this could live lock, so put an upper bound on the iteration. Once we move
            // to kernel owned images infos that will form a natural synchronization point and this won't be necessary.
            for _ in 0..<100 {
                var atlasAddress:   RemoteAddress
                let taskDyldInfo    = try task.dyldInfo()
                var atlasSize       = UInt64(0)
                var timestamp       = UInt64(0)
                if taskDyldInfo.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_64 {
                    let allImageInfos: dyld_all_image_infos_64 = try task.readStruct(address:RemoteAddress(taskDyldInfo.all_image_info_addr))
                    // On the right side we need to access these values atomically but here we don't need to worry since the mapping
                    // is not shared and the vm_copy acted as a memory barrier
                    atlasAddress    = RemoteAddress(allImageInfos.compact_dyld_image_info_addr)
                    atlasSize       = allImageInfos.compact_dyld_image_info_size
                    timestamp       = allImageInfos.infoArrayChangeTimestamp
                } else if taskDyldInfo.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_32 {
                    let allImageInfos: dyld_all_image_infos_32 = try task.readStruct(address:RemoteAddress(taskDyldInfo.all_image_info_addr))
                    atlasAddress    = RemoteAddress(allImageInfos.compact_dyld_image_info_addr)
                    atlasSize       = UInt64(allImageInfos.compact_dyld_image_info_size)
                    timestamp       = allImageInfos.infoArrayChangeTimestamp
                } else {
                    throw AtlasError.taskInfoFormatUnknown(taskDyldInfo.all_image_info_format )
                }
                if atlasSize == 0 || timestamp == 0 {
                    // Process is not started, try to scavenge
                    var scavengeedBufferSize: UInt64 = 0;
                    var scavengeedBuffer: UnsafeMutableRawPointer? = nil
                    if scavengeProcess(task.port, &scavengeedBuffer, &scavengeedBufferSize) {
                        let snapshotBuffer = MemoryBuffer(malloced:scavengeedBuffer!, count:Int(scavengeedBufferSize))
                        let context = Snapshot.DecoderContext(snapshotBuffer:snapshotBuffer)
                        return context.snapshot!
                    }
                    throw AtlasError.scavangerFailed
                }
                if let currentSnapshot,
                   currentSnapshot.timestamp == timestamp {
                    return currentSnapshot
                }
                do throws(AtlasError) {
                    let snapshotBuffer = try task.readData(address:atlasAddress, size:atlasSize)
                    // Fast CRC validation before parsing - if it fails, treat as memory shearing and retry
                    guard AppleArchive.validateCRCs(bytes: snapshotBuffer.bytes) else {
                        continue
                    }
                    let context = Snapshot.DecoderContext(snapshotBuffer:snapshotBuffer, snapshotPreValidated:true)
                    guard let result = context.snapshot else { throw .missingPlist }
                    return result
                } catch {
                    switch error {
                    case .truncatedVMRead, .machError(_), .aarDecoderError(_):
                        // These errors can happen do to memory shearing during the update, so retry
                        continue
                    default:
                        // All other errors we throw
                        throw error
                    }
                }
            }
            throw .memoryUpdateError
        }

        func handleMachMessage() {
            guard case let .connected(source, _) = notifierState else {
                return
            }
            // This event handler block has an implicit reference to "this"
            // if incrementing the count goes to one, that means the object may have already been destroyed
            // FIXME: Once we remove the legacy notifier which uses in band messages we can reduce the size of the of the buffer here
            // We do not need to anything with the legacy messages, we service the old APIs with the new messages, but we need to be
            // able to correctly receive them and respond to them so that the observed process continues running.
            let bufferSize = 32*1024
            //        let bufferSize = MemoryLayout<mach_msg_header_t>.size + MemoryLayout<mach_msg_max_trailer_t>.size
            let messagePtr = UnsafeMutableRawPointer.allocate(byteCount:bufferSize, alignment:16)
            var header = mach_msg_header_t()
            defer {
                messagePtr.deallocate()
            }
            do {
                try messagePtr.withMemoryRebound(to:mach_msg_header_t.self, capacity:1) {
                    let kr = mach_msg($0, mach_msg_option_t(MACH_RCV_MSG | MACH_RCV_VOUCHER), 0, mach_msg_size_t(bufferSize),
                                      source.handle, MACH_MSG_TIMEOUT_NONE, mach_port_name_t(MACH_PORT_NULL))
                    guard kr == KERN_SUCCESS else {
                        teardownNotifications(&notifierState, error:kr)
                        throw AtlasError.machError(kr)
                    }
                    header = $0[0]
                }
            } catch {
                // We are tossing the error, we should find some way to bubble it up in a Swift native API
                return
            }
            defer {
                messagePtr.withMemoryRebound(to:mach_msg_header_t.self, capacity:1) {
                    mach_msg_destroy($0)
                }
            }
            let msgID = UInt32(bitPattern:header.msgh_id)
            guard (header.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0,
                  ((msgID & 0xFFFFF000) == DYLD_PROCESS_EVENT_ID_BASE
                   || msgID == DYLD_PROCESS_INFO_NOTIFY_LOAD_ID
                   || msgID == DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID
                   || msgID == DYLD_PROCESS_INFO_NOTIFY_MAIN_ID),
                  //FIXME: Once we disable the legacy path we can validate the exact size
                  header.msgh_size <= 32*1024 else {
                //              header.msgh_size == MemoryLayout<mach_msg_header_t>.size else {
                // Unknown message that is not a notification, disconnect
                //FIXME: Notification fixes
                teardownNotifications(&notifierState, error:KERN_INVALID_ARGUMENT)
                // Someone might have tried to spoof a disconnect here... we should log it somehow
                return
            }
            if (msgID & 0xFFFFF000) == DYLD_PROCESS_EVENT_ID_BASE {
                handleNotifications(&notifierState, UInt32(bitPattern:header.msgh_id) & ~0xFFFFF000)
            }
            var replyHeader = mach_msg_header_t()

            replyHeader.msgh_bits           = header.msgh_bits & 0x0000001f /* MACH_MSGH_BITS_SET(MACH_MSGH_BITS_REMOTE(header.msgh_bits), 0, 0, 0) */
            replyHeader.msgh_id             = 0;
            replyHeader.msgh_local_port     = mach_port_t(MACH_PORT_NULL)
            replyHeader.msgh_remote_port    = header.msgh_remote_port
            replyHeader.msgh_voucher_port   = 0;
            replyHeader.msgh_size           = mach_msg_size_t(MemoryLayout<mach_msg_header_t>.size)
            let kr = mach_msg(&replyHeader, MACH_SEND_MSG, replyHeader.msgh_size, 0, mach_port_t(MACH_PORT_NULL), 0, mach_port_t(MACH_PORT_NULL));
            guard kr == KERN_SUCCESS else {
                teardownNotifications(&notifierState, error:kr)
                return
            }
            messagePtr.withMemoryRebound(to:mach_msg_header_t.self, capacity:1) {
                $0.pointee.msgh_remote_port = mach_port_t(MACH_PORT_NULL)
            }
        }

        //FIXME: Probably change this a typed throw once we moved to Swift 6
        func setupNotifications(_ state: inout NotifierState) -> kern_return_t {
            precondition(queue!.isOnHACKQueue == true)
            var port: mach_port_t
            var portContext: mach_port_context_t
            switch state {
            case .connected:
                // If the connect is already connected do not try to reconnect, but return success
                return KERN_SUCCESS
            case .failed(let kr), .failing(let kr, _, _):
                return kr
            case .disconnecting(let existingPort, let existingPortContext):
                // Reuse the existing port and context, we will create a new source from them
                // and change the state to connected to prevent their disposal in setCancelHandler:
                port = existingPort
                portContext = existingPortContext
            case .disconnected:
                // A fully disconnected state means we need to setup a port and register it
                var kr = kern_return_t(KERN_SUCCESS)

                // Allocate a port to listen on in this monitoring task
                portContext             = mach_port_context_t.random(in:0..<mach_port_context_t.max)
                var options             = mach_port_options_t()
                options.flags           = UInt32(MPO_IMPORTANCE_RECEIVER | MPO_CONTEXT_AS_GUARD | MPO_STRICT)
                options.mpl.mpl_qlimit  = mach_port_msgcount_t(MACH_PORT_QLIMIT_DEFAULT)

                port = mach_port_t(MACH_PORT_NULL)
                kr = mach_port_construct(mach_task_self_, &options, portContext, &port)
                guard kr == KERN_SUCCESS else {
                    state = .failed(kr)
                    return kr
                }
                kr = task_dyld_process_info_notify_register(task.port, port);
                guard kr == KERN_SUCCESS else {
                    mach_port_destruct(mach_task_self_, port, 0, portContext);
                    state = .failed(kr)
                    return kr
                }
            }

            let machSource = DispatchSource.makeMachReceiveSource(port:port, queue:queue)
            machSource.setEventHandler { [self] in
                handleMachMessage()
            }
            machSource.setCancelHandler { [self] in
                switch notifierState {
                case .connected, .disconnected, .failed:
                    // We either reconnected, already disconnected, or failed, nothing to do here
                    return
                case .disconnecting(let port, let context):
                    // We only destroy this port if the state is disconnecting. There is a chance that after the tear down we started to reconnect
                    // If so then the state would have been changed to .connected. While this source will be torn down, the underlying mach
                    task_dyld_process_info_notify_deregister(task.port, port)
                    mach_port_destruct(mach_task_self_,port, 0, context);
                    notifierState = .disconnected
                case .failing(let kr, let port, let context):
                    // Same as above, but we go from failing to failed
                    task_dyld_process_info_notify_deregister(task.port, port)
                    mach_port_destruct(mach_task_self_,port, 0, context);
                    notifierState = .failed(kr)
                }
            }
            machSource.activate()
            state = .connected(machSource, portContext)
            return KERN_SUCCESS
        }

        func teardownNotifications(_ state: inout NotifierState, error: kern_return_t? = nil) {
            precondition(queue!.isOnHACKQueue == true)
            switch state {
            case .connected(let source, let portContext):
                updateClients.removeAll()
                notifierClients.removeAll()
                if let error {
                    state = .failing(error, source.handle, portContext)
                } else {
                    state = .disconnecting(source.handle, portContext)
                }
                source.cancel()
            case .disconnected:
                if let error {
                    state = .failed(error)
                }
            case .disconnecting(let port, let context):
                if let error {
                    state = .failing(error, port, context)
                }
            case .failed, .failing:
                // Already failed, return
                return
            }
        }
    }
}

//MARK: -
//MARK: Utility extensions

internal extension UUID  {
    init?(unsafeData: Data?) {
        guard let unsafeData else { return nil }
        let uuid = unsafeData.withUnsafeBytes { (pointer: UnsafeRawBufferPointer) -> uuid_t in
            return pointer.load(as: uuid_t.self)
        }
        self.init(uuid:uuid)
    }
    init(unsafeData: Data) {
        let uuid = unsafeData.withUnsafeBytes { (pointer: UnsafeRawBufferPointer) -> uuid_t in
            return pointer.load(as: uuid_t.self)
        }
        self.init(uuid:uuid)
    }
    init(bytes: RawSpan) {
        let uuid = bytes.unsafeLoadUnaligned(as:uuid_t.self)
        self.init(uuid:uuid)
    }

    init?(bytes: RawSpan?) {
        guard let bytes else { return nil }
        let uuid = bytes.unsafeLoadUnaligned(as:uuid_t.self)
        self.init(uuid:uuid)
    }
}

internal extension Range {
    func contains(_ other: Range) -> Bool {
        lowerBound <= other.lowerBound && upperBound >= other.upperBound
    }
}

internal extension UnsafePointer {
    func qpointer<Property>(to property: KeyPath<Pointee, Property>) -> UnsafePointer<Property>? {
        guard let offset = MemoryLayout<Pointee>.offset(of: property) else { return nil }
        return (UnsafeRawPointer(self) + offset).assumingMemoryBound(to: Property.self)
    }
}

internal extension dirent {
    var name: String {
        // We want to always use unsafe pointers here and not to ever use nthe "safe" types. The issue is that C struct
        // lies about its size which may be truncated to the length of the string (aka `self.d_namlen` here), which means
        // tries to copy the struct it can read past the end of the buffer. Instead we access it by the pointer and limit
        // the capacity manually.
        // We would declare a conformance to non-Copyable, but that would need to be public and code be a compatibility
        // issue for other code linking to us.
        return  withUnsafePointer(to: self.d_name) {
            // d_namlen does not include the null terminator, so add one to it since d_name is null terminated
            return $0.withMemoryRebound(to: CChar.self, capacity: MemoryLayout.size(ofValue: Int(self.d_namlen+1))) {
                return String(cString: $0)
            }
        }
    }
}

//MARK: -
//MARK: Bitmap

internal class Bitmap {
    fileprivate let buffer: UnsafeRawBufferPointer
    init(bytes: borrowing RawSpan) {
        let mutableBuffer = UnsafeMutableRawBufferPointer.allocate(byteCount:bytes.byteCount, alignment:0)
        // FIXME: This should be @discardableResult in the SDK
        _ = bytes.withUnsafeBytes {
            $0.copyBytes(to:mutableBuffer)
        }
        self.buffer = UnsafeRawBufferPointer(mutableBuffer)
    }
    deinit {
        buffer.deallocate()
    }
    var entries: BitmapSequence {
        return BitmapSequence(self.buffer)
    }
    struct Iterator: IteratorProtocol {
        var index: Int
        let buffer: UnsafeRawBufferPointer
        init(_ buffer: UnsafeRawBufferPointer) {
            self.buffer = buffer
            self.index = 0
        }
        public mutating func next() -> Bool? {
            let byteIndex = index>>3
            if byteIndex >= buffer.count {
                return nil
            }
            defer { index += 1 }

            return buffer.withMemoryRebound(to: UInt8.self) { (typedBuffer: UnsafeBufferPointer<UInt8>) throws(Never) -> Bool in
                let currentByte = typedBuffer[byteIndex]
                let mask: UInt8 = 1<<(index & ((1<<3)-1))
                let result: Bool = (currentByte & mask) == mask
                return result
            }
        }
    }
    struct BitmapSequence: Sequence {
        let buffer: UnsafeRawBufferPointer
        init(_ buffer: UnsafeRawBufferPointer) {
            self.buffer = buffer
        }
        public func makeIterator() -> Iterator {
            return Iterator(self.buffer)
        }
    }
}

