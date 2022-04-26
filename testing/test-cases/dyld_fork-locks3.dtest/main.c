
// BUILD:  $CC main.c -o $BUILD_DIR/dyld_fork_test3.exe

// RUN:  ./dyld_fork_test3.exe 

#include <stdio.h>
#include <dlfcn.h>
#include <mach-o/dyld_priv.h>
#include <pthread.h>

#include "test_support.h"

// Similar to dyld_fork_test but also tests for pthread_atfork() to make sure
// we don't crash if the atfork() handler calls dlopen/dlclose

void* prepareHandle = 0;

void prepare()
{
    prepareHandle = dlopen("/usr/lib/libz.dylib", RTLD_LAZY);
}

void parent()
{
    // We expect this to fail, as the fork handlers shouldn't be able to dlopen
    // until fork() is done
    void* handle = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    if ( handle != NULL ) {
        FAIL("Expected dlopen to fail");
        return;
    }

    // Also try a dlclose to make sure it also bails out before taking the lock
    dlclose(prepareHandle);

    PASS("Success");
}

void child()
{
    void* handle = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    if ( handle != NULL )
        FAIL("Expected dlopen to fail");

    // Also try a dlclose to make sure it also bails out before taking the lock
    dlclose(prepareHandle);
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) 
{
    pthread_atfork(&prepare, &parent, &child);
    
    pid_t sChildPid = fork();
    if ( sChildPid < 0 ) {
        FAIL("Didn't fork");
    }

    return 0;
}
