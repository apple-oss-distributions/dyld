/*
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

// implementationOnly so we don't leak the c types into the public interface
@_implementationOnly import dyld_cache_module
//@_implementationOnly import MachO_Private.dyld_cache_format

enum SharedCacheRuntimeError: Error {
    /// ran out of buffer while parsing
    case outOfBuffer(limit: Int, required: Int)
}

// Swift API to be used exclusively by the ExclaveKit loader
public struct MappingInfo {
    public let address     : UInt64
    public let size        : UInt64
    public let fileOffset  : UInt64
    public let slideOffset : UInt64
    public let slideSize   : UInt64
    public let flags       : UInt64
    public let maxProt     : UInt32
    public let initProt    : UInt32
}

public struct CodeSignatureInfo {
    public let offset : UInt64
    public let size   : UInt64
}

private func loadFromRawBuffer<T>(cacheBuffer: UnsafeRawBufferPointer, offset: Int, end: Int) throws -> T {
    guard cacheBuffer.count > end else {
        throw SharedCacheRuntimeError.outOfBuffer(
            limit: cacheBuffer.count,
            required: end
        )
    }
    return cacheBuffer.load(fromByteOffset: offset,
                                  as: T.self)
}
public func hasValidMagic(cacheBuffer: UnsafeRawBufferPointer) throws -> Bool {
    let headerSize = MemoryLayout<dyld_cache_header>.stride
    let header : dyld_cache_header = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                       offset: 0,
                                                       end: headerSize)
    let isValidMagic = withUnsafeBytes(of: header.magic) {
        buffer in
        let magicString = String(decoding: [UInt8](buffer), as: UTF8.self)
        return magicString == "dyld_v1  arm64e\0"
    }
    return isValidMagic
}

public func getPlatform(cacheBuffer: UnsafeRawBufferPointer) throws -> UInt32 {
    let headerSize = MemoryLayout<dyld_cache_header>.stride
    let header : dyld_cache_header = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                       offset: 0,
                                                       end: headerSize)
    return header.platform
 }

public func getUUID(cacheBuffer: UnsafeRawBufferPointer) throws -> [UInt8] {
    let headerSize = MemoryLayout<dyld_cache_header>.stride
    let header : dyld_cache_header = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                       offset: 0,
                                                       end: headerSize)
    return withUnsafeBytes(of: header.uuid) { buf in
            [UInt8](buf)
    }
 }


public func getMappingsInfo(cacheBuffer: UnsafeRawBufferPointer) throws -> [MappingInfo] {
    let headerSize = MemoryLayout<dyld_cache_header>.stride
    let header : dyld_cache_header = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                       offset: 0,
                                                       end: headerSize)

    let mappings = try (0..<Int(header.mappingWithSlideCount)).map { index in
        let structSize = MemoryLayout<dyld_cache_mapping_and_slide_info>.stride
        let offset = Int(header.mappingWithSlideOffset) + index * structSize
        let mappingEnd = offset + structSize
        let m : dyld_cache_mapping_and_slide_info = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                                          offset: offset,
                                                                          end: mappingEnd)
        return MappingInfo(address: m.address, size: m.size, fileOffset: m.fileOffset,
                           slideOffset: m.slideInfoFileOffset, slideSize: m.slideInfoFileSize,
                           flags: m.flags, maxProt: m.maxProt, initProt: m.initProt)
    }
    return mappings
}

public func getCodeSignatureInfo(cacheBuffer: UnsafeRawBufferPointer)throws -> CodeSignatureInfo {
    let headerSize = MemoryLayout<dyld_cache_header>.stride
    let header : dyld_cache_header = try loadFromRawBuffer(cacheBuffer: cacheBuffer,
                                                       offset: 0,
                                                       end: headerSize)
    return CodeSignatureInfo(offset: header.codeSignatureOffset, size: header.codeSignatureSize)
}
