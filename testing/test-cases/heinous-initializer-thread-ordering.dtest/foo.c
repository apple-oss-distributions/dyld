#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <crt_externs.h>
#include <mach-o/getsect.h>


struct SynchronizationState {
    pthread_mutex_t mutex;
    pthread_cond_t libraryInInitializer;
    pthread_cond_t mainExecutableFinishedDyld;
};

void __attribute__((constructor))
myInit()
{
    unsigned long sectSize = 0;
    struct SynchronizationState* syncState = (struct SynchronizationState*)(getsectiondata(_NSGetMachExecuteHeader(), "__DATA", "__syncState", &sectSize));

    if (!syncState) {
        fprintf(stderr, "FAIL: Missing sync state\n");
        exit(-1);
    }
    // Signal the main thread so it resumes
    int err = pthread_mutex_lock(&syncState->mutex);
    if (err != 0) {
        fprintf(stderr, "FAIL: pthread_mutex_lock(%u): %s\n", err, strerror(err));
        exit(-2);
    }
    if (pthread_cond_signal(&syncState->libraryInInitializer) != 0) {
        fprintf(stderr, "FAIL: pthread_cond_signal\n");
        exit(-3);
    }
    if (pthread_cond_wait(&syncState->mainExecutableFinishedDyld, &syncState->mutex) != 0) {
        fprintf(stderr, "FAIL: pthread_cond_wait\n");
        exit(-4);
    }
    if (pthread_mutex_unlock(&syncState->mutex) != 0) {
        fprintf(stderr, "FAIL: pthread_mutex_unlock\n");
        exit(-5);
    }
}

