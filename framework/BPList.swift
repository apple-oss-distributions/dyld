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

// TODO: Move to Span and make foundation an optional import
import Foundation

enum BPListError: Error, Equatable, BPListErrorWrapper {
    case badMagic(UInt64)
    case invalidKind
    case truncated
    case invalidIntSize(UInt8)
    case expectedInteger
    case expectedString
    case expectedArray
    case expectedData
    case expectedDictionary
    case bufferReadError(BufferError)
    case keyLookupError(String)
    case valueSizeError
    init(_ plistError: BPListError) {
        self = plistError
    }
}

protocol BPListErrorWrapper {
    init(_ plistError: BPListError)
}

extension BPListError: ConvertibleFromBufferError {
    init(_ bufferReaderError: BufferError) {
        self = .bufferReadError(bufferReaderError)
    }
}

// MARK: -

struct BPList {
    let metadata    : Metadata
    let objectIndex : Int
    init(metadata: Metadata, objectIndex: Int) {
        self.metadata = metadata
        self.objectIndex = objectIndex
    }
    init(data:Data, range: Range<Int>) throws(BPListError) {
        let metadata = try Metadata(data:data[range])
        self = metadata.topObject
    }
    init(data:Data) throws(BPListError) {
        let metadata = try Metadata(data:data[data.indices])
        self = metadata.topObject
    }
}

extension BPList {
    // TODO: Replace this with a lifetime restricted struct
    internal final class Metadata {
        fileprivate let data:                   Data
        fileprivate let offsetTableOffsetSize:  IntDescripter
        fileprivate let objectRefSize:          IntDescripter
        fileprivate let numObjects:             Int
        fileprivate let topObjectIndex:         Int
        fileprivate let objectTableOffset:      Int
        // This is essentiallly a prespecializeation hack to avoid enum dispatch for object index size in the hot path
        fileprivate let inedexArrayLookupFunction: (borrowing Metadata, Int) -> Int
        public init(data:Data) throws(BPListError) {
            self.data = data
            do {
                (self.offsetTableOffsetSize, self.objectRefSize, self.numObjects, self.topObjectIndex, self.objectTableOffset) = try data.withUnsafeBytes { bytesIn in
                    var bytes = bytesIn[0..<bytesIn.count]
                    let magic: UInt64 = try bytes.read(endian:.big, throwAs:BPListError.self)
                    guard magic             == 0x62_70_6c_69_73_74_30_30 else { throw BPListError.badMagic(magic) } // magic: bplist00
                    guard bytes.count       >= 32 else { throw BPListError.truncated  } // trailerSize(32)
                    var trailerBytes         = bytes.suffix(26)                         // First 6 bytes of trailer are zero
                    let offsetTableOffsetSize:  UInt8   = try trailerBytes.read(endian:.big, throwAs:BPListError.self)
                    let objectRefSize:          UInt8   = try trailerBytes.read(endian:.big, throwAs:BPListError.self)
                    let numObjects:             UInt64  = try trailerBytes.read(endian:.big, throwAs:BPListError.self)
                    let topObjectIndex:         UInt64  = try trailerBytes.read(endian:.big, throwAs:BPListError.self)
                    let objectTableOffset:      UInt64  = try trailerBytes.read(endian:.big, throwAs:BPListError.self)
                    return (try IntDescripter(offsetTableOffsetSize), try IntDescripter(objectRefSize), Int(numObjects), Int(topObjectIndex), Int(objectTableOffset))
                }
            } catch {
                throw error as! BPListError
            }

            switch offsetTableOffsetSize {
            case .oneByte:
                inedexArrayLookupFunction = { (metadata: borrowing Metadata, index: Int) -> Int in
                    let tableOffset = index * Int(metadata.offsetTableOffsetSize.size) + metadata.objectTableOffset
                    return data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                        return Int(UInt8(bigEndian:buffer.loadUnaligned(fromByteOffset:tableOffset, as: UInt8.self)))
                    }
                }
            case .twoBytes:
                inedexArrayLookupFunction = { (metadata: borrowing Metadata, index: Int) -> Int in
                    let tableOffset = index * Int(metadata.offsetTableOffsetSize.size) + metadata.objectTableOffset
                    return data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                        return Int(UInt16(bigEndian:buffer.loadUnaligned(fromByteOffset:tableOffset, as: UInt16.self)))
                    }
                }
            case .fourBytes:
                inedexArrayLookupFunction = { (metadata: borrowing Metadata, index: Int) -> Int in
                    let tableOffset = index * Int(metadata.offsetTableOffsetSize.size) + metadata.objectTableOffset
                    return data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                        return Int(UInt32(bigEndian:buffer.loadUnaligned(fromByteOffset:tableOffset, as: UInt32.self)))
                    }
                }
            case .eightBytes:
                inedexArrayLookupFunction = { (metadata: borrowing Metadata, index: Int) -> Int in
                    let tableOffset = index * Int(metadata.offsetTableOffsetSize.size) + metadata.objectTableOffset
                    return data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                        return Int(UInt64(bigEndian:buffer.loadUnaligned(fromByteOffset:tableOffset, as: UInt64.self)))
                    }
                }
            }
        }
    }
}

