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

// This an abstraction to reprsent an owned element of memory. Conceptually
// it is the same as Data, but there are a number of differences that motivate
// the use of a custom abstraction:
//
// 1. Data is a Foundation type, but we need a memory ownership type we can
//    use in embedded environments without Foundation.
// 2. We also need a type that can be used without allocations, which means
//    it must be a `struct`, not a `class`
// 3. Data pre-dates `~Copyable`, which makes it inefficient in many of our
//    contexts
//
// We do need interoperability with Data in public interfaces, so when built
// with Foundation MemmoryBuffer includes support for creating a memory buffer
// from a data.
//
// `MemoryBuffer`s created via `uncheckedUnowned` are more dangerous than normal
// as they do not own the underlying memory. There are two safe use case for them:
//
// 1. When the use is scoped inside of a non-escaping closure
// 2. When we know another higher layer object is holding the memory for us. In
//    the case of `Dyld.framework` this occurs primarily in two cases. The first
//    is that the `AppleArchive` object is owned by the process snapshot. The
//    the snapshot does not want to copy the buffer for the BPList's, ans since
//    it own's the archive it can safely use the memory until the archive is
//    released. A similiar situation occurs with the shared cache archives and
//    the atlas cache. In the future when lifetime annotations exist we can have
//    the compiler enforce those constraints, for now they have to be managed
//    manually.

import os
import Darwin
import Compression

#if canImport(Foundation)
import Foundation
#endif

#if canImport(Dispatch)
import Dispatch
#endif

extension FixedWidthInteger {
    /// Rounds up to the nearest multiple of the given value.
    /// - Parameter multiple: The value to round up to a multiple of. Must be a power of 2.
    /// - Returns: The smallest value that is greater than or equal to self and is a multiple of the given value.
    @_transparent
    func roundedUp<T: ExpressibleByIntegerLiteral>(toMultipleOf multiple: T) -> Self where T == Self {
        assert(multiple > 0 && (multiple & (multiple - 1)) == 0, "Multiple must be a power of 2")
        return (self + multiple - 1) & ~(multiple - 1)
    }
    
    /// Rounds down to the nearest multiple of the given value.
    /// - Parameter multiple: The value to round down to a multiple of. Must be a power of 2.
    /// - Returns: The largest value that is less than or equal to self and is a multiple of the given value.
    @_transparent
    func roundedDown<T: ExpressibleByIntegerLiteral>(toMultipleOf multiple: T) -> Self where T == Self {
        assert(multiple > 0 && (multiple & (multiple - 1)) == 0, "Multiple must be a power of 2")
        return self & ~(multiple - 1)
    }
    
    var roundedUpToPageSize: Self {
        return roundedUp(toMultipleOf: Self(vm_page_size))
    }
    
    var roundedDownToPageSize: Self {
        return roundedDown(toMultipleOf: Self(vm_page_size))
    }
}

struct MappedFileLRUCache {
#if os(macOS)
    static let maxSize = UInt64(8 * 1024 * 1024  * 1024)    // 8GB
#elseif os(watchOS)
    static let maxSize = UInt64(384 * 1024  * 1024)         // 364MB
#else
    static let maxSize = UInt64(1024 * 1024  * 1024)        // 1GB
#endif

