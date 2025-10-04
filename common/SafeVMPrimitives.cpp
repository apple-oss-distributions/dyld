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

#if !TARGET_OS_EXCLAVEKIT
#include <span>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dispatch/dispatch.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <os/security_config_private.h>

#include "SafeVMPrimitives.h"



kern_return_t vm_read_safe(vm_map_read_t target_task,
                   mach_vm_address_t address,
                   mach_vm_size_t size,
                   vm_offset_t *data,
                   mach_msg_type_number_t *dataCnt) {
    vm_offset_t bounceBuffer;
    // Mask out TBI bits
    mach_vm_address_t mask = 0x00ff'ffff'ffff'ffffUL;
    kern_return_t kr = mach_vm_read(target_task, mask & address, size, &bounceBuffer, dataCnt);
    if (kr != KERN_SUCCESS) {
        return kr;
    }
    // Allocate a copy buffer we can move bytes into in case the VM decides to share pages behind our backs
    kr = vm_allocate(mach_task_self(), (vm_address_t *)data, *dataCnt, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) {
        (void)vm_deallocate(mach_task_self(), (vm_address_t)bounceBuffer, *dataCnt);
        return kr;
    }
    // Copy the memory with hooks for audit builds
    remote_memory_audit_start();
    memcpy((void*)(*data), (const void*)bounceBuffer,* dataCnt);
    remote_memory_audit_end();
    (void)vm_deallocate(mach_task_self(), (vm_address_t)bounceBuffer, *dataCnt);
    return kr;
}

#if __cplusplus
SafeRemoteBuffer::SafeRemoteBuffer(vm_map_read_t target_task, mach_vm_address_t address, mach_vm_size_t size, kern_return_t* kr) {
    *kr = vm_read_safe(target_task, address, size, &_buffer, &_bufferSize);
    if (*kr != KERN_SUCCESS) {
        _buffer = 0;
        _bufferSize = 0;
    }
}
SafeRemoteBuffer::~SafeRemoteBuffer() {
    if (_buffer == 0) { return; }
    if (_bufferSize == 0) { return; }
    (void)vm_deallocate(mach_task_self(), _buffer, _bufferSize);
}

std::span<std::byte> SafeRemoteBuffer::data() {
    return std::span<std::byte>((std::byte*)_buffer, _bufferSize);
}
#endif // __cplusplus
#endif // !TARGET_OS_EXCLAVEKIT
