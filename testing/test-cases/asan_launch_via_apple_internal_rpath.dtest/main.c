// Setting RC_XBS=YES causes clang to add an extra rpath to /usr/appleinternal/lib/sanitizers
// BUILD(macos,ios,tvos,watchos): RC_XBS=YES $CC main.c -fsanitize=address  -o $BUILD_DIR/asan_launch_via_apple_internal_rpath.exe

// FIXME: Workaround rdar://70577455. An rpath to the ASan runtime inside the toolchain is implicitly added
// by clang. This causes the test to fail when the host and target are the same machine.
// To workaround this we remove the rpath.
// BUILD(macos,ios,tvos,watchos): $INSTALL_NAME_TOOL -delete_rpath $clangRuntimeDir $BUILD_DIR/asan_launch_via_apple_internal_rpath.exe

// RUN:  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 ./asan_launch_via_apple_internal_rpath.exe

#include "../sanitizer_common/asan_no_error.inc"
#include "../sanitizer_common/utils.h"

void check_asan_dylib_path(const char* asan_dylib_path) {
    check_dylib_in_expected_dir("ASan", asan_dylib_path, "/usr/appleinternal/lib/sanitizers");
}
