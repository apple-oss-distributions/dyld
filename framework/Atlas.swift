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
// implement facade structs with the names we want (like `Image`), which are thing shims on non-public classes (like `Image.Impl`)
// that do all the work to load the data. This seperatation has several nice properties:
//
// * It reduces our ABI surface
// * It allows to us use classes for the internal implementations. This is useful because a single `Image.Impl` can back
//   multiple images, for example in the list of shared cache images and the list of active images in the process

//TODO: Figure out what kind of errors we want to hand to public API users

//internal enum Atlas: Error {
////    case archiveError(ArchiveError)
//    case uuidDecodeSizeError(Int)
//    case cacheDecodeError
//    case uuidDecodeError
//    case mapperError
//    case dataLoadError
//    case machError(kern_return_t)
//    case openError(errno_t)
//    case fstatError(errno_t)
//    case mmapError(errno_t)
//    case unknownDyldInfoFormat
//    case partialVMReadError
//    case plistDecodeError
//    case fixme(String)
//}

enum AtlasError: Error, BPListErrorWrapper {
    case machError(kern_return_t)
    case bplistError(BPListError)
    case aarDecoderError(AARDecoderError)
    case schemaValidationError
    case truncatedVMRead
    case missingPlist
    case memoryUpdateError
    case placeHolder
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

//MARK: -
//MARK: Image

internal struct Image: Equatable, Hashable {
    private let impl:                   Image.Impl
    public var filePath:                String?             { return impl.filePath }
    public var installname:             String?             { return impl.installname }
    public var address:                 RebasedAddress      { return impl.address }
    public var segments:                [Segment]           { return impl.segments.map { Segment(wrapping:$0) } }
    public var preferredLoadAddress:    PreferredAddress    { return impl.preferredLoadAddress }
    public var uuid:                    UUID?               { return impl.uuid }
    public var pointerSize:             UInt64              { return impl.pointerSize }
    public var sharedCache:             SharedCache?        { return impl.sharedCache }

    internal init(wrapping impl: Image.Impl) {
        self.impl       = impl
    }

    var info: Image.Info { return impl.info }

    static func == (lhs: Image, rhs: Image) -> Bool {
        return lhs.impl.identifier == rhs.impl.identifier
    }
    func hash(into hasher: inout Hasher) {
        hasher.combine(impl.identifier)
    }
}

internal extension Image {
    struct Info {
        let uuid:                   UUID?
        let filePath:               BPList.FastString?
        let installname:            BPList.FastString?
        let preferredAddress:       PreferredAddress?
        let address:                RebasedAddress
        let sharedCache:            SharedCache.Impl?
    }
}

extension Image {
    internal final class Impl: Equatable, Hashable {
        static func == (lhs: Image.Impl, rhs: Image.Impl) -> Bool {
            return lhs.identifier == rhs.identifier
        }
        func hash(into hasher: inout Hasher) {
            hasher.combine(identifier)
        }
        var segments: [Segment.Impl] {
            let slide = address - preferredLoadAddress
            return bplist["segs", asArrayOf:BPList.UnsafeRawDictionary.self]!.map {
                return Segment.Impl(bplist:$0, context:context, slide: slide, mapper:mapper)
            }
        }
        var uuid:                   UUID?               { return info.uuid }
        var filePath:               String?             { return info.filePath?.value }
        var installname:            String?             {
            return info.installname?.value
        }
        var preferredLoadAddress:   PreferredAddress    { return info.preferredAddress ?? PreferredAddress(UInt64(0)) }
        var mapper: Mapper? {
            if bplist["addr", as:Int64.self] != nil {
                return MachOMapper(image:self, segments:bplist["segs", asArrayOf:BPList.UnsafeRawDictionary.self]!)
            } else {
                return context.sharedCache?.cacheMapper
            }
        }

        private var _info:  Info?
        var info: Info {
            if _info != nil { return _info! }
            var uuid:                   UUID?
            var filePath:               BPList.FastString?
            var installname:            BPList.FastString?
            var preferredAddress:       PreferredAddress?
            var address:                RebasedAddress?
            var sharedCache:            SharedCache.Impl?
            do {
                for (key, value) in bplist {
                    if key == .string("name") {
                        if  try! value.isAsciiString {
                            installname = .asciiBuffer(UnsafeMutableRawBufferPointer(mutating:try value.unsafeRawBuffer()))
                        } else {
                            installname = .bplist(value)
                        }
                    }
                    if key == .string("file") {
                        if try! value.isAsciiString {
                            filePath = .asciiBuffer(UnsafeMutableRawBufferPointer(mutating:try value.unsafeRawBuffer()))
                        }
                        filePath =  .bplist(value)
                    }
                    if key == .string("uuid") {
                        let unsafeData = try value.unsafeRawBuffer()
                        let uuidData = unsafeData.withUnsafeBytes { (pointer: UnsafeRawBufferPointer) -> uuid_t in
                            return pointer.load(as: uuid_t.self)
                        }
                        uuid = UUID(uuid:uuidData)
                    }
                    if key == .string("addr") {
                        address = RebasedAddress(try value.asInt64())
                    }
                    if key == .string("padr") {
                        preferredAddress = PreferredAddress(try value.asInt64())
                    }
                }
                // Images will either:
                // 1. Just have a preferred load address. These are image records in the shared cache atlas records, and need to fixed up by adding the cache
                //    slide, which we will do here.
                // 2. Have just an address, in which case we should use it
                // 3. Both a preferred load address, and a load address. These are in memory atlas records generated by dyld, and we should use the
                //    address. They can only happen on platforms which support preferred load addresses (`x86_64`)
                // 4. Have none. these are malformed, but we should return nil in that case
                if let preferredAddress, address == nil {
                    // This handles case 1
                    address     = preferredAddress + context.sharedCacheSlide
                    sharedCache = context.sharedCache!
                }
                _info = Info(uuid:uuid, filePath:filePath, installname:installname, preferredAddress:preferredAddress, address:address!, sharedCache:sharedCache)
            } catch {
                fatalError("Image parse failure")
            }
//            _info = Info(name:name, vmSize:vmSize, fileSize:fileSize, preferredAddress:preferredAddress, address:address, permissions:permissions)
            return _info!
        }

