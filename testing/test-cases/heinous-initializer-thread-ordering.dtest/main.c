
// BUILD:  $CC foo.c -dynamiclib -o $BUILD_DIR/libfoo.dylib -install_name $RUN_DIR/libfoo.dylib
// BUILD:  $CC main.c -o $BUILD_DIR/heinous-initializer-thread-ordering.exe -DRUN_DIR="$RUN_DIR"

// RUN:  ./heinous-initializer-thread-ordering.exe

// We are testing that dyld operates correctly when a thread is created and completes a call to dlopen() before static initializers
// are done running on the main thread. We do that by spawning and joining the thread that performs the dlopen in a static initializer.

// This behaviour is heinous and there is no reason to do it... and yet it turns out it really happens in production code

#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <mach-o/dyld_priv.h>

#include "test_support.h"

typedef void (*FooProc)(void);
pthread_t workerThread;

// We need access to this in the dylib before it has finished running initializers, and there is no way to pass it directly
// so lets stuff it in a custom section fo tbe found
struct SynchronizationState {
    pthread_mutex_t mutex;
    pthread_cond_t libraryInInitializer;
    pthread_cond_t mainExecutableFinishedDyld;
};

__attribute__((section("__DATA,__syncState")))
struct SynchronizationState syncState;

static void* work(void* mh)
{
    void* h = dlopen(RUN_DIR "/libfoo.dylib", 0);
    return NULL;
}

void __attribute__((constructor))
mainConstructor() {
    // Init mutex and cond vars
    if (pthread_mutex_init(&syncState.mutex, NULL) != 0) {
        FAIL("pthread_mutex_init");
    }
    if (pthread_cond_init(&syncState.libraryInInitializer, NULL) != 0) {
        FAIL("pthread_cond_init");
    }
    if (pthread_cond_init(&syncState.mainExecutableFinishedDyld, NULL) != 0) {
        FAIL("pthread_cond_init");
    }
    if (pthread_mutex_lock(&syncState.mutex) != 0) {
        FAIL("pthread_mutex_lock");
    }
    // Make a thread for dlopen() on libfoo.dylib
    if ( pthread_create(&workerThread, NULL, &work, NULL) != 0 ) {
        FAIL("pthread_create");
    }
    if (pthread_cond_wait(&syncState.libraryInInitializer, &syncState.mutex) != 0) {
        FAIL("pthread_cond_signal");
    }
    if (pthread_mutex_unlock(&syncState.mutex) != 0) {
        FAIL("pthread_mutex_unlock");
    }
}


int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    fprintf(stderr, "Locking mutex MT1\n");
    if (pthread_mutex_lock(&syncState.mutex) != 0) {
        FAIL("pthread_mutex_lock");
    }
    if (pthread_cond_signal(&syncState.mainExecutableFinishedDyld) != 0) {
        FAIL("pthread_cond_signal");
    }
    if (pthread_mutex_unlock(&syncState.mutex) != 0) {
        FAIL("pthread_mutex_unlock");
    }
    PASS("Success");
}

