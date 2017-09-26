/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <dirent.h>
#include <libgen.h>
#include <pthread.h>
#include <fts.h>

#include <vector>
#include <array>
#include <set>
#include <map>
#include <unordered_set>
#include <algorithm>

#include <spawn.h>

#include <Bom/Bom.h>

#include "Manifest.h"
#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "BuilderUtils.h"
#include "FileUtils.h"
#include "StringUtils.h"
#include "MachOParser.h"

#if !__has_feature(objc_arc)
#error The use of libdispatch in this files requires it to be compiled with ARC in order to avoid leaks
#endif

extern char** environ;

static dispatch_queue_t build_queue;

static const char* tempRootDirTemplate = "/tmp/dyld_shared_cache_builder.XXXXXX";
static char*       tempRootDir = nullptr;

int runCommandAndWait(Diagnostics& diags, const char* args[])
{
    pid_t pid;
    int   status;
    int   res = posix_spawn(&pid, args[0], nullptr, nullptr, (char**)args, environ);
    if (res != 0)
        diags.error("Failed to spawn %s: %s (%d)", args[0], strerror(res), res);

    do {
        res = waitpid(pid, &status, 0);
    } while (res == -1 && errno == EINTR);
    if (res != -1) {
        if (WIFEXITED(status)) {
            res = WEXITSTATUS(status);
        } else {
            res = -1;
        }
    }

    return res;
}

void processRoots(Diagnostics& diags, std::set<std::string>& roots)
{
    std::set<std::string> processedRoots;
    struct stat           sb;
    int                   res = 0;
    const char*           args[8];

    for (const auto& root : roots) {
        res = stat(root.c_str(), &sb);

        if (res == 0 && S_ISDIR(sb.st_mode)) {
            roots.insert(root);
            return;
        } else if (endsWith(root, ".cpio") || endsWith(root, ".cpio.gz") || endsWith(root, ".cpgz") || endsWith(root, ".cpio.bz2") || endsWith(root, ".cpbz2") || endsWith(root, ".pax") || endsWith(root, ".pax.gz") || endsWith(root, ".pgz") || endsWith(root, ".pax.bz2") || endsWith(root, ".pbz2")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-x";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else if (endsWith(root, ".tar")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.gz") || endsWith(root, ".tgz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xzf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".tar.bz2")
            || endsWith(root, ".tbz2")
            || endsWith(root, ".tbz")) {
            args[0] = (char*)"/usr/bin/tar";
            args[1] = (char*)"xjf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".xar")) {
            args[0] = (char*)"/usr/bin/xar";
            args[1] = (char*)"-xf";
            args[2] = (char*)root.c_str();
            args[3] = (char*)"-C";
            args[4] = tempRootDir;
            args[5] = nullptr;
        } else if (endsWith(root, ".zip")) {
            args[0] = (char*)"/usr/bin/ditto";
            args[1] = (char*)"-xk";
            args[2] = (char*)root.c_str();
            args[3] = tempRootDir;
            args[4] = nullptr;
        } else {
            diags.error("unknown archive type: %s", root.c_str());
            continue;
        }

        if (res != runCommandAndWait(diags, args)) {
            fprintf(stderr, "Could not expand archive %s: %s (%d)", root.c_str(), strerror(res), res);
            exit(-1);
        }
        for (auto& existingRoot : processedRoots) {
            if (existingRoot == tempRootDir)
                return;
        }

        processedRoots.insert(tempRootDir);
    }

    roots = processedRoots;
}

bool writeRootList(const std::string& dstRoot, const std::set<std::string>& roots)
{
    if (roots.size() == 0)
        return false;

    std::string rootFile = dstRoot + "/roots.txt";
    FILE*       froots = ::fopen(rootFile.c_str(), "w");
    if (froots == NULL)
        return false;

    for (auto& root : roots) {
        fprintf(froots, "%s\n", root.c_str());
    }

    ::fclose(froots);
    return true;
}

std::set<std::string> cachePaths;

BOMCopierCopyOperation filteredCopy(BOMCopier copier, const char* path, BOMFSObjType type, off_t size)
{
    std::string absolutePath = &path[1];
    if (cachePaths.count(absolutePath)) {
        return BOMCopierSkipFile;
    }
    return BOMCopierContinue;
}

