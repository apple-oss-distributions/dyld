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

enum MachOError: Error {
    case truncatedRead
    case badMagic(UInt32)
}

// TODO: Convert everything in this file from UnsafeRawBufferPointer to Span
// Only implement what we need until then
//

enum MachO {
    case notMachO
    case thinMachO(MachO.File)
    case universalMachO(MachO.UniversalFile)
    init(_ buffer: UnsafeRawBufferPointer) throws(MachOError) {
        var slice = buffer[0..<buffer.count]
        guard let magic: UInt32 = try? slice.read(as: UInt32.self) else {
            throw .truncatedRead
        }
        switch magic {
        case 0xFEED_FACE, 0xFEED_FACF, 0xCEFA_EDFE, 0xCFFA_EDFE:
            self = .thinMachO(try MachO.File(buffer))
        case 0xCAFE_BABE, 0xCAFE_BABF, 0xBEBA_FECA, 0xBFBA_FECA:
            self = .universalMachO(try MachO.UniversalFile(buffer))
        default:
            self = .notMachO
        }
    }
}

extension MachO {
    struct File {
        fileprivate let buffer: UnsafeRawBufferPointer
        fileprivate let endian: Endian
        fileprivate let is64bit: Bool
        init(_ buffer: UnsafeRawBufferPointer) throws(MachOError) {
            var slice = buffer[0..<buffer.count]
            guard let magic: UInt32 = try? slice.read(as: UInt32.self) else {
                throw .truncatedRead
            }
            switch magic {
            case 0xFEED_FACE:
                endian = .native
                is64bit = false
            case 0xFEED_FACF:
                endian = .native
                is64bit = true
            case 0xCEFA_EDFE:
                endian = .reverse
                is64bit = false
            case 0xCFFA_EDFE:
                endian = .reverse
                is64bit = true
            default:
                throw .badMagic(magic)
            }
            self.buffer = buffer
        }
        var uuid: UUID? {
            for loadCommand  in loadCommands {
                switch loadCommand {
                case .uuid(let uuid):
                    return uuid
                default:
                    continue
                }
            }
            return nil
        }
        var loadCommands: LoadCommandSequence {
            return LoadCommandSequence(self)
        }
        fileprivate struct Header {
            let buffer: UnsafeRawBufferPointer
            let endian: Endian
            init(_ macho: MachO.File) {
                self.buffer = macho.buffer
                self.endian = macho.endian
            }
            var magic:      UInt32          { return buffer.load(fromByteOffset:0, as:UInt32.self).from(endian:endian) }
            var cputype:    cpu_type_t      { return buffer.load(fromByteOffset:4, as:Int32.self).from(endian:endian) }
            var cpusubtype: cpu_subtype_t   { return buffer.load(fromByteOffset:8, as:Int32.self).from(endian:endian) }
            var filetype:   UInt32          { return buffer.load(fromByteOffset:12, as:UInt32.self).from(endian:endian) }
            var ncmds:      UInt32          { return buffer.load(fromByteOffset:16, as:UInt32.self).from(endian:endian) }
            var sizeofcmds: UInt32          { return buffer.load(fromByteOffset:20, as:UInt32.self).from(endian:endian) }
            var flags:      UInt32          { return buffer.load(fromByteOffset:24, as:UInt32.self).from(endian:endian) }
        }
    }
}

fileprivate let LC_UUID: UInt32 = 0x0000001b

extension MachO {
    enum LoadCommand {
        case uuid(UUID)
        case unknown(UInt32)
    }

    struct LoadCommandSequence: Sequence {
        fileprivate var macho: MachO.File
        fileprivate init(_ macho: MachO.File) {
            self.macho = macho
        }
        func makeIterator() -> LoadCommandIterator {
            return LoadCommandIterator(macho)
        }
    }

    struct LoadCommandIterator: IteratorProtocol {
        fileprivate var slice: Slice<UnsafeRawBufferPointer>
        fileprivate let endian: Endian
        init(_ macho: MachO.File) {
            let header  = MachO.File.Header(macho)
            let lowerBound = macho.buffer.indices.lowerBound
            self.slice  = macho.buffer[lowerBound..<lowerBound+Int(header.sizeofcmds)]
            if macho.is64bit {
                self.slice =  self.slice.dropFirst(32)
            } else {
                self.slice =  self.slice.dropFirst(28)
            }
            self.endian = macho.endian
        }
        mutating func next() -> LoadCommand? {
            guard   let commandType = try? slice.read(endian:endian, as: UInt32.self),
                    let commandSize = try? slice.read(endian:endian, as: UInt32.self) else {
                return nil
            }
            switch commandType {
                case LC_UUID:
                guard let uuid = try? slice.readUuid() else { return nil }
                let remainingBytes = Int(commandSize) - 24   //  We already read the type, size, and uuid data
                slice = slice.dropFirst(remainingBytes)         // Skip to the next comand
                return .uuid(uuid)
            default:
                let remainingBytes = Int(commandSize) - 8    // We already read the type and size
                slice = slice.dropFirst(remainingBytes)         // Skip to the next comand
                return .unknown(commandType)
            }
        }
    }
}

