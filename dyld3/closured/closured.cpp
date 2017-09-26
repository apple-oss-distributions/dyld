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
#include <string.h>
#include <sandbox/private.h>
#include <bootstrap.h>
#include <mach/mach.h>
#include <os/log.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include <dispatch/dispatch.h>
#include <dispatch/private.h>
#include <bootstrap_priv.h>      // for bootstrap_check_in()
#include <CrashReporterClient.h>
#include <libproc.h>
#include <uuid/uuid.h>

#include <string>
#include <vector>
#include <unordered_map>

#include "dyld_priv.h"
#include "ImageProxy.h"
#include "DyldSharedCache.h"
#include "FileUtils.h"

extern "C" {
    #include "closuredProtocolServer.h"
}


static os_log_t sLog = os_log_create("com.apple.dyld.closured", "closured");

static char sCrashMessageBuffer[1024];


kern_return_t
do_CreateClosure(
    mach_port_t             port,
    task_t                  requestor,
    vm_address_t            buffer,
    mach_msg_type_number_t  bufferCnt,
    vm_address_t*           returnData,
    mach_msg_type_number_t* returnDataCnt)
{
    dyld3::ClosureBuffer clsBuff((void*)buffer, bufferCnt);
    const char* imagePath = clsBuff.targetPath();
    os_log(sLog, "request to build closure for %s\n", imagePath);

    // set crash log message in case there is an assert during processing
    strlcpy(sCrashMessageBuffer, "building closure for: ", sizeof(sCrashMessageBuffer));
    strlcat(sCrashMessageBuffer, imagePath, sizeof(sCrashMessageBuffer));
    CRSetCrashLogMessage(sCrashMessageBuffer);

    Diagnostics diag;
    const dyld3::launch_cache::binary_format::Closure* cls = dyld3::ImageProxyGroup::makeClosure(diag, clsBuff, requestor);

    os_log_info(sLog, "finished closure build, closure=%p\n", cls);
    for (const std::string& message: diag.warnings())
        os_log(sLog, "Image generated warning: %s\n", message.c_str());

    if ( diag.noError() ) {
        // on success return the closure binary in the "returnData" buffer
        dyld3::ClosureBuffer result(cls);
        *returnData    = result.vmBuffer();
        *returnDataCnt = result.vmBufferSize();
    }
    else {
        // on failure return the error message in the "returnData" buffer
        os_log_error(sLog, "failed to create ImageGroup: %s\n", diag.errorMessage().c_str());
        dyld3::ClosureBuffer err(diag.errorMessage().c_str());
        *returnData    = err.vmBuffer();
        *returnDataCnt = err.vmBufferSize();
    }

    CRSetCrashLogMessage(nullptr);
    return KERN_SUCCESS;
}

kern_return_t
do_CreateImageGroup(
    mach_port_t             port,
    task_t                  requestor,
    vm_address_t            buffer,
    mach_msg_type_number_t  bufferCnt,
    vm_address_t*           returnData,
    mach_msg_type_number_t* returnDataCnt)
{
    dyld3::ClosureBuffer clsBuff((void*)buffer, bufferCnt);
    const char* imagePath = clsBuff.targetPath();
    int requestorPid;
    char requestorName[MAXPATHLEN];
    if ( pid_for_task(requestor, &requestorPid) == 0 ) {
        int nameLen = proc_name(requestorPid, requestorName, sizeof(requestorName));
        if ( nameLen <= 0 )
            strcpy(requestorName, "???");
        os_log(sLog, "request from %d (%s) to build dlopen ImageGroup for %s\n", requestorPid, requestorName, imagePath);
    }

    // set crash log message in case there is an assert during processing
    strlcpy(sCrashMessageBuffer, "building ImageGroup for dlopen(", sizeof(sCrashMessageBuffer));
    strlcat(sCrashMessageBuffer, imagePath, sizeof(sCrashMessageBuffer));
    strlcat(sCrashMessageBuffer, ") requested by ", sizeof(sCrashMessageBuffer));
    strlcat(sCrashMessageBuffer, requestorName, sizeof(sCrashMessageBuffer));
    CRSetCrashLogMessage(sCrashMessageBuffer);

    uuid_string_t  uuidStr;
    dyld3::ClosureBuffer::CacheIdent cacheIdent = clsBuff.cacheIndent();
    uuid_unparse(cacheIdent.cacheUUID, uuidStr);
    os_log_info(sLog, "findDyldCache(): cache addr=0x%llX, size=0x%0llX, uuid = %s\n", cacheIdent.cacheAddress, cacheIdent.cacheMappedSize, uuidStr);

    Diagnostics diag;
    const dyld3::launch_cache::binary_format::ImageGroup* imageGroup = dyld3::ImageProxyGroup::makeDlopenGroup(diag, clsBuff, requestor, {""});

    os_log(sLog, "finished ImageGroup build, imageGroup=%p\n", imageGroup);
    for (const std::string& message: diag.warnings()) {
        os_log(sLog, "Image generated warning: %s\n", message.c_str());
    }

    // delete incoming out-of-line data 
    vm_deallocate(mach_task_self(), buffer, bufferCnt);

    if ( diag.noError() ) {
        // on success return the ImageGroup binary in the "returnData" buffer
        dyld3::ClosureBuffer result(imageGroup);
        os_log_info(sLog, "returning closure buffer: 0x%lX, size=0x%X\n", (long)result.vmBuffer(), result.vmBufferSize());
        *returnData    = result.vmBuffer();
        *returnDataCnt = result.vmBufferSize();
        free((void*)imageGroup);
    }
    else {
        // on failure return the error message in the "returnData" buffer
        os_log_error(sLog, "failed to create ImageGroup: %s\n", diag.errorMessage().c_str());
        dyld3::ClosureBuffer err(diag.errorMessage().c_str());
        *returnData    = err.vmBuffer();
        *returnDataCnt = err.vmBufferSize();
    }

    CRSetCrashLogMessage(nullptr);
    return KERN_SUCCESS;
}




