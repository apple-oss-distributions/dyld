/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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

#ifndef PrebuiltSwift_h
#define PrebuiltSwift_h

#include "DyldRuntimeState.h"
#include "PrebuiltObjC.h"
#include "OptimizerSwift.h"
#include "Map.h"


namespace dyld4 {

//
// PrebuiltSwift computes read-only optimized data structures to store in the PrebuiltLoaderSet
//
struct PrebuiltSwift {

public:


    PrebuiltSwift() = default;
    ~PrebuiltSwift() = default;

    void make(Diagnostics& diag, PrebuiltObjC& prebuiltObjC, RuntimeState& state);

    TypeProtocolMap     typeProtocolConformances = { nullptr };
    MetadataProtocolMap metadataProtocolConformances = { nullptr };
    ForeignProtocolMap  foreignProtocolConformances = { nullptr };

    bool builtSwift              = false;

private:
    bool findProtocolConformances(Diagnostics& diag, PrebuiltObjC& prebuiltObjC, RuntimeState& state);

};

} // namespace dyld4



#endif /* PrebuiltSwift_h */