        var sharedCache: SharedCache? {
            if bplist["addr", as:Int64.self] != nil {
                return nil
            } else {
                return SharedCache(wrapping:context.sharedCache!)
            }
        }
        var address: RebasedAddress {
            if let loadAddress = bplist["addr", as:Int64.self]  {
                return RebasedAddress(loadAddress)
            } else {
                return PreferredAddress(bplist["padr", as:Int64.self]!) + context.sharedCacheSlide
            }
        }

        var pointerSize: UInt64 { return context.pointerSize }
        var identifier:     FileIdentifier { return FileIdentifier(uuid:uuid, path: filePath) }

        init(bplist: BPList.UnsafeRawDictionary, context:Snapshot.DecoderContext) {
            self.bplist = bplist
            self.context = context
        }
        let bplist:  BPList.UnsafeRawDictionary
        let context: Snapshot.DecoderContext
    }
}

internal struct AOTImage {
    private let impl: Impl
    public var x86Address:  RebasedAddress  { return impl.x86Address }
    public var aotAddress:  RebasedAddress  { return impl.aotAddress }
    public var aotSize:     UInt64          { return impl.aotSize }
    public var aotImageKey: Data            { return impl.aotImageKey }
    internal init(wrapping impl: AOTImage.Impl) {
        self.impl = impl
    }
}

extension AOTImage {
    internal final class Impl {
        var x86Address:     RebasedAddress  { return RebasedAddress(bplist["xadr", as:Int64.self]!) }
        var aotAddress:     RebasedAddress  { return RebasedAddress(bplist["aadr", as:Int64.self]!) }
        var aotSize:        UInt64          { return UInt64(bplist["asze", as:Int64.self]!) }
        var aotImageKey:    Data            { return bplist["ikey", as:Data.self]! }
        let bplist:         BPList.UnsafeRawDictionary
        public init(bplist: BPList.UnsafeRawDictionary) {
            self.bplist = bplist
        }
    }
}

internal struct Segment {
    private let impl:                   Impl
    public var name:                    String              { return impl.name }
    public var address:                 RebasedAddress      { return impl.address }
    public var preferredLoadAddress:    PreferredAddress    { return impl.preferredLoadAddress }
    public var vmSize:                  UInt64              { return impl.vmSize }
    public var permissions:             UInt64              { return impl.permissions }

    func withSegmentData<ResultType>(_ body: (Data) throws(AtlasError) -> ResultType) throws(AtlasError) -> ResultType {
        return try impl.withSegmentData(body)
    }
    
    internal init(wrapping impl: Impl) {
        self.impl       = impl
    }
}

 extension Segment {
    final class Impl {
        var name:                   String              { return bplist["name", as:String.self]! }
        var vmSize:                 UInt64              { return UInt64(bplist["size", as:Int64.self]!) }
        var fileSize:               UInt64              { return UInt64(bplist["fsze", as:Int64.self]!) }
        var preferredLoadAddress:   PreferredAddress    { return PreferredAddress(bplist["padr", as:Int64.self]!) }
        var address:                RebasedAddress      { return PreferredAddress(bplist["padr", as:Int64.self]!) + slide }
        var permissions:            UInt64              { return UInt64(bplist["perm", as:Int64.self]!) }

        let bplist:                 BPList.UnsafeRawDictionary
        let context:                Snapshot.DecoderContext
        let slide:                  Slide
        var mapper:                 Mapper?
        init(bplist: BPList.UnsafeRawDictionary, context:Snapshot.DecoderContext, slide: Slide, mapper: Mapper?) {
            self.bplist = bplist
            self.context = context
            self.slide  = slide
            self.mapper = mapper
        }
        func withSegmentData<ResultType>(_ body: (Data) throws(AtlasError) -> ResultType) throws(AtlasError) -> ResultType {
            guard vmSize > 0 else { return try body(Data()) }
            let endAddress = preferredLoadAddress + vmSize
            guard let data = try mapper?.mapping(unslidRange:preferredLoadAddress..<endAddress, context:context) else {
                throw AtlasError.placeHolder
            }
            return try body(data)
        }
    }
}

internal struct Environment {
    private let impl: Impl
    public var rootPath:    String?      { return impl.rootPath }
    internal init?(wrapping impl: Environment.Impl?) {
        guard let impl else { return nil }
        self.impl = impl
    }
}

extension Environment {
    internal final class Impl {
        var rootPath:   String?  { bplist["root", as:String.self] }
        let bplist:     BPList.UnsafeRawDictionary
        public init(bplist: BPList.UnsafeRawDictionary) {
            self.bplist = bplist
        }
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

internal struct SharedCache {
    private let impl:                   SharedCache.Impl
    public var address:                 RebasedAddress      { return impl.address}
    public var images:                  [Image]             { return impl.images.map { Image(wrapping:$0) } }
    public var preferredLoadAddress:    PreferredAddress    { return impl.preferredLoadAddress }
    public var uuid:                    UUID                { return impl.uuid }
    public var vmSize:                  UInt64              { return impl.vmSize }
    public var mappedPrivate:           Bool                { return impl.mappedPrivate }
    public var filePaths:               [String]            { return impl.filePaths}
    public var aotAddress:              RebasedAddress?     { return impl.aotAddress }
    public var aotUuid:                 UUID?               { return impl.aotUuid }
    public var localSymbolPath:         String?             { return impl.localSymbolsPath }
    public var localSymbolData:         Data?               { return impl.localSymbolData }
    public var subCaches:               [SubCache]          { return impl.subCaches.map { SubCache(wrapping:$0) } }

    // The contexts is now on the Impl instead of the facade, which is a more natural design and prevents a lot of extra releases
    // all of which were to a single hot object. One issue though is that leads to a retain loop between the context and the shared
    // cache, which is solved with by making SharedCache.Impl's DecoderContext weak. That means for the shared cache we still
    // need to store a reference up in the facade to anchor it in case it is the only remaining reference, but at lease we can avoid
    // it on most objects like Image and Segment.
    private let context:                Snapshot.DecoderContext

