
// BUILD(macos|x86_64):  $CC -target apple-macos10.14 base.c  -dynamiclib -install_name $RUN_DIR/libbase.dylib  -o $BUILD_DIR/libbase.dylib
// BUILD(macos|x86_64):  $CC -target apple-macos10.14 foo.c   -dynamiclib -install_name $RUN_DIR/libdynamic.dylib  -o $BUILD_DIR/libdynamic.dylib $BUILD_DIR/libbase.dylib
// BUILD(macos|x86_64):  $CC -target apple-macos10.14 main.c -o $BUILD_DIR/static-terminators-old.exe -DRUN_DIR="$RUN_DIR" $BUILD_DIR/libbase.dylib

// BUILD(ios,tvos,watchos,bridgeos):

// RUN(macos|x86_64):  ./static-terminators-old.exe

#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

// verify all static terminators run in proper order


extern void mainTerminated();


static void myTerm()
{
    LOG("main's static terminator\n");
    mainTerminated();
}

// don't use attribute destructor because clang will transform
// we want to be built like an old binary
__attribute__((used,section("__DATA,__mod_term_func,mod_term_funcs")))
static void* proc = &myTerm;

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    BEGIN();
    // load dylib
    void* handle = dlopen(RUN_DIR "/libdynamic.dylib", RTLD_LAZY);
    if ( handle == NULL ) {
        FAIL("libdynamic.dylib could not be loaded, %s", dlerror());
    }

    // PASS is printed in libbase.dylib terminator
}