    struct Key: Hashable {
        let path:               String
        let range:              Range<UInt64>
        let zerofillExtension:  UInt64
    }
    class Entry {
        let path:               String
        let buffer:             MemoryBuffer
        let range:              Range<UInt64>
        let zerofillExtension:  UInt64
        init(path: String, range: Range<UInt64>, zerofillExtension: UInt64 = 0, buffer: MemoryBuffer) {
            self.path               = path
            self.buffer             = buffer
            self.range              = range
            self.zerofillExtension  = zerofillExtension
        }
        var count: Int { buffer.count }
        unowned(unsafe) var next: Entry? = nil
        unowned(unsafe) var prev: Entry? = nil
    }
    struct CacheState {
        var entries:    [String : [Entry]]   = [:]
        var first:      Entry?          = nil
        var last:       Entry?          = nil
        var size:       UInt64          = 0
        var checkedSize: UInt64 {
            var result = UInt64(0)
            for (_, entryArray) in entries {
                entryArray.map { result += UInt64($0.count) }
            }
            return result
        }
        private mutating func removeLastEntry() {
            // If there are zero entries do nothing
            guard let last else         { return }
            // If there is one entry clear everything
            if last === first {
                entries     = [:]
                first       = nil
                self.last   = nil
                size        = 0
                assert(checkedSize == size)
                return
            }
            // There are 2+ entries
            size -= UInt64(last.count)
            entries[last.path]!.removeAll { $0 === last }
            if entries[last.path]!.isEmpty {
                entries[last.path] = nil
            }
            if let lastPrev = last.prev {
                lastPrev.next = nil
                self.last = lastPrev
            }
            assert(checkedSize == size)
        }
        private mutating func reduceToBelowMaxSize() {
            while size > MappedFileLRUCache.maxSize {
                removeLastEntry()
            }
        }
        mutating func insert(key: Key, buffer: MemoryBuffer) {
            // First check to see if there is already an entry, if so dreop this and exit early
            assert(buffer.count == UInt64(key.range.count) + key.zerofillExtension)
            let existingEntry = lookup(key:key)
            if existingEntry != nil { return }
            // Next drop the size down below the max VM threshold
            reduceToBelowMaxSize()
            var mapping = entries[key.path] ?? []
            let entry = Entry(path: key.path, range: key.range, zerofillExtension: key.zerofillExtension, buffer: buffer)
            size += UInt64(entry.count)
            guard let first, let last else {
                assert(first === last)
                first  = entry
                last = entry
                mapping.append(entry)
                entries[key.path] = mapping
                return
            }
            entry.next = first
            first.prev = entry
            self.first = entry
            mapping.append(entry)
            entries[key.path] = mapping
            assert(last.next == nil)
            assert(self.first!.prev == nil)
            assert(last.prev != nil)
            assert(self.first!.next != nil)
            assert(checkedSize == size)
        }
        mutating func lookup(key: Key) -> Entry? {
            guard let mapping = entries[key.path] else { return nil }
            let result = mapping.first { entry in
                let keyInRange = entry.range.contains(key.range)
                let zerofillFits = entry.zerofillExtension >= key.zerofillExtension
                let zerofillAligned = key.range.upperBound == entry.range.upperBound
                let hasZerofill = key.zerofillExtension > 0
                return keyInRange && (!hasZerofill || (zerofillFits && zerofillAligned))
            }
            guard let result else { return nil }
            
            // Already firt entry, no need to update
            if result === first {
                return result
            }
            // Last (and we know it is not first
            if result === last {
                last = result.prev
            }
            if let resultNext = result.next {
                resultNext.prev = result.prev
            }
            if let resultPrev = result.prev {
                resultPrev.next = result.next
            }
            result.prev = nil
            result.next = first
            if let first {
                first.prev = result
            }
            first = result
            assert(self.last!.next == nil)
            assert(self.first!.prev == nil)
            assert(self.last!.prev != nil)
            assert(self.first!.next != nil)
            return result
        }
    }
    let cache = OSAllocatedUnfairLock(initialState:CacheState())
    
