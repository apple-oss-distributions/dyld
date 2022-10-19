
// BUILD:  $CC foo.m -dynamiclib -o $BUILD_DIR/foo.dylib -Wl,-rename_section,__DATA,__objc_selrefs,__DATA_CONST,__objc_selrefs
// BUILD:  $CC main.m -DRUN_DIR="$RUN_DIR" -o $BUILD_DIR/objc-read-only-selectors.exe -lobjc -Wl,-rename_section,__DATA,__objc_selrefs,__DATA_CONST,__objc_selrefs

// RUN:  ./objc-read-only-selectors.exe

#include <dlfcn.h>

#import <Foundation/Foundation.h>

#include "test_support.h"

extern SEL sel_registerName(const char *name);
extern SEL foo();

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    // Get a selector we know is in the shared cache.   This will get fixed up in libobjc
    // so tests __DATA_CONST
    SEL initSel = @selector(init);
    if ( initSel != (SEL)sel_registerName("init") )
        FAIL("main init is wrong");

    // dlopen a binary which also needs a selector to be set
    const char* path = RUN_DIR "/foo.dylib";
    void* handle = dlopen(path, RTLD_LAZY);
    if ( handle == NULL ) {
        FAIL("dlopen(\"%s\"), dlerror()=%s", path, dlerror());
    }

    void* sym = dlsym(handle, "foo");
    if ( sym == NULL ) {
        FAIL("dlsym(\"foo\") for \"%s\" returned NULL, dlerror()=%s", path, dlerror());
    }

    SEL fooSel = ((__typeof(&foo))sym)();
    if ( fooSel != initSel )
        FAIL("foo init is wrong");

    PASS("objc-read-only-selectors");

    return 0;
}
