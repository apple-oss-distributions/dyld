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

#include "MetadataVisitor.h"

#if SUPPORT_VM_LAYOUT
#include "MachOAnalyzer.h"
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "ASLRTracker.h"
#endif

#if POINTERS_ARE_UNSLID
#include "DyldSharedCache.h"
#endif

using namespace metadata_visitor;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
typedef cache_builder::Fixup::Cache32 Cache32;
typedef cache_builder::Fixup::Cache64 Cache64;
#endif

//
// MARK: --- ResolvedValue methods ---
//

#if SUPPORT_VM_LAYOUT

ResolvedValue::ResolvedValue(const void* targetValue, VMAddress vmAddr)
    : targetValue(targetValue), vmAddr(vmAddr)
{
}

#else

ResolvedValue::ResolvedValue(const Segment& cacheSegment, VMOffset segmentVMOffset)
    : cacheSegment(cacheSegment), segmentVMOffset(segmentVMOffset)
{
}
ResolvedValue::ResolvedValue(const ResolvedValue& parentValue, const void* childLocation)
    : cacheSegment(parentValue.cacheSegment)
{
    this->segmentVMOffset = VMOffset((uint64_t)((uint8_t*)childLocation - parentValue.cacheSegment.bufferStart));
}

std::optional<uint16_t> ResolvedValue::chainedPointerFormat() const
{
    return this->cacheSegment.onDiskDylibChainedPointerFormat;
}

uint32_t ResolvedValue::segmentIndex() const
{
    return this->cacheSegment.segIndex;
}

#endif // SUPPORT_VM_LAYOUT

void* ResolvedValue::value() const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    return this->cacheSegment.bufferStart + this->segmentVMOffset.rawValue();
#else
    return (void*)this->targetValue;
#endif
}

VMAddress ResolvedValue::vmAddress() const
{
#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    return this->cacheSegment.startVMAddr + this->segmentVMOffset;
#else
    return this->vmAddr;
#endif
}

//
// MARK: --- Visitor methods ---
//

#if POINTERS_ARE_UNSLID

Visitor::Visitor(const DyldSharedCache* dyldCache, const dyld3::MachOAnalyzer* dylibMA)
    : dylibMA(dylibMA), dylibBaseAddress(dylibMA->preferredLoadAddress())
{
    pointerSize = dylibMA->pointerSize();

    this->onDiskDylibChainedPointerBaseAddress = dylibBaseAddress;

    if ( dylibMA->inDyldCache() ) {
        dyldCache->forEachCache(^(const DyldSharedCache *cache, bool& stopCache) {
            cache->forEachSlideInfo(^(uint64_t mappingStartAddress, uint64_t mappingSize, const uint8_t *mappingPagesStart, uint64_t slideInfoOffset, uint64_t slideInfoSize, const dyld_cache_slide_info *slideInfoHeader) {
                if ( slideInfoHeader->version == 1 ) {
                    this->sharedCacheChainedPointerFormat       = SharedCacheFormat::v1;
                    this->onDiskDylibChainedPointerBaseAddress  = VMAddress(0ULL);
                } else if ( slideInfoHeader->version == 2 ) {
                    const dyld_cache_slide_info2* slideInfo = (dyld_cache_slide_info2*)(slideInfoHeader);
                    assert(slideInfo->delta_mask == 0x00FFFF0000000000);
                    this->sharedCacheChainedPointerFormat       = SharedCacheFormat::v2_x86_64_tbi;
                    this->onDiskDylibChainedPointerBaseAddress  = VMAddress(slideInfo->value_add);
                } else if ( slideInfoHeader->version == 3 ) {
                    this->sharedCacheChainedPointerFormat       = SharedCacheFormat::v3;
                    this->onDiskDylibChainedPointerBaseAddress  = VMAddress(dyldCache->unslidLoadAddress());
                } else if ( slideInfoHeader->version == 4 ) {
                    const dyld_cache_slide_info4* slideInfo = (dyld_cache_slide_info4*)(slideInfoHeader);
                    assert(slideInfo->delta_mask == 0x00000000C0000000);
                    this->sharedCacheChainedPointerFormat       = SharedCacheFormat::v4;
                    this->onDiskDylibChainedPointerBaseAddress  = VMAddress(slideInfo->value_add);
                } else {
                    assert(false);
                }
            });
        });
    } else {
        if ( dylibMA->hasChainedFixups() )
            this->chainedPointerFormat = dylibMA->chainedPointerFormat();
    }
}

