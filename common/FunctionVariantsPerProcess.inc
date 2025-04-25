/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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


// included by:
//   ld to convert function variant names to keyIndexes
//   dyld to drive which function to run to set keyIndex bool
//
// FUNCTION_VARIANT_PER_PROCESS(_keyIndex, _name, _runtimeCheck)
//


//
//  per-process function variants
//
FUNCTION_VARIANT_PER_PROCESS(0, "default", true)

// These were added in Spring 2025
FUNCTION_VARIANT_PER_PROCESS(1, "translated",   config.process.isTranslated)
FUNCTION_VARIANT_PER_PROCESS(3, "no_overread", (config.process.environ("LIBPLATFORM_NO_OVERREAD") != nullptr))

// These were added in Fall 2025

