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

// Due to the complexity of the VM interfaces and fragility around certain
// security features dyld needs wrappers to use them. These interfaces
// exist to provide a funnel where we can handloe those issues as well
// as providing as a place to insert audting hooks.

#if !TARGET_OS_EXCLAVEKIT
#include "Defines.h"

#if __cplusplus
extern "C"
#endif
// This is the safe read primitive. Memory returned for it must be vm_deallocated
VIS_HIDDEN kern_return_t vm_read_safe(vm_map_read_t target_task,
                                      mach_vm_address_t address,
                                      mach_vm_size_t size,
                                      vm_offset_t *data,
                                      mach_msg_type_number_t *dataCnt);

#if __cplusplus
// Wrapper to handle memory ownership and deallocation
struct VIS_HIDDEN SafeRemoteBuffer {
    SafeRemoteBuffer() = delete;
    SafeRemoteBuffer(const SafeRemoteBuffer&) = delete;
    SafeRemoteBuffer(SafeRemoteBuffer&&) = delete;
    SafeRemoteBuffer& operator=(const SafeRemoteBuffer&) = delete;
    SafeRemoteBuffer& operator=(SafeRemoteBuffer&&) = delete;
    SafeRemoteBuffer(vm_map_read_t target_task, mach_vm_address_t address, mach_vm_size_t size, kern_return_t* kr);
    ~SafeRemoteBuffer();
    std::span<std::byte> data();
private:
    vm_offset_t             _buffer         = 0;
    mach_msg_type_number_t  _bufferSize     = 0;
};
#endif // __cplusplus

#endif // !TARGET_OS_EXCLAVEKIT
