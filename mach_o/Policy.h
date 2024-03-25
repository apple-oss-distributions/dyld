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


#ifndef mach_o_Policy_h
#define mach_o_Policy_h

#include <string_view>

#include "Defines.h"
#include "Platform.h"
#include "Architecture.h"

namespace mach_o {

/*!
 * @class Policy
 *
 * @abstract
 *      Class for encapsulating policy for mach-o format details.
 *
 * @discussion
 *      The mach-o format is evolving over time. There are two categories
 *      of changes: new features and new restrictions.
 *
 *      A new feature is a new load command or new section, which only a new
 *      enough OS will understand. Each feature has a "use<xxx>()" method which
 *      ld checks to decide to emit a mach-o with the new feature.  The
 *      result of that method is a Usage value that specifies if the policy
 *      is to use or not use that feature, and if that use is a "must" or "preferred".
 *      A preferred policy can be overridden by a command line arg
 *      (e.g. -no\_fixup\_chains), whereas a must cannot be overridden.
 *
 *      A restriction is a constraint on existing mach-o details. These are driven
 *      by security, performance, or correctness concerns.  Each restriction
 *      has an "enforce<xxx>()" method which dyld and dyld\_info check to validate
 *      the binary. Restrictions are based on the SDK version the binary was built
 *      with. That is, the an old binary is allowed to violate the restriction,
 *      whereas a newer binary (build against newer SDK) is not.  The restriction
 *      logic is that the "enforce<xxx>()" will all return true for a binary built
 *      with the latest SDK.
 *
 */
class VIS_HIDDEN Policy
{
public:
                Policy(Architecture arch, PlatformAndVersions pvs, uint32_t filetype, bool pathMayBeInSharedCache=false);

    enum Usage { preferUse, mustUse, preferDontUse, mustNotUse };

    // features
    Usage       useBuildVersionLoadCommand() const;
    Usage       useDataConst() const;
    Usage       useConstClassRefs() const;
    Usage       useGOTforClassRefs() const;
    Usage       useChainedFixups() const;
    Usage       useOpcodeFixups() const;
    Usage       useRelativeMethodLists() const;
    Usage       useAuthStubsInKexts() const;
    Usage       useDataConstForSelRefs() const;
    Usage       useSourceVersionLoadCommand() const;
    Usage       useLegacyLinkedit() const;
    bool        use4KBLoadCommandsPadding() const;
    bool        canUseDelayInit() const;

    // restrictions
    bool        enforceReadOnlyLinkedit() const;
    bool        enforceLinkeditContentAlignment() const;
    bool        enforceOneFixupEncoding() const;
    bool        enforceSegmentOrderMatchesLoadCmds() const;
    bool        enforceTextSegmentPermissions() const;
    bool        enforceFixupsInWritableSegments() const;
    bool        enforceCodeSignatureAligned() const;
    bool        enforceSectionsInSegment() const;
    bool        enforceHasLinkedDylibs() const;
    bool        enforceInstallNamesAreRealPaths() const;
    bool        enforceHasUUID() const;
    bool        enforceMainFlagsCorrect() const;
    bool        enforceNoDuplicateDylibs() const;
    bool        enforceNoDuplicateRPaths() const;
    bool        enforceDataSegmentPermissions() const;

private:
    bool              dyldLoadsOutput() const;

    Platform::Epoch   _featureEpoch;
    Platform::Epoch   _enforcementEpoch;
    Architecture      _arch;
    PlatformAndVersions   _pvs;
    uint32_t          _filetype;
    bool              _pathMayBeInSharedCache;
};




} // namespace mach_o

#endif // mach_o_Policy_h


