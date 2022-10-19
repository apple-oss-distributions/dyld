// BUILD(macos):  $CC myzlib.c -dynamiclib -o $BUILD_DIR/override/libz.1.dylib -install_name /usr/lib/libz.1.dylib -compatibility_version 1.0 -target apple-macos -target-variant x86_64-apple-ios-macabi
// BUILD(macos):  $CC reexported-myzlib.c -dynamiclib -o $BUILD_DIR/re-export-override/reexported.dylib -compatibility_version 1.0  -install_name $RUN_DIR/re-export-override/reexported.dylib -target apple-macos -target-variant x86_64-apple-ios-macabi
// BUILD(macos):  $CC reexporter.c -dynamiclib -o $BUILD_DIR/re-export-override/libz.1.dylib -install_name /usr/lib/libz.1.dylib -compatibility_version 1.0 -Wl,-reexport_library,$BUILD_DIR/re-export-override/reexported.dylib -Wl,-debug_variant -target apple-macos -target-variant x86_64-apple-ios-macabi
// BUILD(macos):  $CC main.c  -o $BUILD_DIR/env-DYLD_LIBRARY_PATH-cache-iOSMac.exe -lz -target x86_64-apple-ios-macabi
// BUILD(macos):  $DYLD_ENV_VARS_ENABLE $BUILD_DIR/env-DYLD_LIBRARY_PATH-cache-iOSMac.exe

// BUILD(ios,tvos,watchos,bridgeos):

// RUN:  ./env-DYLD_LIBRARY_PATH-cache-iOSMac.exe
// RUN:  DYLD_LIBRARY_PATH=$RUN_DIR/override/ ./env-DYLD_LIBRARY_PATH-cache-iOSMac.exe
// RUN:  DYLD_LIBRARY_PATH=$RUN_DIR/re-export-override/ ./env-DYLD_LIBRARY_PATH-cache-iOSMac.exe

#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <stdbool.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

// The test here is to override libz.1.dylib which is in the dyld cache with our own implementation.

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    BEGIN();
    bool expectMyDylib = (getenv("DYLD_LIBRARY_PATH") != NULL) && !_dyld_shared_cache_optimized();

    bool usingMyDylib = (strcmp(zlibVersion(), "my") == 0);

    if ( usingMyDylib == expectMyDylib ) {
        PASS("Succes");
    } else {
        FAIL("Expected %s, got %s", expectMyDylib ? "my" : "os", expectMyDylib ? "os" : "my");
    }
}

