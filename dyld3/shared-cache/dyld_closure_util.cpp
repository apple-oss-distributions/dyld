/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
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


#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syslimits.h>
#include <mach-o/arch.h>
#include <mach-o/loader.h>
#include <mach-o/dyld_priv.h>
#include <bootstrap.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>

#include <map>
#include <vector>

#include "LaunchCache.h"
#include "LaunchCacheWriter.h"
#include "DyldSharedCache.h"
#include "FileUtils.h"
#include "ImageProxy.h"
#include "StringUtils.h"
#include "ClosureBuffer.h"

extern "C" {
    #include "closuredProtocol.h"
}

static const DyldSharedCache* mapCacheFile(const char* path)
{
    struct stat statbuf;
    if (stat(path, &statbuf)) {
        fprintf(stderr, "Error: stat failed for dyld shared cache at %s\n", path);
        return nullptr;
    }
        
    int cache_fd = open(path, O_RDONLY);
    if (cache_fd < 0) {
        fprintf(stderr, "Error: failed to open shared cache file at %s\n", path);
        return nullptr;
    }
    
    void* mapped_cache = mmap(NULL, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (mapped_cache == MAP_FAILED) {
        fprintf(stderr, "Error: mmap() for shared cache at %s failed, errno=%d\n", path, errno);
        return nullptr;
    }
    close(cache_fd);

    return (DyldSharedCache*)mapped_cache;
}

struct CachedSections
{
    uint32_t            mappedOffsetStart;
    uint32_t            mappedOffsetEnd;
    uint64_t            vmAddress;
    const mach_header*  mh;
    std::string         segmentName;
    std::string         sectionName;
    const char*         dylibPath;
};

static const CachedSections& find(uint32_t mappedOffset, const std::vector<CachedSections>& sections)
{
    for (const CachedSections& entry : sections) {
        //printf("0x%08X -> 0x%08X\n", entry.mappedOffsetStart, entry.mappedOffsetEnd);
        if ( (entry.mappedOffsetStart <= mappedOffset) && (mappedOffset < entry.mappedOffsetEnd) )
            return entry;
    }
    assert(0 && "invalid offset");
}

/*
static const dyld3::launch_cache::BinaryClosureData*
callClosureDaemon(const std::string& mainPath, const std::string& cachePath, const std::vector<std::string>& envArgs)
{

    mach_port_t   serverPort = MACH_PORT_NULL;
    mach_port_t   bootstrapPort = MACH_PORT_NULL;
    kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bootstrapPort);
    kr = bootstrap_look_up(bootstrapPort, "com.apple.dyld.closured", &serverPort);
    switch( kr ) {
        case BOOTSTRAP_SUCCESS :
            // service currently registered, "a good thing" (tm)
            break;
        case BOOTSTRAP_UNKNOWN_SERVICE :
            // service not currently registered, try again later
            fprintf(stderr, "bootstrap_look_up(): %s\n", mach_error_string(kr));
            return nullptr;
        default:
            // service not currently registered, try again later
            fprintf(stderr, "bootstrap_look_up(): %s [%d]\n", mach_error_string(kr), kr);
            return nullptr;
    }

    //printf("serverPort=%d, replyPort=%d\n", serverPort, replyPort);



    bool success;
    char envBuffer[2048];
    vm_offset_t reply = 0;
    uint32_t  replySize = 0;
    envBuffer[0] = '\0';
    envBuffer[1] = '\0';
//    kr = closured_CreateLaunchClosure(serverPort, mainPath.c_str(), cachePath.c_str(), uuid, envBuffer, &success, &reply, &replySize);

    printf("success=%d, buf=%p, bufLen=%d\n", success, (void*)reply, replySize);

    if (!success)
        return nullptr;
    return (const dyld3::launch_cache::BinaryClosureData*)reply;
}
*/

static void usage()
{
    printf("dyld_closure_util program to create of view dyld3 closures\n");
    printf("  mode:\n");
    printf("    -create_closure <prog-path>            # create a closure for the specified main executable\n");
    printf("    -create_image_group <dylib-path>       # create an ImageGroup for the specified dylib/bundle\n");
    printf("    -list_dyld_cache_closures              # list all closures in the dyld shared cache with size\n");
    printf("    -list_dyld_cache_other_dylibs          # list all group-1 (non-cached dylibs/bundles)\n");
    printf("    -print_image_group <closure-path>      # print specified ImageGroup file as JSON\n");
    printf("    -print_closure_file <closure-path>     # print specified closure file as JSON\n");
    printf("    -print_dyld_cache_closure <prog-path>  # find closure for specified program in dyld cache and print as JSON\n");
    printf("    -print_dyld_cache_dylibs               # print group-0 (cached dylibs) as JSON\n");
    printf("    -print_dyld_cache_other_dylibs         # print group-1 (non-cached dylibs/bundles) as JSON\n");
    printf("    -print_dyld_cache_other <path>         # print just one group-1 (non-cached dylib/bundle) as JSON\n");
    printf("    -print_dyld_cache_patch_table          # print locations in shared cache that may need patching\n");
    printf("  options:\n");
    printf("    -cache_file <cache-path>               # path to cache file to use (default is current cache)\n");
    printf("    -build_root <path-prefix>              # when building a closure, the path prefix when runtime volume is not current boot volume\n");
    printf("    -o <output-file>                       # when building a closure, the file to write the (binary) closure to\n");
    printf("    -include_all_dylibs_in_dir             # when building a closure, add other mach-o files found in directory\n");
    printf("    -env <var=value>                       # when building a closure, DYLD_* env vars to assume\n");
    printf("    -dlopen <path>                         # for use with -create_closure to append ImageGroup if target had called dlopen\n");
    printf("    -verbose_fixups                        # for use with -print* options to force printing fixups\n");
}

int main(int argc, const char* argv[])
{
    const char*               cacheFilePath = nullptr;
    const char*               inputMainExecutablePath = nullptr;
    const char*               inputTopImagePath = nullptr;
    const char*               outPath = nullptr;
    const char*               printPath = nullptr;
    const char*               printGroupPath = nullptr;
    const char*               printCacheClosure = nullptr;
    const char*               printCachedDylib = nullptr;
    const char*               printOtherDylib = nullptr;
    bool                      listCacheClosures = false;
    bool                      listOtherDylibs = false;
    bool                      includeAllDylibs = false;
    bool                      printClosures = false;
    bool                      printCachedDylibs = false;
    bool                      printOtherDylibs = false;
    bool                      printPatchTable = false;
    bool                      useClosured = false;
    bool                      verboseFixups = false;
    std::vector<std::string>  buildtimePrefixes;
    std::vector<std::string>  envArgs;
    std::vector<const char*>  dlopens;

    if ( argc == 1 ) {
        usage();
        return 0;
    }

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-cache_file") == 0 ) {
            cacheFilePath = argv[++i];
            if ( cacheFilePath == nullptr ) {
                fprintf(stderr, "-cache_file option requires path to cache file\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-create_closure") == 0 ) {
            inputMainExecutablePath = argv[++i];
            if ( inputMainExecutablePath == nullptr ) {
                fprintf(stderr, "-create_closure option requires a path to an executable\n");
                return 1;
            }
        }
       else if ( strcmp(arg, "-create_image_group") == 0 ) {
            inputTopImagePath = argv[++i];
            if ( inputTopImagePath == nullptr ) {
                fprintf(stderr, "-create_image_group option requires a path to a dylib or bundle\n");
                return 1;
            }
        }
       else if ( strcmp(arg, "-dlopen") == 0 ) {
            const char* path = argv[++i];
            if ( path == nullptr ) {
                fprintf(stderr, "-dlopen option requires a path to a packed closure list\n");
                return 1;
            }
            dlopens.push_back(path);
        }
       else if ( strcmp(arg, "-verbose_fixups") == 0 ) {
           verboseFixups = true;
        }
        else if ( strcmp(arg, "-build_root") == 0 ) {
            const char* buildRootPath = argv[++i];
            if ( buildRootPath == nullptr ) {
                fprintf(stderr, "-build_root option requires a path \n");
                return 1;
            }
            buildtimePrefixes.push_back(buildRootPath);
        }
        else if ( strcmp(arg, "-o") == 0 ) {
            outPath = argv[++i];
            if ( outPath == nullptr ) {
                fprintf(stderr, "-o option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-print_closure_file") == 0 ) {
            printPath = argv[++i];
            if ( printPath == nullptr ) {
                fprintf(stderr, "-print_closure_file option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-print_image_group") == 0 ) {
            printGroupPath = argv[++i];
            if ( printGroupPath == nullptr ) {
                fprintf(stderr, "-print_image_group option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-list_dyld_cache_closures") == 0 ) {
            listCacheClosures = true;
        }
        else if ( strcmp(arg, "-list_dyld_cache_other_dylibs") == 0 ) {
            listOtherDylibs = true;
        }
        else if ( strcmp(arg, "-print_dyld_cache_closure") == 0 ) {
            printCacheClosure = argv[++i];
            if ( printCacheClosure == nullptr ) {
                fprintf(stderr, "-print_dyld_cache_closure option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-print_dyld_cache_closures") == 0 ) {
            printClosures = true;
        }
        else if ( strcmp(arg, "-print_dyld_cache_dylibs") == 0 ) {
            printCachedDylibs = true;
        }
        else if ( strcmp(arg, "-print_dyld_cache_other_dylibs") == 0 ) {
            printOtherDylibs = true;
        }
        else if ( strcmp(arg, "-print_dyld_cache_dylib") == 0 ) {
            printCachedDylib = argv[++i];
            if ( printCachedDylib == nullptr ) {
                fprintf(stderr, "-print_dyld_cache_dylib option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-print_dyld_cache_other") == 0 ) {
            printOtherDylib = argv[++i];
            if ( printOtherDylib == nullptr ) {
                fprintf(stderr, "-print_dyld_cache_other option requires a path \n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-print_dyld_cache_patch_table") == 0 ) {
            printPatchTable = true;
        }
        else if ( strcmp(arg, "-include_all_dylibs_in_dir") == 0 ) {
            includeAllDylibs = true;
        }
        else if ( strcmp(arg, "-env") == 0 ) {
            const char* envArg = argv[++i];
            if ( (envArg == nullptr) || (strchr(envArg, '=') == nullptr) ) {
                fprintf(stderr, "-env option requires KEY=VALUE\n");
                return 1;
            }
            envArgs.push_back(envArg);
        }
        else if ( strcmp(arg, "-use_closured") == 0 ) {
            useClosured = true;
        }
        else {
            fprintf(stderr, "unknown option %s\n", arg);
            return 1;
        }
    }

    if ( (inputMainExecutablePath || inputTopImagePath) && printPath ) {
        fprintf(stderr, "-create_closure and -print_closure_file are mutually exclusive");
        return 1;
    }

    const DyldSharedCache* dyldCache = nullptr;
    bool dyldCacheIsRaw = false;
    if ( cacheFilePath != nullptr ) {
        dyldCache = mapCacheFile(cacheFilePath);
        dyldCacheIsRaw = true;
    }
    else {
#if !defined(__MAC_OS_X_VERSION_MIN_REQUIRED) || (__MAC_OS_X_VERSION_MIN_REQUIRED >= 101300)
        size_t cacheLength;
        dyldCache = (DyldSharedCache*)_dyld_get_shared_cache_range(&cacheLength);
        dyldCacheIsRaw = false;
#endif
    }
    dyld3::ClosureBuffer::CacheIdent cacheIdent;
    dyldCache->getUUID(cacheIdent.cacheUUID);
    cacheIdent.cacheAddress     = (unsigned long)dyldCache;
    cacheIdent.cacheMappedSize  = dyldCache->mappedSize();
    dyld3::DyldCacheParser cacheParser(dyldCache, dyldCacheIsRaw);

    if ( buildtimePrefixes.empty() )
        buildtimePrefixes.push_back("");

    std::vector<const dyld3::launch_cache::binary_format::ImageGroup*> existingGroups;
    const dyld3::launch_cache::BinaryClosureData* mainClosure = nullptr;
    if ( inputMainExecutablePath != nullptr ) {
        dyld3::PathOverrides pathStuff(envArgs);
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 3+dlopens.size(), theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        dyld3::launch_cache::DynArray<const dyld3::launch_cache::binary_format::ImageGroup*> groupList(2, &theGroups[0]);
        dyld3::ClosureBuffer clsBuffer(cacheIdent, inputMainExecutablePath, groupList, pathStuff);

        std::string mainPath = inputMainExecutablePath;
        for (const std::string& prefix : buildtimePrefixes) {
            if ( startsWith(mainPath, prefix) ) {
                mainPath = mainPath.substr(prefix.size());
                if ( mainPath[0] != '/' )
                    mainPath = "/" + mainPath;
                break;
            }
        }
        
        Diagnostics closureDiag;
        //if ( useClosured )
        //    mainClosure = closured_makeClosure(closureDiag, clsBuffer);
       // else
            mainClosure = dyld3::ImageProxyGroup::makeClosure(closureDiag, clsBuffer, mach_task_self(), buildtimePrefixes);
        if ( closureDiag.hasError() ) {
            fprintf(stderr, "dyld_closure_util: %s\n", closureDiag.errorMessage().c_str());
            return 1;
        }
        for (const std::string& warn : closureDiag.warnings() )
            fprintf(stderr, "dyld_closure_util: warning: %s\n", warn.c_str());

        dyld3::launch_cache::Closure closure(mainClosure);
        if ( outPath != nullptr ) {
            safeSave(mainClosure, closure.size(), outPath);
        }
        else {
            dyld3::launch_cache::Closure theClosure(mainClosure);
            theGroups[2] = theClosure.group().binaryData();
            if ( !dlopens.empty() )
                printf("[\n");
            closure.printAsJSON(dyld3::launch_cache::ImageGroupList(3, &theGroups[0]), true);

            int groupIndex = 3;
            for (const char* path : dlopens) {
                printf(",\n");
                dyld3::launch_cache::DynArray<const dyld3::launch_cache::binary_format::ImageGroup*> groupList2(groupIndex-2, &theGroups[2]);
                dyld3::ClosureBuffer dlopenBuffer(cacheIdent, path, groupList2, pathStuff);
                Diagnostics  dlopenDiag;
                //if ( useClosured )
                //    theGroups[groupIndex] = closured_makeDlopenGroup(closureDiag, clsBuffer);
                //else
                    theGroups[groupIndex] = dyld3::ImageProxyGroup::makeDlopenGroup(dlopenDiag, dlopenBuffer, mach_task_self(), buildtimePrefixes);
                if ( dlopenDiag.hasError() ) {
                    fprintf(stderr, "dyld_closure_util: %s\n", dlopenDiag.errorMessage().c_str());
                    return 1;
                }
                for (const std::string& warn : dlopenDiag.warnings() )
                    fprintf(stderr, "dyld_closure_util: warning: %s\n", warn.c_str());
                dyld3::launch_cache::ImageGroup dlopenGroup(theGroups[groupIndex]);
                dlopenGroup.printAsJSON(dyld3::launch_cache::ImageGroupList(groupIndex+1, &theGroups[0]), true);
                ++groupIndex;
            }
            if ( !dlopens.empty() )
                printf("]\n");
        }

    }
#if 0
    else if ( inputTopImagePath != nullptr ) {
        std::string imagePath = inputTopImagePath;
        for (const std::string& prefix : buildtimePrefixes) {
            if ( startsWith(imagePath, prefix) ) {
                imagePath = imagePath.substr(prefix.size());
                if ( imagePath[0] != '/' )
                    imagePath = "/" + imagePath;
                break;
            }
        }

        Diagnostics igDiag;
        existingGroups.push_back(dyldCache->cachedDylibsGroup());
        existingGroups.push_back(dyldCache->otherDylibsGroup());
        if ( existingClosuresPath != nullptr ) {
            size_t mappedSize;
            const void* imageGroups = mapFileReadOnly(existingClosuresPath, mappedSize);
            if ( imageGroups == nullptr ) {
                fprintf(stderr, "dyld_closure_util: could not read file %s\n", printPath);
                return 1;
            }
            uint32_t sentGroups = *(uint32_t*)imageGroups;
            uint16_t lastGroupNum = 2;
            existingGroups.resize(sentGroups+2);
            const uint8_t* p   = (uint8_t*)(imageGroups)+4;
            //const uint8_t* end = (uint8_t*)(imageGroups) + mappedSize;
            for (uint32_t i=0; i < sentGroups; ++i) {
                const dyld3::launch_cache::binary_format::ImageGroup* aGroup = (const dyld3::launch_cache::binary_format::ImageGroup*)p;
                existingGroups[2+i] = aGroup;
                dyld3::launch_cache::ImageGroup imgrp(aGroup);
                lastGroupNum = imgrp.groupNum();
                p += imgrp.size();
            }
        }
        const dyld3::launch_cache::binary_format::ImageGroup* ig = dyld3::ImageProxyGroup::makeDlopenGroup(igDiag, dyldCache, existingGroups.size(), existingGroups, imagePath, envArgs);
        if ( igDiag.hasError() ) {
            fprintf(stderr, "dyld_closure_util: %s\n", igDiag.errorMessage().c_str());
            return 1;
        }

        dyld3::launch_cache::ImageGroup group(ig);
        group.printAsJSON(dyldCache, true);
    }
#endif
    else if ( printPath != nullptr ) {
        size_t mappedSize;
        const void* buff = mapFileReadOnly(printPath, mappedSize);
        if ( buff == nullptr ) {
            fprintf(stderr, "dyld_closure_util: could not read file %s\n", printPath);
            return 1;
        }
        dyld3::launch_cache::Closure theClosure((dyld3::launch_cache::binary_format::Closure*)buff);
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 3, theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        theGroups[2] = theClosure.group().binaryData();
        theClosure.printAsJSON(theGroups, verboseFixups);
        //closure.printStatistics();
        munmap((void*)buff, mappedSize);
    }
    else if ( printGroupPath != nullptr ) {
        size_t mappedSize;
        const void* buff = mapFileReadOnly(printGroupPath, mappedSize);
        if ( buff == nullptr ) {
            fprintf(stderr, "dyld_closure_util: could not read file %s\n", printPath);
            return 1;
        }
        dyld3::launch_cache::ImageGroup group((dyld3::launch_cache::binary_format::ImageGroup*)buff);
//        group.printAsJSON(dyldCache, verboseFixups);
        munmap((void*)buff, mappedSize);
    }
    else if ( listCacheClosures ) {
        cacheParser.forEachClosure(^(const char* runtimePath, const dyld3::launch_cache::binary_format::Closure* closureBinary) {
            dyld3::launch_cache::Closure closure(closureBinary);
            printf("%6lu  %s\n", closure.size(), runtimePath);
        });
    }
    else if ( listOtherDylibs ) {
        dyld3::launch_cache::ImageGroup dylibGroup(cacheParser.otherDylibsGroup());
        for (uint32_t i=0; i < dylibGroup.imageCount(); ++i) {
            dyld3::launch_cache::Image image = dylibGroup.image(i);
            printf("%s\n", image.path());
        }
    }
    else if ( printCacheClosure ) {
        const dyld3::launch_cache::BinaryClosureData* cls = cacheParser.findClosure(printCacheClosure);
        if ( cls != nullptr ) {
            dyld3::launch_cache::Closure theClosure(cls);
            STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 3, theGroups);
            theGroups[0] = cacheParser.cachedDylibsGroup();
            theGroups[1] = cacheParser.otherDylibsGroup();
            theGroups[2] = theClosure.group().binaryData();
            theClosure.printAsJSON(theGroups, verboseFixups);
        }
        else {
            fprintf(stderr, "no closure in cache for %s\n", printCacheClosure);
        }
    }
    else if ( printClosures ) {
        cacheParser.forEachClosure(^(const char* runtimePath, const dyld3::launch_cache::binary_format::Closure* closureBinary) {
            dyld3::launch_cache::Closure theClosure(closureBinary);
            STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 3, theGroups);
            theGroups[0] = cacheParser.cachedDylibsGroup();
            theGroups[1] = cacheParser.otherDylibsGroup();
            theGroups[2] = theClosure.group().binaryData();
            theClosure.printAsJSON(theGroups, verboseFixups);
        });
    }
    else if ( printCachedDylibs ) {
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 2, theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        dyld3::launch_cache::ImageGroup dylibGroup(theGroups[0]);
        dylibGroup.printAsJSON(theGroups, verboseFixups);
    }
    else if ( printCachedDylib != nullptr ) {
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 2, theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        dyld3::launch_cache::ImageGroup dylibGroup(cacheParser.cachedDylibsGroup());
        uint32_t imageIndex;
        const dyld3::launch_cache::binary_format::Image* binImage = dylibGroup.findImageByPath(printCachedDylib, imageIndex);
        if ( binImage != nullptr ) {
            dyld3::launch_cache::Image image(binImage);
            image.printAsJSON(theGroups, true);
        }
        else {
            fprintf(stderr, "no such other image found\n");
        }
    }
    else if ( printOtherDylibs ) {
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 2, theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        dyld3::launch_cache::ImageGroup dylibGroup(theGroups[1]);
        dylibGroup.printAsJSON(theGroups, verboseFixups);
    }
    else if ( printOtherDylib != nullptr ) {
        STACK_ALLOC_DYNARRAY(const dyld3::launch_cache::binary_format::ImageGroup*, 2, theGroups);
        theGroups[0] = cacheParser.cachedDylibsGroup();
        theGroups[1] = cacheParser.otherDylibsGroup();
        dyld3::launch_cache::ImageGroup dylibGroup(cacheParser.otherDylibsGroup());
        uint32_t imageIndex;
        const dyld3::launch_cache::binary_format::Image* binImage = dylibGroup.findImageByPath(printOtherDylib, imageIndex);
        if ( binImage != nullptr ) {
            dyld3::launch_cache::Image image(binImage);
            image.printAsJSON(theGroups, true);
        }
        else {
            fprintf(stderr, "no such other image found\n");
        }
    }
    else if ( printPatchTable ) {
        __block uint64_t cacheBaseAddress = 0;
        dyldCache->forEachRegion(^(const void* content, uint64_t vmAddr, uint64_t size, uint32_t permissions) {
            if ( cacheBaseAddress == 0 )
                cacheBaseAddress = vmAddr;
        });
        __block std::vector<CachedSections> sections;
        __block bool hasError = false;
        dyldCache->forEachImage(^(const mach_header* mh, const char* installName) {
            dyld3::MachOParser parser(mh, dyldCacheIsRaw);
            parser.forEachSection(^(const char* segName, const char* sectionName, uint32_t flags, uint64_t addr, const void* content, 
                                    uint64_t size, uint32_t alignP2, uint32_t reserved1, uint32_t reserved2, bool illegalSectionSize, bool& stop) {
                if ( illegalSectionSize ) {
                    fprintf(stderr, "dyld_closure_util: section size extends beyond the end of the segment %s/%s\n", segName, sectionName);
                    stop = true;
                    return;
                }
                uint32_t offsetStart = (uint32_t)(addr - cacheBaseAddress);
                uint32_t offsetEnd   = (uint32_t)(offsetStart + size);
                sections.push_back({offsetStart, offsetEnd, addr, mh, segName, sectionName, installName});
            });
        });
        if (hasError)
            return 1;
        dyld3::launch_cache::ImageGroup dylibGroup(cacheParser.cachedDylibsGroup());
        dylibGroup.forEachDyldCachePatchLocation(cacheParser, ^(uint32_t targetCacheVmOffset, const std::vector<uint32_t>& usesPointersCacheVmOffsets, bool& stop) {
            const CachedSections& targetSection = find(targetCacheVmOffset, sections);
            dyld3::MachOParser targetParser(targetSection.mh, dyldCacheIsRaw);
            const char* symbolName;
            uint64_t symbolAddress;
            if ( targetParser.findClosestSymbol(targetSection.vmAddress + targetCacheVmOffset - targetSection.mappedOffsetStart, &symbolName, &symbolAddress) ) {
                printf("%s:  [cache offset = 0x%08X]\n", symbolName, targetCacheVmOffset);
            }
            else {
                printf("0x%08X from %40s    %10s   %16s  + 0x%06X\n", targetCacheVmOffset, strrchr(targetSection.dylibPath, '/')+1, targetSection.segmentName.c_str(), targetSection.sectionName.c_str(), targetCacheVmOffset - targetSection.mappedOffsetStart);
            }
            for (uint32_t offset : usesPointersCacheVmOffsets) {
                const CachedSections& usedInSection  = find(offset, sections);
                printf("%40s    %10s   %16s  + 0x%06X\n",  strrchr(usedInSection.dylibPath, '/')+1, usedInSection.segmentName.c_str(), usedInSection.sectionName.c_str(), offset - usedInSection.mappedOffsetStart);
            }
        });
    }


    return 0;
}