extension BPList {
    enum Kind: RawRepresentable {
        case data
        case asciiString
        case unicodeString
        case int
        case dictionary
        case array
        public init?(rawValue: UInt8) {
            switch rawValue &>> 4 {
            case 0b0100:
                self = .data
            case 0b0101:
                self = .asciiString
            case 0b0110:
                self = .unicodeString
            case 0b0001:
                self = .int
            case 0b1101:
                self = .dictionary
            case 0b1010:
                self = .array
            default:
                return nil
            }
        }
        public var rawValue: UInt8 {
            switch self {
            case .data:
                return 0b0100_0000
            case .asciiString:
                return 0b0101_0000
            case .unicodeString:
                return 0b0110_0000
            case .int:
                return 0b0001_0000
            case .dictionary:
                return 0b1101_0000
            case .array:
                return 0b1010_0000
            }
        }
    }
}

extension BPList.Metadata {
    private func getCount(marker:UInt8, bytes: inout Slice<UnsafeRawBufferPointer>) throws(BPListError) -> Int {
        let inlineCount = marker & 0x0f
        if inlineCount != 0x0f { return Int(inlineCount) }
        let intMarker: UInt8 = try bytes.read(endian:.big, throwAs:BPListError.self)
        guard BPList.Kind(rawValue:intMarker) != nil else {
            throw BPListError.invalidIntSize(intMarker)
        }
        let intSize = try BPList.IntDescripter(1<<(intMarker & 0x0f))
        return Int(try BPList.readInteger(bytes: &bytes, size:intSize))
    }
    
    @inline(__always)
    func withSlice<T>(object: Int, body: (BPList.Kind, Int, Slice<UnsafeRawBufferPointer>) throws(BPListError) -> T) throws(BPListError) -> T {
        let objectOffset = inedexArrayLookupFunction(self, object)
        do {
            return try data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                var bytes = buffer[objectOffset..<buffer.count]
                let marker: UInt8 = try bytes.read(endian:.big, throwAs:BPListError.self)
                let count = try getCount(marker:marker, bytes:&bytes)
                guard let kind = BPList.Kind(rawValue: marker) else {
                    throw BPListError.invalidKind
                }
                return try body(kind, count, bytes)
            }
        } catch {
            throw error as! BPListError
        }
    }

    @inline(__always)
    func withSlice<T>(offset: Int, body: (Slice<UnsafeRawBufferPointer>) throws(BPListError) -> T) throws(BPListError) -> T {
        do {
            return try data.withUnsafeBytes { (buffer:UnsafeRawBufferPointer) in
                return try body(buffer[offset..<buffer.count])
            }
        } catch {
            throw error as! BPListError
        }
    }

    var topObject: BPList {
        return BPList(metadata: self, objectIndex: topObjectIndex)
    }
}

