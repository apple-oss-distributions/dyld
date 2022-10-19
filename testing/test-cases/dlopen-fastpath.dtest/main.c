
// BUILD:  $CC foo.c -bundle               -o $BUILD_DIR/test.bundle
// BUILD:  $CC main.c -DRUN_DIR="$RUN_DIR" -o $BUILD_DIR/dlopen-fastpath.exe

// RUN:  ./dlopen-fastpath.exe

#include <dlfcn.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/dyld_process_info.h>
#include <pthread.h>
#include <stdio.h>

#include "test_support.h"

bool sDoneLaunchImageNotifiers = false;

// This method runs on another thread
static void* work(void* arg)
{
    void* handle = dlopen("/usr/lib/system/libsystem_platform.dylib", RTLD_NOW);
    if ( handle == NULL ) {
        FAIL("Couldn't dlopen libsystem_platform.dylib because: %s", dlerror());
    }
    return 0;
}

static void notify(const struct mach_header* mh, intptr_t vmaddr_slide)
{
    if ( !sDoneLaunchImageNotifiers )
        return;

    // If we hit here, then we are doing the notifier for test.bundle, which is being dlopen()ed from the main thread

    // Spawn a thread to do another dlopen
    static pthread_t worker;
    int result = pthread_create(&worker, NULL, work, NULL);
    if ( result != 0 ) {
        FAIL("work pthread_create failed because %d\n", result);
        return;
    }

    void* dummy;
    pthread_join(worker, &dummy);
}

static void* timeoutWork(void* arg)
{
    sleep(5);
    FAIL("Timeout, probably because dyld hash doesn't match\n");
    exit(1);
    return 0;
}

// The dlopen fast path only works if dyld can use the shared cache prebuilt loaders.
// That is only possible if dyld and the shared cache prebuilt loader hash matches, so in the
// case of a new dyld, we might have a mismatch.  Detect this and bail out
static void exitOnMismatchedHash()
{
    size_t sharedCacheLength;
    uintptr_t sharedCacheBaseAddress = (uintptr_t)_dyld_get_shared_cache_range(&sharedCacheLength);
    if ( sharedCacheBaseAddress == 0 )
        PASS("Success");

    kern_return_t result;
    dyld_process_info info = _dyld_process_info_create(mach_task_self(), 0, &result);
    if (result != KERN_SUCCESS) {
        FAIL("dyld_process_info() should succeed, get return code %d", result);
    }
    if (info == NULL) {
        FAIL("dyld_process_info(task, 0) always returns a value");
    }

    __block bool foundDyld = false;
    __block uintptr_t dyldLoadAddress = 0;
    _dyld_process_info_for_each_image(info, ^(uint64_t machHeaderAddress, const uuid_t uuid, const char* path) {
        if ( strstr(path, "/dyld") != NULL ) {
            foundDyld = true;
            dyldLoadAddress = (uintptr_t)machHeaderAddress;
        }
    });
    _dyld_process_info_release(info);

    if (!foundDyld) {
        FAIL("dyld should always be in the image list");
    }

    if ( dyldLoadAddress == 0 ) {
        FAIL("dyld __TEXT not found");
    }

    // This is horrible.  We're going to interpret the handle to work out if its in the shared cache
    // The handle is:
    //   void* handle = (void*)(((uintptr_t)ldr ^ dyldStart) | flags);
    //   handle = ptrauth_sign_unauthenticated(handle, ptrauth_key_process_dependent_data, ptrauth_string_discriminator("dlopen"));
    void* handle = dlopen("/usr/lib/system/libsystem_platform.dylib", RTLD_NOW);
    if ( handle == NULL ) {
        FAIL("Couldn't dlopen libsystem_platform.dylib because: %s", dlerror());
    }

    // Stolen from dyld
    // static const Loader* loaderFromHandle(void* h, bool& firstOnly)
    // {
    //     uintptr_t dyldStart  = (uintptr_t)&__dso_handle;
    // #if __has_feature(ptrauth_calls)
    //     if ( h != nullptr ) {
    //         // Note we don't use ptrauth_auth_data, as we don't want to crash on bad handles
    //         void* strippedHandle = ptrauth_strip(h, ptrauth_key_process_dependent_data);
    //         void* validHandle = ptrauth_sign_unauthenticated(strippedHandle, ptrauth_key_process_dependent_data, ptrauth_string_discriminator("dlopen"));
    //         if ( h == validHandle )
    //             h = strippedHandle;
    //     }
    // #endif
    //     firstOnly = (((uintptr_t)h) & 1);
    //     return (Loader*)((((uintptr_t)h) & ~1) ^ dyldStart);
    // }

#if __has_feature(ptrauth_calls)
    handle = ptrauth_strip(handle, ptrauth_key_process_dependent_data);
#endif

    uintptr_t loaderAddress = (uintptr_t)((((uintptr_t)handle) & ~1) ^ dyldLoadAddress);
    if ( loaderAddress < sharedCacheBaseAddress )
        PASS("Success");
    if ( loaderAddress >= (sharedCacheBaseAddress + sharedCacheLength) )
        PASS("Success");
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {

    exitOnMismatchedHash();

    // Get notified about all initial images
    _dyld_register_func_for_add_image(&notify);
    sDoneLaunchImageNotifiers = true;

    // Spawn a thread quit if we take too long
    static pthread_t worker;
    int result = pthread_create(&worker, NULL, timeoutWork, NULL);
    if ( result != 0 ) {
        FAIL("timeoutWork pthread_create failed because %d\n", result);
        return 1;
    }

    // dlopen something new on the main thread.  The worker thread will run the notifier for this
    void* handle = dlopen(RUN_DIR "/test.bundle", RTLD_NOW);

    // If we got here then we didn't deadlock, so kill the timeout thread
    pthread_cancel(worker);

    PASS("Success");
}

