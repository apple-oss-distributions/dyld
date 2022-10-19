
// BUILD(macos):  $CC main.c -o $BUILD_DIR/dlopen-framework-fallback.exe
// BUILD(ios,tvos,watchos,bridgeos):

// RUN:  ./dlopen-framework-fallback.exe

#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    // Verify dyld will fallback and look for framework in /System/Library/Frameworks/
    // fallbacks only work in binaries targeting macOS 12.x or earlier
    void* handle = dlopen("/System/Library/BadPath/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    if ( handle == NULL ) {
        FAIL("dlerror(): %s", dlerror());
    }

    // validate handle works to find symbols
    void* sym = dlsym(handle, "CFRetain");
    if ( sym == NULL ) {
        FAIL("dlerror(): %s", dlerror());
    }

    PASS("Success");

    return 0;
}

