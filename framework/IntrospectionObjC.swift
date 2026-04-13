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

import System
import Foundation
@_implementationOnly import Dyld_Internal


@objc @implementation
extension _DYSegment {
    @objc var name:                 String  { return impl.name }
    @objc var permissions:          UInt64  { return impl.permissions }
    @objc var vmsize:               UInt64  { return impl.vmSize }
    @objc var address:              UInt64  { return impl.address.value }
    @objc var preferredLoadAddress: UInt64  { return impl.preferredLoadAddress.value }
    @inline(__always)
    @objc var data:                 dispatch_data_t?   {
        guard let memoryBuffer = try? impl.memoryBuffer else { return nil }
        return memoryBuffer.data() as dispatch_data_t
    }

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience fileprivate init(_ impl: Segment) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: Segment!
}

@objc @implementation
extension _DYImage {
    @objc var installname:          String?         { return impl.installname }
    @objc var filePath:             String?         { return impl.filePath }
    @objc var uuid:                 UUID?           { return impl.uuid }
    @objc var address:              UInt64          { return impl.address.value }
    @objc var pointerSize:          UInt64          { return impl.pointerSize }
    @objc var preferredLoadAddress: UInt64          { return impl.preferredLoadAddress.value }
    @objc var sharedCache:          _DYSharedCache? {
        guard let sharedCache = impl.sharedCache else {
            return nil
        }
        return _DYSharedCache(sharedCache)
    }

    @objc lazy var segments: [_DYSegment] = {
        return impl.segments.map { _DYSegment($0) }
    }()

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience fileprivate init(_ impl: Image) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: Image!
}

@objc @implementation
extension _DYEnvironment {
    @objc var rootPath:   String?  { return impl.rootPath }
    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience fileprivate init?(_ impl: Environment?) {
        guard let impl else { return nil }
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: Environment!
}

@objc @implementation
extension _DYAOTImage {
    @objc var x86Address:   UInt64                  { return impl.x86Address.value }
    @objc var aotAddress:   UInt64                  { return impl.aotAddress.value }
    @objc var aotSize:      UInt64                  { return impl.aotSize }
    @objc var aotImageKey:  Data                    {
        impl.aotImageKey.span.withUnsafeBytes {
            return Data($0)
        }
    }

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience fileprivate init(_ impl: AOTImage) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: AOTImage!
}

@objc @implementation
extension _DYSubCache {
    func withVMLayoutBytes(_ block:((UnsafeRawPointer, size_t) -> Void)) -> Bool {
        do throws {
            let bytes = unsafe _overrideLifetime(try impl.bytes, copying:self)
            return bytes.withUnsafeBytes {
                block($0.baseAddress!, $0.count)
                return true
            }
        } catch {
            switch error as! MemoryMapError {
            case .zeroLengthSegment(let start):
                block(start, 0)
                return true
            case .inaccessibleFile, .outOfRange:     return false
            }
        }
    }
    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience fileprivate init(_ impl: SubCache) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: SubCache!
}

@objc @implementation
extension _DYSharedCache {
    @objc var uuid:                 UUID        { return impl.uuid }
    @objc var address:              UInt64      { return impl.address.value }
    @objc var vmsize:               UInt64      { return impl.vmSize }
    @objc var preferredLoadAddress: UInt64      { return impl.preferredLoadAddress.value }
    @objc var mappedPrivate:        Bool        { return impl.mappedPrivate }
    @objc var filePaths:            [String]    { return impl.filePaths }
    @objc var aotUuid:              UUID?       { return impl.aotUuid }
    @objc var aotAddress:           UInt64      { return impl.aotAddress?.value ?? 0 }
    @objc var localSymbolPath:      String?     { return impl.localSymbolPath }
    func withLocalSymbolData(_ block:((UnsafeRawPointer, size_t) -> Void)) -> Bool {
        do {
            return try impl.withLocalSymbolFileBytes {
                guard let baseAddress = $0.baseAddress else { return false }
                block(baseAddress, $0.count)
                return true
            }
        } catch {
            return false
        }
    }

