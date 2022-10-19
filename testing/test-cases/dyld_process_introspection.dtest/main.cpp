// BOOT_ARGS: dyld_flags=0x00010000

// BUILD:  $CC target.c          -o $BUILD_DIR/target.exe -framework CoreFoundation -DRUN_DIR="$RUN_DIR"
// BUILD:  $CC foo.c             -o $BUILD_DIR/libfoo.bundle -bundle
// BUILD:  $CXX main.cpp         -std=c++17 -o $BUILD_DIR/dyld_process_introspection.exe -DRUN_DIR="$RUN_DIR"
// BUILD:  $TASK_FOR_PID_ENABLE  $BUILD_DIR/dyld_process_introspection.exe

// RUN:  $SUDO ./dyld_process_introspection.exe

#include <Block.h>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <sys/socket.h>
#include <mach/machine.h>
#include <mach-o/dyld_introspection.h>
#include <Availability.h>

#include "test_support.h"


static void inspectProcess(task_t task, bool launchedSuspended, bool expectCF)
{
    kern_return_t result;
    dyld_process_t process = dyld_process_create_for_task(task, &result);
    if (result != KERN_SUCCESS) {
        FAIL("dyld_process_create_for_task() should succeed, get return code %d", result);
    }
    if (process == NULL) {
        FAIL("dyld_process_create_for_task(task, 0) alwats return a value");
    }
    dyld_process_snapshot_t snapshot = dyld_process_snapshot_create_for_process(process, &result);
    dyld_process_dispose(process);
    if (result != KERN_SUCCESS) {
        FAIL("dyld_process_snapshot_create_for_process() should succeed, got return code %d", result);
    }
    if (process == NULL) {
        FAIL("dyld_process_snapshot_create_for_process(process, 0) alwats return a value");
    }

    __block bool foundDyld = false;
    __block bool foundMain = false;
    __block bool foundCF = false;

    dyld_process_snapshot_for_each_image(snapshot, ^(dyld_image_t image) {
        const char* path = dyld_image_get_file_path(image);
        const char* installname = dyld_image_get_installname(image);
        if ( installname && (strcmp(installname, "/usr/lib/dyld") == 0)) {
            foundDyld = true;
        }
        if ( path && strstr(path, "/target.exe") != NULL ) {
            foundMain = true;
        }
        if ( path && (strstr(path, "/dyld_process_introspection.exe") != NULL )) {
            foundMain = true;
        }
        if ( installname && strstr(installname, "/CoreFoundation.framework/") != NULL ) {
            foundCF = true;
        }
        __block bool isFirstSegment = true;
        __block uint64_t textAddr = 0;
        __block uint64_t textSize = 0;
        dyld_image_for_each_segment_info(image, ^(const char *segmentName, uint64_t vmAddr, uint64_t vmSize, int perm) {
            if ( isFirstSegment && vmAddr == 0 ) {
                char* displayPath = NULL;
                if ( path )
                    displayPath = (char*)path;
                if ( installname )
                    displayPath = (char*)installname;
                FAIL("dyld_image_for_each_segment_info returned incorrect vmAddr: %s(%s): (0x%llx - 0x%llx)", displayPath, segmentName, vmAddr, vmAddr+vmSize);
            }
            isFirstSegment = false;
            if (strcmp(segmentName, "__TEXT") == 0) {
                textAddr = vmAddr;
                textSize = vmSize;
                char* displayPath = NULL;
                if ( path )
                    displayPath = (char*)path;
                if ( installname )
                    displayPath = (char*)installname;
//                fprintf(stderr,"%s 0x%llx 0x%llx\n", displayPath, vmAddr, vmAddr);
                if (perm != (PROT_EXEC | PROT_READ) ) {
                    FAIL("dyld_image_for_each_segment_info returned incorrect permissions for __TEXT segment: 0x%lx)", perm);
                }
            }
        });
        __block bool didRunTEXTCallback = false;
        dyld_image_content_for_segment(image, "__TEXT", ^(const void *content, uint64_t vmAddr, uint64_t vmSize) {
            didRunTEXTCallback = true;
            auto mh = (const mach_header*)content;
            if (mh->magic != MH_MAGIC_64 && mh->magic != MH_MAGIC) {
                FAIL("dyld_image_content_for_segment returned incorrect magic: 0x%lx", mh->magic);
            }
            if (vmAddr != textAddr) {
                FAIL("dyld_image_content_for_segment returned incorrect vmAddr: 0x%llx, expected 0x%llx", vmAddr, textAddr);
            }
            if (vmAddr != textAddr) {
                FAIL("dyld_image_content_for_segment returned incorrect vmSize: 0x%llx, expected 0x%llx", vmSize, textSize);
            }
        });
        if( !didRunTEXTCallback ) {
            FAIL("callback did not run for __TEXT segment");
        }
    });

    if (!foundDyld) { FAIL("dyld should always be in the image list"); }
    if (!foundMain) { FAIL("The main executable should always be in the image list"); }
    if (expectCF && !foundCF) { FAIL("CF should be in the image list"); }

     dyld_process_snapshot_dispose(snapshot);
}

