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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

#include "SwiftVisitor.h"

#if SUPPORT_VM_LAYOUT
#include "MachOAnalyzer.h"
#endif

#include <assert.h>

using namespace metadata_visitor;
using mach_o::Header;

//
// MARK: --- SwiftVisitor methods ---
//

void SwiftVisitor::forEachProtocolConformance(void (^callback)(const SwiftConformance& swiftConformance,
                                                               bool& stopConformance)) const
{
    SwiftConformanceList swiftConformanceList = this->getSwiftConformances();

    for ( uint32_t i = 0, e = swiftConformanceList.numConformances(); i != e; ++i ) {
        SwiftConformance swiftConformance = swiftConformanceList.getConformance(*this, i);

        bool stop = false;
        callback(swiftConformance, stop);
        if ( stop )
            break;
    }
}

SwiftConformanceList SwiftVisitor::getSwiftConformances() const
{
    std::optional<SectionContent> protoListSection = findTextSection("__swift5_proto");
    if ( !protoListSection.has_value() )
        return SwiftConformanceList(std::nullopt, 0);

    assert((protoListSection->sectSize % 4) == 0);
    uint64_t numElements = protoListSection->sectSize / 4;
    const ResolvedValue& sectionValue = protoListSection->sectionBase;
    return SwiftConformanceList(sectionValue, (uint32_t)numElements);
}

