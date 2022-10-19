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

#ifndef SwiftVisitor_hpp
#define SwiftVisitor_hpp

#include "MetadataVisitor.h"

namespace metadata_visitor
{

struct SwiftConformance;
struct SwiftConformanceList;
struct SwiftVisitor;

// Most Swift pointers are offset based.  Either an offset directly to a value,
// or an offset to an indirect pointer, which points to the value.  This abstracts
// those cases
struct SwiftPointer
{
    // True -> runtimeOffset points to the value
    // False -> runtimeOffset points to a pointer, which points to the value
    bool                            isDirect        = false;
    metadata_visitor::ResolvedValue targetValue;
};

// A wrapper around a list of swift conformances.  Points to the conformance list in the mach-o
// buffer, and can be used to find the conformances.
struct SwiftConformanceList
{
    SwiftConformanceList(std::optional<metadata_visitor::ResolvedValue> listPos,
                         uint32_t numElements)
        : conformanceListPos(listPos), numElements(numElements) { }

    uint32_t numConformances() const;

    SwiftConformance getConformance(const Visitor& swiftVisitor, uint32_t i) const;

private:
    const std::optional<metadata_visitor::ResolvedValue> conformanceListPos;
    uint32_t numElements = 0;
};

// A wrapper around a swift conformance.  Points to the conformance in the mach-o
// buffer, and can be used to find the fields of the conformance.
struct SwiftConformance
{
private:

    // These types are in the layout defined by the Swift ABI.
    // Their names probably don't match the Swift runtime, but are hopefully easy to understand

    // A 32-bit relative pointer to a value.  The offset value is either:
    // - a direct 32-bit offset to the value, if the low bit is 0, or
    // - an offset to a pointer sized slot, if the low bit is 1
    struct relative_pointer_t
    {
        int32_t relativeOffset;
    };

    // A 32-bit relative pointer to a value
    // The type the pointer depends on the SwiftProtocolConformanceFlags::TypeReferenceKind
    struct typeref_pointer_t
    {
        int32_t relativeOffset;
    };

    struct protocol_conformance_flags_t
    {
        uint32_t flags;
    };

    struct conformance_t
    {
        const relative_pointer_t            protocolRelativePointer;
        const typeref_pointer_t             typeRef;
        int32_t                             witnessTable;
        const protocol_conformance_flags_t  flags;
    };

    struct type_context_descriptor_t
    {
        uint32_t            flags;
        int32_t             parent;
        relative_pointer_t  name;
        int32_t             accessFunction;
        int32_t             fields;
    };

public:

    // Wraps a relative_pointer_t, which is a relative offset to a pointer
    struct SwiftRelativePointer
    {
        SwiftRelativePointer(metadata_visitor::ResolvedValue pos) : pos(pos) { };

        // Note, maye return { } if the relative pointer points to null
        std::optional<VMAddress> getTargetVMAddr(const SwiftVisitor& swiftVisitor) const;

        SwiftPointer getTargetPointer(const SwiftVisitor &swiftVisitor) const;

    private:
        const metadata_visitor::ResolvedValue pos;
    };

    // Wraps a protocol_conformance_flags_t
    struct SwiftProtocolConformanceFlags
    {
        SwiftProtocolConformanceFlags(metadata_visitor::ResolvedValue pos) : pos(pos) { };

        // Taken from MetadataValues.h
        enum class TypeMetadataKind : uint32_t {
            mask    = 0x7 << 3, // 8 type reference kinds
            shift   = 3,
        };

        // Taken from MetadataValues.h
        enum class TypeReferenceKind : uint32_t {
            // The conformance is for a nominal type referenced directly;
            // getTypeDescriptor() points to the type context descriptor.
            directTypeDescriptor = 0x00,

            // The conformance is for a nominal type referenced indirectly;
            // getTypeDescriptor() points to the type context descriptor.
            indirectTypeDescriptor = 0x01,

            // The conformance is for an Objective-C class that should be looked up
            // by class name.
            directObjCClassName = 0x02,

