/*
 * Copyright (c) 2019 Apple Inc. All rights reserved.
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

#ifndef _libSystem_h
#define _libSystem_h

#include <TargetConditionals.h>
#include "Defines.h"

//FIXME: Hack to avoid <sys/commpage.h> being included by <System/machine/cpu_capabilities.h>

#if TARGET_OS_EXCLAVEKIT
  #include <threads.h>
  typedef tss_t                       dyld_thread_key_t;
  typedef mtx_t                       dyld_mutex;
  typedef mtx_t*                      dyld_mutex_t;
  typedef mtx_t                       dyld_recursive_mutex;
  typedef mtx_t*                      dyld_recursive_mutex_t;
  inline int   dyld_thread_key_create(dyld_thread_key_t* key, void (*destructor)(void*)) { return ::tss_create(key, destructor); }
  inline int   dyld_thread_key_init_np(dyld_thread_key_t key, void (*destructor)(void*)) { return 0; }
  inline int   dyld_thread_setspecific(dyld_thread_key_t key, const void* value) { return ::tss_set(key, (void*)value); }
  inline void* dyld_thread_getspecific(dyld_thread_key_t key) { return ::tss_get(key); }
  typedef unsigned int                vm_map_t;
  typedef uintptr_t                   vm_address_t;
  typedef uintptr_t                   vm_size_t;
  typedef uintptr_t                   vm_offset_t;
  typedef int                         vm_prot_t;
  typedef uint64_t                    user_addr_t;
  typedef int                         kern_return_t;
  typedef struct os_unfair_lock_options_s {
    uint32_t foo;
  } *os_unfair_lock_options_t;
  #define OS_UNFAIR_LOCK_NONE 0
#else
  #include <pthread.h>
  #include <pthread/tsd_private.h>
  #include <unistd.h>
  #include <malloc/malloc.h>
  #include <os/lock_private.h>
  #include <mach/mach.h>
  #include <mach/vm_types.h>
  typedef pthread_key_t                dyld_thread_key_t;
  typedef os_unfair_recursive_lock     dyld_recursive_mutex;
  typedef os_unfair_recursive_lock_t   dyld_recursive_mutex_t;
  typedef os_unfair_lock               dyld_mutex;
  typedef os_unfair_lock_t             dyld_mutex_t;
  inline int   dyld_thread_key_create(dyld_thread_key_t* key, void (*destructor)(void*)) { return ::pthread_key_create(key, destructor); }
  inline int   dyld_thread_key_init_np(dyld_thread_key_t key, void (*destructor)(void*)) { return ::pthread_key_init_np((int)key, destructor); }
  inline int   dyld_thread_setspecific(dyld_thread_key_t key, const void* value) { return ::pthread_setspecific(key, value); }
  inline void* dyld_thread_getspecific(dyld_thread_key_t key) { return ::pthread_getspecific(key); }
#endif // !TARGET_OS_EXCLAVEKIT

#include "DyldSharedCache.h"

// mach_o
#include "Error.h"
#include "Header.h"

using mach_o::Header;


namespace dyld4 {
struct ProgramVars;
typedef bool (*FuncLookup)(const char* name, void** addr);

//
// Helper for performing "up calls" from dyld into libSystem.dylib.
//
// Note: driverkit and base OS use the same dyld, but different libdyld.dylibs.  We use the clang attribute
// to ensure both libdyld implementations use the same vtable pointer authentication. Similarly, we cannot use
// the generic pthread_key_create() because it takes a clean function pointer parameter and the authentication
// for that may differ in the two libdyld.dylibs.
//
struct VIS_HIDDEN  [[clang::ptrauth_vtable_pointer(process_independent, address_discrimination, type_discrimination)]] LibSystemHelpers
{
    typedef void  (*ThreadExitFunc)(void* storage);

    virtual uintptr_t       version() const;
    virtual void*           malloc(size_t size) const;
    virtual void            free(void* p) const;
    virtual size_t          malloc_size(const void* p) const;
    virtual kern_return_t   vm_allocate(vm_map_t target_task, vm_address_t* address, vm_size_t size, int flags) const;
    virtual kern_return_t   vm_deallocate(vm_map_t target_task, vm_address_t address, vm_size_t size) const;
    virtual int             pthread_key_create_free(dyld_thread_key_t* key) const;
    virtual void*           pthread_getspecific(dyld_thread_key_t key) const;
    virtual int             pthread_setspecific(dyld_thread_key_t key, const void* value) const;
    virtual void            __cxa_atexit(void (*func)(void*), void* arg, void* dso) const;
    virtual void            __cxa_finalize_ranges(const struct __cxa_range_t ranges[], unsigned int count) const;
    virtual bool            isLaunchdOwned() const;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
    virtual void            os_unfair_recursive_lock_lock_with_options(dyld_recursive_mutex_t lock, os_unfair_lock_options_t options) const;
    virtual void            os_unfair_recursive_lock_unlock(dyld_recursive_mutex_t lock) const;
#pragma clang diagnostic pop
    virtual void            exit(int result) const  __attribute__((__noreturn__));
    virtual const char*     getenv(const char* key) const;
    virtual int             mkstemp(char* templatePath) const;

    // Added in version 2
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
    virtual void            os_unfair_recursive_lock_unlock_forked_child(dyld_recursive_mutex_t lock) const;
#pragma clang diagnostic pop

    // Added in version 3
    virtual void            setDyldPatchedObjCClasses() const;

    // Added in version 5
    virtual void            run_async(void* (*func)(void*), void* context) const;

    // Added in version 6
    virtual void            os_unfair_lock_lock_with_options(dyld_mutex_t lock, os_unfair_lock_options_t options) const;
    virtual void            os_unfair_lock_unlock(dyld_mutex_t lock) const;

    // Added in version 7
    virtual void            setDefaultProgramVars(ProgramVars& vars) const;
    virtual FuncLookup      legacyDyldFuncLookup() const;  // only works on x86_64 macOS
    virtual mach_o::Error   setUpThreadLocals(const DyldSharedCache* cache, const Header* hdr) const;
};

// __DATA_CONST,__helper section in libdyld.dylib
struct LibdyldHelperSection {
    const LibSystemHelpers helper;
};

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

// Wrapper for LibSystemHelpers to force it to take a read only context before calling methods
struct LibSystemHelpersWrapper
{
    LibSystemHelpersWrapper() = default;
    LibSystemHelpersWrapper(const LibSystemHelpersWrapper&) = default;
    LibSystemHelpersWrapper(LibSystemHelpersWrapper&&) = default;
    LibSystemHelpersWrapper& operator=(const LibSystemHelpersWrapper&) = default;
    LibSystemHelpersWrapper& operator=(LibSystemHelpersWrapper&&) = default;

    LibSystemHelpersWrapper(std::nullptr_t) : helpers(nullptr) { }

    LibSystemHelpersWrapper(const LibSystemHelpers* helpers, lsl::MemoryManager* memoryManager)
        : helpers(helpers), memoryManager(memoryManager) { }

    bool operator!=(std::nullptr_t) const { return helpers != nullptr; }
    bool operator==(std::nullptr_t) const { return helpers == nullptr; }

    // Provide access to some methods directly, without the use of read only memory
    // These are either safe by inspection/convention, or required due to current behaviour (see the locks below)
    uintptr_t version() const {
        return helpers->version();
    }

    void setDefaultProgramVars(ProgramVars& vars) const
    {
        // This is writing in to TPRO_CONST in dyld, so needs to stay mutable
        this->helpers->setDefaultProgramVars(vars);
    }

    // These methods can't be called from a read-only context.
    // FIXME: Instead we should obsolete these and use their implementations in dyld itself
    void os_unfair_recursive_lock_lock_with_options(dyld_recursive_mutex_t lock, os_unfair_lock_options_t options) const {
        return helpers->os_unfair_recursive_lock_lock_with_options(lock, options);
    }
    void os_unfair_recursive_lock_unlock(dyld_recursive_mutex_t lock) const {
        return helpers->os_unfair_recursive_lock_unlock(lock);
    }
    void os_unfair_recursive_lock_unlock_forked_child(dyld_recursive_mutex_t lock) const {
        return helpers->os_unfair_recursive_lock_unlock_forked_child(lock);
    }
    void os_unfair_lock_lock_with_options(dyld_mutex_t lock, os_unfair_lock_options_t options) const {
        return helpers->os_unfair_lock_lock_with_options(lock, options);
    }
    void os_unfair_lock_unlock(dyld_mutex_t lock) const {
        return helpers->os_unfair_lock_unlock(lock);
    }

    // Normal helpers, all of which need to be read-only during calls
    void* malloc(size_t size) const
    {
        void* result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->malloc(size);
        });
        return result;
    }

    void free(void* p) const
    {
        bool unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->free(p);
            return false;
        });
        (void)unused;
    }

    size_t malloc_size(const void* p) const
    {
        size_t result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->malloc_size(p);
        });
        return result;
    }

    kern_return_t vm_allocate(vm_map_t target_task, vm_address_t* address, vm_size_t size, int flags) const
    {
        kern_return_t result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->vm_allocate(target_task, address, size, flags);
        });
        return result;
    }

    kern_return_t vm_deallocate(vm_map_t target_task, vm_address_t address, vm_size_t size) const
    {
        kern_return_t result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->vm_deallocate(target_task, address, size);
        });
        return result;
    }

    int pthread_key_create_free(dyld_thread_key_t* key) const
    {
        int result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->pthread_key_create_free(key);
        });
        return result;
    }

    void* pthread_getspecific(dyld_thread_key_t key) const
    {
        void* result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->pthread_getspecific(key);
        });
        return result;
    }

    int pthread_setspecific(dyld_thread_key_t key, const void* value) const
    {
        int result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->pthread_setspecific(key, value);
        });
        return result;
    }

    void __cxa_atexit(void (*func)(void*), void* arg, void* dso) const
    {
        bool unused = false;
        unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->__cxa_atexit(func, arg, dso);
            return false;
        });
    }

    void __cxa_finalize_ranges(const struct __cxa_range_t ranges[], unsigned int count) const
    {
        bool unused = false;
        unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->__cxa_finalize_ranges(ranges, count);
            return false;
        });
    }

    bool isLaunchdOwned() const
    {
        bool result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->isLaunchdOwned();
        });
        return result;
    }

    void exit(int result) const  __attribute__((__noreturn__))
    {
        bool unused = false;
        unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->exit(result);
            return false;
        });
        (void)unused;
        __builtin_unreachable();
    }

    const char* getenv(const char* key) const
    {
        const char* result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->getenv(key);
        });
        return result;
    }

    int mkstemp(char* templatePath) const
    {
        int result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->mkstemp(templatePath);
        });
        return result;
    }

    void setDyldPatchedObjCClasses() const
    {
        bool unused = false;
        unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->setDyldPatchedObjCClasses();
            return false;
        });
        (void)unused;
    }

    void run_async(void* (*func)(void*), void* context) const
    {
        bool unused = false;
        unused = this->memoryManager->withReadOnlyTPROMemory([&] {
            this->helpers->run_async(func, context);
            return false;
        });
        (void)unused;
    }

    FuncLookup legacyDyldFuncLookup() const
    {
        FuncLookup result = this->memoryManager->withReadOnlyTPROMemory([&] {
            return this->helpers->legacyDyldFuncLookup();
        });
        return result;
    }

    // Note, an error result here was strdup()ed and should be free()d
    mach_o::Error setUpThreadLocals(const DyldSharedCache* cache, const Header* hdr) const
    {
        char* result = this->memoryManager->withReadOnlyTPROMemory([&] {
            mach_o::Error error = this->helpers->setUpThreadLocals(cache, hdr);
            // withReadOnlyTPROMemory can't return a struct, so get the error out if we need it
            if ( error ) {
                size_t size = strlen(error.message()) + 1;
                char* buffer = (char*)this->helpers->malloc(size);
                memcpy(buffer, error.message(), size);
                return buffer;
            } else {
                return (char*)nullptr;
            }
        });
        if ( result ) {
            mach_o::Error error("%s", (const char*)result);
            this->free(result);
            return error;
        }
        return mach_o::Error::none();
    }


private:
    const LibSystemHelpers* helpers     = nullptr;
    lsl::MemoryManager* memoryManager   = nullptr;
};

#pragma clang diagnostic pop

} // namespace

#endif /* _libSystem_h */

