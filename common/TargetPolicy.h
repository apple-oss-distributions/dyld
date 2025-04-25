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

// Centralize flags which differ based on which target we are building

#ifndef DYLD_TARGET_POLICY_H
#define DYLD_TARGET_POLICY_H

// FIXME: Share this with another file
#define VIS_HIDDEN      __attribute__((visibility("hidden")))

// True if mach_o::Header adds an implicit platform to binaries if they don't have
extern VIS_HIDDEN const bool gHeaderAddImplicitPlatform;

// True if mach_o::Header is allows files to have no platform
extern VIS_HIDDEN const bool gHeaderAllowEmptyPlatform;

// True if mach_o::Image should validate initializers
extern VIS_HIDDEN const bool gImageValidateInitializers;

// True if mach_o::Image can assume content has been rebased, ie, this is dyld
extern VIS_HIDDEN const bool gImageAssumeContentRebased;

#endif /* DYLD_DEFINES_H */