    subscript(path path: String, offset offset: UInt64, size size: UInt64, zerofill zerofill: UInt64) -> MemoryBuffer? {
        get {
            let range = offset..<(offset+size)
            let key = Key(path:path, range:range, zerofillExtension:zerofill)
            return cache.withLock { (cache: inout CacheState) -> MemoryBuffer? in
                guard let entry = cache.lookup(key:key) else { return nil }
                if entry.range == key.range && entry.zerofillExtension == key.zerofillExtension {
                    return entry.buffer
                }
                let offset = key.range.lowerBound - entry.range.lowerBound
                var actualSize = size
                if size == 0 {
                    actualSize = entry.range.upperBound - offset
                }
                return MemoryBuffer(buffer:entry.buffer, range:Int(offset)..<Int(offset+actualSize))
            }
        }
        set (newValue) {
            guard let newValue else { return }
            let range = offset..<(offset+size)
            let key = Key(path:path, range:range, zerofillExtension:zerofill)
            return cache.withLock { (cache: inout  CacheState) in
                cache.insert(key:key, buffer:newValue)
            }
        }
    }
    mutating func fileBuffer(path: String, offset: UInt64, size: UInt64? = nil , zerofill: UInt64) -> MemoryBuffer? {
        if let cacheLookup = self[path:path, offset:offset, size:size ?? 0, zerofill:zerofill] {
//            print("CACHE HIT")
            return cacheLookup
        } else {
//            print("CACHE MISS")
            let fd = open(path, O_RDONLY)
            guard fd >= 0 else { return nil }
            defer { close(fd) }
            var actualSize: UInt64
            if let size {
                actualSize = size
            } else {
                var statBuf = stat()
                guard fstat(fd, &statBuf) == 0 else {
                    return nil
                }
                actualSize = UInt64(statBuf.st_size)
            }
            guard let result = MemoryBuffer(fd:fd, offset:offset, size:actualSize, zerofill:zerofill) else { return nil }
            self[path:path, offset:offset, size:actualSize, zerofill:zerofill] = result
            return result
        }
    }
}

var mappedFileCache = MappedFileLRUCache()

final class MemoryBuffer: Sendable {
    enum Deallocator {
        case indirect(MemoryBuffer)
        case swiftDeallocate
        case munmap
        case free
        case vmDeallocate
        case none

        func deallocate(_ buffer: UnsafeRawBufferPointer) {
            switch self {
            case .swiftDeallocate:
                UnsafeMutableRawBufferPointer(mutating:buffer).deallocate()
            case .munmap:
                Darwin.munmap(UnsafeMutableRawPointer(mutating:buffer.baseAddress!), buffer.count)
            case .free:
                Darwin.free(UnsafeMutableRawPointer(mutating:buffer.baseAddress!))
            case .vmDeallocate:
                let address = vm_address_t(UInt(bitPattern:buffer.baseAddress))
                let size = vm_size_t(buffer.count)
                vm_deallocate(mach_task_self_, address, size)
            case .none, .indirect:
                break
            }
        }
    }
    let buffer: UnsafeRawBufferPointer
    let deallocator: Deallocator

    convenience init?(path: String, offset: UInt64 = 0, size: UInt64? = nil, zerofill: UInt64 = 0, decompress: Bool = false) {
        let fd = open(path, O_RDONLY)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var actualSize: UInt64
        if let size {
            actualSize = size
        } else {
            var statBuf = stat()
            guard fstat(fd, &statBuf) == 0 else {
                return nil
            }
            actualSize = UInt64(statBuf.st_size)
        }
        self.init(fd:fd, offset:offset, size:actualSize, zerofill:zerofill, decompress:decompress)
    }
    
    convenience init?(fd: Int32, offset: UInt64, size: UInt64, zerofill: UInt64, decompress: Bool = false) {
        guard let buffer = mmap(nil, Int(size+zerofill), PROT_READ, MAP_PRIVATE, fd, off_t(offset)),
              buffer != UnsafeMutableRawPointer(bitPattern:Int(bitPattern:MAP_FAILED)) else {
            print("Errno \(errno): \(String(describing: strerror(errno)))")
            return nil
        }
        self.init(buffer:UnsafeRawBufferPointer(start:buffer, count:Int(size+zerofill)), transparentlyDecompress:decompress, deallocator:.munmap)
    }
    
    convenience init(buffer: MemoryBuffer, range: Range<Int>) {
        self.init(buffer:UnsafeRawBufferPointer(rebasing:buffer.buffer[range]), transparentlyDecompress:false, deallocator:.indirect(buffer))
    }
    convenience init() {
        self.init(buffer:UnsafeRawBufferPointer(start:nil, count:0), transparentlyDecompress:false, deallocator:.none)
    }
    convenience init(localMemory: UnsafeRawBufferPointer) {
        self.init(buffer:localMemory, transparentlyDecompress:false, deallocator:.none)
    }
    convenience init(vmAllocated: UnsafeRawBufferPointer) {
        self.init(buffer:vmAllocated, transparentlyDecompress:false, deallocator:.vmDeallocate)
    }
    
