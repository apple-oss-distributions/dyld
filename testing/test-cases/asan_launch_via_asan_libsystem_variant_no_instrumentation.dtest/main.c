// We deliberately don't pass `-fsanitize=address` here so that we don't link against the
// ASan runtime. The goal of this test is to see if the ASan runtime gets loaded when we
// ask to load `_asan` variants of libraries. This should cause the `libSystem.B_asan.dylib`
// to be loaded. That library upward links the ASan runtime and 
// includes an rpath to `/usr/appleinternal/lib/sanitizers` which
// means that the ASan runtime should be found by dyld.

// `RC_XBS=NO` explicitly disables adding /usr/appleinternal/lib/sanitizers rpath
// BUILD(macos,ios,tvos,watchos): RC_XBS=NO $CC main.c -o $BUILD_DIR/asan_launch_via_asan_libsystem_variant.exe
// BUILD(macos,ios,tvos,watchos): $DYLD_ENV_VARS_ENABLE $BUILD_DIR/asan_launch_via_asan_libsystem_variant.exe

// RUN:  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_IMAGE_SUFFIX=_asan ./asan_launch_via_asan_libsystem_variant.exe

#define CHECK_LIBSYSTEM_ASAN_VARIANT_WAS_LOADED 1
#define SKIP_ASAN_INSTRUMENTATION_CHECK 1
#define CALL_SANITIZER_FN_VIA_DLSYM 1
#include "../sanitizer_common/asan_no_error.inc"
#include "../sanitizer_common/utils.h"

void check_asan_dylib_path(const char* asan_dylib_path) {
    check_dylib_in_expected_dir("ASan", asan_dylib_path, "/usr/appleinternal/lib/sanitizers");
}
