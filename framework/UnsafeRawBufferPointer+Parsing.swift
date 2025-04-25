/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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

enum BufferError: Error, Equatable, ConvertibleFromBufferError {
    case truncatedRead(UnsafeRawPointer?, UnsafeRawPointer?, Int)
    case stringError
    init(_ bufferReaderError: BufferError) {
        self = bufferReaderError
    }
}

protocol ConvertibleFromBufferError: Error {
    init(_ bufferReaderError: BufferError)
}

// Convenience methods for accessing mixed endian integers
internal enum Endian {
    case little
    case big
    // TODO: Make this correct for big endian platforms
    static var native: Endian {
        return .little
    }
    static var reverse: Endian {
        return .big
    }
}

extension FixedWidthInteger {
    func from(endian:Endian) -> Self {
        switch endian {
        case .little:
            return .init(littleEndian:self)
        case .big:
            return .init(bigEndian:self)
        }
    }
}

// These functions support reading from a range by enabling unaligned loads and endian swapping as
// necessary.

internal extension Slice where Base == UnsafeRawBufferPointer {
    mutating func read<T: FixedWidthInteger, E: ConvertibleFromBufferError>(endian: Endian = .little, as: T.Type = T.self, throwAs: E.Type = BufferError.self) throws(E) -> T {
        guard self.count >= MemoryLayout<T>.size else {
            let rebasedBuffer = UnsafeRawBufferPointer(rebasing:self)
            let startAddress = rebasedBuffer.baseAddress
            let endAddress: UnsafeRawPointer
            if let startAddress {
                endAddress = startAddress+rebasedBuffer.count
            } else {
                endAddress = UnsafeRawPointer(bitPattern:rebasedBuffer.count)!
            }
            throw E(.truncatedRead(startAddress, endAddress, MemoryLayout<T>.size))
        }
        let result = self.loadUnaligned(as:T.self).from(endian:endian)
        self = dropFirst(MemoryLayout<T>.size)
        return result
    }

    mutating func read<E: ConvertibleFromBufferError>(stringLength: Int, throwAs: E.Type = BufferError.self) throws(E) -> String {
        guard self.count >= stringLength else {
            let rebasedBuffer = UnsafeRawBufferPointer(rebasing:self)
            let startAddress = rebasedBuffer.baseAddress
            let endAddress: UnsafeRawPointer
            if let startAddress {
                endAddress = startAddress+rebasedBuffer.count
            } else {
                endAddress = UnsafeRawPointer(bitPattern:rebasedBuffer.count)!
            }
            throw E(.truncatedRead(startAddress, endAddress, stringLength))
        }
        let startIndex = indices.startIndex
        guard let result = String(bytes:base[startIndex..<startIndex+stringLength], encoding:.ascii) else { throw E(.stringError) }
        removeFirst(stringLength)
        return result
    }

    mutating func readUuid<E: ConvertibleFromBufferError>(throwAs: E.Type = BufferError.self) throws(E) -> UUID {
        guard self.count >= 16 else {
            let rebasedBuffer = UnsafeRawBufferPointer(rebasing:self)
            let startAddress = rebasedBuffer.baseAddress
            let endAddress: UnsafeRawPointer
            if let startAddress {
                endAddress = startAddress+rebasedBuffer.count
            } else {
                endAddress = UnsafeRawPointer(bitPattern:rebasedBuffer.count)!
            }
            throw E(.truncatedRead(startAddress, endAddress, 16))
        }
        let result =  self.withMemoryRebound(to: uuid_t.self) {
            return UUID(uuid:$0[0])
        }
        removeFirst(16)
        return result
    }
}

// Support for accessing unaligned arrays of integerss
struct UnalignedIntArray<T>: Collection where T: FixedWidthInteger {
    let bytes: Slice<UnsafeRawBufferPointer>
    let endian: Endian
    init(bytes: Slice<UnsafeRawBufferPointer>, endian:Endian = .little) {
        self.bytes = bytes
        self.endian = endian
    }

    // Sequence Protocol
    func makeIterator() -> Iterator {
        return Iterator(bytes, endian:endian)
    }

    // Collection protocol support
    var startIndex: Int { 0 }
    var endIndex: Int { bytes.count }
    subscript(index: Int) -> T {
        return  bytes.loadUnaligned(fromByteOffset:index*MemoryLayout<T>.size, as:T.self).from(endian:endian)
    }
    func index(after i: Int) -> Int {
        return i+1
    }

    struct Iterator: IteratorProtocol {
        var bytes: Slice<UnsafeRawBufferPointer>
        let endian: Endian
        init(_ bytes: Slice<UnsafeRawBufferPointer>, endian:Endian) {
            self.bytes = bytes
            self.endian = endian
        }

        mutating func next() -> T? {
            guard bytes.count >= MemoryLayout<T>.size else { return nil }
            let result = bytes.loadUnaligned(as:T.self).from(endian:endian)
            bytes = bytes.dropFirst(MemoryLayout<T>.size);
            return result
        }
    }
}