// /usr/libexec/closured -create_closure /Applications/TextEdit.app -pipefd 4  -env DYLD_FOO=1  -cache_uuid C153F90A-69F2-323E-AC9F-2BBAE48ABAF7
int runAsTool(int argc, const char* argv[])
{
    const char*               progPath  = nullptr;
    int                       pipeNum   = -1;
    bool                      verbose   = false;
    std::vector<std::string>  dyldEnvVars;
    
    dyld3::ClosureBuffer::CacheIdent cacheIdent;
    bzero(&cacheIdent, sizeof(cacheIdent));

    // set crash log message in case there is an assert during processing
    sCrashMessageBuffer[0] = '\0';
    for (int i=0; i < argc; ++i) {
        strlcat(sCrashMessageBuffer, argv[i], sizeof(sCrashMessageBuffer));
        strlcat(sCrashMessageBuffer, " ", sizeof(sCrashMessageBuffer));
    }
    CRSetCrashLogMessage(sCrashMessageBuffer);

    for (int i=1; i < argc; ++i) {
        const char* arg = argv[i];
        if ( strcmp(arg, "-create_closure") == 0 ) {
            progPath = argv[++i];
            if ( progPath == nullptr ) {
                fprintf(stderr, "-create_closure option requires a path to follow\n");
                return 1;
            }
        }
        else if ( strcmp(arg, "-cache_uuid") == 0 ) {
            const char* uuidStr = argv[++i];
            if ( uuidStr == nullptr ) {
                fprintf(stderr, "-cache_uuid option requires a path to follow\n");
                return 1;
            }
            uuid_parse(uuidStr, cacheIdent.cacheUUID);
        }
        else if ( strcmp(arg, "-cache_address") == 0 ) {
            const char* cacheAddr = argv[++i];
            if ( cacheAddr == nullptr ) {
                fprintf(stderr, "-cache_address option requires a path to follow\n");
                return 1;
            }
            char *end;
            cacheIdent.cacheAddress = strtol(cacheAddr, &end, 0);
        }
        else if ( strcmp(arg, "-cache_size") == 0 ) {
            const char* cacheSize = argv[++i];
            if ( cacheSize == nullptr ) {
                fprintf(stderr, "-cache_size option requires a path to follow\n");
                return 1;
            }
            char *end;
            cacheIdent.cacheMappedSize = strtol(cacheSize, &end, 0);
        }
        else if ( strcmp(arg, "-pipefd") == 0 ) {
            const char* numStr = argv[++i];
            if ( numStr == nullptr ) {
                fprintf(stderr, "-pipefd option requires an file descriptor number to follow\n");
                return 1;
            }
            char *end;
            pipeNum = (int)strtol(numStr, &end, 0);
            if ( (pipeNum == 0) && (errno != 0) ) {
                fprintf(stderr, "bad value (%s) for -pipefd option %d\n", numStr, pipeNum);
                return 1;
            }
        }
        else if ( strcmp(arg, "-env") == 0 ) {
            const char* var = argv[++i];
            if ( var == nullptr ) {
                fprintf(stderr, "-env option requires a following VAR=XXX\n");
                return 1;
            }
            dyldEnvVars.push_back(var);
        }
        else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return 1;
        }
    }
    if ( progPath == nullptr ) {
        fprintf(stderr, "missing required -create_closure option\n");
        return 1;
    }
    if ( pipeNum == -1 ) {
        fprintf(stderr, "missing required -pipefd option\n");
        return 1;
    }

    if ( verbose ) {
        fprintf(stderr, "closured: runAsTool()\n");
        for (int i=1; i < argc; ++i)
            fprintf(stderr, "   argv[%d] = %s\n", i, argv[i]);
    }

    os_log(sLog, "fork/exec request to build closure for %s\n", progPath);
    SocketBasedClousureHeader header;

    // find dyld cache for requested arch
    size_t currentCacheSize;
    const DyldSharedCache* currentCache = (const DyldSharedCache*)_dyld_get_shared_cache_range(&currentCacheSize);
    if ( currentCache == nullptr ) {
        os_log_error(sLog, "closured is running without a dyld cache\n");
        return 1;
    }
    uuid_t currentCacheUUID;
    currentCache->getUUID(currentCacheUUID);
    if ( memcmp(currentCacheUUID, cacheIdent.cacheUUID, 16) != 0 ) {
        const char* errorString = "closured is running with a different dyld cache than client";
        os_log_error(sLog, "%s\n", errorString);
        header.success = 0;
        header.length  = (int)strlen(errorString) + 1;
        write(pipeNum, &header, sizeof(SocketBasedClousureHeader));
        write(pipeNum, errorString, header.length);
        close(pipeNum);
        return 0;
    }
    dyld3::DyldCacheParser cacheParser(currentCache, false);

    Diagnostics diag;
    os_log_info(sLog, "starting closure build\n");
    const dyld3::launch_cache::BinaryClosureData* cls = dyld3::ImageProxyGroup::makeClosure(diag, cacheParser, progPath, false, {""}, dyldEnvVars);
    os_log_info(sLog, "finished closure build, cls=%p\n", cls);
    if ( diag.noError() ) {
        // on success write the closure binary after the header to the socket
        dyld3::launch_cache::Closure closure(cls);
        os_log(sLog, "returning closure, size=%lu\n", closure.size());
        header.success = 1;
        header.length  = (uint32_t)closure.size();
        write(pipeNum, &header, sizeof(SocketBasedClousureHeader));
        write(pipeNum, cls, closure.size());
    }
    else {
        // on failure write the error message after the header to the socket
        const std::string& message = diag.errorMessage();
        os_log_error(sLog, "closure could not be created: %s\n", message.c_str());
        header.success = 0;
        header.length  = (uint32_t)message.size() + 1;
        write(pipeNum, &header, sizeof(SocketBasedClousureHeader));
        write(pipeNum, message.c_str(), header.length);
    }

    close(pipeNum);

    return 0;
}