    func pinMappings() -> Bool {
        return impl.pinMappings()
    }
    func unpinMappings() {
        impl.unpinMappings()
    }
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
    public static func installedSharedCaches(systemPath:FilePath = "/") -> [SharedCache] {
        if #available(macOS 12.0, *) {
            var result = [SharedCache]()
            var enumeratedCaches = Set<UUID>()
            for pathString in sharedCachePaths {
                guard let children  = systemPath.appending(pathString).realPath?.children else {
                    continue
                }
                for child in children {
                    if child.extension == nil || child.extension == "development" {
                        // Development subcaches end in .development, filter those out here
                        if (child.lastComponent!.string.components(separatedBy:".").count > 2) {
                            continue;
                        }
                        guard let sharedCache = try? SharedCache(path:child) else {
                            continue
                        }
                        if !enumeratedCaches.contains(sharedCache.uuid) {
                            enumeratedCaches.insert(sharedCache.uuid)
                            result.append(sharedCache)
                        }
                    }
                }
            }
            return result
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }

    init(path: FilePath, forceScavenge: Bool = false) throws(AtlasError) {
        guard let bplist = Snapshot.findCacheBPlist(uuid:nil, path:path, forceScavenge:forceScavenge) else {
            throw AtlasError.placeHolder
        }
        // We are being created outside of a snapshot, create a context and store it to anchor the graph
        self.context = Snapshot.DecoderContext()
        self.context.sharedCachePath = path
        self.impl = try SharedCache.Impl(bplist:bplist, context: context)
    }

    internal init(wrapping impl: SharedCache.Impl) {
        self.context    = impl.context
        self.impl       = impl
    }
}

extension SharedCache {
    internal final class Impl {
        let bplist:                 BPList.UnsafeRawDictionary
        weak var context:           Snapshot.DecoderContext!
        var mappedPrivate:          Bool                                        { return context.privateSharedRegion }
        var aotAddress:             RebasedAddress?                             { return context.sharedCacheRecord?.aotAddress }
        var aotUuid:                UUID?                                       { return context.sharedCacheRecord?.aotUuid }
        var address:                RebasedAddress                              { return preferredLoadAddress + context.sharedCacheSlide }
        var preferredLoadAddress:   PreferredAddress                            { return PreferredAddress(bplist["padr", as:Int64.self]!) }
        var uuid:                   UUID                                        { return UUID(unsafeData:bplist["uuid", as:Data.self]!) }
        var pointerSize:            UInt64                                      { return UInt64(bplist["psze", as:Int64.self]!) }
        var vmSize:                 UInt64                                      { return UInt64(bplist["size", as:Int64.self]!) }
        var images: [Image.Impl] {
            return bplist["imgs", asArrayOf:BPList.UnsafeRawDictionary.self]!.map {
                return Image.Impl(bplist:$0, context:context)
            }
        }
        var subCaches: [SubCache.Impl] {
            return bplist["dscs", asArrayOf:BPList.UnsafeRawDictionary.self]!.map {
                return SubCache.Impl(bplist:$0, context:context)
            }
        }

        var filePaths: [String] {
            if #available(macOS 12.0, *) {
                guard let sharedCachePath = context.sharedCachePath?.removingLastComponent() else {
                    return subCaches.map { $0.name }
                }
                return subCaches.map {
                    return sharedCachePath.appending($0.name).string
                }
            } else {
                fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
            }
        }

        func localSymbolsFilePath(context: Snapshot.DecoderContext) -> FilePath? {
            if #available(macOS 12.0, *) {
                guard let localSymbolsFileName = bplist["snme", as:String.self] else {
                    return nil
                }
                guard let sharedCachePath = context.sharedCachePath else {
                    return FilePath(localSymbolsFileName)
                }
                return sharedCachePath.removingLastComponent().appending(localSymbolsFileName)
            } else {
                fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
            }
        }

        var localSymbolsPath: String? {
            if #available(macOS 12.0, *) {
                return localSymbolsFilePath(context: context)?.string
            } else {
                fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
            }
        }

        var localSymbolsUuid: UUID? { return UUID(unsafeData:bplist["suid", as:Data.self]) }

        var _cacheMapper: SharedCacheMapper?
        var cacheMapper: SharedCacheMapper {
            if _cacheMapper == nil {
                _cacheMapper = SharedCacheMapper(uuid:uuid, subcaches:subCaches, cachePath:context.sharedCachePath!);
            }
            return _cacheMapper!
        }

        init(bplist: BPList.UnsafeRawDictionary, context: Snapshot.DecoderContext) throws(AtlasError) {
            precondition(context.sharedCachePath != nil)
            self.bplist = bplist
            context.sharedCache = self
            self.context = context
        }

        func pinMappings() -> Bool {
            return cacheMapper.pin(context:context)
        }
        func unpinMappings() {
            cacheMapper.unpin()
        }
        var localSymbolData: Data? {
            if #available(macOS 12.0, *) {
                guard  localSymbolsUuid != nil || localSymbolsPath != nil else { return nil }
                return context.delegate.loadFileBlob(identifier:FileIdentifier(uuid:localSymbolsUuid, path:localSymbolsPath))
            } else {
                fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
            }
        }
    }
}

extension SharedCache {
    internal final class ProcessRecord {
        var uuid:       UUID            { return UUID(unsafeData:bplist["uuid", as:Data.self]!) }
        var address:    RebasedAddress  { return RebasedAddress(bplist["addr", as:Int64.self]!) }
        var bitmap:     Bitmap          { return Bitmap(data:bplist["bitm", as:Data.self]!) }
        var filePath:   FilePath        { return FilePath(bplist["file", as:String.self]!) }
        var aotUuid:    UUID?           { return UUID(unsafeData:bplist["auid", as:Data.self]) }
        var aotAddress: RebasedAddress? { return RebasedAddress(bplist["aadr", as:Int64.self]) }

        let bplist: BPList.UnsafeRawDictionary
        public init(bplist: BPList.UnsafeRawDictionary) {
            self.bplist = bplist
        }
    }
}

internal struct SubCache {
    let impl:       SubCache.Impl
    var name:       String  { return impl.name }
    var vmOffset:   UInt64  { return impl.virtualOffset }
    var uuid:       UUID    { return impl.uuid }
    internal init(wrapping impl: SubCache.Impl) {
        self.impl = impl
    }
    func withVMLayoutData(_ block:(@escaping (Data) -> Void)) -> Bool {
        return impl.withVMLayoutData(block)
    }
}