    @objc lazy var images: [_DYImage] = {
        return impl.images.map { _DYImage($0) }
    }()
    @objc func pinMappings() -> Bool {
        return impl.pinMappings()
    }
    @objc func unpinMappings() {
        impl.unpinMappings()
    }
    @objc lazy var subCaches: [_DYSubCache] = {
        return impl.subCaches.map { _DYSubCache($0) }
    }()
    @objc class func systemSharedCaches() -> [_DYSharedCache] {
        return SharedCache.systemSharedCaches().map { _DYSharedCache($0) }
    }
    @objc(systemSharedCachesForSystemPath:) class func systemSharedCaches(path: String) -> [_DYSharedCache] {
        return SharedCache.systemSharedCaches(path:FilePath(path)).map { _DYSharedCache($0) }
    }
    @objc(installedSharedCaches) class func installedSharedCaches() -> [_DYSharedCache] {
        return SharedCache.installedSharedCaches().map { _DYSharedCache($0) }
    }

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @objc convenience init(path:String) throws {
        self.init(internal:true)
        impl = try SharedCache(path:FilePath(path))
    }
    @nonobjc convenience fileprivate init(_ impl: SharedCache) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: SharedCache!
}

@objc @implementation
extension _DYSnapshot {
    @objc var pid:                  pid_t               { return impl.pid }
    @objc var pageSize:             size_t              { return impl.pageSize }
    @objc var images:               [_DYImage]!         { return impl.images.map { _DYImage($0) } }
    @objc var aotImages:            [_DYAOTImage]?      { return impl.aotImages?.map { _DYAOTImage($0) } }
    @objc var timestamp:            UInt64              { return impl.timestamp }
    @objc var initialImageCount:    UInt64              { return impl.initialImageCount }
    @objc var state:                UInt8               { return impl.state }
    @objc var platform:             UInt64              { return impl.platform }
    @objc var environment:          _DYEnvironment?     { return _DYEnvironment(impl.environment) }
    @objc lazy var metrics:         [String:NSNumber]?  = {
        return impl.metrics?.reduce(into: [String:NSNumber]()) { result, element in
            result[element.0] = NSNumber(value: element.1)
        }
    }()

    @objc lazy var sharedCache: _DYSharedCache? = {
        guard let sharedCache = impl.sharedCache else {
            return nil
        }
        return _DYSharedCache(sharedCache)
    }()

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @objc convenience init(data: Data) throws {
        self.init(internal:true)
        self.impl = try Snapshot(data:data)
    }
    @nonobjc convenience fileprivate init(_ impl: Snapshot) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: Snapshot!
}


//MARK: -
//MARK: Process

@objc @implementation
extension _DYEventHandlerToken {
    @nonobjc private var _value: UInt32!
    @objc var value: UInt32 {
        return _value
    }
    @objc private override init() {
        self._value = nil
        super.init()
    }
    @objc public init(value: UInt32) {
        self._value = value
    }
}

@objc @implementation
extension _DYProcess {
    @objc var queue: DispatchQueue? {
        get {
            return impl.queue
        }
        set {
            impl.queue = newValue
        }
    }

    @objc(initWithTask:queue:error:) init(task: task_read_t, queue: DispatchQueue? = nil) throws {
        self.impl = try Process(task:task, queue:queue)
    }

    @objc(processForCurrentTask) class func forCurrentTask() -> Self {
        let process = Process.forCurrentTask()
        return _DYProcess(process) as! Self
    }

    @objc(registerChangeNotificationsWithError:handler:)
    func registerChangeNotifications(handler: @escaping (_ image: _DYImage, _ load: Bool) -> Void) throws -> _DYEventHandlerToken {
        let tokenValue = try impl.registerForChangeNotifications() {
            handler(_DYImage($0), $1)
        }
        return _DYEventHandlerToken(value:tokenValue)
    }

    @objc(registerForEvent:error:handler:)
    func register(event:UInt32, handler: @escaping () -> Void) throws -> _DYEventHandlerToken {
        let tokenValue = try impl.register(event:event, handler)
        return _DYEventHandlerToken(value:tokenValue)
    }

    @objc(unregisterForEvent:) func unregister(event: _DYEventHandlerToken) {
        impl.unregister(handle:event.value);
    }
        
    @objc(getCurrentSnapshotAndReturnError:) func getCurrentSnapshot() throws -> _DYSnapshot {
        return _DYSnapshot(try impl.getCurrentSnapshot())
    }

    @objc private override init() {
        fatalError("init() not supported")
    }
    @objc private init(internal: Bool) {
        super.init()
    }
    @nonobjc convenience private init(_ impl: Process) {
        self.init(internal:true)
        self.impl = impl
    }
    @nonobjc private var impl: Process!
}

extension Data {
    init(bytes: RawSpan) {
        let newBytes = malloc(bytes.byteCount)
        let newBuffer = UnsafeMutableRawBufferPointer(start:newBytes, count: bytes.byteCount)
        bytes.withUnsafeBytes { src in
            newBuffer.copyMemory(from:src)
        }
        self.init(bytesNoCopy:newBuffer.baseAddress!, count:newBuffer.count, deallocator:.free)
    }
}

