
// BUILD:  $CC prog.c -o $BUILD_DIR/force-4k-pages-prog.exe -DRUN_DIR="$RUN_DIR"
// BUILD:  $CC main.c -o $BUILD_DIR/force-4k-pages.exe -DRUN_DIR="$RUN_DIR"
// BUILD:  $CC foo.c -dynamiclib -o $BUILD_DIR/test.dylib
// BUILD:  $CC foo.c -dynamiclib -o $BUILD_DIR/test2.dylib
// BUILD:  $CC foo.c -dynamiclib -o $BUILD_DIR/test3.dylib

// RUN:  ./force-4k-pages.exe

#include <sandbox.h>
#include <spawn.h>
#include <spawn_private.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/spawn_internal.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_support.h"

#ifndef _POSIX_SPAWN_FORCE_4K_PAGES
#define _POSIX_SPAWN_FORCE_4K_PAGES 0x1000
#endif /* _POSIX_SPAWN_FORCE_4K_PAGES */

// Note: Copied from vm_spawn_tool.c

static void spawn_4k(const char* progPath, char* const env[])
{
    pid_t newpid = 0;
    posix_spawn_file_actions_t fileactions;
    posix_spawnattr_t spawnattrs;
    if (posix_spawnattr_init(&spawnattrs)) {
        FAIL("posix_spawnattr_init");
    }
    if (posix_spawn_file_actions_init(&fileactions)) {
        FAIL("posix_spawn_file_actions_init");
    }
    short sp_flags = POSIX_SPAWN_SETEXEC;

    /* Need to set special flags */
    int supported = 0;
    size_t supported_size = sizeof(supported);

    int r = sysctlbyname("debug.vm_mixed_pagesize_supported", &supported, &supported_size, NULL, 0);
    if (r == 0 && supported) {
        sp_flags |= _POSIX_SPAWN_FORCE_4K_PAGES;
    } else {
        /*
         * We didnt find debug.vm.mixed_page.supported OR its set to 0.
         * Skip the test.
         */
        //printf("Hardware doesn't support 4K pages, skipping test...");
        PASS("Success");
        return;
    }

    posix_spawnattr_setflags(&spawnattrs, sp_flags);
    const char* args[] = { progPath, NULL };
    posix_spawn(&newpid, progPath, &fileactions, &spawnattrs, (char *const *)args, env);

    /* Should not have reached here */
    FAIL("should not have reached here");
}

int main(int argc, const char* argv[], char *env[])
{
    spawn_4k(RUN_DIR "/force-4k-pages-prog.exe", env);
    return 0;
}