extension SubCache {
    struct Mapping {
        var preferredLoadAddress:   PreferredAddress    { return PreferredAddress(bplist["padr", as:Int64.self]!) }
        var size:                   UInt64              { return UInt64(bplist["size", as:Int64.self]!) }
        var fileOffset:             UInt64              { return UInt64(bplist["foff", as:Int64.self]!) }
        var maxProt:                UInt64              { return UInt64(bplist["prot", as:Int64.self]!) }
        internal var bplist: BPList.UnsafeRawDictionary
        public init(bplist: BPList.UnsafeRawDictionary) {
            self.bplist = bplist
        }
    }
}

extension SubCache {
    internal final class Impl {
        let bplist:                 BPList.UnsafeRawDictionary
        weak var context:           Snapshot.DecoderContext?
        var identifier:             FileIdentifier      { return FileIdentifier(uuid:uuid, path:name) }
        var name:                   String              { return bplist["name", as:String.self]! }
        var uuid:                   UUID                { return UUID(unsafeData:bplist["uuid", as:Data.self]!) }
        var vmSize:                 UInt64              { return UInt64(bplist["size", as:Int64.self]!) }
        var fileSize:               UInt64              { return UInt64(bplist["fsze", as:Int64.self]!) }
        var virtualOffset:          UInt64              { return UInt64(bplist["voff", as:Int64.self]!) }
        var preferredLoadAddress:   PreferredAddress    { return PreferredAddress(bplist["padr", as:Int64.self]!) }
        init(bplist: BPList.UnsafeRawDictionary, context: Snapshot.DecoderContext?) {
            self.bplist = bplist
            self.context = context
        }

        var mappings: [Mapping] {
            return bplist["maps", asArrayOf:BPList.UnsafeRawDictionary.self]!.map {
                return Mapping(bplist:$0)
            }
        }

        func VMLayout(into buffer: UnsafeRawBufferPointer, context localContext: Snapshot.DecoderContext? = nil) throws(AtlasError) {
            var mmapRanges = [MmapRange]()
            for mapping in mappings {
                mmapRanges.append(MmapRange(fileOffset:mapping.fileOffset, vmOffset:UInt64(mapping.preferredLoadAddress-preferredLoadAddress), size: mapping.size))
            }
            guard let cachePath = localContext?.sharedCachePath ?? self.context?.sharedCachePath else {
                throw AtlasError.placeHolder
            }
            guard let path = identifier.changing(path:cachePath).path,
                  (localContext ?? self.context)?.delegate.mmapFile(baseAddr:buffer.baseAddress!, path:path, size:UInt64(vmSize), mappings:mmapRanges) != nil else {
                throw AtlasError.placeHolder
            }
        }

        func withVMLayoutData(_ block:(@escaping (Data) -> Void)) -> Bool {
            var baseAddr: vm_address_t = 0
            let kr = vm_allocate(mach_task_self_, &baseAddr, vm_size_t(vmSize), VM_FLAGS_ANYWHERE)
            guard kr == KERN_SUCCESS else { return false }
            let buffer = UnsafeRawBufferPointer(start: UnsafeRawPointer(bitPattern:UInt(baseAddr)), count:Int(vmSize))
            do {
                try VMLayout(into:buffer)
            } catch {
                vm_deallocate(mach_task_self_, baseAddr, vm_size_t(vmSize))
                return false
            }
            block(Data(bytesNoCopy:UnsafeMutableRawPointer(mutating:buffer.baseAddress!), count:Int(vmSize), deallocator:.unmap))
            return true
        }
    }
}

//MARK: -
//MARK: Snapshot

internal struct Snapshot {
    public protocol Delegate {
        func getDataFor(uuid: UUID) -> Data?
        func getDataFor(file: FilePath) -> Data?

        func sharedCacheAtlas(uuid: UUID) -> Data?
        func sharedCacheAtlas(file: FilePath) -> Data?
    }
    internal struct DefaultDelegate: Delegate {
        func getDataFor(uuid: UUID) -> Data? { return nil }
        func getDataFor(file: FilePath) -> Data? { return nil }
        func sharedCacheAtlas(uuid: UUID) -> Data?  { return nil }
        func sharedCacheAtlas(file: FilePath) -> Data? { return nil }
    }
    internal class DecoderContext {
        let delegate:               Delegate
        var sharedCacheRecord:      SharedCache.ProcessRecord?
        var sharedCachePath:        FilePath?
        var privateSharedRegion:    Bool
        var sharedCacheSlide:       Slide
        var pointerSize:            UInt64
        var sharedCache:            SharedCache.Impl?
        init(delegate: Delegate = Snapshot.DefaultDelegate()) {
            self.delegate               = delegate
            self.privateSharedRegion    = false
            self.sharedCacheSlide       = Slide(0)
            self.pointerSize            = 8
        }
    }

    private let impl:       Snapshot.Impl
    let context:    Snapshot.DecoderContext
    var pageSize:           Int             { return Int(impl.pageSize) }
    var pid:                pid_t           { return impl.pid }
    var images:             [Image]         { return impl.images.map { return Image(wrapping:$0) } }
    var aotImages:          [AOTImage]?     { return impl.aotImages?.map { AOTImage(wrapping:$0) } }
    var timestamp:          UInt64          { return impl.timestamp }
    var initialImageCount:  UInt64          { return UInt64(impl.initialImageCount) }
    var state:              UInt8           { return impl.state }
    var platform:           UInt64          { return impl.platform }
    var environment:        Environment?    { return Environment(wrapping:impl.env) }

    public var sharedCache: SharedCache? {
        guard let sharedCache = impl.sharedCache else { return nil }
        return SharedCache(wrapping:sharedCache)
    }

    internal init(wrapping impl:Impl, context: Snapshot.DecoderContext) {
        self.impl       = impl
        self.context    = context
    }
    public init(data: Data, delegate:Delegate = DefaultDelegate()) throws(AtlasError) {
        self.context    = DecoderContext(delegate:delegate)
        self.impl       = try Snapshot.Impl(data:data, context: self.context)
    }

    // Internal implemenation class. We do this so that the `Decodable` conformance does not
    // leak into the ABI
    internal final class Impl {
        var archive:        AARDecoder
        let bplist:         BPList.UnsafeRawDictionary
        let context:        Snapshot.DecoderContext
        private var flags:  SnapshotFlags

