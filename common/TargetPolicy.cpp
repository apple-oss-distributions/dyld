/*
* Copyright (c) 2019 Apple Inc. All rights reserved.
*
* @APPLE_LICENSE_HEADER_START@
*
* "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
* Reserved.  This file contains Original Code and/or Modifications of
* Original Code as defined in and that are subject to the Apple Public
* Source License Version 1.0 (the 'License').  You may not use this file
* except in compliance with the License.  Please obtain a copy of the
* License at http://www.apple.com/publicsource and read it before using
* this file.
*
* The Original Code and all software distributed under the License are
* distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
* INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
* License for the specific language governing rights and limitations
* under the License."
*
* @APPLE_LICENSE_HEADER_END@
*/

#include "TargetPolicy.h"
#include "Defines.h"

#if BUILDING_DYLD
    #if TARGET_OS_OSX
    const bool gHeaderAddImplicitPlatform = true;
    #else
    const bool gHeaderAddImplicitPlatform = false;
    #endif // TARGET_OS_OSX
#else
const bool gHeaderAddImplicitPlatform = false;
#endif // BUILDING_DYLD

#if BUILDING_LD || BUILDING_LD_UNIT_TESTS
const bool gHeaderAllowEmptyPlatform = true;
#else
const bool gHeaderAllowEmptyPlatform = false;
#endif // BUILDING_LD

#if BUILDING_LD || BUILDING_LD_UNIT_TESTS
// don't need deep inspection of dylibs we are linking with
const bool gImageValidateInitializers = false;
#else
const bool gImageValidateInitializers = true;
#endif

#if BUILDING_DYLD
// only dyld knows for sure that content was rebased when walking initializers
const bool gImageAssumeContentRebased = true;
#else
const bool gImageAssumeContentRebased = false;
#endif
