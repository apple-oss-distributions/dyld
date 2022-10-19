// `RC_XBS=NO` explicitly disables adding /usr/appleinternal/lib/sanitizers rpath
// BUILD(macos,ios,tvos,watchos): RC_XBS=NO $CXX main.cpp -fsanitize=address  -o $BUILD_DIR/asan_launch_via_dyld_insert_libraries_cxx.exe
// BUILD(macos,ios,tvos,watchos): $DYLD_ENV_VARS_ENABLE $BUILD_DIR/asan_launch_via_dyld_insert_libraries_cxx.exe
// BUILD(macos,ios,tvos,watchos): $CP $asanDylibPath $BUILD_DIR/some_dir2/$asanDylibName


// RUN(macos):  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES=`pwd`/some_dir2/libclang_rt.asan_osx_dynamic.dylib ./asan_launch_via_dyld_insert_libraries_cxx.exe
// RUN(ios):  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES=`pwd`/some_dir2/libclang_rt.asan_ios_dynamic.dylib ./asan_launch_via_dyld_insert_libraries_cxx.exe
// RUN(watchos):  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES=`pwd`/some_dir2/libclang_rt.asan_watchos_dynamic.dylib ./asan_launch_via_dyld_insert_libraries_cxx.exe
// RUN(tvos):  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_INSERT_LIBRARIES=`pwd`/some_dir2/libclang_rt.asan_tvos_dynamic.dylib ./asan_launch_via_dyld_insert_libraries_cxx.exe

// Don't test bridgeos for now.
// RUN(bridgeos):

#include <sys/syslimits.h>
#include <unistd.h>
#include "../sanitizer_common/asan_no_error.inc"
#include "../sanitizer_common/utils.h"

void check_asan_dylib_path(const char* asan_dylib_path) {
    check_dylib_in_dir_under_cwd("ASan", asan_dylib_path, "/some_dir2");
}