        var pid:                pid_t           { return pid_t(bplist["proc", as:Int64.self]!) }
        var platform:           UInt64          { return UInt64(bplist["plat", as:Int64.self]!) }
        var timestamp:          UInt64          { return UInt64(bplist["time", as:Int64.self]!) }
        var state:              UInt8           { return UInt8(bplist["stat", as:Int64.self]!) }
        var initialImageCount:  Int64           { return bplist["init", as:Int64.self]! }
        var pageSize:           UInt64          { return flags.contains(.pageSize4k) ? 4096 : 16384 }
        var pointerSize:        UInt64          { return flags.contains(.pointerSize4Bytes) ? 4 : 8 }

        var env: Environment.Impl? {
            guard let env = bplist["envp", as:BPList.UnsafeRawDictionary.self] else { return nil }
            return Environment.Impl(bplist:env)
        }

        var aotImages: [AOTImage.Impl]? {
            return bplist["aots", asArrayOf:BPList.UnsafeRawDictionary.self]?.map {
                return AOTImage.Impl(bplist:$0)
            }
        }

        var images: [Image.Impl] {
            var images = bplist["imgs", asArrayOf:BPList.UnsafeRawDictionary.self]!.map {
                return Image.Impl(bplist:$0, context:context)
            }
            if let sharedCacheProcessRecord = context.sharedCacheRecord,
               let sharedCache = self.sharedCache {
                let cacheImages = zip(sharedCache.images, sharedCacheProcessRecord.bitmap.entries).filter { $0.1 }.map { $0.0 }
                images = Array.mergeArraySorted(images, cacheImages) {
                    $0.address < $1.address
                }
            }
            return images
        }

        var sharedCacheProcessRecord: SharedCache.ProcessRecord? {
            guard let dictionary = bplist["dsc1", as:BPList.UnsafeRawDictionary.self] else {
                return nil
            }
            return SharedCache.ProcessRecord(bplist:dictionary)
        }
        var sharedCache: SharedCache.Impl? {
            guard let sharedCacheRecord = context.sharedCacheRecord else { return nil }
            var bplist = Snapshot.findCacheBPlist(uuid:sharedCacheRecord.uuid, path:sharedCacheRecord.filePath)
            // If we did not find the cache atlas in the system atlases check to see if it was embedded in the process atlas
            if  bplist == nil {
                bplist = findEmbeddedSharedCachePlist(uuid:sharedCacheRecord.uuid)
            }
            guard let bplist else { return nil }
            return try? SharedCache.Impl(bplist:bplist, context:context)
        }

        private func findEmbeddedSharedCachePlist(uuid: UUID) -> BPList.UnsafeRawDictionary? {
            guard let cachePlistData   = try? archive.data(path: uuid.cacheAtlasArchivePath),
                  let cachePlist        = try? BPList(data:cachePlistData),
                  let byUuidDict        = try? cachePlist.asDictionary()["uuids"]?.asDictionary(),
                  let bplist = try? byUuidDict[uuid.uuidString.uppercased()]?.asDictionary() else {
                return nil
            }
            do {
                try SharedCache.Impl.validate(bplist:bplist)
            } catch {
                return nil
            }
            return bplist
        }

        public init(data: Data, context: Snapshot.DecoderContext) throws(AtlasError) {
            self.context = context
            do throws(AARDecoderError) {
                self.archive  = try AARDecoder(data: data)
            } catch {
                throw .aarDecoderError(error)
            }
            guard let bplistData = try? archive.data(path: "process.plist") else {
                throw .missingPlist
            }
            do throws(BPListError) {
                self.bplist = try BPList(data:bplistData).asDictionary()
            } catch {
                throw .bplistError(error)
            }
            try Snapshot.Impl.validate(bplist:bplist)
            // Setup all the raw values we need for parsing
            self.flags  = SnapshotFlags(rawValue:bplist["flags", as:Int64.self] ?? 0)
            if let sharedCacheDict = bplist["dsc1", as:BPList.UnsafeRawDictionary.self] {
                let sharedCacheInfo = SharedCache.ProcessRecord(bplist:sharedCacheDict)
                context.sharedCacheRecord = sharedCacheInfo
                context.sharedCachePath = sharedCacheInfo.filePath
                var bplist = Snapshot.findCacheBPlist(uuid:sharedCacheInfo.uuid, path:sharedCacheInfo.filePath)
                // If we did not find the cache atlas in the system atlases check to see if it was embedded in the process atlas
                if  bplist == nil {
                    bplist = findEmbeddedSharedCachePlist(uuid:sharedCacheInfo.uuid)
                }
                if let bplist {
                    context.sharedCachePath = sharedCacheInfo.filePath
                    let cache = try SharedCache.Impl(bplist:bplist, context:context)
                    context.sharedCacheSlide = sharedCacheInfo.address - cache.preferredLoadAddress
                    context.sharedCache = cache
                }
            }
            context.privateSharedRegion = flags.contains(.privateSharedRegion);
        }
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

internal struct Process {
    private let impl: Process.Impl

    init(task: task_read_t, queue: DispatchQueue? = nil) throws {
        self.impl = try Impl(task:task, queue:queue)
    }

    func registerForChangeNotifications( block: @escaping (_ image: Image, _ load: Bool) -> Void) throws -> UInt32 {
        return try impl.registerForChangeNotifications(block)
    }

    func register(event:UInt32, _ handler: @escaping () -> Void) throws -> UInt32 {
        return try impl.register(event: event, handler)
    }

    func unregister(event: UInt32) {
        impl.unregister(event:event)
    }
    func getCurrentSnapshot() -> Snapshot {
        return impl.currentSnapshot
    }

    static func forCurrentTask() -> Process {
        //FIXME: Use mach_task_self() (how do we get it into Swift? What module is it in ?)
        return try! Process(task:mach_task_self_)
    }

    var queue: DispatchQueue? {
        get {
            return impl.queue
        }
        set {
            impl.queue = newValue
        }
    }

    internal class Impl {
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