std::optional<SwiftVisitor::SectionContent> SwiftVisitor::findSection(const char* segmentName, const char *sectionName) const
{
#if SUPPORT_VM_LAYOUT
    const dyld3::MachOFile* mf = this->dylibMA;
#else
    const dyld3::MachOFile* mf = this->dylibMF;
#endif

    __block std::optional<SwiftVisitor::SectionContent> sectionContent;
    ((const Header*)mf)->forEachSection(^(const Header::SegmentInfo& segInfo, const Header::SectionInfo& sectInfo, bool& stop) {
        if ( sectInfo.segmentName != segmentName )
            return;
        if ( sectInfo.sectionName != sectionName )
            return;

#if SUPPORT_VM_LAYOUT
        const void* targetValue = (const void*)(sectInfo.address + this->dylibMA->getSlide());
        ResolvedValue target(targetValue, VMAddress(sectInfo.address));
#else
        VMOffset offsetInSegment(sectInfo.address - segInfo.vmaddr);
        ResolvedValue target(this->segments[sectInfo.segIndex], offsetInSegment);
#endif
        sectionContent.emplace(std::move(target));
        sectionContent->sectSize       = sectInfo.size;

        stop = true;
    });
    return sectionContent;
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void SwiftVisitor::forEachPointerHashTable(Diagnostics &diag, void(^callback)(ResolvedValue sectionBase, size_t tableIndex, uint8_t* tableStart, size_t numEntries)) const
{
    std::optional<SectionContent> sect = findPointerHashTableSection();
    if ( !sect )
        return;

    if ( sect->sectSize < sizeof(uint64_t) )
        return;

    // Pointer hash table section consists of:
    // struct PointerHashTable {
    //   size_t count;
    //   struct PointerHashTableEntry entries[];
    // };
    // struct PointerHashTableEntry {
    //   struct PointerHashTableKey *key;
    //   void *value;
    // };

    const size_t sizeTySize = this->pointerSize;
    uint8_t* const sectBase = (uint8_t*)sect->sectionBase.value();
    uint8_t* const sectEnd = (uint8_t*)sectBase + sect->sectSize;
    uint8_t* pos = sectBase;
    size_t   tableIndex = 0;
    while ( pos < sectEnd ) {
        if ( (sectEnd-pos) < sizeof(sizeTySize) ) {
            diag.error("Bad hash table section, missing count");
            return;
        }

        const uint64_t count = sizeTySize == 4 ? *(uint32_t*)pos : *(uint64_t*)pos;
        const size_t headerSize = sizeTySize;
        const size_t entrySize = this->pointerSize * 2;
        const size_t minSize = (entrySize * count) + headerSize;
        if ( (sectEnd-pos) < minSize ) {
            diag.error("Bad hash table section, found count: %llu, expected section size: %lu, actual size: %llu", count, minSize, sect->sectSize);
            return;
        }

        callback(sect->sectionBase, tableIndex, pos, count);
        pos = pos + minSize;
        ++tableIndex;
    }
}

size_t SwiftVisitor::numPointerHashTables(Diagnostics& diag) const
{
    __block size_t res = 0;
    forEachPointerHashTable(diag, ^(ResolvedValue sectionBase, size_t tableIndex, uint8_t *tableStart, size_t numEntries) {
        ++res;
    });
    return res;
}

std::optional<ResolvedValue> SwiftVisitor::forEachPointerHashTableRelativeEntry(Diagnostics& diag, uint8_t* tableStart,
                                                                                VMAddress sharedCacheBaseAddr,
                                                                                void(^callback)(size_t index, std::span<uint64_t> cacheOffsetKeys, uint64_t cacheOffsetValue)) const
{
    std::optional<SectionContent> sect = findPointerHashTableSection();
    if ( !sect )
        return {};

    if ( sect->sectSize < sizeof(uint64_t) )
        return {};

    std::vector<ResolvedValue> res;
    uint8_t* const sectBase = (uint8_t*)sect->sectionBase.value();
    uint8_t* const sectEnd = (uint8_t*)sectBase + sect->sectSize;
    if ( tableStart < sectBase || tableStart >= sectEnd ) {
        assert(false && "invalid pointer table addr");
        return std::nullopt;
    }

    const size_t sizeTySize = pointerSize;
    if ( (sectEnd-tableStart) < sizeTySize ) {
        diag.error("Bad hash table section, missing count");
        return {};
    }

    const uint64_t count = sizeTySize == 4 ? *(uint32_t*)tableStart : *(uint64_t*)tableStart;
    const size_t headerSize = sizeTySize;
    const size_t ptrSize = this->pointerSize;
    const size_t entrySize = ptrSize * 2;
    const size_t minSize = (entrySize * count) + headerSize;
    if ( (sectEnd-tableStart) < minSize ) {
        diag.error("Bad hash table section, found count: %llu, expected section size: %lu, actual size: %llu", count, minSize, sect->sectSize);
        return {};
    }

    // Entry and key layout are:
    //
    // struct PointerHashTableEntry {
    //     struct PointerHashTableKey *key;
    //     void *value;
    // };
    // struct PointerHashTableKey {
    //     size_t  count;
    //     void *pointers[];
    // };
    uint8_t* entry = (uint8_t*)(tableStart + headerSize);
    std::vector<uint64_t> keysBuffer;
    for ( size_t i = 0; i < count; ++i ) {
        ResolvedValue keyPtrResolved = resolveRebase(getField(sect->sectionBase, entry));
        entry += ptrSize;

        bool wasBind;
        ResolvedValue valueResolved = resolveBindOrRebase(getField(sect->sectionBase, entry), wasBind);
        entry += ptrSize;

        uint8_t* keyPtr = (uint8_t*)keyPtrResolved.value();
        const uint64_t numKeys = sizeTySize == 4 ? *(uint32_t*)keyPtr : *(uint64_t*)keyPtr;
        keyPtr += sizeTySize;

        keysBuffer.clear();
        for ( uint64_t k = 0; k < numKeys; ++k ) {
            ResolvedValue key = resolveBindOrRebase(getField(keyPtrResolved, keyPtr), wasBind);
            keyPtr += ptrSize;

            keysBuffer.push_back((key.vmAddress()-sharedCacheBaseAddr).rawValue());
        }

        callback(i, keysBuffer, (valueResolved.vmAddress()-sharedCacheBaseAddr).rawValue());
    }
    assert((uint8_t*)entry == (tableStart+minSize));
    return getField(sect->sectionBase, tableStart);
}
#endif

//
// MARK: --- SwiftConformanceList methods ---
//

SwiftConformance SwiftConformanceList::getConformance(const Visitor &swiftVisitor, uint32_t i) const
{
    // Protocol conformances are a 32-bit offset from the list entry
    const uint8_t* fieldPos = (const uint8_t*)this->conformanceListPos->value() + (i * sizeof(int32_t));

    int64_t relativeOffsetFromField = (int64_t)*(int32_t*)fieldPos;
    VMOffset relativeOffsetFromList((uint64_t)relativeOffsetFromField + (i * 4));

    VMAddress listVMAddr = this->conformanceListPos->vmAddress();
    VMAddress conformanceVMAddr = listVMAddr + relativeOffsetFromList;

    ResolvedValue conformanceValue = swiftVisitor.getValueFor(conformanceVMAddr);
    return SwiftConformance(conformanceValue);
}

uint32_t SwiftConformanceList::numConformances() const
{
    return this->numElements;
}

//
// MARK: --- SwiftConformance methods ---
//

std::optional<VMAddress> SwiftConformance::getProtocolVMAddr(const SwiftVisitor& swiftVisitor) const
{
    const conformance_t* rawConformance = (const conformance_t*)this->getLocation();

    // The protocol is found via the relative pointer
    const relative_pointer_t* relativePtr = &rawConformance->protocolRelativePointer;
    ResolvedValue relativePtrField = swiftVisitor.getField(this->conformancePos, relativePtr);
    return SwiftRelativePointer(relativePtrField).getTargetVMAddr(swiftVisitor);
}

SwiftPointer SwiftConformance::getProtocolPointer(const SwiftVisitor& swiftVisitor) const
{
    const conformance_t* rawConformance = (const conformance_t*)this->getLocation();

    // The protocol is found via the relative pointer
    const relative_pointer_t* relativePtr = &rawConformance->protocolRelativePointer;
    ResolvedValue relativePtrField = swiftVisitor.getField(this->conformancePos, relativePtr);
    return SwiftRelativePointer(relativePtrField).getTargetPointer(swiftVisitor);
}

SwiftConformance::SwiftProtocolConformanceFlags
SwiftConformance::getProtocolConformanceFlags(const SwiftVisitor& swiftVisitor) const
{
    const conformance_t* rawConformance = (const conformance_t*)this->getLocation();

    const protocol_conformance_flags_t* flagsPtr = &rawConformance->flags;
    ResolvedValue flagsField = swiftVisitor.getField(this->conformancePos, flagsPtr);
    return SwiftProtocolConformanceFlags(flagsField);
}

SwiftConformance::SwiftTypeRefPointer
SwiftConformance::getTypeRef(const SwiftVisitor& swiftVisitor) const
{
    const conformance_t* rawConformance = (const conformance_t*)this->getLocation();

    // The typeref is found via the relative pointer
    const typeref_pointer_t* relativePtr = &rawConformance->typeRef;
    ResolvedValue relativePtrField = swiftVisitor.getField(this->conformancePos, relativePtr);

    auto kind = getProtocolConformanceFlags(swiftVisitor).typeReferenceKind();
    return SwiftTypeRefPointer(relativePtrField, kind);
}

bool SwiftConformance::isNull() const
{
    const conformance_t* rawConformance = (const conformance_t*)this->getLocation();
    uint8_t emptyConformance[sizeof(conformance_t)] = { 0 };
    return memcmp(rawConformance, emptyConformance, sizeof(conformance_t)) == 0;
}

VMAddress SwiftConformance::getVMAddress() const
{
    return this->conformancePos.vmAddress();
}

const void* SwiftConformance::getLocation() const
{
    return this->conformancePos.value();
}

//
// MARK: --- SwiftConformance::SwiftRelativePointer methods ---
//

std::optional<VMAddress> SwiftConformance::SwiftRelativePointer::getTargetVMAddr(const SwiftVisitor &swiftVisitor) const
{
    const relative_pointer_t* relativePtr = (const relative_pointer_t*)this->pos.value();
    int32_t relativeOffset = relativePtr->relativeOffset;
    if ( (relativeOffset & 0x1) == 0 ) {
        // Relative offset directly to the target value
        int64_t offset64 = (int64_t)relativeOffset;
        VMAddress targetVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
        return targetVMAddr;
    } else {
        // Relative offset to a pointer.  The pointer contains the target value
        int32_t offset = relativeOffset & ~0x1ULL;
        int64_t offset64 = (int64_t)offset;
        VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
        ResolvedValue pointerValue = swiftVisitor.getValueFor(pointerVMAddr);

        // Dereference the pointer to get the final target
        return swiftVisitor.resolveOptionalRebaseToVMAddress(pointerValue);
    }
}

SwiftPointer SwiftConformance::SwiftRelativePointer::getTargetPointer(const SwiftVisitor &swiftVisitor) const
{
    const relative_pointer_t* relativePtr = (const relative_pointer_t*)this->pos.value();
    int32_t relativeOffset = relativePtr->relativeOffset;
    if ( (relativeOffset & 0x1) == 0 ) {
        // Relative offset directly to the target value
        int64_t offset64 = (int64_t)relativeOffset;
        VMAddress targetVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
        return { true, swiftVisitor.getValueFor(targetVMAddr) };
    } else {
        // Relative offset to a pointer.  The pointer contains the target value
        int32_t offset = relativeOffset & ~0x1ULL;
        int64_t offset64 = (int64_t)offset;
        VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
        return { false, swiftVisitor.getValueFor(pointerVMAddr) };
    }
}

//
// MARK: --- SwiftConformance::SwiftProtocolConformanceFlags methods ---
//

SwiftConformance::SwiftProtocolConformanceFlags::TypeReferenceKind
SwiftConformance::SwiftProtocolConformanceFlags::typeReferenceKind() const
{
    const protocol_conformance_flags_t* flagsPtr = (const protocol_conformance_flags_t*)this->pos.value();
    uint32_t flags = flagsPtr->flags;
    return (TypeReferenceKind)((flags & (uint32_t)TypeMetadataKind::mask) >> (uint32_t)TypeMetadataKind::shift);
}

//
// MARK: --- SwiftConformance::SwiftProtocolConformanceFlags methods ---
//

std::optional<ResolvedValue>
SwiftConformance::SwiftTypeRefPointer::getTypeDescriptor(const SwiftVisitor &swiftVisitor) const
{
    assert((this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor)
           || (this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor));

    if ( this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor ) {
        // Relative offset directly to the target value
        int32_t relativeOffset = *(int32_t*)this->pos.value();
        int64_t offset64 = (int64_t)relativeOffset;
        VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);

        return swiftVisitor.getValueFor(pointerVMAddr);
    }

    if ( this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor ) {
        // Relative offset to a pointer.  The pointer contains the target value
        int32_t relativeOffset = *(int32_t*)this->pos.value();
        int32_t offset = relativeOffset & ~0x1ULL;
        int64_t offset64 = (int64_t)offset;
        VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
        ResolvedValue pointerValue = swiftVisitor.getValueFor(pointerVMAddr);

        // Dereference the pointer to get the final target
        return swiftVisitor.resolveOptionalRebase(pointerValue);
    }
    return { };
}