void waitForTargetCheckin(pid_t pid) {
    dispatch_queue_t queue = dispatch_queue_create("com.apple.test.dyld_process_introspection", NULL);
    // We do this instead of using a dispatch_semaphore to prevent priority inversions
    dispatch_block_t oneShotSemaphore = dispatch_block_create(DISPATCH_BLOCK_INHERIT_QOS_CLASS, ^{});
    dispatch_source_t signalSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1,
                                                            0, queue);
    dispatch_source_set_event_handler(signalSource, ^{
        LOG("Received signal");
        oneShotSemaphore();
        dispatch_source_cancel(signalSource);
    });
    dispatch_resume(signalSource);
    kill(pid, SIGCONT);
    dispatch_block_wait(oneShotSemaphore, DISPATCH_TIME_FOREVER);
    dispatch_release(queue);
}

static std::pair<_process,task_t> launchTarget(bool launchSuspended) {
    LOG("launchTarget %s", launchSuspended ? "suspended" : "unsuspended");
    const char * program = RUN_DIR "/target.exe";

    _process process;
    process.set_executable_path(RUN_DIR "/target.exe");
    process.set_launch_suspended(true);
    const char* env[] = { "TEST_OUTPUT=None", NULL};
    process.set_env(env);
    pid_t pid = process.launch();
    LOG("launchTarget pid: %d", pid);

    task_t task;
    if (task_read_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
        FAIL("task_read_for_pid() failed");
    }
    LOG("launchTarget task: %u", task);

    // wait until process is up and has suspended itself
    if (!launchSuspended) {
        waitForTargetCheckin(pid);
    }
    LOG("task running");
    return {process, task};
}

static void testNotifications(task_t task, pid_t pid) {
    kern_return_t result;
    dyld_process_t process = dyld_process_create_for_task(task, &result);
    if (result != KERN_SUCCESS) {
        FAIL("dyld_process_create_for_task() should succeed, get return code %d", result);
    }
    if (process == NULL) {
        FAIL("dyld_process_create_for_task(task, 0) alwats return a value");
    }
    // We do this instead of using a dispatch_semaphore to prevent priority inversions
    dispatch_block_t oneShotSemaphore = dispatch_block_create(DISPATCH_BLOCK_INHERIT_QOS_CLASS, ^{});
    dispatch_block_t mainReady = dispatch_block_create(DISPATCH_BLOCK_INHERIT_QOS_CLASS, ^{});
    dispatch_queue_t queue = dispatch_queue_create("com.apple.test.dyld_process_introspection.notifier", NULL);
    __block bool loadedCF = false;
    __block bool initializersReadyNotificationFired = false;
    __block bool mainReadyNotificationFired = false;
    __block uint32_t dlopenCount = 0;
    __block uint32_t dlcloseCount = 0;
    kern_return_t kr = KERN_SUCCESS;
    uint32_t mainHandle = dyld_process_register_for_event_notification(process, &kr, DYLD_REMOTE_EVENT_MAIN, queue, ^{
        mainReadyNotificationFired = true;
        mainReady();
    });
    if (kr != KERN_SUCCESS) {
        FAIL("dyld_process_register_for_event_notification() should succeed, got return code %d", kr);
    }
    uint32_t cacheHandle = dyld_process_register_for_event_notification(process, &kr, DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS, queue, ^{
        initializersReadyNotificationFired = true;
    });
    if (kr != KERN_SUCCESS) {
        FAIL("dyld_process_register_for_event_notification() should succeed, got return code %d", kr);
    }
    uint32_t updateHandle = dyld_process_register_for_image_notifications(process, &kr, queue, ^(dyld_image_t image, bool load) {
        const char* installname = dyld_image_get_installname(image);
        const char* filename = dyld_image_get_file_path(image);
        if ( load && installname && strstr(installname, "/CoreFoundation.framework/") != NULL ) {
            loadedCF = true;
        }
        if ( mainReadyNotificationFired && filename && strstr(filename, "/libfoo.bundle") != NULL ) {
            if (load) {
                ++dlopenCount;
            } else {
                ++dlcloseCount;
            }
        }
        // TODO: This should be explicitly send over the socket, but the test infra needs improvements
        if (dlcloseCount == 999) {
            oneShotSemaphore();
        }
    });
    if (kr != KERN_SUCCESS) {
        FAIL("dyld_process_register_for_image_notifications() should succeed, got return code %d", kr);
    }
    waitForTargetCheckin(pid);
    dispatch_block_wait(oneShotSemaphore, DISPATCH_TIME_FOREVER);
    if (!loadedCF) { FAIL("CF should be loaded"); }
    if (!initializersReadyNotificationFired) { FAIL("initializers ready notification should fire"); }
    if (!mainReadyNotificationFired) { FAIL("Main ready notification should fire"); }
    if (dlopenCount != 999) { FAIL("libfoo should be dlopen()ed 999 times"); }
    if (dlcloseCount != 999) { FAIL("libfoo should be dlclosed()ed 999 times"); }
    dyld_process_unregister_for_notification(process, mainHandle);
    dyld_process_unregister_for_notification(process, cacheHandle);
    dyld_process_unregister_for_notification(process, updateHandle);
    dyld_process_dispose(process);
    dispatch_release(queue);
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[]) {
    signal(SIGUSR1, SIG_IGN);
    inspectProcess(mach_task_self(), false, false);
    //FIXME: The signals in these tests deadlock, need to rework them or move to sockets and a state machine

//    {
//        auto [process, task] = launchTarget(true);
//        testNotifications(task, process.get_pid());
//    }
//    {
//        auto [process, task] = launchTarget(true);
//        inspectProcess(task, true, false);
//    }
//    {
//        auto [process, task] = launchTarget(false);
//        inspectProcess(task, false, true);
//    }
    PASS("SUCCESS");
}
