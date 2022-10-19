/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
 *
 * Copyright (c) 2014 Apple Inc. All rights reserved.
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

#ifndef SectionCoalescer_h
#define SectionCoalescer_h

#include "Chunk.h"
#include "MachOFile.h"
#include "Types.h"

#include <string>
#include <unordered_map>

struct CoalescedSection
{
    // Points to the chunk in the subCache where these coalesced values live.
    cache_builder::Chunk* cacheChunk = nullptr;

    // Note this is for debugging only
    uint64_t              savedSpace = 0;
};

struct CoalescedStringsSection : CoalescedSection
{
    CoalescedStringsSection(std::string_view sectionName) : sectionName(sectionName) { }

    void clear()
    {
        *this = CoalescedStringsSection(this->sectionName);
    }

    std::string_view                                sectionName;
    // Map from class strings to offsets in to the strings buffer
    std::unordered_map<std::string_view, uint32_t>  stringsToOffsets;
};

struct CoalescedGOTSection : CoalescedSection
{
    struct GOTKey
    {
        std::string_view                    targetSymbolName;
        std::string_view                    targetDylibName;
        dyld3::MachOFile::PointerMetaData   pmd;
    };

    struct Hash
    {
        size_t operator()(const GOTKey& v) const
        {
            static_assert(sizeof(v.pmd) == sizeof(uint32_t));

            size_t hash = 0;
            hash ^= std::hash<std::string_view>{}(v.targetSymbolName);
            hash ^= std::hash<std::string_view>{}(v.targetDylibName);
            hash ^= std::hash<uint32_t>{}(*(uint32_t*)&v.pmd);
            return hash;
        }
    };

    struct EqualTo
    {
        bool operator()(const GOTKey& a, const GOTKey& b) const
        {
            return (a.targetSymbolName == b.targetSymbolName)
                && (a.targetDylibName == b.targetDylibName)
                && (memcmp(&a.pmd, &b.pmd, sizeof(a.pmd)) == 0);
        }
    };

    // Map from bind target to offsets in to the GOTs buffer
    std::unordered_map<GOTKey, uint32_t, Hash, EqualTo> gotTargetsToOffsets;
};

struct DylibSectionCoalescer
{
    typedef std::unordered_map<uint32_t, uint32_t> DylibSectionOffsetToCacheSectionOffset;

    // A section may be completely coalesced and removed, eg, strings,
    // or it may be coalesced and copies made elsewhere, eg, GOTs.  In the GOTs case, we
    // don't remove the original section
    struct OptimizedSection
    {
        OptimizedSection(bool sectionWillBeRemoved, const char* name)
            : sectionWillBeRemoved(sectionWillBeRemoved), name(name) { }

        DylibSectionOffsetToCacheSectionOffset  offsetMap;

        // Some offsets are not in the above offsetMap, even though we'd typically want to know about every
        // reference to the given section.  Eg, we only optimize binds in __got, not rebases.  But we want
        // to track the rebases just so that we know of every element in the section.
        std::set<uint32_t>                      unoptimizedOffsets;

        // Different subCache's may contain their own GOTs/strings.  We can't deduplicate
        // cache-wide in to a single buffer due to constraints such as 32-bit offsets
        // This points to the cache section we coalesced into, for this section in this dylib
        CoalescedSection*                       subCacheSection = nullptr;

        // Whether or not this section will be removed.  Eg, GOTs aren't currently removed from
        // their original binary
        bool        sectionWillBeRemoved;

        const char* name;

        // The index of the segment this section is in
        std::optional<uint32_t> segmentIndex;

        // The VMOffset of this section within the segment
        VMOffset                sectionVMOffsetInSegment;
    };

    bool                    sectionWasRemoved(std::string_view segmentName, std::string_view sectionName) const;
    bool                    sectionWasOptimized(std::string_view segmentName, std::string_view sectionName) const;
    OptimizedSection*       getSection(std::string_view segmentName, std::string_view sectionName);
    const OptimizedSection* getSection(std::string_view segmentName, std::string_view sectionName) const;

    OptimizedSection objcClassNames = { true, "objc class names" };
    OptimizedSection objcMethNames  = { true, "objc method names" };
    OptimizedSection objcMethTypes  = { true, "objc method types" };
    OptimizedSection gots           = { false, "gots" };
    OptimizedSection auth_gots      = { false, "auth gots" };
};

#endif /* SectionCoalescer_h */