#elif SUPPORT_VM_LAYOUT

Visitor::Visitor(const dyld3::MachOAnalyzer* dylibMA)
    : dylibMA(dylibMA), dylibBaseAddress(dylibMA->preferredLoadAddress())
{
    pointerSize = dylibMA->pointerSize();
}

#else

// Cache builder dylib
Visitor::Visitor(CacheVMAddress cacheBaseAddress, const dyld3::MachOFile* dylibMF,
                 std::vector<Segment>&& segments, std::optional<VMAddress> selectorStringsBaseAddress,
                 std::vector<uint64_t>&& bindTargets)
    : isOnDiskDylib(false), dylibMF(dylibMF), sharedCacheBaseAddress(cacheBaseAddress),
      segments(std::move(segments)), bindTargets(std::move(bindTargets)),
      selectorStringsBaseAddress(selectorStringsBaseAddress)
{
    pointerSize = dylibMF->pointerSize();

    // Cache dylibs should never have a chain value set, as they always use the in-cache builder
    // representation of values
    for ( const Segment& segment : this->segments ) {
        assert(!segment.onDiskDylibChainedPointerFormat.has_value());
    }
}

// On disk dylib/executable
Visitor::Visitor(VMAddress chainedPointerBaseAddress, const dyld3::MachOFile* dylibMF,
                 std::vector<Segment>&& segments, std::optional<VMAddress> selectorStringsBaseAddress,
                 std::vector<uint64_t>&& bindTargets)
    : isOnDiskDylib(true), dylibMF(dylibMF), onDiskDylibChainedPointerBaseAddress(chainedPointerBaseAddress),
      segments(std::move(segments)), bindTargets(std::move(bindTargets)),
      selectorStringsBaseAddress(selectorStringsBaseAddress)
{
    pointerSize = dylibMF->pointerSize();

    // On-disk dylibs should have a chain value set, even if its a 0 for opcode fixup dylibs
    for ( const Segment& segment : this->segments ) {
        assert(segment.onDiskDylibChainedPointerFormat.has_value());
    }
}

#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
VMAddress Visitor::sharedCacheSelectorStringsBaseAddress() const
{
    return this->selectorStringsBaseAddress.value();
}

VMAddress Visitor::getOnDiskDylibChainedPointerBaseAddress() const
{
    assert(this->isOnDiskDylib);
    return this->onDiskDylibChainedPointerBaseAddress;
}

const dyld3::MachOFile* Visitor::mf() const
{
    return this->dylibMF;
}

bool Visitor::isOnDiskBinary() const
{
    return this->isOnDiskDylib;
}
#endif

ResolvedValue Visitor::getField(const ResolvedValue& parent, const void* fieldPos) const
{
#if SUPPORT_VM_LAYOUT
    // In dyld, we just use raw pointers for everything, and don't need to indirect via segment+offset like
    // in the cache builder
    VMOffset offsetInDylib((uint64_t)fieldPos - (uint64_t)this->dylibMA);
    VMAddress fieldVMAddr(this->dylibBaseAddress + offsetInDylib);
    return ResolvedValue(fieldPos, fieldVMAddr);
#else
    // In the cache builder, everything is an offset in a segment, as we don't know where the segments
    // will be in memory when running
    return ResolvedValue(parent, fieldPos);
#endif
}

ResolvedValue Visitor::getValueFor(VMAddress vmAddr) const
{
#if SUPPORT_VM_LAYOUT
    // In dyld, we just use raw pointers for everything, and don't need to indirect via segment+offset like
    // in the cache builder
    VMOffset offsetInDylib = vmAddr - this->dylibBaseAddress;
    const void* valueInDylib = (const uint8_t*)this->dylibMA + offsetInDylib.rawValue();
    return ResolvedValue(valueInDylib, vmAddr);
#else
    // Find the segment containing the target address
    for ( const Segment& cacheSegment : segments ) {
        if ( (vmAddr >= cacheSegment.startVMAddr) && (vmAddr < cacheSegment.endVMAddr) ) {
            // Skip segments which don't contribute to the cache.  This is a hack to account
            // for LINKEDIT, which doesn't really get its own buffer.  We don't want to match
            // an address to LINKEDIT, when we actually wanted to find it in the selector strings
            // "segment" we also track here
            if ( cacheSegment.bufferStart == nullptr )
                continue;

            VMOffset segmentVMOffset = vmAddr - cacheSegment.startVMAddr;
            return ResolvedValue(cacheSegment, segmentVMOffset);
        }
    }

    assert(0);
#endif
}

