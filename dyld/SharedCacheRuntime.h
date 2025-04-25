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



#ifndef __DYLD_SHARED_CACHE_RUNTIME_H__
#define __DYLD_SHARED_CACHE_RUNTIME_H__

#include <string.h>
#include <stdint.h>

#include "Allocator.h"
#include "DyldSharedCache.h"
#include "Platform.h"

namespace dyld4 {
    class ProcessConfig;
}

using dyld4::ProcessConfig;

namespace dyld3 {

struct SharedCacheOptions {
#if !TARGET_OS_EXCLAVEKIT
    // Note we look in the default cache dir first, and then the fallback, if not null
    int                 cacheDirFD = -1;
#else
    DyldSharedCache*     cacheHeader = nullptr;
    size_t               cacheSize;
    const char*          cachePath   = nullptr;
#endif // !TARGET_OS_EXCLAVEKIT
    bool                 forcePrivate;
    bool                 useHaswell;
    bool                 verbose;
    bool                 disableASLR;
    bool                 enableReadOnlyDataConst;
    bool                 enableTPRO;
    bool                 preferCustomerCache;
    bool                 forceDevCache;
    bool                 isTranslated;
    bool                 usePageInLinking;
    mach_o::Platform     platform;
    const ProcessConfig* config = nullptr;
};

struct SharedCacheLoadInfo {
    const DyldSharedCache*      loadAddress     = nullptr;
    long                        slide           = 0;
    const char*                 errorMessage    = nullptr;
    bool                        cacheFileFound  = false;
    bool                        development     = false;
#if !TARGET_OS_EXCLAVEKIT
    FileIdTuple                 cacheFileID;
#endif // !TARGET_OS_EXCLAVEKIT
};

bool loadDyldCache(const SharedCacheOptions& options, SharedCacheLoadInfo* results);

struct SharedCacheFindDylibResults {
    const mach_header*          mhInCache;
    const char*                 pathInCache;
    long                        slideInCache;
};

bool findInSharedCacheImage(const SharedCacheLoadInfo& loadInfo, const char* dylibPathToFind, SharedCacheFindDylibResults* results);

bool pathIsInSharedCacheImage(const SharedCacheLoadInfo& loadInfo, const char* dylibPathToFind);

void deallocateExistingSharedCache();


} // namespace dyld3

#endif // __DYLD_SHARED_CACHE_RUNTIME_H__


