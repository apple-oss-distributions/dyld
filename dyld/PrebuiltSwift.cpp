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


#include "objc-shared-cache.h"
#include "OptimizerObjC.h"
#include "PrebuiltSwift.h"
#include "SwiftVisitor.h"

using metadata_visitor::ResolvedValue;
using metadata_visitor::SwiftPointer;
using metadata_visitor::SwiftVisitor;
using metadata_visitor::SwiftConformance;

namespace dyld4 {

static bool getBindTarget(RuntimeState& state, const uint64_t vmAddr, PrebuiltLoader::BindTarget& target)
{
    bool found = false;
    for (const Loader* ldr : state.loaded) {
        const mach_o::MachOFileRef mf = ldr->mf(state);
        uint64_t baseAddress = mf->preferredLoadAddress();
        uint64_t mappedSize = mf->mappedSize();

        if ( vmAddr < baseAddress )
            continue;
        if ( vmAddr >= (baseAddress + mappedSize) )
            continue;

        target.loader = ldr;
        target.runtimeOffset = vmAddr - baseAddress;
        found = true;
        break;
    }
    return found;
}

#if !(BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS)
static bool getBindTarget(RuntimeState& state, const void* runtimeAddress, PrebuiltLoader::BindTarget& target)
{
    __block bool found = false;
    uint64_t index = 0;
    for (const Loader* ldr : state.loaded) {
        const void* sgAddr;
        uint64_t    sgSize;
        uint8_t     sgPerm;
        if ( ldr->contains(state, runtimeAddress, &sgAddr, &sgSize, &sgPerm) ) {
            uint64_t loadAddress = (uint64_t)ldr->loadAddress(state);
            target.loader = ldr;
            target.runtimeOffset = (uint64_t)runtimeAddress - loadAddress;
            found = true;
            break;
        }
        index++;
    }
    return found;
}
#endif // !(BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS)


// dyld at runtime can just chase pointers, but in offline tools, we need to build
// a map of where all the fixups will point, to let us chase pointers
#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS
typedef std::pair<Loader::ResolvedSymbol, uint64_t> TargetAndAddend;
typedef dyld3::Map<uint64_t, TargetAndAddend, HashUInt64, EqualUInt64> VMAddrToFixupTargetMap;

static void getFixupTargets(RuntimeState& state, Diagnostics& diag, const JustInTimeLoader* ldr,
                            VMAddrToFixupTargetMap& vmAddrToFixupTargetMap)
{
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Loader::ResolvedSymbol, bindTargets, 32);
    STACK_ALLOC_OVERFLOW_SAFE_ARRAY(Loader::ResolvedSymbol, overrideBindTargets, 32);

    ldr->forEachBindTarget(diag, state, nullptr, true,
                           ^(const Loader::ResolvedSymbol& resolvedTarget, bool& stop) {
        // Regular and lazy binds
        bindTargets.push_back(resolvedTarget);
    }, ^(const Loader::ResolvedSymbol& resolvedTarget, bool& stop) {
        // Opcode based weak binds
        overrideBindTargets.push_back(resolvedTarget);
    });

    if ( diag.hasError() )
        return;

    const mach_o::MachOFileRef& mf = ldr->mf(state);

    ldr->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        mach_o::Fixups fixups(layout);

