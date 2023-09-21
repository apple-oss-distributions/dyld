/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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

#include <TargetConditionals.h>
#include "Defines.h"

#if !TARGET_OS_EXCLAVEKIT
    #include <_simple.h>
    #include <libc_private.h>
    #include <pthread/pthread.h>
    #include <pthread/tsd_private.h>
    #include <sys/errno.h>
    #include <sys/mman.h>
    #include <sys/stat.h>

// atexit header is missing C++ guards
extern "C" {
    #include <System/atexit.h>
}
#endif

#if !TARGET_OS_DRIVERKIT && !TARGET_OS_EXCLAVEKIT
  #include <vproc_priv.h>
#endif

#include <string.h>
#include <stdint.h>
#include <mach-o/dyld_priv.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <dlfcn.h>
#include <dlfcn_private.h>
#include <ptrauth.h>

// libc is missing header declaration for this
extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* dso);

#include "LibSystemHelpers.h"
#include "DyldProcessConfig.h"
#include "DyldAPIs.h"

// implemented in assembly
extern "C" void* tlv_get_addr(dyld3::MachOAnalyzer::TLV_Thunk*);

uint8_t dyld_process_has_objc_patches = 0;

namespace dyld4 {

uintptr_t LibSystemHelpers::version() const
{
    return 6;
}

void* LibSystemHelpers::malloc(size_t size) const
{
    return ::malloc(size);
}

void LibSystemHelpers::free(void* p) const
{
    ::free(p);
}

size_t LibSystemHelpers::malloc_size(const void* p) const
{
    return ::malloc_size(p);
}

kern_return_t LibSystemHelpers::vm_allocate(vm_map_t task, vm_address_t* address, vm_size_t size, int flags) const
{
#if !TARGET_OS_EXCLAVEKIT
    return ::vm_allocate(task, address, size, flags);
#else
    //TODO: EXCLAVES. Then replace calls to MemoryManager::allocate_pages
    return 0;
#endif
}

kern_return_t LibSystemHelpers::vm_deallocate(vm_map_t task, vm_address_t address, vm_size_t size) const
{
#if !TARGET_OS_EXCLAVEKIT
    return ::vm_deallocate(task, address, size);
#else
    return 0;
#endif
}

// Note: driverkit uses a different arm64e ABI, so we cannot call libSystem's pthread_key_create() from dyld
int LibSystemHelpers::pthread_key_create_free(dyld_thread_key_t* key) const
{
#if TARGET_OS_EXCLAVEKIT
    return ::tss_create(key, &::free);
#else
    return ::pthread_key_create(key, &::free);
#endif
}

int LibSystemHelpers::pthread_key_init_free(int key) const
{
#if TARGET_OS_EXCLAVEKIT
    return 0; // ::tss_create already sets up the destructor;
#else
    return ::pthread_key_init_np(key, &::free);
#endif
}

void LibSystemHelpers::run_async(void* (*func)(void*), void* context) const
{
#if TARGET_OS_EXCLAVEKIT
    thrd_t workerThread;
    ::thrd_create(&workerThread, (thrd_start_t)func, context);
    ::thrd_detach(workerThread);
#else
    pthread_t workerThread;
    ::pthread_create(&workerThread, NULL, func, context);
    ::pthread_detach(workerThread);
#endif
}

static void finalizeListTLV_thunk(void* list)
{
    // Called by pthreads when the current thread is going away
    gDyld.apis->_finalizeListTLV(list);
}

// Note: driverkit uses a different arm64e ABI, so we cannot call libSystem's pthread_key_create() from dyld
int LibSystemHelpers::pthread_key_create_thread_exit(dyld_thread_key_t* key) const
{
#if TARGET_OS_EXCLAVEKIT
    return ::tss_create(key, &finalizeListTLV_thunk);
#else
    return ::pthread_key_create(key, &finalizeListTLV_thunk);
#endif
}

void* LibSystemHelpers::pthread_getspecific(dyld_thread_key_t key) const
{
#if TARGET_OS_EXCLAVEKIT
    return ::tss_get(key);
#else
    return ::pthread_getspecific(key);
#endif
}

int LibSystemHelpers::pthread_setspecific(dyld_thread_key_t key, const void* value) const
{
#if TARGET_OS_EXCLAVEKIT
    return ::tss_set(key, (void*)value);
#else
    return ::pthread_setspecific(key, value);
#endif
}

void LibSystemHelpers::__cxa_atexit(void (*func)(void*), void* arg, void* dso) const
{
#if !__arm64e__
    // Note: for arm64e driverKit uses a different ABI for function pointers,
    // but dyld does not support static terminators for arm64e
    ::__cxa_atexit(func, arg, dso);
#endif
}

void LibSystemHelpers::__cxa_finalize_ranges(const __cxa_range_t ranges[], unsigned int count) const
{
#if TARGET_OS_EXCLAVEKIT
    //TODO: EXCLAVES
#else
    ::__cxa_finalize_ranges(ranges, count);
#endif
}

bool LibSystemHelpers::isLaunchdOwned() const
{
#if TARGET_OS_DRIVERKIT || TARGET_OS_EXCLAVEKIT
    return false;
#else
    // the vproc_swap_integer() call has to be to libSystem.dylib's function - not a static copy in dyld
    int64_t val = 0;
    ::vproc_swap_integer(nullptr, VPROC_GSK_IS_MANAGED, nullptr, &val);
	return ( val != 0 );
#endif
}

void LibSystemHelpers::os_unfair_recursive_lock_lock_with_options(dyld_recursive_mutex_t lock, os_unfair_lock_options_t options) const
{
#if TARGET_OS_EXCLAVEKIT
    mtx_lock(lock);
#else
    ::os_unfair_recursive_lock_lock_with_options(lock, options);
#endif
}

void LibSystemHelpers::os_unfair_recursive_lock_unlock(dyld_recursive_mutex_t lock) const
{
#if TARGET_OS_EXCLAVEKIT
    mtx_unlock(lock);
#else
    ::os_unfair_recursive_lock_unlock(lock);
#endif
}

void LibSystemHelpers::exit(int result) const
{
    ::exit(result);
}

const char* LibSystemHelpers::getenv(const char* key) const
{
#if TARGET_OS_EXCLAVEKIT
    return NULL;
#else
    return ::getenv(key);
#endif
}

int LibSystemHelpers::mkstemp(char* templatePath) const
{
#if TARGET_OS_EXCLAVEKIT
    return -1;
#else
    return ::mkstemp(templatePath);
#endif
}

LibSystemHelpers::TLVGetAddrFunc LibSystemHelpers::getTLVGetAddrFunc() const
{
    return &::tlv_get_addr;
}

// Added in version 2

void LibSystemHelpers::os_unfair_recursive_lock_unlock_forked_child(dyld_recursive_mutex_t lock) const
{
#if !TARGET_OS_EXCLAVEKIT
    ::os_unfair_recursive_lock_unlock_forked_child(lock);
#endif
}

// Added in version 3

void LibSystemHelpers::setDyldPatchedObjCClasses() const
{
    dyld_process_has_objc_patches = 1;
}

// Added in version 6
void LibSystemHelpers::os_unfair_lock_lock_with_options(dyld_mutex_t lock, os_unfair_lock_options_t options) const
{
#if TARGET_OS_EXCLAVEKIT
    //TODO: EXCLAVES?
#else
    ::os_unfair_lock_lock_with_options(lock, options);
#endif
}
void LibSystemHelpers::os_unfair_lock_unlock(dyld_mutex_t lock) const
{
#if TARGET_OS_EXCLAVEKIT
    //TODO: EXCLAVES?
#else
    ::os_unfair_lock_unlock(lock);
#endif
}

} // namespace dyld4