// Dereferences the given value.  It must not resolve to nullptr
ResolvedValue Visitor::resolveRebase(const ResolvedValue& value) const
{
#if POINTERS_ARE_UNSLID
    uint64_t runtimeOffset = 0;

    if ( this->sharedCacheChainedPointerFormat != SharedCacheFormat::none ) {
        // Crack the shared cache slide format
        switch ( this->sharedCacheChainedPointerFormat ) {
            case SharedCacheFormat::none:
                assert(false);
            case SharedCacheFormat::v1: {
                // Nothing to do here.  We don't have chained fixup bits to remove, or a value_add to apply
                break;
            }
            case SharedCacheFormat::v2_x86_64_tbi: {
                const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                uint64_t rawValue = fixup->raw64;

                const uint64_t   deltaMask    = 0x00FFFF0000000000;
                const uint64_t   valueMask    = ~deltaMask;
                rawValue = (rawValue & valueMask);
                // Already a runtime offset, so no need to do anything with valueAdd
                runtimeOffset = rawValue;
                break;
            }
            case SharedCacheFormat::v3: {
                // Just use the chained pointer format for arm64e
                auto* chainedValue = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                chainedValue->isRebase(DYLD_CHAINED_PTR_ARM64E,
                                       onDiskDylibChainedPointerBaseAddress.rawValue(),
                                       runtimeOffset);
                break;
            }
            case SharedCacheFormat::v4: {
                const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                uint64_t rawValue = fixup->raw32;

                const uint64_t   deltaMask    = 0x00000000C0000000;
                const uint64_t   valueMask    = ~deltaMask;
                rawValue = (rawValue & valueMask);
                // Already a runtime offset, so no need to do anything with valueAdd
                runtimeOffset = rawValue;
                break;
            }
        }
    } else {
        const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
        if ( this->chainedPointerFormat == 0 ) {
            // HACK: 32-bit cache dylibs don't have enough bits to have real chains, so we pretend they
            // have no chains, just raw VMAddr's
            assert(dylibMA->hasOpcodeFixups());

            // HACK: This is a binary without chained fixups.  Is it safe to assume this is a rebase?
            uint64_t rebaseVMAddr = (pointerSize == 8) ? fixup->raw64 : fixup->raw32;
            runtimeOffset = rebaseVMAddr - this->onDiskDylibChainedPointerBaseAddress.rawValue();
        } else {
            bool isRebase = fixup->isRebase(this->chainedPointerFormat,
                                            onDiskDylibChainedPointerBaseAddress.rawValue(),
                                            runtimeOffset);
            assert(isRebase);
        }
    }

    VMAddress targetVMAddress = onDiskDylibChainedPointerBaseAddress + VMOffset(runtimeOffset);
    return this->getValueFor(targetVMAddress);
#elif SUPPORT_VM_LAYOUT
    // In dyld, we just use raw pointers for everything, and don't need to indirect via segment+offset like
    // in the cache builder
    const void* targetValue = (const void*)*(uintptr_t*)value.value();

    // FIXME: We didn't expect a null here.  Should we find a way to error out, or just let the parser
    // crash with a nullptr dereference.
    if ( targetValue == nullptr )
        return ResolvedValue(nullptr, VMAddress());

    // The value may have been signed.  Strip the signature if that is the case
#if __has_feature(ptrauth_calls)
    targetValue = __builtin_ptrauth_strip(targetValue, ptrauth_key_asia);
#endif

    VMOffset offsetInDylib((uint64_t)targetValue - (uint64_t)this->dylibMA);
    return ResolvedValue(targetValue, this->dylibBaseAddress + offsetInDylib);
#else
    // In on-disk dylibs, we crack the chained fixups or other fixups
    if ( this->isOnDiskBinary() ) {
        uint64_t runtimeOffset = 0;
        const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
        uint16_t chainedPointerFormat = value.chainedPointerFormat().value();
        if ( chainedPointerFormat == 0 ) {
            // HACK: 32-bit cache dylibs don't have enough bits to have real chains, so we pretend they
            // have no chains, just raw VMAddr's
            assert(dylibMF->hasOpcodeFixups() || (dylibMF->inDyldCache() && !dylibMF->is64()));

            // HACK: This is a binary without chained fixups.  Is it safe to assume this is a rebase?
            uint64_t rebaseVMAddr = (pointerSize == 8) ? fixup->raw64 : fixup->raw32;
            runtimeOffset = rebaseVMAddr - this->onDiskDylibChainedPointerBaseAddress.rawValue();
        } else {
            bool isRebase = fixup->isRebase(chainedPointerFormat, onDiskDylibChainedPointerBaseAddress.rawValue(), runtimeOffset);
            assert(isRebase);
        }

        VMAddress targetVMAddress = onDiskDylibChainedPointerBaseAddress + VMOffset(runtimeOffset);
        return this->getValueFor(targetVMAddress);
    } else {
        // Cache builder dylib.  These use values in a packed format
        if ( this->pointerSize == 4 ) {
            const void* fixupLocation = value.value();
            assert(!Cache32::isNull(fixupLocation));

            CacheVMAddress targetCacheVMAddress = Cache32::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return this->getValueFor(targetVMAddress);
        } else {
            const void* fixupLocation = value.value();
            assert(!Cache64::isNull(fixupLocation));

            CacheVMAddress targetCacheVMAddress = Cache64::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return this->getValueFor(targetVMAddress);
        }
    }
#endif
}

