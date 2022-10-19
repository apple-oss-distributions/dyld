
// BUILD(macos|x86_64):  $CC main.c -o $BUILD_DIR/dlclose-basic-rosetta.exe
// BUILD(ios,tvos,watchos,bridgeos):

// RUN(macos|x86_64):  ./dlclose-basic-rosetta.exe
// RUN(macos|x86_64):  ROSETTA_DISABLE_AOT=1 ./dlclose-basic-rosetta.exe

#include <dlfcn.h>

#include "test_support.h"

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    void* handleDisk = dlopen("/usr/lib/libgmalloc.dylib", RTLD_NOW);
    if ( handleDisk == NULL ) {
        FAIL("dlopen(\"/usr/lib/libgmalloc.dylib\"), dlerror()=%s", dlerror());
    }
    int result = dlclose(handleDisk);
    if ( result != 0 ) {
        FAIL("dlclose(handleDisk) returned %d: %s", result, dlerror());
    }

    PASS("Success");
}