    convenience init(malloced start: UnsafeRawPointer?, count:Int) {
        self.init(buffer:UnsafeRawBufferPointer(start:start, count:count), transparentlyDecompress:false, deallocator:.free)
    }
    
#if canImport(Foundation)
    convenience init(data: Data) {
        let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: data.count, alignment: 0)
        data.withUnsafeBytes {
            buffer.copyMemory(from: $0)
        }
        self.init(buffer:UnsafeRawBufferPointer(buffer), transparentlyDecompress:false, deallocator:.swiftDeallocate)
    }
#endif
    init(buffer:UnsafeRawBufferPointer, transparentlyDecompress:Bool, deallocator: Deallocator) {
        if transparentlyDecompress,
           let compressionInfo = try? buffer.bytes.compressionInfo {
            let decompressedBuffer = UnsafeMutableRawBufferPointer.allocate(byteCount:compressionInfo.1, alignment:0)
            var decompressedBytes = unsafe MutableRawSpan(_unsafeBytes:decompressedBuffer)
            buffer.bytes.decompress(algorithm: compressionInfo.0, into: &decompressedBytes)
            self.buffer = UnsafeRawBufferPointer(decompressedBuffer)
            self.deallocator = .swiftDeallocate
            deallocator.deallocate(buffer)
        } else {
            self.buffer = buffer
            self.deallocator = deallocator
        }
    }

    var count: Int {
        return buffer.count
    }
    var indices: Range<Int> { 0..<buffer.count }

    subscript(range: Range<Int>) -> MemoryBuffer {
        return MemoryBuffer(buffer:self, range:range)
    }

    deinit {
        deallocator.deallocate(buffer)
    }

    var bytes: RawSpan {
        @_lifetime(borrow self)
        borrowing get {
            return unsafe _overrideLifetime(RawSpan(_unsafeBytes:buffer), copying:self)
        }
    }
}

extension MemoryBuffer {
    enum ParallelCompressionAlgoirthm: RawRepresentable {
        case zlib
        case lzma
        case lz4
        case lzfse
        case lzbitmap
        case none
        init?(rawValue: compression_algorithm) {
            switch rawValue {
            case COMPRESSION_ZLIB:                  self = .zlib        // pbzz (ZLIB)
            case COMPRESSION_LZMA:                  self = .lzma        // pbzx (LZMA)
            case COMPRESSION_LZ4:                   self = .lz4         // pbz4 (LZ4)
            case COMPRESSION_LZFSE:                 self = .lzfse       // pbze (LZFSE)
            case COMPRESSION_LZBITMAP:              self = .lzbitmap    // pbzb (LZBITMAP)
            case compression_algorithm(rawValue:0): self = .none        // pbz- (NO COMPRESSION)
            default: return nil
            }
        }
        public init?(fromMagic: UInt32) {
            switch fromMagic {
            case 0x70_62_7a_7a: self = .zlib        // pbzz (ZLIB)
            case 0x70_62_7a_78: self = .lzma        // pbzx (LZMA)
            case 0x70_62_7a_34: self = .lz4         // pbz4 (LZ4)
            case 0x70_62_7a_65: self = .lzfse       // pbze (LZFSE)
            case 0x70_62_7a_62: self = .lzbitmap    // 'TYP'
            case 0x70_62_7a_2d: self = .none        // pbz- (NO COMPRESSION)
            default: return nil
            }
        }
        public var rawValue: compression_algorithm {
            switch self {
            case .zlib:     return COMPRESSION_ZLIB
            case .lzma:     return COMPRESSION_LZMA
            case .lz4:      return COMPRESSION_LZ4
            case .lzfse:    return COMPRESSION_LZFSE
            case .lzbitmap: return COMPRESSION_LZBITMAP
            case .none:     return compression_algorithm(rawValue:0)
            }
        }
    }
}

