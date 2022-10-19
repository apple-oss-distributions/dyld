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

#include "BuilderOptions.h"

using namespace cache_builder;
using dyld3::GradedArchs;

//
// MARK: --- cache_builder::Options methods ---
//

BuilderOptions::BuilderOptions(std::string_view archName, dyld3::Platform platform,
                               bool dylibsRemovedFromDisk, bool isLocallyBuiltCache,
                               CacheKind kind, bool forceDevelopmentSubCacheSuffix)
    : archs(GradedArchs::forName(archName.data()))
    , platform(platform)
    , dylibsRemovedFromDisk(dylibsRemovedFromDisk)
    , isLocallyBuiltCache(isLocallyBuiltCache)
    , forceDevelopmentSubCacheSuffix(forceDevelopmentSubCacheSuffix)
    , kind(kind)
{
}

bool BuilderOptions::isSimultor() const
{
    return dyld3::MachOFile::isSimulatorPlatform(this->platform);
}

//
// MARK: --- cache_builder::InputFile methods ---
//

void InputFile::setError(error::Error&& err)
{
    // This is a good place to catch if a specific dylib has an error
    this->error = std::move(err);
}

const error::Error& InputFile::getError() const
{
    return this->error;
}

bool InputFile::hasError() const
{
    return this->error.hasError();
}
