#include "test_support.h"
#include <dlfcn.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <sanitizer/allocator_interface.h>
#include <sanitizer/asan_interface.h>
#include <stdio.h>
#include <sys/syslimits.h>

#ifndef CHECK_LIBSYSTEM_ASAN_VARIANT_WAS_LOADED
#define CHECK_LIBSYSTEM_ASAN_VARIANT_WAS_LOADED 0
#endif

#ifndef SKIP_ASAN_INSTRUMENTATION_CHECK
#    if defined(__has_feature)
#        if !__has_feature(address_sanitizer)
#            error ASan should be enabled.
#        endif
#    else
#        error Compiler should support __has_feature
#    endif
#endif


bool asan_was_loaded = false;

// Tests should implement this function to perform any
// test specific checks.
void check_asan_dylib_path(const char* asan_dylib_path);

#define ARRAY_SIZE(X) (sizeof(X)/sizeof(X[0]))


// This is an abstraction around functions we need to call from
// the ASan runtime library. It exists so we can conveniently call
// them even if this binary was not linked against the ASan runtime.
typedef struct {
    #define SAN_FUNC(FUNC_NAME, RET_TYPE, ...) RET_TYPE (*FUNC_NAME)(__VA_ARGS__);
    #include "asan_interface_fns.inc"
    #undef SAN_FUNC
} SanitizerInterfaceFns;

SanitizerInterfaceFns san_funcs;


#ifdef CALL_SANITIZER_FN_VIA_DLSYM

void* get_fn_ptr(const char* fn_name) {
    void *fn = dlsym(RTLD_DEFAULT, fn_name);
    if (!fn) {
        const char* error_msg = dlerror();
        FAIL("Failed to get function \"%s\" with error %s", fn_name, error_msg);
    }
    return fn;
}

// Emit type defs
#define SAN_FUNC(FUNC_NAME, RET_TYPE, ...) typedef RET_TYPE (*FUNC_NAME ## _FN_TY)(__VA_ARGS__);
#include "asan_interface_fns.inc"
#undef SAN_FUNC

// Call via dlsym
void init_san_fns() {
    #define SAN_FUNC(FUNC_NAME, RET_TYPE, ...) san_funcs.FUNC_NAME = (FUNC_NAME ## _FN_TY) get_fn_ptr(#FUNC_NAME);
    #include "asan_interface_fns.inc"
    #undef SAN_FUNC
}
#else
// Direct call
void init_san_fns() {
    #define SAN_FUNC(FUNC_NAME, RET_TYPE, ...) san_funcs.FUNC_NAME = &FUNC_NAME;
    #include "asan_interface_fns.inc"
    #undef SAN_FUNC
}
#endif


bool libsystem_asan_variant_loaded = false;

void load_notify(const struct mach_header* mh, const char* path, bool unloadable) {
    LOG("dylib \"%s\" was loaded", path);
    char dylib_name[PATH_MAX];
    if (!basename_r(path, dylib_name))
        FAIL("Call to basename_r failed");
    // e.g. libclang_rt.asan_osx_dynamic.dylib
    const char asan_dylib_prefix[] = "libclang_rt.asan_";
    if (strncmp(asan_dylib_prefix, dylib_name, ARRAY_SIZE(asan_dylib_prefix) -1) == 0) {
        LOG("Found ASan dylib: %s", path);
        asan_was_loaded = true;
        // Run test specific check
        check_asan_dylib_path(path);
        return;
    }

    if (strcmp(path, "/usr/lib/libSystem.B_asan.dylib") == 0) {
        LOG("Found Libsystem ASan variant");
        libsystem_asan_variant_loaded = true;
    }
}

void check_memory_ownership(int* some_memory) {
    if (!san_funcs.__sanitizer_get_ownership(some_memory)) {
        FAIL("Allocated memory doesn't belong to ASan");
    }
    LOG("Allocted memory belongs to ASan");
    void* is_poisoned = san_funcs.__asan_region_is_poisoned(some_memory, sizeof(int));
    if (is_poisoned) {
        FAIL("Allocated should not be poisoned");
    }
    LOG("Allocated memory is not poisoned");
}

void check_memory_is_poisoned(int* some_memory) {
    // Check the freed memory is poisoned.
    void* is_poisoned = san_funcs.__asan_region_is_poisoned(some_memory, sizeof(int));
    if (!is_poisoned) {
        FAIL("Freed memory should be poisoned but isn't");
    }
    LOG("Freed memory is poisoned");
}


int main(int argc, char** argv) {
    init_san_fns();

    // Register notifications about previously loaded dylibs and anything
    // that gets dlopened.
    _dyld_register_for_image_loads(&load_notify);

    if (!asan_was_loaded) {
        FAIL("ASan dylib was not loaded");
    }

    if (CHECK_LIBSYSTEM_ASAN_VARIANT_WAS_LOADED && !libsystem_asan_variant_loaded) {
        FAIL("LibSystem asan variant was not loaded");
    }

    size_t size = san_funcs.__sanitizer_get_heap_size();
    LOG("Estimated heap size: %zu\n", size);

    // Allocate some memory and make sure it came from our allocator.
    int* some_memory = (int*) malloc(sizeof(int));
    if (!some_memory) {
        FAIL("Failed to allocate memory");
    }
    LOG("Allocated memory: %p", some_memory);

    check_memory_ownership(some_memory);

    // Try to free
    free(some_memory);
    LOG("Allocated memory has been freed");

    check_memory_is_poisoned(some_memory);


#ifdef __cplusplus
    // Allocate some memory via new and make sure it came from our allocator
    int* some_memory_cxx = new int();
    if (!some_memory_cxx) {
        FAIL("Failed to allocate cxx memory");
    }
    LOG("Allocated cxx memory: %p", some_memory_cxx);

    check_memory_ownership(some_memory_cxx);

    // Try to free
    delete some_memory_cxx;
    LOG("Allocated cxx memory has been freed");

    check_memory_is_poisoned(some_memory_cxx);
#endif


    PASS("DONE");
    return 0;
}