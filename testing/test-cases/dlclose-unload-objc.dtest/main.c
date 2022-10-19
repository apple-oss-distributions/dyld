
// BUILD:  $CC foo.m -bundle -o $BUILD_DIR/foo.bundle
// BUILD:  $CC foo.m -dynamiclib -o $BUILD_DIR/libfoo.dylib
// BUILD:  $CC main.c -DRUN_DIR="$RUN_DIR" -o $BUILD_DIR/dlclose-objc-bundle.exe

// RUN:  ./dlclose-objc-bundle.exe

// Make sure that ObjC bundles can be unloaded, but dylibs can't

#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

extern int foo();
typedef __typeof(&foo) fooPtr;

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    // test ObjC bundle
    void* handle = dlopen(RUN_DIR "/foo.bundle", RTLD_LAZY);
    if ( handle == NULL ) {
        FAIL("dlopen(\"foo.bundle\"), dlerror()=%s", dlerror());
    }

    fooPtr sym = (fooPtr)dlsym(handle, "foo");
    if ( sym == NULL ) {
        FAIL("dlsym(\"foo\") returned NULL, dlerror()=%s", dlerror());
    }

    if ( sym() != 0x64 ) {
        FAIL("Expected 0x64 on the first call to foo()");
    }

    int result = dlclose(handle);
    if ( result != 0 ) {
        FAIL("dlclose(handle) returned %d, dlerror()=%s", result, dlerror());
    }

    // close a second time and verify it failed
    result = dlclose(handle);
    if ( result == 0 ) {
        FAIL("second dlclose() unexpectedly returned 0");
    }

    // test ObjC dylib
    handle = dlopen(RUN_DIR "/libfoo.dylib", RTLD_LAZY);
    if ( handle == NULL ) {
        FAIL("dlopen(\"libfoo.dylib\"), dlerror()=%s", dlerror());
    }

    sym = (fooPtr)dlsym(handle, "foo");
    if ( sym == NULL ) {
        FAIL("dlsym(\"foo\") returned NULL, dlerror()=%s", dlerror());
    }

    if ( sym() != 0x64 ) {
        FAIL("Expected 0x64 on the first call to foo()");
    }

    result = dlclose(handle);
    if ( result != 0 ) {
        FAIL("dlclose(handle) returned %d, dlerror()=%s", result, dlerror());
    }

    // close a second time and verify it didn't fail
    result = dlclose(handle);
    if ( result != 0 ) {
        FAIL("dlclose(handle) returned %d, dlerror()=%s", result, dlerror());
    }

    PASS("Success");
}
