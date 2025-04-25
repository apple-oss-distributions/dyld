/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2009-2012 Apple Inc. All rights reserved.
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

#ifndef other_tools_MiscFileUtils_h
#define other_tools_MiscFileUtils_h

// mach_o
#include "Header.h"

#include "DyldSharedCache.h"

#include <span>

// FIXME: Maybe this should be something like tools_common?
namespace other_tools {

bool withReadOnlyMappedFile(const char* path, void (^handler)(std::span<const uint8_t>)) VIS_HIDDEN;

// used by command line tools to process files that may be on disk or in dyld cache
void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter, const DyldSharedCache* dyldCache,
                             void (^callback)(const char* slicePath, const mach_o::Header* sliceHeader, size_t sliceLength)) VIS_HIDDEN;

void forSelectedSliceInPaths(std::span<const char*> paths, std::span<const char*> archFilter,
                             void (^callback)(const char* slicePath, const mach_o::Header* sliceHeader, size_t sliceLength)) VIS_HIDDEN;

} // namespace other_tools

#endif // other_tools_MiscFileUtils_h