// Dereferences the given value.  It may be either a bind or a rebase.  It must not resolve to nullptr
ResolvedValue Visitor::resolveBindOrRebase(const ResolvedValue& value, bool& wasBind) const
{
#if SUPPORT_VM_LAYOUT
    // dyld will never see a bind or a rebase, just live values.  Use resolveRebase for this as it already
    // handles this case
    wasBind = false;
    return this->resolveRebase(value);
#else
    // In on-disk dylibs, we crack the chained fixups or other fixups
    if ( this->isOnDiskBinary() ) {
        // Check if this is a bind
        {
            const auto* fixupLoc = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
            uint16_t chainedPointerFormat = value.chainedPointerFormat().value();

            // Follow the class reference to get to the actual class
            // With objc patching, this might be a bind to self
            uint32_t bindOrdinal;
            int64_t  bindAddend;
            if ( (chainedPointerFormat != 0) && fixupLoc->isBind(chainedPointerFormat, bindOrdinal, bindAddend) ) {
                wasBind = true;
                VMAddress targetVMAddress(this->bindTargets[bindOrdinal] + bindAddend);
                return this->getValueFor(targetVMAddress);
            }
        }

        // Fall back to resolveRebase() which can handle rebases
        wasBind = false;
        return this->resolveRebase(value);
    } else {
        // Cache builder dylibs don't have binds, so fall back to resolveRebase()
        wasBind = false;
        return this->resolveRebase(value);
    }
#endif
}

