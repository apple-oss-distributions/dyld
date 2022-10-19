/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
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

#ifndef BuilderOptions_hpp
#define BuilderOptions_hpp

#include "Error.h"
#include "JSON.h"
#include "MachOFile.h"

#include <string>
#include <unordered_map>

namespace cache_builder
{

enum class LocalSymbolsMode {
    keep,
    unmap,
    strip
};

enum class CacheKind
{
    development,
    universal
};

struct BuilderOptions
{
    BuilderOptions(std::string_view archName, dyld3::Platform platform,
                   bool dylibsRemovedFromDisk, bool isLocallyBuiltCache,
                   CacheKind kind, bool forceDevelopmentSubCacheSuffix);

    bool isSimultor() const;

    // Core fields
    const dyld3::GradedArchs&                   archs;
    dyld3::Platform                             platform;
    bool                                        dylibsRemovedFromDisk;
    bool                                        isLocallyBuiltCache;
    bool                                        forceDevelopmentSubCacheSuffix;
    CacheKind                                   kind;
    LocalSymbolsMode                            localSymbolsMode;

    // Logging/printing
    std::string                                 logPrefix;
    bool                                        timePasses  = false;
    bool                                        stats       = false;

    // Other
    std::unordered_map<std::string, unsigned>   dylibOrdering;
    std::unordered_map<std::string, unsigned>   dirtyDataSegmentOrdering;
    dyld3::json::Node                           objcOptimizations;
};

// Inputs to the builder can be dylibs, executables, bundles, etc.
struct InputFile
{
    const dyld3::MachOFile* mf      = nullptr;
    uint64_t                inode   = 0;
    uint64_t                mtime   = 0;
    std::string             path;

    bool hasError() const;
    const error::Error& getError() const;
    void setError(error::Error&& err);

private:
    error::Error            error;
};

// Alias for an input file
struct FileAlias
{
    std::string             realPath;
    std::string             aliasPath;
};

} // namespace cache_builder

#endif /* BuilderOptions_hpp */
