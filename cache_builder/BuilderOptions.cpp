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
#include "Platform.h"

using namespace cache_builder;
using dyld3::GradedArchs;

using mach_o::Platform;

//
// MARK: --- cache_builder::Options methods ---
//

BuilderOptions::BuilderOptions(std::string_view archName, Platform platform,
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

bool BuilderOptions::isSimulator() const
{
    return this->platform.isSimulator();
}

bool BuilderOptions::isExclaveKit() const
{
    return this->platform.isExclaveKit();
}

//
// MARK: --- cache_builder::InputFile methods ---
//

void InputFile::addError(error::Error&& err)
{
    // This is a good place to breakpoint and catch if a specific dylib has an error
    this->errors.push_back(std::move(err));
}

std::span<const error::Error> InputFile::getErrors() const
{
    return this->errors;
}

bool InputFile::hasError() const
{
    return !this->errors.empty();
}
