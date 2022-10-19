// `RC_XBS=NO` explicitly disables adding /usr/appleinternal/lib/sanitizers rpath
// BUILD(macos,ios,tvos,watchos): RC_XBS=NO $CXX main.cpp -fsanitize=address  -o $BUILD_DIR/asan_launch_via_asan_libsystem_variant_cxx.exe
// BUILD(macos,ios,tvos,watchos): $DYLD_ENV_VARS_ENABLE $BUILD_DIR/asan_launch_via_asan_libsystem_variant_cxx.exe

// Unfortunately `DYLD_IMAGE_SUFFIX=_asan` on its own isn't enough. Even though the `libSystem.B_asan.dylib`
// has an `/usr/appleinternal/lib/sanitizers` rpath it doesn't seem to get used. I think this
// is because the ASan runtime load command comes before `libSystem.B_asan.dylib`, so dyld doesn't
// know about the rpath yet. So we have to add `DYLD_LIBRARY_PATH=/usr/appleinternal/lib/sanitizers`.

// RUN:  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_LIBRARY_PATH=/usr/appleinternal/lib/sanitizers DYLD_IMAGE_SUFFIX=_asan ./asan_launch_via_asan_libsystem_variant_cxx.exe

#define CHECK_LIBSYSTEM_ASAN_VARIANT_WAS_LOADED 1
#include "../sanitizer_common/asan_no_error.inc"
#include "../sanitizer_common/utils.h"

void check_asan_dylib_path(const char* asan_dylib_path) {
    check_dylib_in_expected_dir("ASan", asan_dylib_path, "/usr/appleinternal/lib/sanitizers");
}