union MaxMsgSize {
    union __RequestUnion__do_closured_subsystem req;
    union __ReplyUnion__do_closured_subsystem   rep;
};

int main(int argc, const char* argv[])
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED
    // establish sandbox around process
    char* errMsg = nullptr;
    if ( sandbox_init_with_parameters("com.apple.dyld.closured", SANDBOX_NAMED, nullptr, &errMsg) != 0 ) {
        os_log_error(sLog, "Failed to enter sandbox: %{public}s", errMsg);
        exit(EXIT_FAILURE);
    }
#endif

    if ( argc != 1 )
        return runAsTool(argc, argv);\

    mach_port_t serverPort = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_check_in(bootstrap_port, CLOSURED_SERVICE_NAME, &serverPort);
    if (kr != KERN_SUCCESS)
        exit(-1);
    
    dispatch_source_t machSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, serverPort, 0, dispatch_get_main_queue());
    if (machSource == nullptr)
        exit(-1);
    
    dispatch_source_set_event_handler(machSource, ^{
        dispatch_mig_server(machSource, sizeof(union MaxMsgSize), closured_server);
    });
    dispatch_source_set_cancel_handler(machSource, ^{
        mach_port_t port = (mach_port_t)dispatch_source_get_handle(machSource);
        kern_return_t kr = mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        if (kr != KERN_SUCCESS)
            exit(-1);
    });
    dispatch_resume(machSource);
    dispatch_main();

    return 0;
}

