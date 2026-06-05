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

/*
 * MemoryMapper System
 * ------------------
 * The MemoryMapper system provides access to memory views of other processes by maintaining
 * a list of mappings that describe an address space. This system is designed to support
 * Dyld.framework's need to inspect memory content from process snapshots or shared caches.
 *
 * Key components:
 * - Mapper: Protocol defining how to get bytes backing a segment of memory
 * - MemoryMap: Maintains a list of mappings from source (file/memory) to destination addresses
 * - Concrete mappers (MachOMapper, SharedCacheMapper): Create memory maps for specific sources
 *
 * Design considerations:
 * 1. Lazy/incremental loading: Atlas data can be expensive to decode, so mappings are resolved
 *    only when needed using the unresolved mapper pattern
 * 2. Memory efficiency: On memory-constrained devices, only required mappings are loaded
 * 3. Performance: For clients that need to access large portions of the map (like debuggers),
 *    support for ahead-of-time mapping ("pinning") is provided
 *
 * This is an internal interface used to back SPIs like `dyld_image_content_for_segment`.
 */

import os
import System
import DarwinPrivate.Mach // mach_vm_remap on embedded platforms
import MachO_Private
@_implementationOnly import Dyld_Internal

enum MemoryMapError: Error {
    // This really should not be an error, but since RawSpan sets the base address of any zero lenth segment to nil
    // we need to do this to differentiate could not find vs nothing to map and it ~Escapable optionals don't work
    case zeroLengthSegment(UnsafeRawPointer)
    case inaccessibleFile(String)
    case outOfRange
}

// Mapper is a protocol for gettinng the bytes backing a segment of memory. There are concrete implementations that can work with MachO files (translating
// from on disk to in memory layout), subcaches, etc. All mappings are done with unslid addresses

internal protocol Mapper {
    var memoryMap:  MemoryMap   { get }
    var address:    UInt64      { get }
}