std::optional<ResolvedValue> Visitor::resolveOptionalRebase(const ResolvedValue& value) const
{
#if POINTERS_ARE_UNSLID
    uint64_t runtimeOffset = 0;

    if ( this->sharedCacheChainedPointerFormat != SharedCacheFormat::none ) {
        // Crack the shared cache slide format
        switch ( this->sharedCacheChainedPointerFormat ) {
            case SharedCacheFormat::none:
                assert(false);
            case SharedCacheFormat::v1: {
                // Nothing to do here.  We don't have chained fixup bits to remove, or a value_add to apply
                break;
            }
            case SharedCacheFormat::v2_x86_64_tbi: {
                const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                uint64_t rawValue = fixup->raw64;
                if ( rawValue == 0 )
                    return { };

                const uint64_t   deltaMask    = 0x00FFFF0000000000;
                const uint64_t   valueMask    = ~deltaMask;
                rawValue = (rawValue & valueMask);
                // Already a runtime offset, so no need to do anything with valueAdd
                runtimeOffset = rawValue;
                break;
            }
            case SharedCacheFormat::v3: {
                // Just use the chained pointer format for arm64e
                auto* chainedValue = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                if ( chainedValue->raw64 == 0 )
                    return { };

                chainedValue->isRebase(DYLD_CHAINED_PTR_ARM64E,
                                       onDiskDylibChainedPointerBaseAddress.rawValue(),
                                       runtimeOffset);
                break;
            }
            case SharedCacheFormat::v4: {
                const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
                uint64_t rawValue = fixup->raw32;
                if ( rawValue == 0 )
                    return { };

                const uint64_t   deltaMask    = 0x00000000C0000000;
                const uint64_t   valueMask    = ~deltaMask;
                rawValue = (rawValue & valueMask);
                // Already a runtime offset, so no need to do anything with valueAdd
                runtimeOffset = rawValue;
                break;
            }
        }
    } else {
        const auto* fixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
        if ( this->chainedPointerFormat == 0 ) {
            // HACK: 32-bit cache dylibs don't have enough bits to have real chains, so we pretend they
            // have no chains, just raw VMAddr's
            assert(dylibMA->hasOpcodeFixups());

            // HACK: This is a binary without chained fixups.  Is it safe to assume this is a rebase?
            uint64_t rebaseVMAddr = (pointerSize == 8) ? fixup->raw64 : fixup->raw32;
            if ( rebaseVMAddr == 0 )
                return { };

            runtimeOffset = rebaseVMAddr - this->onDiskDylibChainedPointerBaseAddress.rawValue();
        } else {
            if ( pointerSize == 8 ) {
                if ( fixup->raw64 == 0 )
                    return { };
            } else {
                if ( fixup->raw32 == 0 )
                    return { };
            }

            bool isRebase = fixup->isRebase(this->chainedPointerFormat,
                                            onDiskDylibChainedPointerBaseAddress.rawValue(),
                                            runtimeOffset);
            assert(isRebase);
        }
    }

    VMAddress targetVMAddress = onDiskDylibChainedPointerBaseAddress + VMOffset(runtimeOffset);
    return this->getValueFor(targetVMAddress);
#elif SUPPORT_VM_LAYOUT
    // In dyld, we just use raw pointers for everything, and don't need to indirect via segment+offset like
    // in the cache builder
    const void* targetValue = (const void*)*(uintptr_t*)value.value();

    // FIXME: We didn't expect a null here.  Should we find a way to error out, or just let the parser
    // crash with a nullptr dereference.
    if ( targetValue == nullptr )
        return std::nullopt;

    // The value may have been signed.  Strip the signature if that is the case
#if __has_feature(ptrauth_calls)
    targetValue = __builtin_ptrauth_strip(targetValue, ptrauth_key_asia);
#endif

    VMOffset offsetInDylib((uint64_t)targetValue - (uint64_t)this->dylibMA);
    return ResolvedValue(targetValue, this->dylibBaseAddress + offsetInDylib);
#else
    // In on-disk dylibs, we crack the chained fixups or other fixups
    if ( this->isOnDiskBinary() ) {
        uint64_t runtimeOffset = 0;
        const auto* fixupLoc = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
        uint16_t chainedPointerFormat = value.chainedPointerFormat().value();
        if ( chainedPointerFormat == 0 ) {
            assert(dylibMF->hasOpcodeFixups() || (dylibMF->inDyldCache() && !dylibMF->is64()));

            // HACK: This is a binary without chained fixups.  Is it safe to assume this is a rebase?
            uint64_t rebaseVMAddr = (pointerSize == 8) ? fixupLoc->raw64 : fixupLoc->raw32;

            // Assume null VMAddr's means there's no value here
            if ( rebaseVMAddr == 0 )
                return { };

            runtimeOffset = rebaseVMAddr - this->onDiskDylibChainedPointerBaseAddress.rawValue();
        } else {
            bool isRebase = fixupLoc->isRebase(chainedPointerFormat, onDiskDylibChainedPointerBaseAddress.rawValue(), runtimeOffset);
            assert(isRebase);

            if ( pointerSize == 8 ) {
                if ( fixupLoc->raw64 == 0 )
                    return { };
            } else {
                if ( fixupLoc->raw32 == 0 )
                    return { };
            }

            // Assume an offset of 0 means its null.  There's no good reason for an objc class to have an offset of 0 from the cache.
            if ( runtimeOffset == 0 )
                return { };
        }

        VMAddress targetVMAddress = this->onDiskDylibChainedPointerBaseAddress + VMOffset(runtimeOffset);
        return this->getValueFor(targetVMAddress);
    } else {
        // Cache builder dylib.  These use values in a packed format
        if ( this->pointerSize == 4 ) {
            const void* fixupLocation = value.value();
            if ( Cache32::isNull(fixupLocation) )
                return { };

            CacheVMAddress targetCacheVMAddress = Cache32::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return this->getValueFor(targetVMAddress);
        } else {
            const void* fixupLocation = value.value();
            if ( Cache64::isNull(fixupLocation) )
                return { };

            CacheVMAddress targetCacheVMAddress = Cache64::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return this->getValueFor(targetVMAddress);
        }
    }
#endif
}

