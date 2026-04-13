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

import Darwin

// MARK: -
// MARK: Error handling

enum BPListError: Error, Equatable, BPListErrorWrapper {
    case badMagic(UInt64)
    case truncated
    case invalidIntSize(UInt8)
    case expectedInteger
    case expectedString
    case expectedArray
    case expectedData
    case expectedDictionary
    init(_ plistError: BPListError) {
        self = plistError
    }
}

protocol BPListErrorWrapper {
    init(_ plistError: BPListError)
}

// MARK: -
// MARK: Binary Plist

struct BPList: ~Escapable {
    fileprivate let bytes: RawSpan
    @_lifetime(copy bytes)
    public init(bytes: RawSpan) throws(BPListError) {
        self.bytes = bytes
        guard bytes.byteCount >= 40 else { throw .truncated } // 40 == sizeof(header) + sizeof(trailer)
        let magic = UInt64(bigEndian:bytes.unsafeLoadUnaligned(as: UInt64.self))
        guard magic == 0x62_70_6c_69_73_74_30_30 else { throw .badMagic(magic) }
        let trailerBytes = bytes._extracting(last:26)
        // Validate the two integer fields in the trailer are correct here, so we can safely access the later without needing to do
        // error handling everywhere
        guard let _ = IntDescripter(rawValue:trailerBytes.unsafeLoad(as: UInt8.self)) else {
            throw .invalidIntSize(trailerBytes.unsafeLoad(as: UInt8.self))
        }
        guard let _ = IntDescripter(rawValue:trailerBytes.unsafeLoad(fromUncheckedByteOffset:1, as: UInt8.self)) else {
            throw .invalidIntSize(trailerBytes.unsafeLoad(fromUncheckedByteOffset:1, as: UInt8.self))
        }
    }
    var topObject: BPList.Object {
        @_lifetime(copy self)
        borrowing get {
            return unsafe _overrideLifetime(BPList.Object(bplist:self, objectIndex:Metadata(bytes:bytes).topObjectIndex), copying:self)
        }
    }
    // Debuggging
    @_lifetime(copy self)
    func object(index: Int) -> BPList.Object {
        return unsafe _overrideLifetime(BPList.Object(bplist:self, objectIndex:index), copying:self)
    }
    @_lifetime(copy metadata)
    fileprivate init(metadata: Metadata) {
        self.bytes = metadata.bytes
    }
}


extension BPList {
    struct Metadata: ~Escapable {
        let bytes: RawSpan
        private var trailerBytes: RawSpan {
            @_lifetime(copy self)
            borrowing get {
                return bytes._extracting(bytes.byteCount-26..<bytes.byteCount)
            }
        }
        var offsetTableOffsetSize: IntDescripter {
            return IntDescripter(rawValue:trailerBytes.unsafeLoad(as: UInt8.self))!
        }
        var objectRefSize: IntDescripter {
            return IntDescripter(rawValue:trailerBytes.unsafeLoad(fromUncheckedByteOffset:1, as: UInt8.self))!
        }
        var numObjects: Int {
            return Int(UInt64(bigEndian:trailerBytes.unsafeLoadUnaligned(fromUncheckedByteOffset:2, as: UInt64.self)))
        }
        var topObjectIndex: Int {
            return Int(UInt64(bigEndian:trailerBytes.unsafeLoadUnaligned(fromUncheckedByteOffset:10, as: UInt64.self)))
        }
        var objectTableOffset: Int {
            return Int(UInt64(bigEndian:trailerBytes.unsafeLoadUnaligned(fromUncheckedByteOffset:18, as: UInt64.self)))
        }

        @_lifetime(copy bytes)
        init(bytes: RawSpan) {
            self.bytes = bytes
        }

