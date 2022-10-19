
#ifndef SANITIZER_COMMON_UTILS_H
#define SANITIZER_COMMON_UTILS_H 1
#include <assert.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include "test_support.h"

// `expected_dylib_dir` should be an absolute path.
void check_dylib_in_expected_dir(const char* sanitizer, const char* found_dylib_path, const char* expected_dylib_dir) {
    assert(expected_dylib_dir[0] == '/');
    char dylib_dir_path[PATH_MAX];
    if (!dirname_r(found_dylib_path, dylib_dir_path))
        FAIL("call to dirname_r failed");
    if (strcmp(dylib_dir_path, expected_dylib_dir) == 0) {
        LOG("Found %s dylib in expected dir %s", sanitizer, expected_dylib_dir);
        return;
    }
    FAIL("%s dylib expected in \"%s\" but found in \"%s\"", sanitizer, expected_dylib_dir, dylib_dir_path);
}

// `dir_in_cwd` must include leading slash.
void check_dylib_in_dir_under_cwd(const char* sanitizer, const char* found_dylib_path, const char* dir_in_cwd) {
    assert(dir_in_cwd[0] == '/');
    char dylib_dir_path[PATH_MAX];
    if (!dirname_r(found_dylib_path, dylib_dir_path))
        FAIL("dirname call failed");
    char expected_path[PATH_MAX];
    char* result = getcwd(expected_path, sizeof(expected_path));
    if (!result)
        FAIL("Failed to call getcwd");
    strlcat(expected_path, dir_in_cwd, sizeof(expected_path));

    if (strcmp(dylib_dir_path, expected_path) == 0) {
        LOG("Found %s dylib in expected dir %s", sanitizer, expected_path);
        return;
    }
    FAIL("%s dylib expected in \"%s\" but found in \"%s\"", sanitizer, expected_path, dylib_dir_path);
}

void check_dylib_in_cwd(const char* sanitizer, const char* found_dylib_path) {
    char dylib_dir_path[PATH_MAX];
    if (!dirname_r(found_dylib_path, dylib_dir_path))
        FAIL("call to dirname_r failed");
    char expected_path[PATH_MAX];
    char* result = getcwd(expected_path, sizeof(expected_path));
    if (!result)
        FAIL("Failed to call getcwd");

    if (strcmp(dylib_dir_path, expected_path) == 0) {
        LOG("Found %s dylib in expected dir %s", sanitizer, expected_path);
        return;
    }
    FAIL("%s dylib expected in \"%s\" but found in \"%s\"", sanitizer, expected_path, dylib_dir_path);
}

#endif