        uint64_t loadAddress = mf->preferredLoadAddress();
        if ( mf->hasChainedFixups() ) {
            // walk all chains
            auto handler = ^(dyld3::MachOFile::ChainedFixupPointerOnDisk *fixupLocation,
                             VMAddress fixupVMAddr, uint16_t pointerFormat,
                             bool& stopChain) {
                uint32_t bindOrdinal;
                int64_t  addend;
                uint64_t targetRuntimeOffset;
                if ( fixupLocation->isBind(pointerFormat, bindOrdinal, addend) ) {
                    if ( bindOrdinal < bindTargets.count() ) {
                        TargetAndAddend targetAndAddend = { bindTargets[bindOrdinal], addend };
                        vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
                    }
                    else {
                        diag.error("out of range bind ordinal %d (max %lu)", bindOrdinal, bindTargets.count());
                        stopChain = true;
                    }
                } else if ( fixupLocation->isRebase(pointerFormat, loadAddress, targetRuntimeOffset) ) {
                    Loader::ResolvedSymbol resolvedTarget;
                    resolvedTarget.kind = Loader::ResolvedSymbol::Kind::rebase;
                    resolvedTarget.targetRuntimeOffset = targetRuntimeOffset;
                    TargetAndAddend targetAndAddend = { resolvedTarget, 0 };
                    vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
                }
            };

            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* startsInfo) {
                fixups.forEachFixupChainSegment(diag, startsInfo, ^(const dyld_chained_starts_in_segment *segInfo,
                                                                    uint32_t segIndex, bool &stopSegment) {
                    VMAddress segmentVMAddr(layout.segments[segIndex].vmAddr);
                    auto adaptor = ^(dyld3::MachOFile::ChainedFixupPointerOnDisk *fixupLocation,
                                     uint64_t fixupSegmentOffset,
                                     bool &stopChain) {
                        VMAddress fixupVMAddr = segmentVMAddr + VMOffset(fixupSegmentOffset);
                        handler(fixupLocation, fixupVMAddr, segInfo->pointer_format, stopChain);
                    };
                    fixups.forEachFixupInSegmentChains(diag, segInfo, segIndex, true, adaptor);
                });
            });
            if ( diag.hasError() )
                return;
        } else if ( mf->hasOpcodeFixups() ) {
            // process all bind opcodes
            fixups.forEachBindLocation_Opcodes(diag, ^(uint64_t runtimeOffset, uint32_t segmentIndex,
                                                       unsigned targetIndex, bool& fixupsStop) {
                VMAddress fixupVMAddr = VMAddress(loadAddress) + VMOffset(runtimeOffset);
                if ( targetIndex < bindTargets.count() ) {
                    TargetAndAddend targetAndAddend = { bindTargets[targetIndex], 0 };
                    vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
                }
                else {
                    diag.error("out of range bind ordinal %d (max %lu)", targetIndex, bindTargets.count());
                    fixupsStop = true;
                }
            }, ^(uint64_t runtimeOffset, uint32_t segmentIndex,
                 unsigned overrideBindTargetIndex, bool& fixupsStop) {
                VMAddress fixupVMAddr = VMAddress(loadAddress) + VMOffset(runtimeOffset);
                if ( overrideBindTargetIndex < overrideBindTargets.count() ) {
                    TargetAndAddend targetAndAddend = { bindTargets[overrideBindTargetIndex], 0 };
                    vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
                }
                else {
                    diag.error("out of range bind ordinal %d (max %lu)", overrideBindTargetIndex, overrideBindTargets.count());
                    fixupsStop = true;
                }
            });
            if ( diag.hasError() )
                return;

            // process all rebase opcodes
            uint32_t ptrSize = mf->pointerSize();
            fixups.forEachRebaseLocation_Opcodes(diag, ^(uint64_t fixupRuntimeOffset, uint32_t segmentIndex,
                                                      bool& stop) {
                const mach_o::SegmentLayout& segment = layout.segments[segmentIndex];
                VMAddress fixupVMAddr = VMAddress(loadAddress) + VMOffset(fixupRuntimeOffset);
                VMOffset segmentOffset    = fixupVMAddr - VMAddress(segment.vmAddr);
                uint8_t* fixupLoc         = (uint8_t*)segment.buffer + segmentOffset.rawValue();

                uint64_t pointerValue = 0;
                if ( ptrSize == 8 ) {
                    pointerValue = *(uint64_t*)fixupLoc;
                } else {
                    pointerValue = *(uint32_t*)fixupLoc;
                }

                Loader::ResolvedSymbol resolvedTarget;
                resolvedTarget.kind = Loader::ResolvedSymbol::Kind::rebase;
                resolvedTarget.targetRuntimeOffset = pointerValue - loadAddress;
                TargetAndAddend targetAndAddend = { resolvedTarget, 0 };
                vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
            });
            if ( diag.hasError() )
                return;
        }
        else {
            // process external relocations
            fixups.forEachBindLocation_Relocations(diag, ^(uint64_t runtimeOffset, unsigned targetIndex,
                                                           bool& fixupsStop) {
               VMAddress fixupVMAddr = VMAddress(loadAddress) + VMOffset(runtimeOffset);
                if ( targetIndex < bindTargets.count() ) {
                    TargetAndAddend targetAndAddend = { bindTargets[targetIndex], 0 };
                    vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
                }
                else {
                    diag.error("out of range bind ordinal %d (max %lu)", targetIndex, bindTargets.count());
                    fixupsStop = true;
                }
            });
            if ( diag.hasError() )
                return;

            uint32_t ptrSize = mf->pointerSize();
            fixups.forEachRebaseLocation_Relocations(diag,
                                                     ^(uint64_t fixupRuntimeOffset, uint32_t segmentIndex,
                                                       bool &stop) {
                const mach_o::SegmentLayout& segment = layout.segments[segmentIndex];
                VMAddress fixupVMAddr = VMAddress(loadAddress) + VMOffset(fixupRuntimeOffset);
                VMOffset segmentOffset    = fixupVMAddr - VMAddress(segment.vmAddr);
                uint8_t* fixupLoc         = (uint8_t*)segment.buffer + segmentOffset.rawValue();

                uint64_t pointerValue = 0;
                if ( ptrSize == 8 ) {
                    pointerValue = *(uint64_t*)fixupLoc;
                } else {
                    pointerValue = *(uint32_t*)fixupLoc;
                }

                Loader::ResolvedSymbol resolvedTarget;
                resolvedTarget.kind = Loader::ResolvedSymbol::Kind::rebase;
                resolvedTarget.targetRuntimeOffset = pointerValue - loadAddress;
                TargetAndAddend targetAndAddend = { resolvedTarget, 0 };
                vmAddrToFixupTargetMap[fixupVMAddr.rawValue()] = targetAndAddend;
            });
        }
    });
}