        private(set) public var currentSnapshot: Snapshot
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
                            updateClient.run(element, false)
                        case .insert(_, let element, _):
                            updateClient.run(element, true)
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
                    block(image, true)
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
                if updateClients.isEmpty && notifierClients.isEmpty {
                    teardownNotifications(&notifierState)
                }
            }
        }
        static func getNewSnapshot(_ task: borrowing MachTask) throws(AtlasError) -> Snapshot {
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
                var atlasAddress:   RebasedAddress
                let taskDyldInfo    = try task.dyldInfo()
                var atlasSize       = UInt64(0)
                var timestamp       = UInt64(0)
                if taskDyldInfo.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_64 {
                    let allImageInfos: dyld_all_image_infos_64 = try task.readStruct(address:RebasedAddress(taskDyldInfo.all_image_info_addr))
                    // On the right side we need to access these values atomically but here we don't need to worry since the mapping
                    // is not shared and the vm_copy acted as a memory barrier
                    atlasAddress    = RebasedAddress(allImageInfos.compact_dyld_image_info_addr)
                    atlasSize       = allImageInfos.compact_dyld_image_info_size
                    timestamp       = allImageInfos.infoArrayChangeTimestamp
                } else if taskDyldInfo.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_32 {
                    let allImageInfos: dyld_all_image_infos_32 = try task.readStruct(address:RebasedAddress(taskDyldInfo.all_image_info_addr))
                    atlasAddress    = RebasedAddress(allImageInfos.compact_dyld_image_info_addr)
                    atlasSize       = UInt64(allImageInfos.compact_dyld_image_info_size)
                    timestamp       = allImageInfos.infoArrayChangeTimestamp
                } else {
                    throw AtlasError.placeHolder
                }
                if atlasSize == 0 || timestamp == 0 {
                    // Process is not started, try to scavenge
                    var data: Data
                    var bufferSize: UInt64 = 0;
                    var buffer: UnsafeMutableRawPointer? = nil
                    if scavengeProcess(task.port, &buffer, &bufferSize) {
                        data = Data(bytes:buffer!, count:Int(bufferSize))
                        free(buffer)
                        return try Snapshot(data:data)
                    }
                    throw AtlasError.placeHolder
                }
                do throws(AtlasError) {
                    let data = try task.readData(address:atlasAddress, size:atlasSize)
                    return try Snapshot(data:data)
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

// Mapper is a protocol for gettinng the bytes backing a segment of memory. There are concrete implementations that can work with MachO files (translating
// from on disk to in memory layout), subcaches, etc. All mappings are done with unslid addresses

internal protocol Mapper {
    // We pass in mapper becasue if it was an ivar it would cause a retain cycle with the sahred cache mapper in the context
    func mapping(unslidRange: Range<PreferredAddress>, context: Snapshot.DecoderContext) throws(AtlasError) -> Data
}

internal struct MachOMapper: Mapper {
    private enum Region {
        case file(size:UInt64, fileOffset:UInt64)
        case zerofill(size:UInt64)
    }
    let identifier: FileIdentifier
    var segments: BPList.UnsafeArray<BPList.UnsafeRawDictionary>

    init(image: Image.Impl, segments:BPList.UnsafeArray<BPList.UnsafeRawDictionary>) {
        self.identifier = image.identifier
        self.segments = segments
    }

    func mapping(unslidRange: Range<PreferredAddress>, context: Snapshot.DecoderContext) throws(AtlasError) -> Data {
        var currentfileOffset: UInt64 = 0
        
        for segment in segments {
            let fileOffset = currentfileOffset
            let vmSize  = UInt64(segment["size", as:Int64.self]!)
            let fileSize = UInt64(segment["fsze", as:Int64.self]!)
            let address = PreferredAddress(segment["padr", as:Int64.self]!)
            currentfileOffset += UInt64(fileSize)

            let segmentRange = Range(uncheckedBounds: (lower:address, upper: address+vmSize))
            guard segmentRange.contains(unslidRange) else { continue }

            guard let result = context.delegate.loadFileBlob(identifier:identifier, offset:fileOffset, size:UInt64(fileSize), zerofill:vmSize-fileSize) else {
                throw AtlasError.placeHolder
            }
            return result
        }
        throw AtlasError.placeHolder
    }
}

class SharedCacheMapper: Mapper {
    let subcaches:[SubCache.Impl]
    let cachePath: FilePath
    var pinnedMapping: Data?
    var localCacheBaseAddress: UnsafeMutableRawPointer? = nil
    var cachedMapping: (range: Range<PreferredAddress>, data: Data)?
    init(uuid: UUID, subcaches: [SubCache.Impl], cachePath: FilePath) {
        self.subcaches = subcaches
        self.cachePath = cachePath
        var rawCacheUuid: uuid_t = UUID_NULL
        let foundUuid = withUnsafeMutablePointer(to: &rawCacheUuid) {
            return _dyld_get_shared_cache_uuid($0)
        }
        if foundUuid && UUID(uuid:rawCacheUuid) == uuid {
            var size = Int(0)
            self.localCacheBaseAddress = UnsafeMutableRawPointer(mutating:_dyld_get_shared_cache_range(&size))
        }
    }
    func mapping(unslidRange: Range<PreferredAddress>, context: Snapshot.DecoderContext) throws(AtlasError) -> Data {
        if let pinnedMapping {
            let start = Data.Index(unslidRange.lowerBound.value - subcaches[0].preferredLoadAddress.value)
            let data = pinnedMapping[Range(uncheckedBounds: (lower: start, upper:start + unslidRange.count))]
            return data
        }
        // Check if we have cached the mapping
        if let cachedMapping, cachedMapping.range == unslidRange {
            return cachedMapping.data
        }
        for subcache in subcaches {
            let subcacheRange = Range(uncheckedBounds:(subcache.preferredLoadAddress, subcache.preferredLoadAddress + subcache.vmSize))
            guard subcacheRange.contains(unslidRange) else { continue }
            for mapping in subcache.mappings {
                let mappingRange = Range(uncheckedBounds:(mapping.preferredLoadAddress, mapping.preferredLoadAddress + mapping.size))
                guard mappingRange.contains(unslidRange) else {
                    continue
                }
                let fileOffset = mapping.fileOffset + UInt64(unslidRange.lowerBound - mappingRange.lowerBound)
                let identifier = subcache.identifier.changing(path:context.sharedCachePath)
                if mapping.maxProt == (PROT_READ | PROT_EXEC), let localCacheBaseAddress {
                    return Data(bytesNoCopy:localCacheBaseAddress+Int(subcache.virtualOffset+fileOffset), count:Int(unslidRange.count), deallocator:.none)
                }
                guard let result = context.delegate.loadFileBlob(identifier:identifier, offset:fileOffset, size:UInt64(unslidRange.count), zerofill:0) else {
                    throw AtlasError.placeHolder
                }
                cachedMapping = (unslidRange, result)
                return result
            }
            throw AtlasError.placeHolder
        }
        throw AtlasError.placeHolder
    }
    func pin(context: Snapshot.DecoderContext) -> Bool {
        var baseAddr: vm_address_t = 0
        let lastSubCache = subcaches.last
        let vmSize = lastSubCache!.vmSize + lastSubCache!.virtualOffset

        let kr = vm_allocate(mach_task_self_, &baseAddr, vm_size_t(vmSize), VM_FLAGS_ANYWHERE)
        guard kr == KERN_SUCCESS else { return false }
        let buffer = UnsafeRawBufferPointer(start: UnsafeRawPointer(bitPattern:UInt(baseAddr)), count:Int(vmSize))
        for subcache in subcaches {
            do {
                let subCacheBuffer = UnsafeRawBufferPointer(start:buffer.baseAddress!+Int(subcache.virtualOffset), count:Int(subcache.vmSize))
                try subcache.VMLayout(into:subCacheBuffer, context:context)
            } catch {
                vm_deallocate(mach_task_self_, baseAddr, vm_size_t(vmSize))
                return false
            }
        }
        pinnedMapping = Data(bytesNoCopy:UnsafeMutableRawPointer(mutating:buffer.baseAddress!), count:Int(vmSize), deallocator:.virtualMemory)
        return true
    }
    func unpin() {
        pinnedMapping = nil
    }
}

//MARK: -
//MARK: -
//MARK: Misc

internal enum FileIdentifier: Hashable, Equatable {
    case uuid(UUID)
    case path(String)
    case both(UUID,String)
    init(uuid:UUID?, path:String?) {
//        precondition(uuid != nil || uuid != nil, "Both FileIdentifier components are optional. but at least one of them must be present")
        switch (uuid, path) {
        case (let uuid?, let path?):
            self = .both(uuid, path)
            break
        case (let uuid?, _):
            self = .uuid(uuid)
            break
        case (_, let path?):
            self = .path(path)
            break
        default:
            fatalError("Both FileIdentifier components are optional. but at least one of them must be present")
        }
    }
    func hash(into hasher: inout Hasher) {
        switch self {
        case .uuid(let uuid):
            hasher.combine(uuid)
        case .path(let path):
            hasher.combine(path)
        case .both(let uuid, let path):
            hasher.combine(uuid)
            hasher.combine(path)
        }
    }
    func changing(path: FilePath?) -> FileIdentifier {
        if #available(macOS 12.0, *) {
            guard let path else {
                return self
            }
            switch self {
            case .path(let currentPath):
                return .path(path.removingLastComponent().appending(FilePath(currentPath).lastComponent!).string)
            case .uuid(_):
                return self
            case .both(let uuid, let currentPath):
                return .both(uuid, path.removingLastComponent().appending(FilePath(currentPath).lastComponent!).string)
            }
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }
    var path: String? {
        switch self {
        case .path(let currentPath):
            return currentPath
        case .uuid(_):
            return nil
        case .both(_, let currentPath):
            return currentPath
        }
    }
}

//MARK: -
//MARK: Default logic for delegate related lookups, etc

internal struct MmapRange {
    let fileOffset: UInt64
    let vmOffset:   UInt64
    let size:       UInt64

    internal init(fileOffset: UInt64, vmOffset: UInt64, size: UInt64) {
        self.fileOffset = fileOffset
        self.vmOffset = vmOffset
        self.size = size
    }
}

@available(macOS 13.0, *)
struct MappedFileCache {
    class WeakData {
        // We need a weak ref to the data. Data is a struct, even though it share its internals with NSData so we use an NSData here in order to
        // get access to as a weak ref
        private weak var _data: NSData?
        var data: NSData? {
            guard let result = _data else {
                return nil
            }
            return result
        }
        init(_ data: NSData) {
            self._data = data
        }
    }
    struct Entry: Hashable {
        let path:       String
        let offset:     UInt64
        let size:       UInt64
        let zerofill:   UInt64
    }
    let cache = OSAllocatedUnfairLock(initialState: [Entry : WeakData]())
    func getEntry(path: String, offset: UInt64, size: UInt64, zerofill: UInt64) -> Data? {
        let entry = Entry(path:path, offset:offset, size:size, zerofill:zerofill)
        return cache.withLock { (cache: inout [Entry : WeakData]) -> Data? in
            guard let value = cache[entry],
                  let result = value.data else {
                return nil
            }
            return result as Data
        }
    }
    mutating func insertEntry(path: String, offset: UInt64, size: UInt64, zerofill: UInt64, data:NSData) -> Data {
        let entry = Entry(path:path, offset:offset, size:size, zerofill:zerofill)
        let weakData = WeakData(data)
        return cache.withLock { (cache: inout [Entry : WeakData]) -> Data in
            if let value = cache[entry],
               let result = value.data {
                // If it is not nil some other thread mapped and inserted while we were doing the same. Return the already in use one so this entry can
                // be thrown out
                return result as Data
            }
            cache[entry] = weakData
            // weakData.data willl be valid because data is still held by the data argument, but if we pass data we get sendability warnings
            return weakData.data! as Data
        }
    }
}

@available(macOS 13.0, *)
var mappingCache = MappedFileCache()

// FIXME: Need to rethink this before SPI/API
internal extension Snapshot.Delegate {
    func mmapFile(path: String, offset: UInt64, size: UInt64? = nil, zerofill: UInt64) -> Data? {
        if #available(macOS 13.0, *) {
            let fd = open(path, O_RDONLY)
            guard fd >= 0 else { return nil }
            defer { close(fd) }
            var actualSize: UInt64
            if size != nil {
                actualSize = size!
            } else {
                var statBuf = stat()
                guard fstat(fd, &statBuf) == 0 else {
                    return nil
                }
                actualSize = UInt64(statBuf.st_size)
            }
            if let cachedResult = mappingCache.getEntry(path:path, offset:offset, size:actualSize, zerofill:zerofill) {
                return cachedResult
            }
            guard let buffer = mmap(nil, Int(actualSize+zerofill), PROT_READ, MAP_PRIVATE, fd, off_t(offset)),
                  buffer != UnsafeMutableRawPointer(bitPattern:Int(bitPattern:MAP_FAILED)) else {
                print("Errno \(errno): \(String(describing: strerror(errno)))")
                return nil
            }
            let data = NSData(bytesNoCopy:buffer, length:Int(actualSize+zerofill)) {
                munmap($0, $1)
            }
            return mappingCache.insertEntry(path:path, offset:offset, size:actualSize, zerofill:zerofill, data:data)
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }

    func mmapFile(baseAddr:UnsafeRawPointer, path: String, size:UInt64, mappings:[MmapRange]) -> Bool {
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        for mapping in mappings {
            let mappingAddress = baseAddr+Int(mapping.vmOffset)
            guard let buffer = mmap(UnsafeMutableRawPointer(mutating:mappingAddress), Int(mapping.size), PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, off_t(mapping.fileOffset)),
                  buffer != UnsafeMutableRawPointer(bitPattern:Int(bitPattern:MAP_FAILED)) else {
                print("Errno \(errno): \(String(describing: strerror(errno)))")
                return false
            }
        }
        return true
    }

    func loadFileBlob(identifier:FileIdentifier, offset:UInt64 = 0, size: UInt64? = nil, zerofill:UInt64 = 0) -> Data? {
        switch identifier {
        case .uuid(_):
            return nil
        case .path(let path):
            return mmapFile(path:path, offset:offset, size:size, zerofill:zerofill)
        case .both(let uuid, let path):
            guard let file = mmapFile(path:path, offset:0, zerofill:zerofill) else {
                return nil
            }
            let sliceOffset = file.sliceOffset(uuid:uuid)
            if let size, zerofill == 0 {
                let dataStart = Data.Index(offset + sliceOffset)
                let dataEnd = dataStart + Data.Index(size)
                return file[dataStart..<dataEnd]
            } else {
                return mmapFile(path:path, offset:offset+sliceOffset, size:size, zerofill:zerofill)
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

    var cacheAtlasArchivePath: String {
        return "caches/uuids/\(uuidString.uppercased()).plist"
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
        return withUnsafePointer(to: self.d_name) {
            return String(cString: $0.qpointer(to: \.0)!)
        }
    }
}

internal extension FilePath {
    // This is sort of an abuse of file path in that it actually does an FS opetation, but it logically makes sense here
    var realPath: FilePath? {
        if #available(macOS 12.0, *) {
            let buffer = ManagedBuffer<CChar, CChar>.create(minimumCapacity: Int(PATH_MAX)) {_ in
                return 0
            }
            return buffer.withUnsafeMutablePointerToElements {
                guard Darwin.realpath(self.string, $0) != nil else {
                    return nil
                }
                return FilePath(String(cString:$0))
            }
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }
    // This is even more of of abuse, but since the Swift stdlib does not include any nice posix file wrappers and FileManager
    // is not likely to work in any code we share with dyld we need this functionality. Until we write nice Posix FS wrappers just
    // leave it here
    var children: [FilePath]? {
        if #available(macOS 12.0, *) {
            var result = [FilePath]()
            guard let dirp = Darwin.opendir(self.string) else {
                return nil
            }
            defer {
                Darwin.closedir(dirp)
            }
            while let dirEntry =  Darwin.readdir(dirp) {
                // We cannot use pointee because that can copy, and Swift does not provide any ergonomic way to get to the underlying data
                // since people should not generally do that, but here we are, so we get the raw pointer and directly load the fields
                // we need to.

                // We assume dirent is greater than a UInt16 (and we can therefore use aligned loads) and that _DARWIN_FEATURE_64_BIT_INODE is
                // set (so we know the types of fields). Both of these are true on all current platforms.
                assert(MemoryLayout<dirent>.alignment > MemoryLayout<UInt16>.alignment)
                assert(_DARWIN_FEATURE_64_BIT_INODE != 0)

                let rawPointer = UnsafeRawPointer(dirEntry)
                let dirType = rawPointer.load(fromByteOffset:MemoryLayout<dirent>.offset(of: \.d_type)!, as:UInt8.self)

                guard dirType == DT_REG else {
                    continue
                }

                let componentSize = rawPointer.load(fromByteOffset:MemoryLayout<dirent>.offset(of: \.d_namlen)!, as:UInt16.self)+1
                let componentStartIndex = MemoryLayout<dirent>.offset(of: \.d_name)!
                let componentBuffer = UnsafeRawBufferPointer(start:rawPointer+componentStartIndex, count:Int(componentSize))

                guard let componentString = componentBuffer.bindMemory(to: UInt8.self).baseAddress.flatMap(String.init(cString:))  else {
                    continue
                }
                guard let component = FilePath.Component(componentString) else {
                    continue
                }
                result.append(self.appending(component))
            }
            return result
        } else {
            fatalError("This is not supported for backdeployment, but needs to build with a reduced minOS in some environments")
        }
    }
}

//MARK: -
//MARK: Bitmap

internal struct Bitmap {
    private let data: Data
    init(data: Data) {
        self.data = data
    }
    var entries: BitmapSequence {
        return BitmapSequence(self)
    }
    struct Iterator: IteratorProtocol {
        var index: Int
        let bitmap: Bitmap
        init(_ bitmap: Bitmap) {
            self.bitmap = bitmap
            self.index = 0
        }
        public mutating func next() -> Bool? {
            let byteIndex = index>>3
            if byteIndex >= bitmap.data.count {
                return nil
            }
            let currentByte = bitmap.data[byteIndex]
            let mask: UInt8 = 1<<(index & ((1<<3)-1))
            let result: Bool = (currentByte & mask) == mask
            index += 1
            return result
        }
    }

    struct BitmapSequence: Sequence {
        let bitmap: Bitmap
        init(_ bitmap: Bitmap) {
            self.bitmap = bitmap
        }
        public func makeIterator() -> Iterator {
            return Iterator(self.bitmap)
        }
    }
}

internal extension Data {
    func sliceOffset(uuid:UUID) -> UInt64 {
        withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
            guard let macho = try?  MachO(buffer) else {
                return 0
            }
            switch macho {
            case .notMachO:
                //FIXME: Wire verbose failures
                return 0
            case .thinMachO:
                return 0
            case .universalMachO(let universalFile):
                for (slice, sliceInfo) in zip(universalFile.slices,universalFile.sliceInfos) {
                    if slice.uuid == uuid {
                        return UInt64(sliceInfo.offset)
                    }
                }
                //TODO: Wire up explicit errors
                return 0
            }
        }
    }
}
