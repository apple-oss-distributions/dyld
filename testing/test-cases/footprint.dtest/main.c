// BUILD:  $CC main.c -o $BUILD_DIR/footprint.exe -DRUN_DIR="$RUN_DIR"

// RUN:  $SUDO ./footprint.exe

#include <spawn.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_support.h"


int main(int argc, const char* argv[], char *env[]) {
    posix_spawnattr_t attrs;
    if ( posix_spawnattr_init(&attrs) != 0 )
        FAIL("posix_spawnattr_init failed");

    pid_t pid;
    const char* args[] = { "/usr/bin/footprint", "-a", NULL};
    int err = posix_spawn(&pid, args[0], NULL, &attrs,  (char *const *)args, env);
    if ( err != 0 )
        FAIL("posix_spawn failed: %s", strerror(err));

    int status;
    if ( waitpid(pid, &status, 0) == -1 )
        FAIL("waitpid failed");

    if ( WIFSIGNALED(status) ) {
        if ( !WTERMSIG(status) )
            FAIL("WTERMSIG failed");
        FAIL("foorprint received signal %d", status);
    }

    if ( !WIFEXITED(status) ) {
        FAIL("foorprint did not exit");
    }

    err = WEXITSTATUS(status);
    if ( err != 0 )
        FAIL("foorprint exit with code %d", err);

    if ( posix_spawnattr_destroy(&attrs) == -1 )
        FAIL("posix_spawnattr_destroy failed");
    
    PASS("Success");
    return 0;
}

