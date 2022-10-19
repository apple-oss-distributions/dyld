
// `RC_XBS=NO` explicitly disables adding /usr/appleinternal/lib/sanitizers rpath
// BUILD(macos,ios,tvos,watchos): RC_XBS=NO $CXX main.cpp -fsanitize=address  -o $BUILD_DIR/asan_find_bug_via_interceptor_cxx.exe
// BUILD(macos,ios,tvos,watchos): $DYLD_ENV_VARS_ENABLE $BUILD_DIR/asan_find_bug_via_interceptor_cxx.exe
// RUN:  DYLD_PRINT_SEARCHING=1 DYLD_PRINT_LIBRARIES=1 DYLD_LIBRARY_PATH=/usr/appleinternal/lib/sanitizers ./asan_find_bug_via_interceptor_cxx.exe
#include "test_support.h"
#include <sanitizer/asan_interface.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARRAY_SIZE(X) sizeof(X)/sizeof(X[0])

#if defined(__has_feature)
#  if !__has_feature(address_sanitizer)
#  error ASan should be enabled.
#  endif
#else
#  error Compiler should support __has_feature
#endif


void asan_report_handler(const char *report) {
  LOG("hit ASan issue");
  const char* needle = strstr(report, "in wrap_printf");
  if (!needle) {
      FAIL("Didn't see printf interceptor in ASan report");
  }
  PASS("ASan issue looks like it came via interceptor");
}

int main() {
    char src[] = "some_string";

    // Remove null terminator
    src[ARRAY_SIZE(src) -1] = 'X';

    __asan_set_error_report_callback(asan_report_handler);

    printf("oh no: %s\n", src); // BOOM

    FAIL("Should not be reached.");
    return 1;

}