extension BPList {
    fileprivate static func readInteger(bytes: inout Slice<UnsafeRawBufferPointer>, size:IntDescripter) throws(BPListError) -> Int64 {
        switch size {
        case .oneByte:
            let value: UInt8 = try bytes.read(endian:.big, throwAs:BPListError.self)
            return Int64(value)
        case .twoBytes:
            let value: UInt16 = try bytes.read(endian:.big, throwAs:BPListError.self)
            return Int64(value)
        case .fourBytes:
            let value: UInt32 = try bytes.read(endian:.big, throwAs:BPListError.self)
            return Int64(value)
        case .eightBytes:
            let value: Int64 = try bytes.read(endian:.big, throwAs:BPListError.self)
            return Int64(value)
        }
    }

    func asInt64() throws(BPListError) -> Int64 {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytesIn) throws(BPListError) -> Int64 in
            guard kind == .int else { throw .expectedInteger }
            var bytes = bytesIn[bytesIn.indices.startIndex..<bytesIn.indices.startIndex+(1<<count)]
            let result = try BPList.readInteger(bytes: &bytes, size:IntDescripter(UInt8(1<<count)))
            return result
        }
    }

    // This will need to be reworked for non-allocating embedded swift once there is some sort of string solution
    func asString() throws(BPListError) -> String {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytesIn) throws(BPListError) -> String in
            var bytes = bytesIn
            switch kind {
            case .asciiString:
                return try bytes.read(stringLength:count, throwAs:BPListError.self)
            case .unicodeString:
                guard bytes.count >= 2*count else { throw BPListError.truncated }
                let stringStartIndex = bytes.indices.lowerBound
                let stringRange = stringStartIndex..<(stringStartIndex+2*count)
                let characters = UnalignedIntArray<UInt16>(bytes:bytes[stringRange], endian:.big)
                let result = String(decoding:characters, as: UTF16.self)
                return result
            default:
                throw BPListError.expectedString
            }
        }
    }

    // This will need to be reworked for non-allocating embedded swift to use Span
    func asData() throws(BPListError) -> Data {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytes) throws(BPListError) -> Data in
            guard kind == .data else { throw .expectedData }
            // Becasue Data is its own slice the indices are shared, whcih is not conducive to later uses, so we create a new Data here to "rebase" it
            // Once we move to Span we will clean this up
            let startingPointer = UnsafeMutableRawPointer(mutating: bytes.base.baseAddress!+bytes.startIndex)
            // The underlying buffer for the plist must stay alive as long as the Data. In the future with restricted lifetimes we can unforce the constraint
            return Data(bytesNoCopy:startingPointer, count:count, deallocator: .none)        
        }
    }

    func asArray() throws(BPListError) -> BPList.UnsafeRawArray {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytes) throws(BPListError) -> BPList.UnsafeRawArray in
            guard kind == .array else { throw .expectedArray }
            return BPList.UnsafeRawArray(metadata:metadata, offset:bytes.indices.startIndex, count:count)
        }
    }

    func asArray<T: Serializable>(of:T.Type) throws(BPListError) -> BPList.UnsafeArray<T> {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytes) throws(BPListError) -> BPList.UnsafeArray<T> in
            guard kind == .array else { throw .expectedArray }
            return BPList.UnsafeArray<T>(metadata:metadata, offset:bytes.indices.startIndex, count:count)
        }
    }

    func asDictionary() throws(BPListError) -> BPList.UnsafeRawDictionary {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytes) throws(BPListError) -> BPList.UnsafeRawDictionary in
            guard kind == .dictionary else { throw .expectedDictionary }
            return BPList.UnsafeRawDictionary(metadata:metadata, offset:bytes.indices.startIndex, count:count)
        }
    }

    func asDictionary<T: Serializable>(of:T.Type) throws(BPListError) -> BPList.UnsafeDictionary<T> {
        return try metadata.withSlice(object:objectIndex) { (kind, count, bytes) throws(BPListError) -> BPList.UnsafeDictionary<T> in
            guard kind == .dictionary else { throw .expectedDictionary }
            return BPList.UnsafeDictionary<T>(metadata:metadata, offset:bytes.indices.startIndex, count:count)
        }
    }

    var isAsciiString: Bool {
        get throws(BPListError) {
            return try metadata.withSlice(object:objectIndex) { (kind, count, bytesIn) throws(BPListError) -> Bool in
                var _ = bytesIn
                switch kind {
                case .asciiString:
                    return true
                default:
                    return false
                }
            }
        }
    }
}

