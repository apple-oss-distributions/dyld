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


#ifndef DyldCacheParser_h
#define DyldCacheParser_h

#include <stdint.h>
#include <uuid/uuid.h>
#include <mach-o/loader.h>

#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "LaunchCacheFormat.h"

namespace dyld3 {

class VIS_HIDDEN DyldCacheParser
{
public:
#if !DYLD_IN_PROCESS
    static bool isValidDyldCache(Diagnostics& diag, const std::string& archName, Platform platform, const void* fileContent, size_t fileLength, const std::string& pathOpened, bool ignoreMainExecutables);
#endif

                            DyldCacheParser(const DyldSharedCache* cacheHeader, bool rawFile);
    const DyldSharedCache*  cacheHeader() const;
    bool                    cacheIsMappedRaw() const;



    //
    // Get ImageGroup for cached dylibs built into this cache files
    //
    const dyld3::launch_cache::binary_format::ImageGroup*   cachedDylibsGroup() const;


    //
    // Get ImageGroup for other OS dylibs and bundles built into this cache files
    //
    const dyld3::launch_cache::binary_format::ImageGroup*   otherDylibsGroup() const;


    //
    // returns closure for given path, or nullptr if no closure found
    //
    const dyld3::launch_cache::binary_format::Closure*      findClosure(const char* path) const;

    //
    // returns what vmOffset of data (r/w) region from cache header will be when cache is used in a process
    //
    uint64_t                                                dataRegionRuntimeVmOffset() const;

#if !DYLD_IN_PROCESS
    //
    // Iterates over closure of OS programs built into shared cache
    //
    void forEachClosure(void (^handler)(const char* runtimePath, const dyld3::launch_cache::binary_format::Closure*)) const;
#endif

private:
    const dyld_cache_header*    header() const;

    long                        _data;  // low bit means rawFile
};

} // namespace dyld3

#endif // DyldCacheParser_h
