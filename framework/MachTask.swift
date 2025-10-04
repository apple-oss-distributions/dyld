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

@_implementationOnly import Dyld_Internal

// We implement this a ~Copyable even though it ref counted because the ref count are syscalls, and thus very expensive. When we do need to copy
// we will do it explicitly via a borrowing constructor.
struct MachTask: ~Copyable {
    let port: task_read_t

    init(_ task: task_read_t) throws {
        let kr = mach_port_mod_refs(mach_task_self_, task, MACH_PORT_RIGHT_SEND, 1)
        guard kr == KERN_SUCCESS else {
            throw AtlasError.machError(kr)
        }
        self.port = task
    }

    // Explicit "Copy constructor"
    init(_ task: borrowing MachTask) throws {
        let kr = mach_port_mod_refs(mach_task_self_, task.port, MACH_PORT_RIGHT_SEND, 1)
        guard kr == KERN_SUCCESS else {
            throw AtlasError.machError(kr)
        }
        self.port = task.port
    }

    deinit {
        // Use this instead of mach_port_mod_refs() to handle DEADNAMES, etc
        mach_port_deallocate(mach_task_self_, self.port)
    }
}

extension MachTask {
    func dyldInfo() throws(AtlasError) -> task_dyld_info_data_t {
        var result              = task_dyld_info_data_t();
        var dyldTaskInfoCount   = Int32(5 /* TASK_DYLD_INFO_COUNT */)
        let kr = withUnsafeMutablePointer(to: &result) {
            return $0.withMemoryRebound(to:integer_t.self, capacity: 5 /* TASK_DYLD_INFO_COUNT */ ) {
                return task_info(self.port, task_flavor_t(TASK_DYLD_INFO), $0, &dyldTaskInfoCount);
            }
        }
        guard kr == KERN_SUCCESS else {
            throw AtlasError.machError(kr)
        }
        return result
    }
    func readStruct<T>(address: RemoteAddress) throws(AtlasError) -> T {
        var vmSize = mach_msg_type_number_t(0)
        var bufferPtrScalar: vm_offset_t = 0
        let kr = vm_read_safe(self.port, address.value, mach_vm_size_t(MemoryLayout<T>.size), &bufferPtrScalar, &vmSize)
        guard kr == KERN_SUCCESS else {
            throw AtlasError.machError(kr)
        }
        defer {
            vm_deallocate(mach_task_self_, bufferPtrScalar, vm_size_t(vmSize))
        }
        guard vmSize >= MemoryLayout<T>.size else {
            throw AtlasError.placeHolder
        }
        let bufferPtr = UnsafeMutableRawPointer(bitPattern: Int(bitPattern: UInt(bufferPtrScalar)))
        let buffer = UnsafeMutableRawBufferPointer(start:bufferPtr, count:Int(MemoryLayout<T>.size))
        return buffer.load(as:T.self)
    }
    func readData(address: RemoteAddress, size: UInt64) throws(AtlasError) -> Data {
        var vmSize = mach_msg_type_number_t(0)
        var bufferPtrScalar: vm_offset_t = 0
        let kr = vm_read_safe(self.port, address.value, size, &bufferPtrScalar, &vmSize)
        guard kr == KERN_SUCCESS else {
            throw AtlasError.machError(kr)
        }
        let bufferPtr = UnsafeMutableRawPointer(bitPattern: Int(bitPattern: UInt(bufferPtrScalar)))

        return Data(bytesNoCopy:bufferPtr!, count:Int(vmSize), deallocator:.virtualMemory)
    }
}