extension BPList {
    func unsafeRawBuffer() throws(BPListError) -> UnsafeRawBufferPointer {
        return try metadata.withSlice(object:objectIndex) { (_, count, bytes) throws(BPListError) -> UnsafeRawBufferPointer in
            let startingPointer = bytes.base.baseAddress!+bytes.startIndex
            return UnsafeRawBufferPointer(start: startingPointer, count: count)
        }
    }
}

// MARK: -

// These are collection wrappers for binary plists. They allow for efficient access to the underlying bplist collections
// without memory allocations, operating directly on the plist. They are unsafe due to depending on the data being well,
// formed, so only use them on bplists that have been validated.

// MARK: Conformances to allow typed collections

extension BPList {
    protocol Serializable {
        init(unsafePlist: BPList)
        init(plist: BPList) throws(BPListError)
        var count: Int { get }
    }
}

extension Int64: BPList.Serializable {
    init(unsafePlist plist: BPList) {
        self = try! plist.asInt64()
    }
    init(plist: BPList) throws(BPListError) {
        self = try plist.asInt64()
    }
    // Kind of degenerate, but since Data and String are semantically collections of
    // bytes andcharacters respectively, Int64 is the only true scalar
    var count: Int { return 1 }
}

extension String: BPList.Serializable {
    init(unsafePlist plist: BPList) {
        self = try! plist.asString()
    }
    init(plist: BPList) throws(BPListError) {
        self = try plist.asString()
    }
}

extension Data: BPList.Serializable {
    init(unsafePlist plist: BPList) {
        self = try! plist.asData()
    }
    init(plist: BPList) throws(BPListError) {
        self = try plist.asData()
    }
//    var count: Int? { return self.count }
}

extension BPList.UnsafeRawDictionary: BPList.Serializable {
    init(unsafePlist plist: BPList) {
        self = try! plist.asDictionary()
    }
    init(plist: BPList) throws(BPListError) {
        self = try plist.asDictionary()
    }
}

// MARK: Typed colllections

extension BPList {
    struct UnsafeArray<T: Serializable>: Collection  {
        let array: BPList.UnsafeRawArray
        init(metadata: Metadata, offset: Int, count: Int) {
            array = BPList.UnsafeRawArray(metadata: metadata, offset: offset, count: count)
        }

        // Sequence Protocol
        func makeIterator() -> Iterator {
            return Iterator(metadata:array.metadata, offset:array.offset, count:array.count)
        }

        // Collection protocol support
        var startIndex: Int { 0 }
        var endIndex: Int { array.count }
        subscript(index: Int) -> T {
            return T(unsafePlist:array[index])
        }
        func index(after i: Int) -> Int {
            return i+1
        }

        struct Iterator: IteratorProtocol {
            var iterator:   BPList.UnsafeRawArray.Iterator
            init(metadata: Metadata, offset: Int, count: Int) {
                self.iterator = BPList.UnsafeRawArray.Iterator(metadata:metadata, offset:offset, count:count)
            }
            mutating func next() -> T? {
                guard let plist = iterator.next() else { fatalError() }
                return T(unsafePlist:plist)
            }
        }
    }
}

// This exists so we can pass around a strings guts without materializing it until and unless we need to
// TODO: Can we merge FastString and DictinaryKey?
extension BPList {
    internal enum FastString {
        // ASCII strings going to legacy C APIs can bypass swift materialization altogether
        case asciiBuffer(UnsafeMutableRawBufferPointer)
        case bplist(BPList)

        var value: String {
            switch self {
            case .asciiBuffer(let buffer):
                return String(bytes:buffer[0..<buffer.count], encoding:.ascii)!
            case .bplist(let bplist):
                return try! bplist.asString()
            }
        }
    }
}

extension BPList {
    enum DictionaryKey {
        case string(String)
        case bplist(BPList)

        func asString() -> String {
            switch self {
            case .string(let result):
                return result
            case .bplist(let object):
                return try! object.asString()
            }
        }

