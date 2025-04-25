/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- 
 *
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


// STL
#include <vector>
#include <span>

// mach_o
#include "Header.h"
#include "Error.h"

// common
#include "Defines.h"
#include "CString.h"


struct VerifierError
{
    VerifierError(CString name) : verifierErrorName(name) { }

    CString         verifierErrorName;
    mach_o::Error   message;
};


/*!
 * @function os_macho_verifier
 *
 * @abstract
 *      Used by B&I verifer to ensure binaries follow Apple's rules for OS mach-o files
 *
 * @param path
 *      Full path to file (in $DSTROOT) to examine.
 *
 * @param buffer
 *      The content of the file.
 *
 * @param verifierDstRoot
 *      $DSTROOT path.
 *
 * @param mergeRootPaths
 *      If B&I moves content file system location
 *
 * @param errors
 *      For each error found in file, a VerifierError is added to this vector.
 *
 */
void os_macho_verifier(CString path, std::span<const uint8_t> buffer, CString verifierDstRoot,
                       const std::vector<CString>& mergeRootPaths, std::vector<VerifierError>& errors);