#endif

static SwiftVisitor makeSwiftVisitor(Diagnostics& diag, RuntimeState& state,
                                     const Loader* ldr)
{

#if POINTERS_ARE_UNSLID
    const dyld3::MachOAnalyzer* dylibMA = ldr->analyzer(state);

    SwiftVisitor swiftVisitor(state.config.dyldCache.addr, dylibMA);
    return swiftVisitor;
#elif SUPPORT_VM_LAYOUT
    const dyld3::MachOAnalyzer* dylibMA = ldr->analyzer(state);

    SwiftVisitor swiftVisitor(dylibMA);
    return swiftVisitor;
#else
    const dyld3::MachOFile* mf = ldr->mf(state);
    VMAddress dylibBaseAddress(mf->preferredLoadAddress());

    __block std::vector<metadata_visitor::Segment> segments;
    __block std::vector<uint64_t> bindTargets;
    ldr->withLayout(diag, state, ^(const mach_o::Layout &layout) {
        for ( uint32_t segIndex = 0; segIndex != layout.segments.size(); ++segIndex ) {
            const auto& layoutSegment = layout.segments[segIndex];
            std::optional<uint16_t> onDiskDylibChainedPointerFormat;
            if ( !ldr->dylibInDyldCache )
                onDiskDylibChainedPointerFormat = 0;
            metadata_visitor::Segment segment {
                .startVMAddr = VMAddress(layoutSegment.vmAddr),
                .endVMAddr = VMAddress(layoutSegment.vmAddr + layoutSegment.vmSize),
                .bufferStart = (uint8_t*)layoutSegment.buffer,
                .onDiskDylibChainedPointerFormat = onDiskDylibChainedPointerFormat,
                .segIndex = segIndex
            };
            segments.push_back(std::move(segment));
        }

        // Shared cache dylibs don't need to get the bind targets so we can return early
        if ( ldr->dylibInDyldCache )
            return;

        // Add chained fixup info to each segment, if we have it
        if ( mf->hasChainedFixups() ) {
            mach_o::Fixups fixups(layout);
            fixups.withChainStarts(diag, ^(const dyld_chained_starts_in_image* starts) {
                mach_o::Fixups::forEachFixupChainSegment(diag, starts,
                                                         ^(const dyld_chained_starts_in_segment *segInfo, uint32_t segIndex, bool &stop) {
                    segments[segIndex].onDiskDylibChainedPointerFormat = segInfo->pointer_format;
                });
            });
        }

        // ObjC patching needs the bind targets for interposable references to the classes
        // build targets table
        if ( mf->hasChainedFixupsLoadCommand() ) {
            mach_o::Fixups fixups(layout);
            fixups.forEachBindTarget_ChainedFixups(diag, ^(const mach_o::Fixups::BindTargetInfo &info, bool &stop) {
                if ( info.libOrdinal != BIND_SPECIAL_DYLIB_SELF ) {
                    bindTargets.push_back(0);
                    return;
                }

                mach_o::Layout::FoundSymbol foundInfo;
                if ( !layout.findExportedSymbol(diag, info.symbolName, info.weakImport, foundInfo) ) {
                    bindTargets.push_back(0);
                    return;
                }

                // We only support header offsets in this dylib, as we are looking for self binds
                // which are likely only to classes
                if ( (foundInfo.kind != mach_o::Layout::FoundSymbol::Kind::headerOffset)
                    || (foundInfo.foundInDylib.value() != mf) ) {
                    bindTargets.push_back(0);
                    return;
                }

                uint64_t vmAddr = layout.textUnslidVMAddr() + foundInfo.value;
                bindTargets.push_back(vmAddr);
            });
        }
    });

    if ( ldr->dylibInDyldCache ) {
        std::optional<VMAddress> selectorStringsBaseAddress;
        CacheVMAddress sharedCacheBaseAddress(state.config.dyldCache.unslidLoadAddress);
        SwiftVisitor swiftVisitor(sharedCacheBaseAddress, mf,
                                  std::move(segments), selectorStringsBaseAddress,
                                  std::move(bindTargets));
        return swiftVisitor;
    } else {
        std::optional<VMAddress> selectorStringsBaseAddress;
        SwiftVisitor swiftVisitor(dylibBaseAddress, mf,
                                  std::move(segments), selectorStringsBaseAddress,
                                  std::move(bindTargets));
        return swiftVisitor;
    }
#endif
}