        static func == (lhs: DictionaryKey, rhs: DictionaryKey) -> Bool {
            switch (lhs, rhs) {
                case (.string(let x), .string(let y)):
                return x == y
                case (.bplist(let x), .bplist(let y)):
                return x.objectIndex == y.objectIndex
            case (.bplist(let object), .string(let str)):
                fallthrough
            case (.string(let str), .bplist(let object)):
                return try! object.metadata.withSlice(object:object.objectIndex) { (kind, count, bytes) in
                    return bytes.withMemoryRebound(to: CChar.self) { objectBufferPtr in
                        return str.withCString { strPtr in
                            return strncmp(strPtr, objectBufferPtr.baseAddress!, count) == 0
                        }
                    }
                }
            }
        }
    }
}

extension BPList {
    struct UnsafeDictionary<T : Serializable>: Collection  {
        typealias Element = (key: DictionaryKey, value: T)

        let dict: BPList.UnsafeRawDictionary
        init(metadata: Metadata, offset: Int, count: Int) {
            dict = BPList.UnsafeRawDictionary(metadata: metadata, offset: offset, count: count)
        }
        // Sequence Protocol
        func makeIterator() -> Iterator {
            return Iterator(metadata:dict.metadata, offset:dict.offset, count:dict.count)
        }
        // Collection protocol support
        var startIndex: Int { 0 }
        var endIndex: Int { dict.count }
        subscript(index: Int) -> (key: DictionaryKey, value: T) {
            let element = dict[index]
            return (key: element.key, value:T(unsafePlist:element.value))
        }
        func index(after i: Int) -> Int {
            return i+1
        }

        subscript(key: DictionaryKey) -> T? {
            guard let plist = dict[key] else { return nil }
            return T(unsafePlist:plist)
        }

        subscript(key: String) -> T? {
            return self[.string(key)]
        }

        struct Iterator: IteratorProtocol {
            var iterator:   BPList.UnsafeRawDictionary.Iterator
            init(metadata: Metadata, offset: Int, count: Int) {
                self.iterator = BPList.UnsafeRawDictionary.Iterator(metadata:metadata, offset:offset, count:count)
            }
            mutating func next() -> (key: DictionaryKey, value: T)? {
                guard let element = iterator.next() else { return nil }
                return (key: element.key, value:T(unsafePlist:element.value))
            }
        }
    }
}

// MARK: Raw collections

// These are used to implement the typed collections. Direct use shuld be rare

extension BPList {
    struct UnsafeRawArray: RandomAccessCollection {
        let metadata    : Metadata
        let offset      : Int
        let count       : Int

        init(metadata: Metadata, offset: Int, count: Int) {
            self.metadata = metadata
            self.offset = offset
            self.count = count
        }

        // Sequence Protocol
        func makeIterator() -> Iterator {
            return Iterator(metadata:metadata, offset:offset, count:count)
        }

        // RandomAccessCollection protocol support
        // We only need to implement the methods for BidirectionalCollection since our Index is Int and Int is Strideable
        var startIndex: Int { 0 }
        var endIndex: Int { count }
        subscript(index: Int) -> BPList {
            let objectIndex = try! metadata.withSlice(offset:offset) { (bytes) throws(BPListError) -> Int in
                let intType = metadata.objectRefSize
                return try bytes.load(intType:intType, fromByteOffset:intType.size*index)
            }
            return BPList(metadata:metadata, objectIndex: objectIndex)
        }
        func index(after i: Int) -> Int {
            return i+1
        }
        func index(before i: Int) -> Int {
            return i-1
        }

        struct Iterator: IteratorProtocol {
            let metadata:   Metadata
            let offset:     Int
            let count:      Int
            var index:      Int
            init(metadata: Metadata, offset: Int, count: Int) {
                self.metadata   = metadata
                self.offset     = offset
                self.count      = count
                self.index      = 0
            }
            mutating func next() -> BPList? {
                guard index < count else { return nil }
                let objectIndex = try! metadata.withSlice(offset:offset) { (bytes) throws(BPListError) -> Int in
                    let intType = metadata.objectRefSize
                    return try bytes.load(intType:intType, fromByteOffset:intType.size*index)
                }
                index += 1
                return BPList(metadata:metadata, objectIndex: objectIndex)
            }
        }
    }
}

// MARK: -
extension BPList {
    struct UnsafeRawDictionary: Collection {
        typealias Element = (key: DictionaryKey, value: BPList)

