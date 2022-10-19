
// rdar://90234634 Add support for swiftc instead of using swift_marker.s
// BUILD:  $CC foo.c swift_marker.s -bundle -o $BUILD_DIR/foo.bundle
// BUILD:  $CC foo.c swift_marker.s -dynamiclib -o $BUILD_DIR/libfoo.dylib
// BUILD:  $CC main.c -DRUN_DIR="$RUN_DIR" -o $BUILD_DIR/dlclose-swift-bundle.exe

// RUN:  ./dlclose-swift-bundle.exe

// Make sure that images with Swift can't be unloaded

#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

extern int foo();
typedef __typeof(&foo) fooPtr;

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    // test Swift bundle
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

    // close a second time and verify it didn't fail
    result = dlclose(handle);
    if ( result != 0 ) {
        FAIL("dlclose(handle) returned %d, dlerror()=%s", result, dlerror());
    }

    // test Swift dylib
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
