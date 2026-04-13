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
    // Allocate a copy buffer we can move bytes into in case the VM decides to share pages behind our backs
    *data = (vm_offset_t)malloc((size_t)size);
    if (*data == 0) {
        return KERN_MEMORY_FAILURE;
    }

    // Mask out TBI bits
    // So it turns out `mach_vm_read` semantics are a bit of nightmare here:
    // 1. If you are an MTE process reading a different MTE process tag checking will be disabled, and you
    //    you can pass in either the canonical or the non-canonical pointer
    // 2. If you are an MTE process reading your own memory then tag checking is enforced by design to avoid vm_read
    //    being used a TCO like bypass, so the actual pointer must be used
    // 3. If you are a non-MTE process and pass in a pointer with the TBI bits set you will get an error even if
    //    you are reading a remote process that is MTE enabled, so you need to canonicalize the pointer.
    //
    // Reading the above canonical pointers work in all cases but 2, which is explicitly the mach_task_self() case
    // so we should canonicalize all pointers unless we are reading mach_task_self(), in which case we should bypass
    // the syscall and just do a memcpy.
    //
    // Since
    // 1. For the mach_task_self() case it needs to have the tag correct to handle the library code reads
    // 2. Pass the canonicalized pointer if we are a non-MTE process
    //   a. For non-MTE processes everything will always be zero
    //   b. For MTE processes the kernel will strip the tags when the pages are mapped in
    if (target_task == mach_task_self()) {
        memcpy((void*)*data, (const void*)address, (size_t)size);
        *dataCnt = (mach_msg_type_number_t)size;
        return KERN_SUCCESS;
    }
    const mach_vm_address_t mask = 0x00ff'ffff'ffff'ffffUL;

    vm_offset_t bounceBuffer = 0;
    kern_return_t kr = mach_vm_read(target_task, mask & address, size, &bounceBuffer, dataCnt);
    if (kr != KERN_SUCCESS) {
        free((void*)*data);
        *data = (vm_offset_t)NULL;
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
    free((void*)_buffer);
}

std::span<std::byte> SafeRemoteBuffer::data() {
    return std::span<std::byte>((std::byte*)_buffer, _bufferSize);
}
#endif // __cplusplus
#endif // !TARGET_OS_EXCLAVEKIT