        // Get the bytes for the object. This span starts before the object header, and extends to the end of legally readable memory
        @_lifetime(borrow self)
        func bytes(objectIndex: Int) throws(BPListError) -> RawSpan {
            let objectOffsetOffset = objectIndex * offsetTableOffsetSize.size + objectTableOffset
            let objectOffsetBytes = bytes._extracting(objectOffsetOffset..<bytes.byteCount)
            
            let objectOffset = Int(try offsetTableOffsetSize.read(bigEngianBytes:objectOffsetBytes))

            guard objectOffset < bytes.byteCount else { throw .truncated }
            let objectBytes = bytes._extracting(objectOffset..<bytes.byteCount)
            return unsafe _overrideLifetime(objectBytes, copying:self)
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

extension BPList {
    struct Object: ~Escapable {
        fileprivate let metadata: Metadata
        let objectIndex : Int
        @_lifetime(borrow bplist)
        fileprivate init(bplist: borrowing BPList, objectIndex: Int) {
            self.metadata = Metadata(bytes:bplist.bytes)
            self.objectIndex = objectIndex
        }
        @_lifetime(borrow bytes)
        fileprivate init(bytes: RawSpan, objectIndex: Int) {
            self.metadata = Metadata(bytes:bytes)
            self.objectIndex = objectIndex
        }
        // Parse out the data. This reads the header, returns the kind and the count for the object, along with
        // removing the header from the span so that the first byte is the first byte of object data
        @_lifetime(rawSpan: copy rawSpan)
        fileprivate func parseObjectInfo(_ rawSpan: inout RawSpan) throws(BPListError) -> (Kind,Int) {
            let marker = rawSpan.unsafeLoad(as: UInt8.self)
            guard let kind = BPList.Kind(rawValue:marker) else { throw BPListError.invalidIntSize(marker) }
            let inlineCount = marker & 0x0f
            if inlineCount != 0x0f {
                rawSpan = rawSpan._extracting(droppingFirst:1)
                return (kind,Int(inlineCount))
            }
            let intMarker = rawSpan.unsafeLoad(fromUncheckedByteOffset:1, as:UInt8.self)
            // TODO: validate int marker type
            guard let intDesc = BPList.IntDescripter(rawValue:1<<(intMarker & 0x0f)) else {
                throw .invalidIntSize(1<<(intMarker & 0x0f))
            }
            rawSpan = rawSpan._extracting(droppingFirst:2)
            let size = Int(try intDesc.read(bigEngianBytes:rawSpan))
            rawSpan = rawSpan._extracting(droppingFirst:intDesc.size)
            return (kind, size)
        }
    }
}

extension BPList.Object {
    func asInt64() throws(BPListError) -> Int64 {
        var rawSpan = try metadata.bytes(objectIndex:objectIndex)
        let (kind, count) = try parseObjectInfo(&rawSpan)
        guard kind == .int else { throw .expectedInteger }
        guard let intDesc = BPList.IntDescripter(rawValue:UInt8(1<<count)) else {
            throw .invalidIntSize(1<<count)
        }
        return try intDesc.read(bigEngianBytes: rawSpan)
    }

    @_lifetime(copy self)
    func asArray() throws(BPListError) -> BPList.ArrayGuts {
        var bytes = try metadata.bytes(objectIndex:objectIndex)
        let (kind, count) = try parseObjectInfo(&bytes)
        guard kind == .array else { throw .expectedArray }
        return unsafe _overrideLifetime(BPList.ArrayGuts(metadata:metadata, bytes:bytes, count:count), copying:self)
    }
    
    @_lifetime(copy self)
    func asDictionary() throws(BPListError) -> BPList.DictionaryGuts {
        var bytes = try metadata.bytes(objectIndex:objectIndex)
        let (kind, count) = try parseObjectInfo(&bytes)
        guard kind == .dictionary else { throw .expectedDictionary }
        return unsafe _overrideLifetime(BPList.DictionaryGuts(metadata:metadata, bytes:bytes, count:count), copying:self)
    }

    @_lifetime(copy self)
    func asData() throws(BPListError) -> RawSpan {
        var rawSpan = try metadata.bytes(objectIndex:objectIndex)
        let (kind, count) = try parseObjectInfo(&rawSpan)
        guard kind == .data else { throw .expectedData }
        return unsafe _overrideLifetime(rawSpan._extracting(0..<count), copying:self)
    }

    func asFastString() throws(BPListError) -> BPList.FastString {
        var rawSpan = try metadata.bytes(objectIndex:objectIndex)
        let (kind, count) = try parseObjectInfo(&rawSpan)
        switch kind {
        case .asciiString:
            guard rawSpan.byteCount >= count else { throw .truncated }
            let buffer = rawSpan.withUnsafeBytes { return UnsafeRawBufferPointer(start:$0.baseAddress, count:count) }
            return BPList.FastString(asciiBuffer:buffer)
        case .unicodeString:
            guard rawSpan.byteCount >= 2*count else { throw .truncated }
            let buffer = rawSpan.withUnsafeBytes { return UnsafeRawBufferPointer(start:$0.baseAddress, count:2*count) }
            return BPList.FastString(unicodeBuffer:buffer)
        case .data:
            guard rawSpan.byteCount >= count else { throw .truncated }
            let buffer = rawSpan.withUnsafeBytes { return UnsafeRawBufferPointer(start:$0.baseAddress, count:count) }
            return BPList.FastString(illegalEncoding:buffer)
        default:
            throw BPListError.expectedString
        }
    }
}
// MARK: -
// MARK: Optimized internal string implementation

// ManagedBuffer subclass for managing C string storage
final class CStringBuffer: ManagedBuffer<Int, CChar> {
    static func create(capacity: Int) -> CStringBuffer {
        return CStringBuffer.create(minimumCapacity: capacity) { _ in
            return capacity
        } as! CStringBuffer
    }
    
    var storage: UnsafeMutablePointer<CChar> {
        return withUnsafeMutablePointerToElements { $0 }
    }
    
    var cString: UnsafePointer<CChar> {
        return UnsafePointer(storage)
    }
    
    deinit {
        // ManagedBuffer handles deallocation automatically
    }
}

// This exists so we can pass around a strings guts without materializing it until and unless we need to
extension BPList {
    internal struct FastString {
        private let guts: FastString.Guts
        private var _stringBuffer: CStringBuffer?
        
        init(asciiBuffer: UnsafeRawBufferPointer) {
            self.guts = .asciiBuffer(asciiBuffer)
        }
        init(unicodeBuffer: UnsafeRawBufferPointer) {
            self.guts = .unicodeBuffer(unicodeBuffer)
        }
        init(illegalEncoding: UnsafeRawBufferPointer) {
            self.guts = .illegalEncoding(illegalEncoding)
        }
        init(_ guts: String) {
            self.guts = .string(guts)
        }
        
        var stringValue: String {
            return guts.stringValue
        }
        
        var cString: UnsafePointer<CChar> {
            mutating get {
                if let _stringBuffer { return _stringBuffer.cString }
                
                switch guts {
                case .asciiBuffer(let buffer):
                    let len = buffer.count
                    let stringBuffer = buffer.bindMemory(to: CChar.self)
                    _stringBuffer = CStringBuffer.create(capacity: len + 1)
                    strlcpy(_stringBuffer!.storage, stringBuffer.baseAddress!, len + 1)
                default:
                    stringValue.withCString(encodedAs:UTF8.self) { (cString: UnsafePointer<Unicode.UTF8.CodeUnit>) in
                        let len = strlen(UnsafeRawPointer(cString).assumingMemoryBound(to: CChar.self))
                        _stringBuffer = CStringBuffer.create(capacity: len + 1)
                        strlcpy(_stringBuffer!.storage, UnsafeRawPointer(cString).assumingMemoryBound(to: CChar.self), len + 1)
                    }
                }
                return _stringBuffer!.cString
            }
        }
        
        func withCString<T,E>(_ body: (UnsafeBufferPointer<CChar>) throws(E) -> T) throws(E) -> T {
            return try guts.withCString(body)
        }

//        static func == (lhs: FastString, rhs: String) -> Bool {
//            return lhs.guts == rhs
//        }
//        static func == (lhs: String, rhs: FastString) -> Bool {
//            return rhs.guts == lhs
//        }
//
//        static func == (lhs: FastString, rhs: FastString) -> Bool {
//            return lhs.guts == rhs.guts
//        }
//
//        static func ~= (pattern: String, value: FastString) -> Bool {
//            return pattern ~= value.guts
//        }

        static func == (lhs: borrowing FastString, rhs: String) -> Bool {
            return lhs.guts == rhs
        }
        static func == (lhs: String, rhs: borrowing FastString) -> Bool {
            return rhs.guts == lhs
        }

        static func == (lhs: borrowing FastString, rhs: borrowing FastString) -> Bool {
            return lhs.guts == rhs.guts
        }

        static func ~= (pattern: String, value: borrowing FastString) -> Bool {
            return pattern ~= value.guts
        }
        private enum Guts {
            // ASCII strings going to legacy C APIs can bypass swift materialization altogether
            case asciiBuffer(UnsafeRawBufferPointer)
            case unicodeBuffer(UnsafeRawBufferPointer)
            case illegalEncoding(UnsafeRawBufferPointer)
            case string(String)

            var stringValue: String {
                switch self {
                case .asciiBuffer(let buffer):
                    return String(decoding:buffer[0..<buffer.count], as:UTF8.self)
                case .unicodeBuffer(let buffer):
                    let characters = UnalignedIntArray<UInt16>(bytes:buffer)
                    return String(decoding:characters, as: UTF16.self)
                case .string(let string):
                    return string
                case .illegalEncoding(let buffer):
                    return buffer.withMemoryRebound(to: Unicode.UTF8.CodeUnit.self) { stringBuffer in
                        let (result, _) = String.decodeCString(stringBuffer.baseAddress!, as: UTF8.self, repairingInvalidCodeUnits: true)!
                        return result
                    }
                }
            }
            
            func withCString<T,E>(_ body: (UnsafeBufferPointer<CChar>) throws(E) -> T) throws(E) -> T {
                do {
                    switch self {
                    case .asciiBuffer(let buffer):
                        return try buffer.withMemoryRebound(to: CChar.self) { stringBuffer in
                            let len = stringBuffer.count+1
                            return try withUnsafeTemporaryAllocation(of:CChar.self, capacity:len) { resultBufer in
                                strlcpy(resultBufer.baseAddress!, stringBuffer.baseAddress!, len)
                                return try body(UnsafeBufferPointer<CChar>(resultBufer))
                            }
                        }
                        
                    default:
                        return try stringValue.utf8CString.withUnsafeBufferPointer {
                            return try body($0)
                        }
                    }
                } catch {
                    throw error as! E
                }
            }
            
            static func == (lhs: FastString.Guts, rhs: String) -> Bool {
                switch lhs {
                case .asciiBuffer(let buffer):
                    return buffer.withMemoryRebound(to: CChar.self) { bufferPtr in
                        return rhs.withCString { strPtr in
                            return strncmp(strPtr, bufferPtr.baseAddress!, bufferPtr.count) == 0 && strPtr[bufferPtr.count] == 0
                        }
                    }
                default:
                    return rhs.withCString { patternCStringPtr in
                        return lhs.stringValue.withCString { strPtr in
                            return strcmp(strPtr, patternCStringPtr) == 0
                        }
                    }
                }
            }

            static func == (lhs: FastString.Guts, rhs: FastString.Guts) -> Bool {
                switch (lhs, rhs) {
                // Default cases for most encodings we first check for pointer identity, then fallback to materialization
                case (.asciiBuffer(let x), .asciiBuffer(let y)):
                    fallthrough
                case (.unicodeBuffer(let x), .unicodeBuffer(let y)):
                    fallthrough
                case (.illegalEncoding(let x), .illegalEncoding(let y)):
                    if x.baseAddress == y.baseAddress, x.count == y.count {
                        return true
                    }
                    return lhs.stringValue == rhs.stringValue
                // Special cases for comparing ascii strings used in dictionary lookups to make them fase
                case (.asciiBuffer(let object), .string(let str)):
                    fallthrough
                case (.string(let str), .asciiBuffer(let object)):
                    return object.withMemoryRebound(to: CChar.self) { objectBufferPtr in
                        return str.withCString { strPtr in
                            return strncmp(strPtr, objectBufferPtr.baseAddress!, objectBufferPtr.count) == 0 && strPtr[objectBufferPtr.count] == 0
                        }
                    }
                // Fallback that may trigger string materializations but will always work
                default:
                    return lhs.stringValue == rhs.stringValue
                }
            }
            static func ~= (pattern: String, value: FastString.Guts) -> Bool {
                switch value {
                case .asciiBuffer(let buffer):
                    return buffer.withMemoryRebound(to: CChar.self) { bufferPtr in
                        return pattern.withCString { strPtr in
                            return strncmp(strPtr, bufferPtr.baseAddress!, bufferPtr.count) == 0 && strPtr[bufferPtr.count] == 0
                        }
                    }
                default:
                    return pattern.withCString { patternCStringPtr in
                        return value.stringValue.withCString { strPtr in
                            return strcmp(strPtr, patternCStringPtr) == 0
                        }
                    }
                }
            }
        }
    }
}

// MARK: Raw collection guts

// These are used to implement the typed collections. They do not conform to actual collection classes because
// of issues with ~Escpabale and lifetimes

extension BPList {
    struct ArrayGuts: ~Escapable {
        let metadata: BPList.Metadata
        let bytes: RawSpan
        let count: Int

        @_lifetime(copy metadata)
        fileprivate init(metadata: BPList.Metadata, bytes: RawSpan, count: Int) {
            self.metadata = metadata
            // bytes was created from metadata, so overriding it lifetime to match is safe
            self.bytes = unsafe _overrideLifetime(bytes, copying:metadata)
            self.count = count
        }
        func forEach<E>(_ body: (BPList.Object) throws(E) -> Void) throws(E) {
            let intType = metadata.objectRefSize
            let bplist = BPList(metadata:metadata)
            for i in 0..<count {
                let objectIndex = try! intType.read(bigEngianBytes: bytes._extracting(droppingFirst: intType.size * i))
                let element = BPList.Object(bplist: bplist, objectIndex: Int(objectIndex))
                try body(element)
            }
        }
        func map<T,E>(_ transform: (BPList.Object) throws(E) -> T) throws(E) -> [T] {
            var result: [T] = []
            result.reserveCapacity(count)
            try self.forEach { (element) throws(E) -> Void in
                result.append(try transform(element))
            }
            return result
        }
    }
}

extension BPList {
    struct DictionaryGuts: ~Escapable {
        let metadata: BPList.Metadata
        let bytes: RawSpan
        let count: Int

        @_lifetime(copy metadata)
        fileprivate init(metadata: BPList.Metadata, bytes: RawSpan, count: Int) {
            self.metadata = metadata
            // bytes was created from metadata, so overriding it lifetime to match is safe
            // this would not be necessary if we could express some sort of compound lifetime
            self.bytes = unsafe _overrideLifetime(bytes, copying:metadata)
            self.count = count
        }
        func forEach<E>(_ body: (BPList.FastString, BPList.Object) throws(E) -> Void) throws(E) {
            let intType = metadata.objectRefSize
            let bplist = BPList(metadata:metadata)
            for i in 0..<count {
                let keyIndex = try! intType.read(bigEngianBytes: bytes._extracting(droppingFirst: intType.size * i))
                let valueIndex = try! intType.read(bigEngianBytes: bytes._extracting(droppingFirst: intType.size * (i + count)))
                let key = try! BPList.Object(bplist: bplist, objectIndex: Int(keyIndex)).asFastString()
                let value = BPList.Object(bplist: bplist, objectIndex: Int(valueIndex))
                try body(key, value)
            }
        }
        func reduce<Result,Error>(into initialResult: Result,
                                  _ updateAccumulatingResult: (inout Result, (BPList.FastString,BPList.Object)) throws(Error) -> ()) throws(Error) -> Result {
            var result = initialResult
            try forEach { (key, value) throws(Error) in
                 try updateAccumulatingResult(&result, (key, value))
            }
            return result
        }
    }
}

extension BPList {
    enum IntDescripter: RawRepresentable {
        case oneByte
        case twoBytes
        case fourBytes
        case eightBytes
        init?(rawValue size: UInt8) {
            switch size {
            case 1: self = .oneByte
            case 2: self = .twoBytes
            case 4: self = .fourBytes
            case 8: self = .eightBytes
            default: return nil
            }
        }
        public var rawValue: UInt8 {
            switch self {
            case .oneByte:      return 1
            case .twoBytes:     return 2
            case .fourBytes:    return 4
            case .eightBytes:   return 8
            }
        }
        var size: Int {
            return Int(self.rawValue)
        }
        //FIXME: Bounds check?
        func read(bigEngianBytes bytes: borrowing RawSpan) throws(BPListError) -> Int64 {
            switch self {
            case .oneByte: return Int64(bytes.unsafeLoad(as: UInt8.self))
            case .twoBytes: return Int64(UInt16(bigEndian: bytes.unsafeLoadUnaligned(as: UInt16.self)))
            case .fourBytes: return Int64(UInt32(bigEndian: bytes.unsafeLoadUnaligned(as: UInt32.self)))
            case .eightBytes: return Int64(Int64(bigEndian: bytes.unsafeLoadUnaligned(as: Int64.self)))
            }
        }
    }
}

// Support for accessing unaligned arrays of big endian integerss
struct UnalignedIntArray<T>: Collection where T: FixedWidthInteger {
    let bytes: UnsafeRawBufferPointer
    init(bytes: UnsafeRawBufferPointer) {
        self.bytes = bytes
    }

    // Sequence Protocol
    func makeIterator() -> Iterator {
        return Iterator(bytes[0..<bytes.count])
    }

    // Collection protocol support
    var startIndex: Int { 0 }
    var endIndex: Int { bytes.count }
    subscript(index: Int) -> T {
        return  T(bigEndian:bytes.loadUnaligned(fromByteOffset:index*MemoryLayout<T>.size, as:T.self))
    }
    func index(after i: Int) -> Int {
        return i+1
    }

    struct Iterator: IteratorProtocol {
        var bytes: Slice<UnsafeRawBufferPointer>
        init(_ bytes: Slice<UnsafeRawBufferPointer>) {
            self.bytes = bytes
        }

        mutating func next() -> T? {
            guard bytes.count >= MemoryLayout<T>.size else { return nil }
            let result = T(bigEndian:bytes.loadUnaligned(as:T.self))
            bytes = bytes.dropFirst(MemoryLayout<T>.size);
            return result
        }
    }
}

extension BPList {
    struct UnsafeObject : Sendable {
        let buffer: UnsafeRawBufferPointer
        let objectIndex: Int
        init(object: BPList.Object) {
            // FIXME: Technically super unsafe, in practice completely correct, discuss better options
            self.buffer = object.metadata.bytes.withUnsafeBytes { return $0 }
            self.objectIndex = object.objectIndex
        }
        var object: BPList.Object {
            @_lifetime(borrow self)
            borrowing get {
                let bytes = unsafe _overrideLifetime(buffer.bytes, copying:self)
                return unsafe _overrideLifetime(BPList.Object(bytes:bytes, objectIndex:objectIndex), copying:self)
            }
        }
    }
}

extension BPList.Object{
    func asUnsafeObject() -> BPList.UnsafeObject {
        return BPList.UnsafeObject(object: self)
    }
}