const char*
SwiftConformance::SwiftTypeRefPointer::getClassName(const SwiftVisitor &swiftVisitor) const
{
    assert(this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::directObjCClassName);

    // Relative offset directly to the class name string
    int32_t relativeOffset = *(int32_t*)this->pos.value();
    int64_t offset64 = (int64_t)relativeOffset;
    VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);

    ResolvedValue pointerValue = swiftVisitor.getValueFor(pointerVMAddr);
    return (const char*)pointerValue.value();
}

std::optional<ResolvedValue>
SwiftConformance::SwiftTypeRefPointer::getClass(const SwiftVisitor &swiftVisitor) const
{
    assert(this->kind == SwiftProtocolConformanceFlags::TypeReferenceKind::indirectObjCClass);

    // Relative offset to a pointer.  The pointer contains the target value
    int32_t relativeOffset = *(int32_t*)this->pos.value();
    int32_t offset = relativeOffset & ~0x1ULL;
    int64_t offset64 = (int64_t)offset;
    VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
    ResolvedValue pointerValue = swiftVisitor.getValueFor(pointerVMAddr);

    // Dereference the pointer to get the final target
    return swiftVisitor.resolveOptionalRebase(pointerValue);
}

SwiftPointer SwiftConformance::SwiftTypeRefPointer::getTargetPointer(const SwiftVisitor &swiftVisitor) const
{
    switch ( this->kind ) {
        case SwiftProtocolConformanceFlags::TypeReferenceKind::directTypeDescriptor: {
            // Relative offset directly to the target value
            int32_t relativeOffset = *(int32_t*)this->pos.value();
            int64_t offset64 = (int64_t)relativeOffset;
            VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
            return { true, swiftVisitor.getValueFor(pointerVMAddr) };
        }
        case SwiftProtocolConformanceFlags::TypeReferenceKind::indirectTypeDescriptor: {
            // Relative offset to a pointer.  The pointer contains the target value
            int32_t relativeOffset = *(int32_t*)this->pos.value();
            int32_t offset = relativeOffset & ~0x1ULL;
            int64_t offset64 = (int64_t)offset;
            VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
            return { false, swiftVisitor.getValueFor(pointerVMAddr) };
        }
        case SwiftProtocolConformanceFlags::TypeReferenceKind::directObjCClassName: {
            // Relative offset directly to the class name string
            int32_t relativeOffset = *(int32_t*)this->pos.value();
            int64_t offset64 = (int64_t)relativeOffset;
            VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
            return { true, swiftVisitor.getValueFor(pointerVMAddr) };
        }
        case SwiftProtocolConformanceFlags::TypeReferenceKind::indirectObjCClass: {
            // Relative offset to a pointer.  The pointer contains the target value
            int32_t relativeOffset = *(int32_t*)this->pos.value();
            int32_t offset = relativeOffset & ~0x1ULL;
            int64_t offset64 = (int64_t)offset;
            VMAddress pointerVMAddr = this->pos.vmAddress() + VMOffset((uint64_t)offset64);
            return { false, swiftVisitor.getValueFor(pointerVMAddr) };
        }
    }
}