int main(int argc, const char* argv[])
{
    @autoreleasepool {
        __block Diagnostics   diags;
        std::set<std::string> roots;
        std::string           dylibCacheDir;
        std::string           release;
        bool                  emitDevCaches = true;
        bool                  emitElidedDylibs = true;
        bool                  listConfigs = false;
        bool                  copyRoots = false;
        bool                  debug = false;
        std::string           dstRoot;
        std::string           configuration;
        std::string           resultPath;

        tempRootDir = strdup(tempRootDirTemplate);
        mkdtemp(tempRootDir);

        for (int i = 1; i < argc; ++i) {
            const char* arg = argv[i];
            if (arg[0] == '-') {
                if (strcmp(arg, "-debug") == 0) {
                    diags = Diagnostics(true);
                    debug = true;
                } else if (strcmp(arg, "-list_configs") == 0) {
                    listConfigs = true;
                } else if (strcmp(arg, "-root") == 0) {
                    roots.insert(realPath(argv[++i]));
                } else if (strcmp(arg, "-copy_roots") == 0) {
                    copyRoots = true;
                } else if (strcmp(arg, "-dylib_cache") == 0) {
                    dylibCacheDir = realPath(argv[++i]);
                } else if (strcmp(arg, "-no_development_cache") == 0) {
                    emitDevCaches = false;
                } else if (strcmp(arg, "-no_overflow_dylibs") == 0) {
                    emitElidedDylibs = false;
                } else if (strcmp(arg, "-development_cache") == 0) {
                    emitDevCaches = true;
                } else if (strcmp(arg, "-overflow_dylibs") == 0) {
                    emitElidedDylibs = true;
                } else if (strcmp(arg, "-dst_root") == 0) {
                    dstRoot = realPath(argv[++i]);
                } else if (strcmp(arg, "-release") == 0) {
                    release = argv[++i];
                } else if (strcmp(arg, "-results") == 0) {
                    resultPath = realPath(argv[++i]);
                } else {
                    //usage();
                    diags.error("unknown option: %s\n", arg);
                }
            } else {
                if (!configuration.empty()) {
                    diags.error("You may only specify one configuration");
                }
                configuration = argv[i];
            }
        }

        time_t mytime = time(0);
        fprintf(stderr, "Started: %s", asctime(localtime(&mytime)));
        processRoots(diags, roots);

        struct rlimit rl = { OPEN_MAX, OPEN_MAX };
        (void)setrlimit(RLIMIT_NOFILE, &rl);

        if (dylibCacheDir.empty() && release.empty()) {
            fprintf(stderr, "you must specify either -dylib_cache or -release");
            exit(-1);
        } else if (!dylibCacheDir.empty() && !release.empty()) {
            fprintf(stderr, "you may not use -dylib_cache and -release at the same time");
            exit(-1);
        }

        if ((configuration.empty() || dstRoot.empty()) && !listConfigs) {
            fprintf(stderr, "Must specify a configuration and a valid -dst_root OR -list_configs\n");
            exit(-1);
        }

        if (dylibCacheDir.empty()) {
            dylibCacheDir = std::string("/AppleInternal/Developer/DylibCaches/") + release + ".dlc";
        }

        //Move into the dir so we can use relative path manifests
        chdir(dylibCacheDir.c_str());

        dispatch_async(dispatch_get_main_queue(), ^{
            auto manifest = dyld3::Manifest(diags, dylibCacheDir + "/Manifest.plist", roots);

            if (manifest.build().empty()) {
                fprintf(stderr, "No manifest found at '%s/Manifest.plist'\n", dylibCacheDir.c_str());
                exit(-1);
            }
            fprintf(stderr, "Building Caches for %s\n", manifest.build().c_str());

            if (listConfigs) {
                manifest.forEachConfiguration([](const std::string& configName) {
                    printf("%s\n", configName.c_str());
                });
            }

            if (!manifest.filterForConfig(configuration)) {
                fprintf(stderr, "No config %s. Please run with -list_configs to see configurations available for this %s.\n",
                    configuration.c_str(), manifest.build().c_str());
                exit(-1);
            }
            manifest.calculateClosure();

            std::vector<dyld3::BuildQueueEntry> buildQueue;

            bool cacheBuildSuccess = build(diags, manifest, dstRoot, false, debug, false, false);

            if (!cacheBuildSuccess) {
                exit(-1);
            }

            writeRootList(dstRoot, roots);

            if (copyRoots) {
                manifest.forEachConfiguration([&manifest](const std::string& configName) {
                    for (auto& arch : manifest.configuration(configName).architectures) {
                        for (auto& dylib : arch.second.results.dylibs) {
                            if (dylib.second.included) {
                                dyld3::MachOParser parser = manifest.parserForUUID(dylib.first);
                                cachePaths.insert(parser.installName());
                            }
                        }
                    }
                });

                BOMCopier copier = BOMCopierNewWithSys(BomSys_default());
                BOMCopierSetCopyFileStartedHandler(copier, filteredCopy);
                for (auto& root : roots) {
                    BOMCopierCopy(copier, root.c_str(), dstRoot.c_str());
                }
                BOMCopierFree(copier);
            }

            


            int err = sync_volume_np(dstRoot.c_str(), SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT);
            if (err) {
                fprintf(stderr, "Volume sync failed errnor=%d (%s)\n", err, strerror(err));
            }

            // Create an empty FIPS data in the root
            (void)mkpath_np((dstRoot + "/private/var/db/FIPS/").c_str(), 0755);
            int fd = open((dstRoot + "/private/var/db/FIPS/fips_data").c_str(), O_CREAT | O_TRUNC, 0644);
            close(fd);

            // Now that all the build commands have been issued lets put a barrier in after then which can tear down the app after
            // everything is written.

            if (!resultPath.empty()) {
                manifest.write(resultPath);
            }

            const char* args[8];
            args[0] = (char*)"/bin/rm";
            args[1] = (char*)"-rf";
            args[2] = (char*)tempRootDir;
            args[3] = nullptr;
            (void)runCommandAndWait(diags, args);

            for (const std::string& warn : diags.warnings()) {
                fprintf(stderr, "dyld_shared_cache_builder: warning: %s\n", warn.c_str());
            }
            exit(0);
        });
    }

    dispatch_main();

    return 0;
}
