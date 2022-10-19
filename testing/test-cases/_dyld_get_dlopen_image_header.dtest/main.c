
// BUILD:  $CC main.c -o $BUILD_DIR/_dyld_get_dlopen_image_header.exe -DRUN_DIR="$RUN_DIR"
// BUILD:  $CC  foo.c -dynamiclib  -install_name $RUN_DIR/libfoo.dylib -o $BUILD_DIR/libfoo.dylib

// RUN:  ./_dyld_get_dlopen_image_header.exe

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

extern const struct mach_header __dso_handle;
extern const struct mach_header* foo();


int main()
{
    void* handle = dlopen(RUN_DIR "/libfoo.dylib", RTLD_FIRST);
    if ( handle == NULL ) {
        FAIL("dlopen(\"%s\") failed with: %s", RUN_DIR "/libfoo.dylib", dlerror());
    }

    const void* fooSym = dlsym(handle, "foo");
    const struct mach_header* fooMH = ((__typeof(&foo))fooSym)();

    const struct mach_header* imageMH = _dyld_get_dlopen_image_header(handle);
    if ( imageMH != fooMH ) {
        FAIL("Image header was incorrect: %p vs %p", imageMH, fooMH);
    }

    void* badHandle = (void*)(long)0x123456789;
    if ( _dyld_get_dlopen_image_header(badHandle) != NULL ) {
        FAIL("_dyld_get_dlopen_image_header(badHandle) did not return NULL");
    }

    void* selfHandle = (void*)(long)RTLD_SELF;
    const struct mach_header* selfMH = _dyld_get_dlopen_image_header(selfHandle);
    if ( selfMH != &__dso_handle ) {
        FAIL("_dyld_get_dlopen_image_header(RTLD_SELF) did not return &__dso_handle");
    }

    void* mainHandle = (void*)(long)RTLD_MAIN_ONLY;
    const struct mach_header* mainMH = _dyld_get_dlopen_image_header(mainHandle);
    if ( mainMH != &__dso_handle ) {
        FAIL("_dyld_get_dlopen_image_header(RTLD_MAIN_ONLY) did not return &__dso_handle");
    }


    PASS("Success");
}