extension MachO {
    struct UniversalFile {
        private let buffer: UnsafeRawBufferPointer
        private let is64bit: Bool
        init(_ buffer: UnsafeRawBufferPointer) throws(MachOError) {
            var slice = buffer[0..<buffer.count]
            guard let magic: UInt32 = try? slice.read(endian:.big, as: UInt32.self) else {
                throw .truncatedRead
            }
            switch magic {
            case 0xCAFE_BABE:
                is64bit = false
            case 0xCAFE_BABF:
                is64bit = true
            default:
                throw .badMagic(magic)
            }
            self.buffer = buffer
        }

        var slices: SliceSequence {
            return SliceSequence(self)
        }

        var sliceInfos: SliceInfoSequence {
            return SliceInfoSequence(self)
        }

        struct SliceInfo {
            private let slice: Slice<UnsafeRawBufferPointer>
            private let is64Bit: Bool
            init(slice: Slice<UnsafeRawBufferPointer>, is64Bit: Bool) {
                self.slice      = slice
                self.is64Bit    = is64Bit
            }
            // FIXME: Move these to enums later
            var cpuType:    cpu_type_t      { return slice.load(fromByteOffset:0, as:Int32.self).from(endian:.big) }
            var cpuSubType: cpu_subtype_t   { return slice.load(fromByteOffset:4, as:Int32.self).from(endian:.big) }
            var offset: Int {
                if is64Bit {
                    return Int(slice.load(fromByteOffset:8, as:UInt64.self).from(endian:.big))
                } else {
                    return Int(slice.load(fromByteOffset:8, as:UInt32.self).from(endian:.big))
                }
            }
            var size: Int {
                if is64Bit {
                    return Int(slice.load(fromByteOffset:16, as:UInt64.self).from(endian:.big))
                } else {
                    return Int(slice.load(fromByteOffset:12, as:UInt32.self).from(endian:.big))
                }
            }
        }

        struct SliceInfoSequence: Sequence {
            fileprivate let universalFile: UniversalFile
            fileprivate init(_ universalFile: UniversalFile) {
                self.universalFile = universalFile
            }
            func makeIterator() -> SliceInfoIterator {
                return SliceInfoIterator(universalFile)
            }
        }

        struct SliceInfoIterator: IteratorProtocol {
            private var slice: Slice<UnsafeRawBufferPointer>
            private var index: Int = 0
            private let header:  MachO.UniversalFile.Header
            private let is64Bit: Bool
            init(_ universalFile: UniversalFile) {
                self.header     = MachO.UniversalFile.Header(universalFile)
                self.slice      = universalFile.buffer[universalFile.buffer.indices]
                self.is64Bit    = universalFile.is64bit
            }
            mutating func next() -> SliceInfo? {
                guard index < header.nfat_arch else { return nil }
                var result: SliceInfo
                if is64Bit {
                    let lowerBound = slice.indices.lowerBound + 8 + (32 * index)  // Header + index * size of 63 bit fat record
                    result = SliceInfo(slice:slice[lowerBound..<slice.indices.upperBound], is64Bit:true)
                } else {
                    let lowerBound = slice.indices.lowerBound + 8 + (20 * index)  // Header + index * size of 63 bit fat record
                    result = SliceInfo(slice:slice[lowerBound..<slice.indices.upperBound], is64Bit:false)
                }
                index += 1
                return result
            }
        }

        // This is a bit confusing because we use `slice` as a term of art within MachO, and swift uses
        // slice to refer to range of collection. Once we have Span most of the uses of swift slices will
        // go away and this will be cleaerer
        struct SliceSequence: Sequence {
            fileprivate let universalFile: UniversalFile
            fileprivate init(_ universalFile: UniversalFile) {
                self.universalFile = universalFile
            }
            func makeIterator() -> SliceIterator {
                return SliceIterator(universalFile)
            }
        }

        struct SliceIterator: IteratorProtocol {
            private let slice: Slice<UnsafeRawBufferPointer>
            private var sliceInfoIterator: SliceInfoIterator
            init(_ universalFile: UniversalFile) {
                self.slice      = universalFile.buffer[universalFile.buffer.indices]
                self.sliceInfoIterator = SliceInfoIterator(universalFile)
            }
            mutating func next() -> MachO.File? {
                guard let sliceInfo = sliceInfoIterator.next() else { return nil }
                let lowerBound = slice.indices.lowerBound+sliceInfo.offset
                let sliceSlice = slice[lowerBound..<lowerBound+sliceInfo.size]
                return try? MachO.File(UnsafeRawBufferPointer(rebasing:sliceSlice))
            }
        }

        fileprivate struct Header {
            let buffer: UnsafeRawBufferPointer
            init(_ universalFile: UniversalFile) {
                self.buffer = universalFile.buffer
            }
            var magic: UInt32 {
                return buffer.load(as:UInt32.self).from(endian:.big)
            }
            var nfat_arch: UInt32 {
                return buffer.load(fromByteOffset:4, as:UInt32.self).from(endian:.big)
            }
        }
        
    }
}
