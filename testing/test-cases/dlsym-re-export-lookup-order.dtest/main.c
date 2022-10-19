// Test dlsym resolution order of explicit symbol reexports implemented in one
// of the dependent's reexported library.

// At link time:
// main
// \ (link)
//  libwrapper.dylib - _foo reexport
//  \
//   libfoo_alt.dylib - empty library at linktime
//  \
//   libfoo.dylib
//   \ (reexport link)
//    libfoo_impl.dylib - _foo implementation

// At runtime use alternative libfoo_alt.dylib implementation, one that also
// implements _foo.

// Link time builds.
// BUILD: $CC foo.c -dynamiclib -o $BUILD_DIR/link/libfoo_impl.dylib -install_name @rpath/libfoo_impl.dylib 
// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/link/libfoo_alt.dylib -install_name @rpath/libfoo_alt.dylib 
// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/link/libfoo.dylib -install_name @rpath/libfoo.dylib -L$BUILD_DIR/link -Wl,-reexport-lfoo_impl $DEPENDS_ON_ARG $BUILD_DIR/link/libfoo_impl.dylib

// Runtime builds.
// BUILD: $CC foo.c -dynamiclib -o $BUILD_DIR/libfoo_impl.dylib -install_name @rpath/libfoo_impl.dylib 
// BUILD: $CC foo_alt.c -dynamiclib -o $BUILD_DIR/libfoo_alt.dylib -install_name @rpath/libfoo_alt.dylib 
// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/libfoo.dylib -install_name @rpath/libfoo.dylib -L$BUILD_DIR/link -Wl,-reexport-lfoo_impl $DEPENDS_ON_ARG $BUILD_DIR/link/libfoo_impl.dylib

// BUILD: $CC dummy.c -dynamiclib -o $BUILD_DIR/libwrapper.dylib -install_name @rpath/libwrapper.dylib -L$BUILD_DIR/link -lfoo_alt -lfoo -Wl,-reexported_symbols_list,$SRC_DIR/foo_reexport.txt $DEPENDS_ON_ARG $BUILD_DIR/link/libfoo_alt.dylib $DEPENDS_ON_ARG $BUILD_DIR/link/libfoo.dylib
// BUILD: $CC main.c -o $BUILD_DIR/main.exe -L$BUILD_DIR -lwrapper -rpath @loader_path $DEPENDS_ON_ARG $BUILD_DIR/libwrapper.dylib $DEPENDS_ON_ARG $BUILD_DIR/libfoo.dylib $DEPENDS_ON_ARG $BUILD_DIR/libfoo_impl.dylib $DEPENDS_ON_ARG $BUILD_DIR/libfoo_alt.dylib

// RUN:  ./main.exe

#include <stdio.h>
#include <dlfcn.h>

#include "test_support.h"

typedef int(*int_ret_fn)(void);

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    void* foo = dlsym(RTLD_DEFAULT, "foo");
    if ( foo == NULL )
        FAIL("dlerror(): %s", dlerror());

    // Expect dlsym(RTLD_DEFAULT, "foo") to return the implementation from
    // libfoo_alt, because it was loaded earlier than libfoo_impl.
    int res = ((int_ret_fn)foo)();
    if ( res == 1 )
        PASS("Success");

    FAIL("Expected foo() == 1, but instead got: %d\n", res);
}