std::optional<VMAddress> Visitor::resolveOptionalRebaseToVMAddress(const ResolvedValue& value) const
{
#if SUPPORT_VM_LAYOUT
    // In dyld, we just use raw pointers for everything, and don't need to indirect via segment+offset like
    // in the cache builder
    const void* targetValue = (const void*)*(uintptr_t*)value.value();

    // FIXME: We didn't expect a null here.  Should we find a way to error out, or just let the parser
    // crash with a nullptr dereference.
    if ( targetValue == nullptr )
        return std::nullopt;

    // The value may have been signed.  Strip the signature if that is the case
#if __has_feature(ptrauth_calls)
    targetValue = __builtin_ptrauth_strip(targetValue, ptrauth_key_asia);
#endif

    VMOffset offsetInDylib((uint64_t)targetValue - (uint64_t)this->dylibMA);
    return ResolvedValue(targetValue, this->dylibBaseAddress + offsetInDylib).vmAddress();
#else
    // In on-disk dylibs, we crack the chained fixups or other fixups
    if ( this->isOnDiskBinary() ) {
        uint64_t runtimeOffset = 0;
        const auto* fixupLoc = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)value.value();
        uint16_t chainedPointerFormat = value.chainedPointerFormat().value();
        if ( chainedPointerFormat == 0 ) {
            assert(dylibMF->hasOpcodeFixups() || (dylibMF->inDyldCache() && !dylibMF->is64()));

            // HACK: This is a binary without chained fixups.  Is it safe to assume this is a rebase?
            uint64_t rebaseVMAddr = (pointerSize == 8) ? fixupLoc->raw64 : fixupLoc->raw32;

            // Assume null VMAddr's means there's no value here
            if ( rebaseVMAddr == 0 )
                return { };

            runtimeOffset = rebaseVMAddr - this->onDiskDylibChainedPointerBaseAddress.rawValue();
        } else {
            bool isRebase = fixupLoc->isRebase(chainedPointerFormat, onDiskDylibChainedPointerBaseAddress.rawValue(), runtimeOffset);
            assert(isRebase);

            if ( pointerSize == 8 ) {
                if ( fixupLoc->raw64 == 0 )
                    return { };
            } else {
                if ( fixupLoc->raw32 == 0 )
                    return { };
            }

            // Assume an offset of 0 means its null.  There's no good reason for an objc class to have an offset of 0 from the cache.
            if ( runtimeOffset == 0 )
                return { };
        }

        VMAddress targetVMAddress = this->onDiskDylibChainedPointerBaseAddress + VMOffset(runtimeOffset);
        return targetVMAddress;
    } else {
        // Cache builder dylib.  These use values in a packed format
        if ( this->pointerSize == 4 ) {
            const void* fixupLocation = value.value();
            if ( Cache32::isNull(fixupLocation) )
                return { };

            CacheVMAddress targetCacheVMAddress = Cache32::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return targetVMAddress;
        } else {
            const void* fixupLocation = value.value();
            if ( Cache64::isNull(fixupLocation) )
                return { };

            CacheVMAddress targetCacheVMAddress = Cache64::getCacheVMAddressFromLocation(sharedCacheBaseAddress, fixupLocation);
            VMAddress targetVMAddress(targetCacheVMAddress.rawValue());
            return targetVMAddress;
        }
    }
#endif
}


#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Visitor::setTargetVMAddress(ResolvedValue& value, CacheVMAddress vmAddr, const dyld3::MachOFile::PointerMetaData& PMD) const
{
    assert(!isOnDiskDylib);

    void* fixupLocation = value.value();
    if ( this->pointerSize == 4 ) {
        Cache32::setLocation(this->sharedCacheBaseAddress, fixupLocation, vmAddr);
    } else {
        uint8_t high8 = 0;
        Cache64::setLocation(this->sharedCacheBaseAddress, fixupLocation, vmAddr, high8,
                             PMD.diversity, PMD.usesAddrDiversity, PMD.key,
                             PMD.authenticated);
    }
}

// Update just the target address in a given location.  Doesn't change any of the other fields
// such as high8 or PointerMetadata
void Visitor::updateTargetVMAddress(ResolvedValue& value, CacheVMAddress vmAddr) const
{
    assert(!isOnDiskDylib);

    void* fixupLocation = value.value();
    if ( this->pointerSize == 4 ) {
        Cache32::updateLocationToCacheVMAddress(this->sharedCacheBaseAddress, fixupLocation, vmAddr);
    } else {
        Cache64::updateLocationToCacheVMAddress(this->sharedCacheBaseAddress, fixupLocation, vmAddr);
    }
}
#endif