bool PrebuiltSwift::findProtocolConformances(Diagnostics& diag, PrebuiltObjC& prebuiltObjC, RuntimeState& state)
{
    if (prebuiltObjC.objcImages.count() == 0) {
        diag.error("Skipped optimizing Swift protocols due to missing objc class optimisations from the on-disk binary");
        return false;
    }

    const objc::ClassHashTable* classHashTable = state.config.dyldCache.objcClassHashTable;
    if ( classHashTable == nullptr ) {
        diag.error("Skipped optimizing Swift protocols due to missing objc class optimisations");
        return false;
    }

    const void* headerInfoRO = state.config.dyldCache.objcHeaderInfoRO;
    const void* headerInfoRW = state.config.dyldCache.objcHeaderInfoRW;
    if ( (headerInfoRO == nullptr) || (headerInfoRW == nullptr) ) {
        diag.error("Skipped optimizing Swift protocols due to missing objc header infos");
        return false;
    }
    VMAddress headerInfoROUnslidVMAddr(state.config.dyldCache.objcHeaderInfoROUnslidVMAddr);
    VMAddress sharedCacheBaseAddress(state.config.dyldCache.unslidLoadAddress);

    for (const Loader* ldr : state.loaded) {
        if ( ldr->isPrebuilt )
            continue;

        const mach_o::MachOFileRef mf = ldr->mf(state);
        const uint64_t loadAddress = mf->preferredLoadAddress();

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS
        const JustInTimeLoader* jitLoader = (const JustInTimeLoader*)ldr;
        __block VMAddrToFixupTargetMap vmAddrToFixupTargetMap;
        getFixupTargets(state, diag, jitLoader, vmAddrToFixupTargetMap);
        if ( diag.hasError() )
            return false;
#else
        const dyld3::MachOAnalyzer* ma = ldr->analyzer(state);
        uint32_t ptrSize = ma->pointerSize();
#endif

        auto isNull = ^(const SwiftPointer& ptr) {
            // Direct pointers are never null
            if ( ptr.isDirect )
                return false;

#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS
            auto it = vmAddrToFixupTargetMap.find(ptr.targetValue.vmAddress().rawValue());
            if ( it != vmAddrToFixupTargetMap.end() ) {
                const PrebuiltLoader::BindTargetRef& bindTarget = it->second.first;
                if ( bindTarget.isAbsolute() && (bindTarget.offset() == 0) )
                    return true;
            }
#else
            uint64_t runtimeOffset = ptr.targetValue.vmAddress().rawValue() - loadAddress;
            uint64_t pointerValue = 0;
            if ( ptrSize == 8 ) {
                pointerValue = *(uint64_t*)((uint8_t*)ma + runtimeOffset);
            } else {
                pointerValue = *(uint32_t*)((uint8_t*)ma + runtimeOffset);
            }
            // This might be a pointer to a missing weak import.  If that is the case, just skip it
            if ( pointerValue == 0 )
                return true;
#endif
            return false;
        };

        auto getTarget = ^(const SwiftPointer& ptr) {
            if ( ptr.isDirect ) {
                std::optional<Loader::BindTarget> target;
                uint64_t runtimeOffset = ptr.targetValue.vmAddress().rawValue() - loadAddress;
                target = Loader::BindTarget({ ldr, runtimeOffset });
                return target;
            }
#if BUILDING_CACHE_BUILDER || BUILDING_CLOSURE_UTIL || BUILDING_CACHE_BUILDER_UNIT_TESTS
            auto it = vmAddrToFixupTargetMap.find(ptr.targetValue.vmAddress().rawValue());
            if ( it != vmAddrToFixupTargetMap.end() ) {
                const Loader::ResolvedSymbol& resolvedTarget = it->second.first;
                switch ( resolvedTarget.kind ) {
                    case Loader::ResolvedSymbol::Kind::rebase: {
                        uint64_t runtimeOffset = resolvedTarget.targetRuntimeOffset + it->second.second;
                        std::optional<Loader::BindTarget> target;
                        target = Loader::BindTarget({ ldr, runtimeOffset });
                        return target;
                    }
                    case Loader::ResolvedSymbol::Kind::bindToImage: {
                        uint64_t runtimeOffset = resolvedTarget.targetRuntimeOffset + it->second.second;
                        std::optional<Loader::BindTarget> target;
                        target = Loader::BindTarget({ resolvedTarget.targetLoader, runtimeOffset });
                        return target;
                    }
                    case Loader::ResolvedSymbol::Kind::bindAbsolute:
                        // We don't handle absolute values
                        return std::optional<Loader::BindTarget>();
                }
            } else {
                // FIXME: What should we do here?  The map should have all binds and rebases.
                // For now I guess we give up
                return std::optional<Loader::BindTarget>();
            }
#else
            uint64_t runtimeOffset = ptr.targetValue.vmAddress().rawValue() - loadAddress;
            uint64_t targetValue = 0;
            if ( ptrSize == 8 ) {
                targetValue = *(uint64_t*)((uint8_t*)ma + runtimeOffset);
            } else {
                targetValue = *(uint32_t*)((uint8_t*)ma + runtimeOffset);
            }
            // This might be a pointer to a missing weak import.  If that is the case, just skip it
            if ( targetValue == 0 )
                return std::optional<Loader::BindTarget>();

#if __has_feature(ptrauth_calls)
            targetValue = (uint64_t)__builtin_ptrauth_strip((void*)targetValue, ptrauth_key_asia);
#endif

            uint8_t* runtimeAddress = (uint8_t*)targetValue;
            PrebuiltLoader::BindTarget typeBindTarget;
            if ( getBindTarget(state, runtimeAddress, typeBindTarget) ) {
                std::optional<Loader::BindTarget> target;
                target = typeBindTarget;
                return target;
            }
#endif
            return std::optional<Loader::BindTarget>();
        };

        __block SwiftVisitor swiftVisitor = makeSwiftVisitor(diag, state, ldr);
        swiftVisitor.forEachProtocolConformance(^(const SwiftConformance &swiftConformance, bool &stopConformance) {
            typedef SwiftConformance::SwiftProtocolConformanceFlags SwiftProtocolConformanceFlags;
            typedef SwiftConformance::SwiftTypeRefPointer SwiftTypeRefPointer;
            typedef SwiftConformance::TypeContextDescriptor TypeContextDescriptor;

            if ( swiftConformance.isNull() ) {
                if ( !mf->enforceFormat(MachOFile::Malformed::zerofillSwiftMetadata) ) {
                    diag.error("Skipped optimizing Swift protocols due to null conformance at 0x%llx",
                               (uint64_t)swiftConformance.getLocation());
                    stopConformance = true;
                    return;
                }
            }

            SwiftPointer protocol = swiftConformance.getProtocolPointer(swiftVisitor);

            // The protocol might be an indirect pointer to null. If so, skip this as its a missing weak import
            if ( isNull(protocol) )
                return;

            SwiftTypeRefPointer typeRef = swiftConformance.getTypeRef(swiftVisitor);
            SwiftProtocolConformanceFlags flags = swiftConformance.getProtocolConformanceFlags(swiftVisitor);

            // The type descriptor might also be null.  If so skip it
            SwiftPointer typePointer = typeRef.getTargetPointer(swiftVisitor);
            if ( isNull(typePointer) )
                return;

            std::optional<PrebuiltLoader::BindTarget> typeBindTarget;
            typeBindTarget = getTarget(typePointer);
            if ( !typeBindTarget.has_value() ) {
                diag.error("Skipped optimizing Swift protocols, could not find image for type conformance pointer");
                stopConformance = true;
                return;
            }

            std::optional<PrebuiltLoader::BindTarget> protocolBindTarget;
            protocolBindTarget = getTarget(protocol);
            if ( !protocolBindTarget.has_value() ) {
                diag.error("Skipped optimizing Swift protocols, could not find image for type protocol pointer");
                stopConformance = true;
                return;
            }

            VMAddress conformanceVMAddr = swiftConformance.getVMAddress();
            VMOffset conformanceVMOffset = conformanceVMAddr - VMAddress(loadAddress);

            switch ( flags.typeReferenceKind() ) {
                case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor:
                case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor: {
                    // The type descriptor might point to a foreign name
                    bool                            foreignMetadataNameHasImportInfo = false;
                    std::optional<ResolvedValue>    nameValue;
                    if ( typeBindTarget->loader == ldr ) {
                        // Our loader, so we can use our SwiftVisitor to find the type desc
                        VMAddress typeDescVMAddr = VMAddress(loadAddress) + VMOffset(typeBindTarget->runtimeOffset);
                        ResolvedValue typeDescValue = swiftVisitor.getValueFor(typeDescVMAddr);
                        TypeContextDescriptor typeDesc(typeDescValue);
                        if ( typeDesc.isForeignMetadata() ) {
                            foreignMetadataNameHasImportInfo = typeDesc.hasImportInfo();
                            nameValue.emplace(typeDesc.getName(swiftVisitor));
                        }
                    } else {
                        // A different loader, so make a visitor for it.
                        SwiftVisitor otherVisitor = makeSwiftVisitor(diag, state, typeBindTarget->loader);
                        VMAddress otherLoadAddress(typeBindTarget->loader->mf(state)->preferredLoadAddress());
                        VMAddress typeDescVMAddr = otherLoadAddress + VMOffset(typeBindTarget->runtimeOffset);
                        ResolvedValue typeDescValue = otherVisitor.getValueFor(typeDescVMAddr);
                        TypeContextDescriptor typeDesc(typeDescValue);
                        if ( typeDesc.isForeignMetadata() ) {
                            foreignMetadataNameHasImportInfo = typeDesc.hasImportInfo();
                            nameValue.emplace(typeDesc.getName(swiftVisitor));
                        }
                    }

                    if ( nameValue.has_value() ) {
                        const char* name = (const char*)nameValue->value();
                        VMAddress nameVMAddr = nameValue->vmAddress();
                        std::string_view fullName(name);
                        if ( foreignMetadataNameHasImportInfo ) {
                            fullName = getForeignFullIdentity(name);
                            nameVMAddr += VMOffset((uint64_t)fullName.data() - (uint64_t)name);
                        }

                        // We only have 16-bits for the length
                        if ( fullName.size() >= (1 << 16) ) {
                            diag.error("Protocol conformance exceeded name length of 16-bits");
                            stopConformance = true;
                            return;
                        }

                        PrebuiltLoader::BindTarget foreignBindTarget;
                        foreignBindTarget.loader = ldr;
                        foreignBindTarget.runtimeOffset = nameVMAddr.rawValue() - ldr->mf(state)->preferredLoadAddress();

                        SwiftForeignTypeProtocolConformanceDiskLocationKey protoLocKey = {
                            (uint64_t)fullName.data(),
                            PrebuiltLoader::BindTargetRef(foreignBindTarget),
                            fullName.size(),
                            PrebuiltLoader::BindTargetRef(protocolBindTarget.value())
                        };

                        PrebuiltLoader::BindTarget conformanceBindTarget = { ldr, conformanceVMOffset.rawValue() };
                        SwiftForeignTypeProtocolConformanceDiskLocation protoLoc = {
                            PrebuiltLoader::BindTargetRef(conformanceBindTarget)
                        };

                        foreignProtocolConformances.insert({ protoLocKey, protoLoc });
                    }

                    SwiftTypeProtocolConformanceDiskLocationKey protoLocKey = {
                        PrebuiltLoader::BindTargetRef(typeBindTarget.value()),
                        PrebuiltLoader::BindTargetRef(protocolBindTarget.value())
                    };

                    PrebuiltLoader::BindTarget conformanceBindTarget = { ldr, conformanceVMOffset.rawValue() };
                    SwiftTypeProtocolConformanceDiskLocation protoLoc = {
                        PrebuiltLoader::BindTargetRef(conformanceBindTarget)
                    };

                    typeProtocolConformances.insert({ protoLocKey, protoLoc });
                    break;
                }
                case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::directObjCClassName: {
                    const char* className = typeRef.getClassName(swiftVisitor);

                    PrebuiltLoader::BindTarget conformanceBindTarget = { ldr, conformanceVMOffset.rawValue() };
                    SwiftMetadataProtocolConformanceDiskLocation protoLoc = {
                        PrebuiltLoader::BindTargetRef(conformanceBindTarget)
                    };

                    __block bool foundClass = false;
                    prebuiltObjC.classMap.forEachEntry(className, ^(const Loader::BindTarget* values[], uint32_t valuesCount) {
                        for (uint32_t i = 0; i < valuesCount; i++) {
                            foundClass = true;
                            const Loader::BindTarget& metadataBindTarget = *values[i];
                            SwiftMetadataProtocolConformanceDiskLocationKey protoLocKey = {
                                PrebuiltLoader::BindTargetRef(metadataBindTarget),
                                PrebuiltLoader::BindTargetRef(protocolBindTarget.value())
                            };
                            metadataProtocolConformances.insert({ protoLocKey, protoLoc });
                        }
                    });

                    classHashTable->forEachClass(className, ^(uint64_t objectCacheOffset, uint16_t dylibObjCIndex, bool &stopObjects) {
                        VMAddress objectVMAddr = sharedCacheBaseAddress + VMOffset(objectCacheOffset);
                        PrebuiltLoader::BindTarget metadataBindTarget;
                        if ( !getBindTarget(state, objectVMAddr.rawValue(), metadataBindTarget) )
                            return;

                        foundClass = true;
                        SwiftMetadataProtocolConformanceDiskLocationKey protoLocKey = {
                            PrebuiltLoader::BindTargetRef(metadataBindTarget),
                            PrebuiltLoader::BindTargetRef(protocolBindTarget.value())
                        };

                        metadataProtocolConformances.insert({ protoLocKey, protoLoc });
                    });

                    if ( !foundClass ) {
                        diag.error("Skipped optimizing Swift protocols, could not find image for ObjCClassName pointer at all");
                        stopConformance = true;
                        return;
                    }
                    break;
                }
                case SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind::indirectObjCClass: {
                    SwiftMetadataProtocolConformanceDiskLocationKey protoLocKey = {
                        PrebuiltLoader::BindTargetRef(typeBindTarget.value()),
                        PrebuiltLoader::BindTargetRef(protocolBindTarget.value())
                    };

                    PrebuiltLoader::BindTarget conformanceBindTarget = { ldr, conformanceVMOffset.rawValue() };
                    SwiftMetadataProtocolConformanceDiskLocation protoLoc = {
                        PrebuiltLoader::BindTargetRef(conformanceBindTarget)
                    };

                    metadataProtocolConformances.insert({ protoLocKey, protoLoc });
                    break;
                }
            }
        });
    }
    return !diag.hasError();
}


void PrebuiltSwift::make(Diagnostics& diag, PrebuiltObjC& prebuiltObjC, RuntimeState& state)
{
    if ( !findProtocolConformances(diag, prebuiltObjC, state) )
        return;

    this->builtSwift = true;
}


} // namespace dyld4
