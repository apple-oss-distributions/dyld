#include <dlfcn.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <dispatch/dispatch.h>

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    dispatch_async(dispatch_get_main_queue(), ^{
        signal(SIGUSR1, SIG_IGN);
    });
    dispatch_source_t exitSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, getppid(),
                                                            DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    dispatch_source_set_event_handler(exitSource, ^{
        exit(0);
    });
    dispatch_resume(exitSource);

    // Setup SIGUSR1 handler
    dispatch_source_t signalSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(signalSource, ^{
        for (int i=1; i < 1000; ++i) {
            void* h = dlopen(RUN_DIR "/libfoo.bundle", 0);
            dlclose(h);
        }
    });
    dispatch_resume(signalSource);
    dispatch_main();
}