internal extension UnsafeRawBufferPointer {
    func sliceOffset(uuid:UUID) -> UInt64 {
        withUnsafeBytes { (buffer: UnsafeRawBufferPointer) in
            guard let macho = try?  MachO(buffer) else {
                return 0
            }
            switch macho {
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

internal final class MachOMapper: Mapper {
    let address:            UInt64
    let unsafeImageBPList:  BPList.UnsafeObject
    init(unsafeImageBPList: BPList.UnsafeObject, address: UInt64) {
        self.address = address
        self.unsafeImageBPList = unsafeImageBPList
    }

    static func mappersForProcessMachOs(unsafeProcessInfoBPList: BPList.UnsafeObject) -> [any Mapper] {
        var result: [any Mapper] = []
        try! unsafeProcessInfoBPList.object.asDictionary().forEach { key, value in
            switch key.stringValue  {
            case "imgs":
                try! value.asArray().forEach { image in
                    let imageBPList = image.asUnsafeObject()
                    var address: UInt64!
                    try! image.asDictionary().forEach { key, value in
                        switch key {
                        case "addr": address   = UInt64(try! value.asInt64())
                        default: return
                        }
                    }
                    result.append(MachOMapper(unsafeImageBPList:imageBPList, address:address))
                }
            default: return
            }
        }
        return result
    }

    var memoryMap: MemoryMap {
        var mappingRanges:      [MemoryMap.Mapping]  = []
        var currentfileOffset:  UInt64  = 0
        var filePathString:     String!
        var uuid:               UUID?
        var preferredAddress:   UInt64  = 0
        var unsafeSegmentArray: BPList.UnsafeObject!
        try! unsafeImageBPList.object.asDictionary().forEach { key, value in
            switch key {
            case "file": filePathString     = try! value.asFastString().stringValue
            case "uuid": uuid               = UUID(bytes:try! value.asData())
            case "padr": preferredAddress   = UInt64(try! value.asInt64())
            case "segs": unsafeSegmentArray = value.asUnsafeObject()
            default: return
            }
        }
        // It is possible (but rare) for the preferred load address to be above the rebased address, resulting in a negative slide
        let slide = address &- preferredAddress

        try! unsafeSegmentArray.object.asArray().forEach { segment in
            var vmSize:         UInt64!
            var fileSize:       UInt64!
            var segmentAddress: UInt64!
            try! segment.asDictionary().forEach { key, value in
                switch key {
                case "size": vmSize         = UInt64(try! value.asInt64())
                case "fsze": fileSize       = UInt64(try! value.asInt64())
                case "padr": segmentAddress = UInt64(try! value.asInt64())
                default: return
                }
            }
            mappingRanges.append(MemoryMap.Mapping(source:.file(0),
                                                   sourceStart:currentfileOffset,
                                                   destStart:segmentAddress &+ slide,
                                                   size:vmSize))
            currentfileOffset += fileSize
        }
        // We need to crack up the file to figure out the slice off set. That would be inefficient as we need to openit again in a second anyway to materialize
        // the segment (this function will only be called lazily in response to such an action). By using the cache we can avoid the extar syscalls without
        // doing tons of fancy things. So long as it is not a zero filled segment (and most of the time it won't be because most user access __TEXT) we will
        // avoid the overhead of double mapping the file
        
        var offset: UInt64 = 0
        if let uuid, let fileBuffer = mappedFileCache.fileBuffer(path:filePathString, offset:0, zerofill:0)  {
            fileBuffer.bytes.withUnsafeBytes {
                offset = $0.sliceOffset(uuid:uuid)
            }
        }
        mappingRanges = mappingRanges.map {
            MemoryMap.Mapping(source:$0.source, sourceStart:$0.sourceStart+offset, destStart:$0.destStart, size:$0.size)
        }
        return MemoryMap(files:[filePathString], mappings:mappingRanges)
    }
}

func localCacheUUID() -> UUID? {
    var rawCacheUuid: uuid_t = UUID_NULL
    let foundUuid = withUnsafeMutablePointer(to: &rawCacheUuid) {
        return _dyld_get_shared_cache_uuid($0)
    }
    if !foundUuid { return nil }
    return UUID(uuid:rawCacheUuid)
}

struct SharedCacheMapper: Mapper {
    let unsafeImageBPList:  BPList.UnsafeObject
    let address:            UInt64
    let prefixPath:         String
    init(unsafeImageBPList: BPList.UnsafeObject, address: UInt64?, prefixPath: String) {
        self.unsafeImageBPList = unsafeImageBPList
        self.prefixPath = prefixPath
        // If address is nil, parse the plist to get the preferred load address.
        // This is used when creating a SharedCache from a path without a process context.
        if let address {
            self.address = address
        } else {
            var preferredLoadAddress: UInt64 = 0
            try! unsafeImageBPList.object.asDictionary().forEach { key, value in
                switch key {
                case "padr": preferredLoadAddress = UInt64(try! value.asInt64())
                default: return
                }
            }
            self.address = preferredLoadAddress
        }
    }
    var memoryMap: MemoryMap {
        var files:              [String]            = []
        var mappingRanges:      [MemoryMap.Mapping] = []
        var subCaches:          BPList.UnsafeObject!
        var uuid:               UUID!
        var preferredLoadAddress: UInt64!
        try! unsafeImageBPList.object.asDictionary().forEach { key, value in
            switch key {
            case "uuid": uuid = UUID(bytes:try! value.asData())
            case "dscs": subCaches = value.asUnsafeObject()
            case "padr": preferredLoadAddress = UInt64(try! value.asInt64())
            default: return
            }
        }
        // It is possible (but rare) for the preferred load address to be above the rebased address, resulting in a negative slide
        let slide = address &- preferredLoadAddress
        let localCacheUUID = localCacheUUID()
        var localCacheSize = size_t(0)
        let localCacheAddress = UInt64(UInt(bitPattern:_dyld_get_shared_cache_range(&localCacheSize)))
        try! subCaches.object.asArray().forEach { subcache in
            var currentFile:                String!
            var vmOffset:                   UInt64!
            var subCachesMappings:          BPList.UnsafeObject!
            try! subcache.asDictionary().forEach { key, value in
                switch key {
                case "name": currentFile                = try! value.asFastString().stringValue
                case "voff": vmOffset              = UInt64(try! value.asInt64())
                case "maps": subCachesMappings          = value.asUnsafeObject()
                default: return
                }
            }
            try! subCachesMappings.object.asArray().forEach { mapping in
                var vmSize:             UInt64!
                var preferredAddress:   UInt64!
                var fileOffset:         UInt64!
                var prot:               UInt64!
                try! mapping.asDictionary().forEach { key, value in
                    switch key {
                    case "padr":    preferredAddress    = UInt64(try! value.asInt64())
                    case "size":    vmSize              = UInt64(try! value.asInt64())
                    case "foff":    fileOffset          = UInt64(try! value.asInt64())
                    case "prot":    prot                = UInt64(try! value.asInt64())
                    default: return
                    }
                }
                if uuid == localCacheUUID, prot == (PROT_READ | PROT_EXEC) {
                    mappingRanges.append(MemoryMap.Mapping(source:.localMemory,
                                                           sourceStart:localCacheAddress + vmOffset + fileOffset,
                                                           destStart:preferredAddress &+ slide,
                                                           size:vmSize))
                } else {
                    mappingRanges.append(MemoryMap.Mapping(source:.file(files.count),
                                                           sourceStart:fileOffset,
                                                           destStart:preferredAddress &+ slide,
                                                           size:vmSize))
                }
            }
            files.append("\(prefixPath)/\(currentFile!)")
        }
        return MemoryMap(files:files, mappings:mappingRanges)
    }
}

    // Mapping can be used two ways:
    // 1. When a file is directly mmapped without using zero fill they can be used as an adapter to adjust rhe addresses. This is sufficient for
    //    most users that read a segment at a time
    // 2. They can be used as inputs to a complex open call that remaps files into the layout specified by the mapper. This used for pinned
    //    cache mappings

extension MemoryMap {
    func dump() {
        for (i, mapping) in self.mappings.enumerated() {
            print("\(i): \(mapping.dump(files:self.files))")
        }
    }
}

extension MemoryMap.Mapping {
    func dump(files: [String]) -> String {
        return "0x\(String(destStart, radix: 16, uppercase: true)) - 0x\(String(destEnd, radix: 16, uppercase: true)) \(source.dump(files,sourceStart,sourceEnd))"
    }
}

extension MemoryMap.Mapping.Source {
    func dump(_ files: [String], _ sourceStart: UInt64, _ sourceEnd: UInt64) -> String {
        switch self {
        case .file(let index):          return "file \(files[index])"
        case .localMemory:              return "local memory 0x\(String(sourceStart, radix: 16, uppercase: true)) - 0x\(String(sourceEnd, radix: 16, uppercase: true))"
        case .unresolved(let mapper):   return "unresolved"
        }
    }
}

final class MemoryMap: Sendable {
    struct Mapping {
        enum Source {
            case file(Int) // File Index, zero fill extension
            case localMemory
            case unresolved(Mapper)
        }
        let source: Source
        let sourceStart:    UInt64
        let destStart:      UInt64
        let size:           UInt64
        var destEnd:        UInt64          { return destStart + size }
        var sourceEnd:      UInt64          { return sourceStart + size }
        var destRange:      Range<UInt64>   { return destStart..<destEnd }
        var sourceRange:    Range<UInt64>   { return sourceStart..<sourceEnd }
        var zerofill:       UInt64          { return UInt64(destRange.count - sourceRange.count) }
    }
    var files:      [String]
    var mappings:     [Mapping]
    init(mappers: [any Mapper]) {
        self.files = []
        mappings = mappers.map { Mapping(source:.unresolved($0), sourceStart:0, destStart:$0.address, size:UInt64.max-$0.address) }.sorted {
            $0.destStart < $1.destStart
        }
        mappings = zip(mappings,0..<mappings.count).map { (mapping: Mapping, index: Int) in
            var nextAddress = mapping.destEnd
            if index+1 < mappings.count {
                nextAddress = mappings[index+1].destStart
            }
            return Mapping(source:mapping.source, sourceStart:0, destStart:mapping.destStart, size:nextAddress-mapping.destStart)
        }
        assert(mappingsOrder)
    }
    init(files:[String], mappings: [Mapping]) {
        self.files = files
        self.mappings = mappings
    }

    /// Creates a fully resolved copy of this MemoryMap.
    /// This allows callers to get their own copy that won't mutate the original.
    /// The copy is fully resolved to prevent any lazy mutations during use.
    func copy() -> MemoryMap {
        let result = MemoryMap(files: files, mappings: mappings)
        result.fullyResolve()
        return result
    }
    lazy var sourceRange: Range<UInt64> = {
        let minMax = mappings.reduce(into: (UInt64.max,UInt64.min)) { (minMax, mapping) in
            minMax = (min(minMax.0, mapping.sourceStart), max(minMax.1, mapping.sourceEnd))
        }
        return Range<UInt64>(uncheckedBounds: minMax)
    }()
    lazy var destRange: Range<UInt64> = {
        let minMax = mappings.reduce(into: (UInt64.max,UInt64.min)) { (minMax, mapping) in
            minMax = (min(minMax.0, mapping.destStart), max(minMax.1, mapping.destEnd))
        }
        return Range<UInt64>(uncheckedBounds: minMax)
    }()
    lazy var continuous: Bool = { return sourceRange == destRange }()
    lazy var zerofill: UInt64 = { return UInt64(destRange.count - sourceRange.count) }()
    lazy var singleFile: Bool = { return files.count == 1 }()
    var count: Int { return destRange.count }
    
    func merge(memoryMap: MemoryMap) {
        var newMappings = memoryMap.mappings
        newMappings = newMappings.map { mapping in
            if case let .file(index) = mapping.source {
                return Mapping(source:.file(index+files.count), sourceStart:mapping.sourceStart, destStart:mapping.destStart, size:mapping.size)
            }
            return mapping
        }
        files.append(contentsOf:memoryMap.files)
        mappings = Array.mergeArraySorted(mappings, newMappings) { x, y in
            return x.destStart < y.destStart
        }
        assert(mappingsOrder)
    }
    
    func fullyResolve() {
        var newMappings: [Mapping] = []
        var currentFileCount = files.count
        
        for mapping in mappings {
            switch mapping.source {
            case .file, .localMemory:
                // Copy file and localMemory mappings directly
                newMappings.append(mapping)
                
            case .unresolved(let mapper):
                // Resolve the unresolved mapper
                let resolvedMemoryMap = mapper.memoryMap
                
                // Process the resolved mappings
                for resolvedMapping in resolvedMemoryMap.mappings {
                    let updatedMapping: Mapping
                    switch resolvedMapping.source {
                    case .localMemory: updatedMapping = resolvedMapping
                    case .file(let originalIndex):
                        // File sources need their file indexes updated
                        updatedMapping = Mapping(
                            source: .file(originalIndex + currentFileCount),
                            sourceStart: resolvedMapping.sourceStart,
                            destStart: resolvedMapping.destStart,
                            size: resolvedMapping.size
                        )
                    case .unresolved:
                        // The resolved memory maps should never have unresolved entries
                        fatalError("Resolved memory map contains unresolved entries")
                    }
                    newMappings.append(updatedMapping)
                }
                
                // Append the resolved memory map's files to our files array
                files.append(contentsOf: resolvedMemoryMap.files)
                currentFileCount += resolvedMemoryMap.files.count
            }
        }
        
        // Replace the mappings with the fully resolved ones
        mappings = newMappings.sorted { $0.destStart < $1.destStart }
        
        // Verify mappings are still in order
        assert(mappingsOrder)
    }
    
    var mappingsOrder: Bool {
        var currentStart = UInt64(0)
        for mapping in mappings {
            if (mapping.destStart < currentStart) { return false }
            currentStart = mapping.destStart
        }
        return true
    }
    
    // The subcache map can have a lot of entries, in an HWTrace like workload
    // binary search is a ~20% improvement
    @inline(__always)
    func mapping(containing range: Range<UInt64>) -> Mapping? {
        var n = mappings.count
        var l = 0

        while n > 0 {
            let half = n / 2
            let mid = mappings.index(l, offsetBy: half)
            let midRange = mappings[mid].destRange
            if midRange.contains(range) {
                return mappings[mid]
            }
            if midRange.lowerBound >= range.upperBound {
                n = half
            } else {
                l = mappings.index(after: mid)
                n -= half + 1
            }
        }
        return nil
    }

    @inline(__always)
    func memoryBuffer(range: Range<UInt64>) throws(MemoryMapError) -> MemoryBuffer {
        guard let mapping = mapping(containing:range) else {
            throw .outOfRange
        }
        guard range.count > 0 else {
            let offsetFromStartOFMapping = range.lowerBound &- mapping.destStart
            let start = UnsafeRawPointer(bitPattern:UInt(mapping.sourceStart + offsetFromStartOFMapping))
            return MemoryBuffer(localMemory:UnsafeRawBufferPointer(start:start, count:0))
        }
        let offsetFromStartOFMapping = range.lowerBound &- mapping.destStart
        switch mapping.source {
        case .unresolved(let mapper):
            // Remove the unresolved mapper}
            mappings.removeAll(where: { $0.destStart == mapping.destStart })
            // Resolve and merge it
            merge(memoryMap:mapper.memoryMap)
            // Re-enter now that our map has a resolved entries for this address
            return try memoryBuffer(range: range)
        case .localMemory:
            let start = UnsafeRawPointer(bitPattern:UInt(mapping.sourceStart))
            assert(mapping.zerofill == 0)
            let sourceBuffer = MemoryBuffer(localMemory:UnsafeRawBufferPointer(start:start, count:mapping.sourceRange.count))
            return MemoryBuffer(buffer:sourceBuffer, range:Int(offsetFromStartOFMapping)..<Int(offsetFromStartOFMapping)+range.count)
        case .file(let fileIndex):
            guard let sourceBuffer = mappedFileCache.fileBuffer(path:files[fileIndex], offset:mapping.sourceStart, size:UInt64(mapping.sourceRange.count), zerofill:mapping.zerofill) else {
                throw .inaccessibleFile(files[fileIndex])
            }
            return MemoryBuffer(buffer:sourceBuffer, range:Int(offsetFromStartOFMapping)..<Int(offsetFromStartOFMapping)+range.count)
        }
    }

    func slide(_ offset: UInt64) -> MemoryMap {
        let slidMappings = mappings.map { mapping in
            return Mapping(source:mapping.source, sourceStart:mapping.sourceStart, destStart:(mapping.destStart &+ offset), size:mapping.size)
        }
        return MemoryMap(files:files, mappings:slidMappings)
    }
}

extension MemoryBuffer {
    static private let mapFailed = UnsafeMutableRawPointer(bitPattern:Int(bitPattern:MAP_FAILED))
    convenience init?(memoryMap: inout MemoryMap) {
        memoryMap.fullyResolve()
        let allocationSize = vm_size_t(memoryMap.destRange.count)
        
        var address = vm_address_t(0)
        let kr = vm_allocate(mach_task_self_, &address, allocationSize, VM_FLAGS_ANYWHERE)
        guard kr == KERN_SUCCESS else { return nil }
        
        var currentFilePath: String = ""
        var currentFileDescriptor = Int32(-1)
        
        // Helper function to clean up and return nil on error
        func cleanup() {
            if currentFileDescriptor != Int32(-1) {
                close(currentFileDescriptor)
            }
            vm_deallocate(mach_task_self_, address, allocationSize)
        }
        
        for mapping in memoryMap.mappings {
            switch mapping.source {
            case .file(let pathIndex):
                let filePath = memoryMap.files[pathIndex]
                
                // Open new file if needed
                if currentFilePath != filePath {
                    if currentFileDescriptor != Int32(-1) {
                        close(currentFileDescriptor)
                    }
                    currentFilePath = filePath
                    currentFileDescriptor = open(currentFilePath, O_RDONLY)
                    guard currentFileDescriptor >= 0 else {
                        cleanup()
                        return nil
                    }
                }
                
                // Map this specific mapping
                let destAddress = UnsafeMutableRawPointer(bitPattern: UInt(address + vm_address_t(mapping.destStart - memoryMap.destRange.lowerBound)))
                guard mmap(destAddress, Int(mapping.size), PROT_READ, MAP_PRIVATE | MAP_FIXED, currentFileDescriptor, off_t(mapping.sourceStart)) != MemoryBuffer.mapFailed else {
                    cleanup()
                    return nil
                }
            case .localMemory:
                // Use mach_vm_copy to copy from local memory
                let sourceAddress = mach_vm_address_t(mapping.sourceStart)
                let destAddress = mach_vm_address_t(address + vm_address_t(mapping.destStart - memoryMap.destRange.lowerBound))
                let copySize = mach_vm_size_t(mapping.size)
                
                let copyResult = mach_vm_copy(mach_task_self_, sourceAddress, copySize, destAddress)
                guard copyResult == KERN_SUCCESS else {
                    cleanup()
                    return nil
                }
            case .unresolved:
                fatalError("Mappings must be fully resolved")
            }
        }
        
        if currentFileDescriptor != Int32(-1) {
            close(currentFileDescriptor)
        }
        let mappings = MemoryMap.Mapping(source:.localMemory, sourceStart:UInt64(address), destStart:memoryMap.destRange.lowerBound, size:UInt64(memoryMap.destRange.count))
        memoryMap = MemoryMap(files:[], mappings:[mappings])
        self.init(vmAllocated: UnsafeRawBufferPointer(start: UnsafeRawPointer(bitPattern: UInt(address)), count: Int(allocationSize)))
    }
}

extension Range where Bound: FixedWidthInteger {
    static func +(lhs: Self, rhs: Self.Bound) -> Self {
        return (lhs.lowerBound &+ rhs)..<(lhs.upperBound &+ rhs)
    }
    static func -(lhs: Self, rhs: Self.Bound) -> Self {
        return (lhs.lowerBound &- rhs)..<(lhs.upperBound &- rhs)
    }
}