extension MemoryBuffer {
    @inline(__always)
    func data() -> DispatchData {
        let retainedBuffer = Unmanaged.passRetained(self)
        return DispatchData(bytesNoCopy:buffer, deallocator:.custom(nil, {
            retainedBuffer.release()
        }))
    }
}

enum DecompressionError : Error {
    case truncatedCompressedArchive
}

extension RawSpan {
    var compressionInfo: (MemoryBuffer.ParallelCompressionAlgoirthm, Int)? {
        get throws {
            guard byteCount >= 12 else { return nil }
            let magic = unsafeLoadUnaligned(as:UInt32.self).bigEndian
            guard let alg = MemoryBuffer.ParallelCompressionAlgoirthm(fromMagic:magic) else { return nil }
            let _ = unsafeLoadUnaligned(fromByteOffset:8, as:UInt64.self).bigEndian
            var outputBufferSize    = 0
            var filePosition        = 12
            while filePosition < byteCount {
                guard byteCount - filePosition >= 16 else { throw DecompressionError.truncatedCompressedArchive }
                outputBufferSize += Int(unsafeLoadUnaligned(fromByteOffset:filePosition, as:UInt64.self).bigEndian)
                let encodedBlockSize = unsafeLoadUnaligned(fromByteOffset:filePosition+8, as:UInt64.self).bigEndian
                filePosition += Int(encodedBlockSize + 16)
                guard filePosition <= byteCount  else { throw DecompressionError.truncatedCompressedArchive  }
            }
            return (alg, outputBufferSize)
        }
    }
    
    @_lifetime(outputBytes: copy outputBytes)
    func decompress(algorithm:MemoryBuffer.ParallelCompressionAlgoirthm, into outputBytes: inout MutableRawSpan) {
        let scratchBufferSize = compression_decode_scratch_buffer_size(algorithm.rawValue)
        let scratchBuffer = UnsafeMutableRawPointer.allocate(byteCount:scratchBufferSize, alignment:MemoryLayout<UInt8>.alignment)
        defer { scratchBuffer.deallocate() }
        var filePosition = 12
        var outputPosition = 0
        while filePosition < byteCount {
            let blockSize                       = Int(unsafeLoadUnaligned(fromByteOffset:filePosition, as:UInt64.self).bigEndian)
            let encodedBlockSize                = Int(unsafeLoadUnaligned(fromByteOffset:filePosition+8, as:UInt64.self).bigEndian)
            let currentCompressedBlockRange     = filePosition+16..<filePosition+encodedBlockSize+16
            let compressedBlockSpan             = self._extracting(currentCompressedBlockRange)
            let decompressedByteCount = compressedBlockSpan.withUnsafeBytes { unsafeInputBuffer in
                outputBytes.withUnsafeMutableBytes { allOutputBytes in
                    let outputBuffer = UnsafeMutableRawBufferPointer(
                        start: allOutputBytes.baseAddress?.advanced(by: outputPosition),
                        count: blockSize
                    )
                    return compression_decode_buffer(outputBuffer.baseAddress!, outputBuffer.count, unsafeInputBuffer.baseAddress!, unsafeInputBuffer.count, scratchBuffer, algorithm.rawValue)
                }
            }
            guard decompressedByteCount <= blockSize else {
                fatalError("Decompressed more bytes than allocated space for block")
            }
            filePosition                += (encodedBlockSize + 16)
            outputPosition              += blockSize
        }
    }
}

internal class MemoryBufferNSDataBridge : NSData {
    let memoryBuffer: MemoryBuffer
    init(memoryBuffer: MemoryBuffer) {
        self.memoryBuffer = memoryBuffer
        super.init()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override var length: Int                { return memoryBuffer.count }
    override var bytes: UnsafeRawPointer    { return memoryBuffer.buffer.baseAddress! }
}
