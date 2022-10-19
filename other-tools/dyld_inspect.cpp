
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <libproc.h>
#include <mach/mach_init.h>
#include <mach/mach_types.h>
#include <System/sys/proc.h>
#include <uuid/uuid.h>

#include <mach-o/dyld_introspection.h>

static void usage()
{
    fprintf(stderr, "Usage: dyld_inspect <options>* -p pid | -all \n"
            "\t-shared_cache_uuid       print shared cache UUID\n"
            "\t-shared_cache_address    print shared cache base address\n"
            //"\t-shared_cache_path       print shared cache path\n"
            "\t-shared_cache            print all shared cache options\n"
        );
}

int main(int argc, const char* argv[], const char* envp[], const char* apple[])
{
    if ( argc == 1 ) {
        usage();
        return 1;
    }

    bool allProcesses = false;
    int specificProcessPID = 0;
    bool printSharedCacheUUID = false;
    bool printSharedCacheAddress = false;
    //bool printSharedCachePath = false;
    bool gotOption = false;
    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-shared_cache_uuid") == 0 ) {
            printSharedCacheUUID = true;
            gotOption = true;
        }
        else if ( strcmp(arg, "-shared_cache_address") == 0 ) {
            printSharedCacheAddress = true;
            gotOption = true;
        }
        // else if ( strcmp(arg, "-shared-cache-path") == 0 ) {
        //     printSharedCachePath = true;
        //     gotOption = true;
        // }
        else if ( strcmp(arg, "-shared_cache") == 0 ) {
            printSharedCacheUUID = true;
            printSharedCacheAddress = true;
            // printSharedCachePath = true;
            gotOption = true;
        }
        else if ( strcmp(arg, "-p") == 0 ) {
            if ( ++i < argc ) {
                specificProcessPID = atoi(argv[i]);
            }
            else {
                fprintf(stderr, "-p missing process PID");
                return 1;
            }
        }
        else if ( strcmp(arg, "-all") == 0 ) {
            allProcesses = true;
        }
        else if ( strcmp(arg, "-help") == 0 ) {
            usage();
            return 0;
        }
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if ( !allProcesses && (specificProcessPID == 0) ) {
        fprintf(stderr, "expected -p PID, or -all flag\n");
        return 1;
    }

    if ( !gotOption ) {
        fprintf(stderr, "expected print option\n");
        usage();
        return 1;
    }

    auto handleProcess = ^(int pid, bool exitOnError) {
        task_t task;
        if (task_read_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
            if ( exitOnError ) {
                fprintf(stderr, "task_read_for_pid(%d) failed due to: %s\n", pid, strerror(errno));
                if ( geteuid() != 0 ) {
                    fprintf(stderr, "note: you may want try again as root\n");
                }
                exit(1);
            }
            return;
        }

        kern_return_t kr = KERN_SUCCESS;
        dyld_process_t dyldProcess = dyld_process_create_for_task(task, &kr);
        if (  !dyldProcess ) {
            if ( exitOnError ) {
                fprintf(stderr, "dyld_process_create_for_task(pid = %d) failed due to: %d\n", pid, kr);
                exit(1);
            }
            return;
        }
        dyld_process_snapshot_t dyldSnapshot = dyld_process_snapshot_create_for_process(dyldProcess, &kr);
        if ( !dyldSnapshot ) {
            if ( exitOnError ) {
                fprintf(stderr, "dyld_process_snapshot_create_for_process(pid = %d) failed due to: %d\n", pid, kr);
                exit(1);
            }
            return;
        }

        // Get the shared cache data
        dyld_shared_cache_t dyldSharedCache = dyld_process_snapshot_get_shared_cache(dyldSnapshot);
        if ( !dyldSharedCache ) {
            if ( exitOnError ) {
                fprintf(stderr, "dyld_process_snapshot_get_shared_cache(pid = %d) failed\n", pid);
                exit(1);
            }
            return;
        }
        uint64_t cacheBaseAddress = dyld_shared_cache_get_base_address(dyldSharedCache);
        uuid_t cacheUUID;
        uuid_clear(cacheUUID);
        dyld_shared_cache_copy_uuid(dyldSharedCache, &cacheUUID);

        // All done.  Free the data structures
        dyld_process_snapshot_dispose(dyldSnapshot);
        dyld_process_dispose(dyldProcess);

        bool printSeparator = false;
        if ( allProcesses ) {
            if ( printSeparator ) printf("  ");
            printSeparator = true;

            printf("% 6d", pid);
        }
        if ( printSharedCacheUUID ) {
            if ( printSeparator ) printf("  ");
            printSeparator = true;

            uuid_string_t uuidString;
            uuid_unparse_upper(cacheUUID, uuidString);
            printf("%s", uuidString);
        }
        if ( printSharedCacheAddress ) {
            if ( printSeparator ) printf("  ");
            printSeparator = true;

            printf("0x%08llx", cacheBaseAddress);
        }
        printf("\n");
    };

    if ( allProcesses ) {
        pid_t pids[2048];
        int pidcount = proc_listallpids(pids, sizeof(pids));
        if ( pidcount == -1 ) {
            fprintf(stderr, "failed to get list of processes due to: %s\n", strerror(errno));
            return 1;
        }

        for ( int i=0; i < pidcount; ++i ) {
            handleProcess(pids[i], false);
        }
    } else {
        handleProcess(specificProcessPID, true);
    }

    return 0;
}