        let metadata    : Metadata
        let offset      : Int
        let count       : Int

        init(metadata: Metadata, offset: Int, count: Int) {
            self.metadata = metadata
            self.offset = offset
            self.count = count
        }

        // Sequence Protocol
        func makeIterator() -> Iterator {
            return Iterator(metadata:metadata, offset:offset, count:count)
        }

        // Collection protocol support
        var startIndex: Int { 0 }
        var endIndex: Int { count }
        subscript(index: Int) -> (key: DictionaryKey, value: BPList) {
            let (keyIndex, valueIndex) = try! metadata.withSlice(offset:offset) { (bytes) throws(BPListError) -> (Int,Int) in
                let intType         = metadata.objectRefSize
                let keyIndex        = try bytes.load(intType:intType, fromByteOffset:intType.size*index)
                let valueIndex      = try bytes.load(intType:intType, fromByteOffset:intType.size*(index+count))
                return (keyIndex, valueIndex)
            }
            let key = BPList(metadata:metadata, objectIndex:keyIndex)
            let value = BPList(metadata:metadata, objectIndex:valueIndex)
            return (key:.bplist(key), value:value)
        }
        func index(after i: Int) -> Int {
            return i+1
        }

        @discardableResult
        func validate<T: Serializable, E: BPListErrorWrapper>(key: String, as:T.Type, throwAs: E.Type = BPListError.self, count: Int? = nil, optional: Bool = false) throws(E) -> T? {
            guard let object = self[.string(key)] else {
                if optional { return nil }
                throw E(.keyLookupError(key))
            }
            var result: T!
            do throws(BPListError) {
                result = try T(plist:object)
            } catch {
                throw E(error)
            }
            if let count, count != result.count {
                throw E(.valueSizeError)
            }
            return result
        }

        @discardableResult
        func validate<T: Serializable>(key: String, asArrayOf:T.Type, optional:Bool = false, block:(T) throws -> Void) throws -> BPList.UnsafeArray<T>? {
            guard let object = self[.string(key)] else {
                if optional { return nil }
                throw BPListError.keyLookupError(key)
            }
            let array = try object.asArray(of:asArrayOf)
            for element in array {
                try block(element)
            }
            return array
        }

        func validateArray<T: Serializable, E: BPListErrorWrapper>(key: String,
                                                                   of: T.Type,
                                                                   throwAs: E.Type = BPListError.self,
                                                                   optional:Bool = false,
                                                                   block:(T) throws(E) -> Void) throws(E) {
            guard let object = self[.string(key)] else {
                if optional { return }
                throw E(.keyLookupError(key))
            }
            var array: BPList.UnsafeRawArray!
            do throws(BPListError) {
                array = try object.asArray()
            } catch {
                throw E(error)
            }
            for element in array {
                let object: T!
                do throws(BPListError) {
                    object = try T(plist:element)
                } catch {
                    throw E(error)
                }
                try block(object)
            }
        }

        subscript(key: DictionaryKey) -> BPList? {
            // This is slow. bplist's do not hash, and we are not going to allocate, so we just need to scan
            // all of the keys until we get a hit. We expect to have a very small number of keys, so it should be fine
            return try! metadata.withSlice(offset:offset) { (bytes) throws(BPListError) -> BPList? in
                let intType         = metadata.objectRefSize
                for index in 0..<count {
                    let keyIndex = try bytes.load(intType:intType, fromByteOffset:intType.size*index)
                    let elementKey = DictionaryKey.bplist(BPList(metadata:metadata, objectIndex:keyIndex))
                    if key == elementKey {
                        let valueIndex = try bytes.load(intType:intType, fromByteOffset:intType.size*(index+count))
                        return BPList(metadata:metadata, objectIndex:valueIndex)
                    }
                }
                return nil
            }
        }

        subscript(key: String) -> BPList? {
            return self[.string(key)]
        }

        subscript<T: Serializable>(key: String, as as:T.Type) -> T? {
            guard let object = self[.string(key)] else { return nil }
            return T(unsafePlist:object)
        }

