// `RC_XBS=NO` explicitly disables adding /usr/appleinternal/lib/sanitizers rpath
// BUILD(macos,ios,tvos,watchos): RC_XBS=NO $CXX main.cpp -fsanitize=address  -o $BUILD_DIR/asan_launch_via_dyld_library_path_cxx.exe
// BUILD(macos,ios,tvos,watchos): $DYLD_ENV_VARS_ENABLE $BUILD_DIR/asan_launch_via_dyld_library_path_cxx.exe
// BUILD(macos,ios,tvos,watchos): $CP $asanDylibPath $BUILD_DIR/some_dir/$asanDylibName
// RUN:  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_LIBRARY_PATH=`pwd`/some_dir ./asan_launch_via_dyld_library_path_cxx.exe

#include <sys/syslimits.h>
#include <unistd.h>
#include "../sanitizer_common/asan_no_error.inc"
#include "../sanitizer_common/utils.h"

void check_asan_dylib_path(const char* asan_dylib_path) {
    check_dylib_in_dir_under_cwd("ASan", asan_dylib_path, "/some_dir");
}