            // The conformance is for an Objective-C class that has no nominal type
            // descriptor.
            // getIndirectObjCClass() points to a variable that contains the pointer to
            // the class object, which then requires a runtime call to get metadata.
            //
            // On platforms without Objective-C interoperability, this case is
            // unused.
            indirectObjCClass = 0x03,
        };

        TypeReferenceKind typeReferenceKind() const;

    private:
        const metadata_visitor::ResolvedValue pos;
    };

    // This represents the type descriptors pointed to by protocol conformances
    // It wraps a type_context_descriptor_t
    struct TypeContextDescriptor
    {
        TypeContextDescriptor(ResolvedValue& pos) : pos(pos) { }

        bool            isForeignMetadata() const;
        bool            hasImportInfo() const;
        ResolvedValue   getName(const SwiftVisitor& swiftVisitor) const;

    private:

        enum TypeContextDescriptorFlags : uint16_t {
            ForeignMetadataInitialization = 0x2
        };

        // The most significant two bytes of the flags word, which can have
        // kind-specific meaning.
        uint16_t getKindSpecificFlags() const;

        ResolvedValue pos;
    };

    // Wraps a typeref_pointer_t.  What it points to is determined by
    // SwiftProtocolConformanceFlags::TypeReferenceKind
    struct SwiftTypeRefPointer
    {
        SwiftTypeRefPointer(metadata_visitor::ResolvedValue pos,
                     SwiftProtocolConformanceFlags::TypeReferenceKind kind)
            : pos(pos), kind(kind) { };

        // If the kind is directTypeDescriptor/indirectTypeDescriptor, then this returns the
        // value pointed to.  Otherwise returns { }.
        // Note it may also return { } if the kind is indirectTypeDescriptor but there is a missing
        // weak import, resulting in a null pointer to a Type.
        std::optional<ResolvedValue> getTypeDescriptor(const SwiftVisitor& swiftVisitor) const;

        // If the kind is directObjCClassName, returns the target class name
        const char* getClassName(const SwiftVisitor& swiftVisitor) const;

        // If the kind is indirectObjCClass, returns a pointer to the class.
        // Note, may return { } if this is a missing weak import, resulting in a null pointer to a Class.
        std::optional<ResolvedValue> getClass(const SwiftVisitor& swiftVisitor) const;

        SwiftPointer getTargetPointer(const SwiftVisitor &swiftVisitor) const;

    private:
        const metadata_visitor::ResolvedValue                   pos;
        const SwiftProtocolConformanceFlags::TypeReferenceKind  kind;
    };

    SwiftConformance(metadata_visitor::ResolvedValue pos)
        : conformancePos(pos) { }

    // Get the Swift protocol this conformance points to
    // As we don't have any useful methods on Protocol right now, we can just return the location
    // Note, protocol references can be waek-import, so may return { } in that case.
    std::optional<VMAddress>        getProtocolVMAddr(const SwiftVisitor& swiftVisitor) const;

    // This returns a SwiftPointer which may still point to an indirect value, and could be null
    SwiftPointer                    getProtocolPointer(const SwiftVisitor& swiftVisitor) const;

    SwiftProtocolConformanceFlags   getProtocolConformanceFlags(const SwiftVisitor& swiftVisitor) const;
    SwiftTypeRefPointer             getTypeRef(const SwiftVisitor& swiftVisitor) const;

    // Bad apps have a null conformance.  This returns true if all fields of the conformance are null
    bool        isNull() const;

    const void* getLocation() const;
    VMAddress   getVMAddress() const;

    const metadata_visitor::ResolvedValue conformancePos;
};

struct SwiftVisitor : metadata_visitor::Visitor
{

    using metadata_visitor::Visitor::Visitor;

    void forEachProtocolConformance(void (^callback)(const SwiftConformance& swiftConformance,
                                                     bool& stopConformance)) const;

private:

    struct SectionContent
    {
        SectionContent(metadata_visitor::ResolvedValue baseAddress)
            : sectionBase(baseAddress) { }
        metadata_visitor::ResolvedValue sectionBase;
        uint64_t                        sectSize       = 0;
    };

    std::optional<SectionContent> findTextSection(const char *sectionName) const;

    SwiftConformanceList getSwiftConformances() const;
};

} // namespace metadata_visitor

#endif /* SwiftVisitor_hpp */