        subscript<T: Serializable>(key: String, asArrayOf as:T.Type) -> UnsafeArray<T>? {
            guard let object = self[.string(key)] else { return nil }
            return try! object.asArray(of:T.self)
        }

        struct Iterator: IteratorProtocol {
            let metadata:   Metadata
            let offset:     Int
            let count:      Int
            var index:      Int
            init(metadata: Metadata, offset: Int, count: Int) {
                self.metadata   = metadata
                self.offset     = offset
                self.count      = count
                self.index      = 0
            }
            mutating func next() -> (key: DictionaryKey, value: BPList)? {
                guard index < count else { return nil }
                let (keyIndex, valueIndex) = try! metadata.withSlice(offset:offset) { (bytes) throws(BPListError) -> (Int,Int) in
                    let intType = metadata.objectRefSize
                    let keyIndex     = try bytes.load(intType:intType, fromByteOffset:intType.size*index)
                    let valueIndex   = try bytes.load(intType:intType, fromByteOffset:intType.size*(index+count))
                    return (keyIndex, valueIndex)
                }
                index += 1
                let key = BPList(metadata:metadata, objectIndex:keyIndex)
                let value = BPList(metadata:metadata, objectIndex:valueIndex)
                return (key:.bplist(key), value:value)
            }
        }
    }
}

// MARK: -

// These are extensions to help with parsing that are specific to the binary plist format

extension BPList {
    enum IntDescripter {
        case oneByte
        case twoBytes
        case fourBytes
        case eightBytes
        init(_ size: UInt8) throws(BPListError) {
            switch size {
            case 1:
                self = .oneByte
                return
            case 2:
                self = .twoBytes
                return
            case 4:
                self = .fourBytes
                return
            case 8:
                self = .eightBytes
                return
            default:
                throw .invalidIntSize(size)
            }
        }
        var size: Int {
            switch self {
            case .oneByte:      return 1
            case .twoBytes:     return 2
            case .fourBytes:    return 4
            case .eightBytes:   return 8
            }
        }
    }
}

extension UnsafeRawBufferPointer {
    func load(intType:BPList.IntDescripter, fromByteOffset byteOffset:Int) throws(BPListError) -> Int {
        guard byteOffset + intType.size <= count else { throw .truncated }
        switch intType {
        case .oneByte:
            return Int(loadUnaligned(fromByteOffset:byteOffset, as:UInt8.self))
        case .twoBytes:
            return Int(UInt16(bigEndian:loadUnaligned(fromByteOffset:byteOffset, as:UInt16.self)))
        case .fourBytes:
            return Int(UInt32(bigEndian:loadUnaligned(fromByteOffset:byteOffset, as:UInt32.self)))
        case .eightBytes:
            return Int(Int64(bigEndian:loadUnaligned(fromByteOffset:byteOffset, as:Int64.self)))
        }
    }
}

extension Slice<UnsafeRawBufferPointer> {
    func load(intType:BPList.IntDescripter, fromByteOffset byteOffset:Int) throws(BPListError)  -> Int {
        // Do the guard here so that it against the slice count instead of the whole buffer in the pass through implementation
        guard byteOffset + intType.size <= count else { throw .truncated }
        return try base.load(intType:intType, fromByteOffset:indices.startIndex+byteOffset)
    }
}

extension UnalignedIntArray {
    static subscript (_ index: Int, intType: BPList.IntDescripter, base base:UnsafeRawPointer, count count:Int) -> Int {
        let byteCount = count * intType.size
        let indexRange = 0..<byteCount
        let buffer = UnsafeRawBufferPointer(start:base, count:byteCount)

        switch intType {
        case .oneByte:
            let array = UnalignedIntArray<UInt8>(bytes:buffer[indexRange], endian:.big)
            return Int(array[index])
        case .twoBytes:
            let array = UnalignedIntArray<UInt16>(bytes:buffer[indexRange], endian:.big)
            return Int(array[index])
        case .fourBytes:
            let array = UnalignedIntArray<UInt32>(bytes:buffer[indexRange], endian:.big)
            return Int(array[index])
        case .eightBytes:
            let array = UnalignedIntArray<UInt64>(bytes:buffer[indexRange], endian:.big)
            return Int(array[index])
        }
    }
}
