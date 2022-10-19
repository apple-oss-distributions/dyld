// Test that resolution of aliased reexports works when the aliased symbol is
// implemented in one of the dependent's reexported library.
// <rdar://91326465> libsystem_c re-exports strcmp as platform_strcmp from
// libsystem_sim_platform, but indirectly by reexporting host's libsystem_platform.

// main
// \ (link)
//  libfoo.dylib - alias reexport _foo -> __platform_foo
//  \ (link)
//   libfoo_platform.dylib
//   \ (reexport link)
//    libfoo_platform_impl.dylib - exports __platform_foo

// BUILD: $CC foo_platform_impl.c -dynamiclib -o $BUILD_DIR/libfoo_platform_impl.dylib -install_name @rpath/libfoo_platform_impl.dylib
// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/libfoo_platform.dylib -L$BUILD_DIR -Wl,-reexport-lfoo_platform_impl -install_name @rpath/libfoo_platform.dylib $DEPENDS_ON_ARG $BUILD_DIR/libfoo_platform_impl.dylib
// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/libfoo.dylib -L$BUILD_DIR -lfoo_platform -Wl,-reexported_symbols_list,$SRC_DIR/foo_reexport.txt -Wl,-alias,__platform_foo,_foo -install_name @rpath/libfoo.dylib $DEPENDS_ON_ARG $BUILD_DIR/libfoo_platform.dylib
// BUILD: $CC main.c -o $BUILD_DIR/main.exe -L$BUILD_DIR -lfoo -rpath @loader_path -DRUN_DIR="$RUN_DIR" $DEPENDS_ON_ARG $BUILD_DIR/libfoo.dylib

// RUN: ./main.exe

#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

int main() {
    void* handle = dlopen(RUN_DIR "/libfoo.dylib", RTLD_NOLOAD);
    if ( handle == NULL )
        FAIL("dlerror(): %s", dlerror());

    // Test resolution through a specific handle.
    void* sym1 = dlsym(handle, "foo");
    if ( sym1 == NULL )
        FAIL("dlerror(): %s", dlerror());

    // Test RTLD_DEFAULT resolution.
    sym1 = dlsym(RTLD_DEFAULT, "foo");
    if ( sym1 == NULL )
        FAIL("dlerror(): %s", dlerror());

    PASS("Success");
}
