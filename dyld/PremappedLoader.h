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

#ifndef PremappedLoader_h
#define PremappedLoader_h

#include <TargetConditionals.h>

#include "JustInTimeLoader.h"
#include "Loader.h"

//
// PremappedLoaders:
//
// Premapped loaders are used in systems that have no disk.
// Binaries are mapped in memory ahead of time by the kernel.
// The images are then passed to dyld, which is in charge of applying fixups
// and performing any neccesaary initializing logic.

namespace dyld4 {

#if SUPPORT_CREATING_PREMAPPEDLOADERS
class PremappedLoader : public JustInTimeLoader
{
public:
    // these are the "virtual" methods that override Loader
    void                        loadDependents(Diagnostics& diag, RuntimeState& state, const LoadOptions& options);
    void                        applyFixups(Diagnostics&, RuntimeState& state, DyldCacheDataConstLazyScopedWriter&, bool allowLazyBinds) const;
    bool                        dyldDoesObjCFixups() const;
    void                        withLayout(Diagnostics &diag, RuntimeState& state, void (^callback)(const mach_o::Layout &layout)) const;
    bool                        hasBeenFixedUp(RuntimeState&) const;
    bool                        beginInitializers(RuntimeState&);


    static Loader*      makePremappedLoader(Diagnostics& diag, RuntimeState& state, const char* path, const LoadOptions& options, const mach_o::Layout* layout);
    static Loader*      makeLaunchLoader(Diagnostics& diag, RuntimeState& state, const MachOAnalyzer* mainExec, const char* mainExecPath, const mach_o::Layout* layout);

private:
    PremappedLoader(const MachOFile* mh, const Loader::InitialOptions& options, const mach_o::Layout* layout);
    static PremappedLoader*     make(RuntimeState& state, const MachOFile* mh, const char* path, bool willNeverUnload, const mach_o::Layout* layout);


};
#endif // SUPPORT_CREATING_PREMAPPEDLOADERS

}  // namespace dyld4

#endif // PremappedLoader_h