//
// MARK: --- SwiftConformance::TypeContextDescriptor methods ---
//

// The most significant two bytes of the flags word, which can have
// kind-specific meaning.
uint16_t SwiftConformance::TypeContextDescriptor::getKindSpecificFlags() const {
    const type_context_descriptor_t* context = (type_context_descriptor_t*)this->pos.value();
    return (context->flags >> 16u) & 0xFFFFu;
}

bool SwiftConformance::TypeContextDescriptor::isForeignMetadata() const
{
    // The botton 2 bits have the flags
    return (this->getKindSpecificFlags() & 0x3) == ForeignMetadataInitialization;
}

bool SwiftConformance::TypeContextDescriptor::hasImportInfo() const
{
    // Bit 2 tells us if we have import info, ie, a name containing NULLs
    return (this->getKindSpecificFlags() & (1 << 2)) != 0;
}

ResolvedValue SwiftConformance::TypeContextDescriptor::getName(const SwiftVisitor& swiftVisitor) const
{
    // The name is found via a relative offset
    const type_context_descriptor_t* context = (type_context_descriptor_t*)this->pos.value();
    ResolvedValue nameField = swiftVisitor.getField(this->pos, &context->name.relativeOffset);

    // Add the offset to the field to get the target value
    int32_t nameOffset = *(int32_t*)nameField.value();
    int64_t nameOffset64 = (int64_t)nameOffset;
    VMOffset offsetFromNameField((uint64_t)nameOffset64);
    VMAddress targetVMAddr = nameField.vmAddress() + offsetFromNameField;
    return swiftVisitor.getValueFor(targetVMAddr);
}

#endif // !TARGET_OS_EXCLAVEKIT
