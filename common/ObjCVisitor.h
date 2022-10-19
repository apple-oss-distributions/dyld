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

#ifndef ObjCVisitor_h
#define ObjCVisitor_h

#include "MetadataVisitor.h"
#include "MachOFile.h"
#include "Types.h"

#include <optional>

#if !BUILDING_DYLD
#include <vector>
#endif

namespace dyld3
{
#if BUILDING_DYLD
struct MachOAnalyzer;
#endif

struct MachOFile;
}

//
// MARK: --- ObjCVisitor ---
//

namespace objc_visitor
{

struct Category;
struct Class;
struct IVar;
struct IVarList;
struct Method;
struct MethodList;
struct Protocol;
struct ProtocolList;
struct Visitor;

struct Visitor : metadata_visitor::Visitor
{
    using metadata_visitor::Visitor::Visitor;

    void forEachClass(bool visitMetaClasses, void (^callback)(Class& objcClass, bool isMetaClass, bool& stopClass));
    void forEachClass(void (^callback)(const Class& objcClass, bool& stopClass));
    void forEachClass(void (^callback)(Class& objcClass, bool& stopClass));
    void forEachClassAndMetaClass(void (^callback)(const Class& objcClass, bool& stopClass));
    void forEachClassAndMetaClass(void (^callback)(Class& objcClass, bool& stopClass));
    void forEachCategory(void (^callback)(const Category& objcCategory, bool& stopCategory));
    void forEachProtocol(void (^callback)(const Protocol& objcProtocol, bool& stopProtocol));
    void forEachProtocolReference(void (^callback)(metadata_visitor::ResolvedValue& value));
    void forEachSelectorReference(void (^callback)(metadata_visitor::ResolvedValue& value)) const;
    void forEachSelectorReference(void (^callback)(VMAddress selRefVMAddr, VMAddress selRefTargetVMAddr,
                                                   const char* selectorString)) const;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    // We need everything public to write tests
public:
#else
private:
#endif

    struct DataSection
    {
        DataSection(metadata_visitor::ResolvedValue baseAddress,
                    uint64_t sectSize)
            : sectionBase(baseAddress), sectSize(sectSize)
        {
        }

        metadata_visitor::ResolvedValue sectionBase;
        uint64_t                        sectSize;
    };

    std::optional<Visitor::DataSection> findObjCDataSection(const char *sectionName) const;

    void forEachClass(bool visitMetaClasses, const Visitor::DataSection& classListSection,
                      void (^callback)(Class& objcClass, bool isMetaClass, bool& stopClass));
};

// A wrapped around an individual objc Method in a Method list.
struct Method
{
    enum class Kind
    {
        // All fields are relative.  The name field is a relative offset to a
        // selector reference
        relativeIndirect,

        // All fields are relative.  The name field is an offset from the start
        // of the selector strings buffer.
        relativeDirect,

        // All fields are pointers
        pointer
    };

    Method(Kind kind, metadata_visitor::ResolvedValue methodPos) : kind(kind), methodPos(methodPos) { }

    metadata_visitor::ResolvedValue getNameField(const Visitor& objcVisitor) const;
    metadata_visitor::ResolvedValue getTypesField(const Visitor& objcVisitor) const;
    metadata_visitor::ResolvedValue getIMPField(const Visitor& objcVisitor) const;

    const char* getName(const Visitor& objcVisitor) const;
    const char* getTypes(const Visitor& objcVisitor) const;
    const void* getIMP(const Visitor& objcVisitor) const;

    VMAddress                   getNameVMAddr(const Visitor& objcVisitor) const;
    VMAddress                   getTypesVMAddr(const Visitor& objcVisitor) const;
    std::optional<VMAddress>    getIMPVMAddr(const Visitor& objcVisitor) const;

    // For indirect methods, the indirect offset points to a selector reference.  This returns
    // the vmAddr of that selector reference
    VMAddress getNameSelRefVMAddr(const Visitor& objcVisitor) const;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    // Only used by the cache builder when sorting method lists
    void setName(const Visitor& objcVisitor, VMAddress nameVMAddr);
    void setTypes(const Visitor& objcVisitor, VMAddress typesVMAddr);
    void setIMP(const Visitor& objcVisitor, std::optional<VMAddress> impVMAddr);
#endif

    void convertNameToOffset(const Visitor& objcVisitor, uint32_t nameOffset);

private:

    struct relative_method_t {
        int32_t nameOffset;   // SEL*
        int32_t typesOffset;  // const char *
        int32_t impOffset;    // IMP
    };

    struct method32_t {
        uint32_t nameVMAddr;   // SEL
        uint32_t typesVMAddr;  // const char *
        uint32_t impVMAddr;    // IMP
    };

    struct method64_t {
        uint64_t nameVMAddr;   // SEL
        uint64_t typesVMAddr;  // const char *
        uint64_t impVMAddr;    // IMP
    };

    enum class Field
    {
        name,
        types,
        imp
    };

    const void* getFieldPos(const Visitor& objcVisitor, Field field) const;

    const Kind                              kind;
    const metadata_visitor::ResolvedValue   methodPos;
};

// A wrapper around a Method list.  Points to the merhod list in the mach-o buffer, and can be used
// to find the other fields of the method list.
struct MethodList
{
    MethodList(std::optional<metadata_visitor::ResolvedValue> methodListPos) : methodListPos(methodListPos) { }

    // This matches the bits in the objc runtime
    enum : uint32_t {
        methodListIsUniqued             = 0x1,
        methodListIsSorted              = 0x2,

        methodListUsesSelectorOffsets   = 0x40000000,
        methodListIsRelative            = 0x80000000,

        // The size is bits 2 through 16 of the entsize field
        // The low 2 bits are uniqued/sorted as above.  The upper 16-bits
        // are reserved for other flags
        methodListSizeMask              = 0x0000FFFC
    };

    // Note a method list looks like this to libobjc:
    struct method_list_t {
        // The method list stores the size of each element:
        // - pointer based method lists: 3 * uintptr_t
        // - relative method lists: 3 * uint32_t
        uint32_t getMethodSize() const {
            return this->entsize & MethodList::methodListSizeMask;
        }

        uint32_t getMethodCount() const {
            return this->count;
        }

        const uint8_t* methodBase() const {
            return &this->methodArrayBase[0];
        }

        // The shared cache changes selectors to be offsets from a base pointer
        // This returns true if a method list has been converted to this form
        bool usesOffsetsFromSelectorBuffer() const {
            return (entsize & methodListUsesSelectorOffsets) != 0;
        }

        // Returns true if this is a relative method list.  False if its pointer based
        bool usesRelativeOffsets() const {
            return (entsize & methodListIsRelative) != 0;
        }

        void setIsUniqued()
        {
            this->entsize |= methodListIsUniqued;
        }

        void setIsSorted()
        {
            this->entsize |= methodListIsSorted;
        }

        void setUsesOffsetsFromSelectorBuffer()
        {
            this->entsize |= methodListUsesSelectorOffsets;
        }

    private:
        uint32_t    entsize;
        uint32_t    count;
        uint8_t     methodArrayBase[]; // Note this is the start the array method_t[0]
    };

    uint32_t numMethods() const;

    bool usesOffsetsFromSelectorBuffer() const;
    bool usesRelativeOffsets() const;

    Method getMethod(const Visitor& objcVisitor, uint32_t i) const;

    void setIsUniqued();
    void setIsSorted();
    void setUsesOffsetsFromSelectorBuffer();

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    const void*                 getLocation() const;
    std::optional<VMAddress>    getVMAddress() const;
#endif

private:
    const std::optional<metadata_visitor::ResolvedValue> methodListPos;
};

// A wrapper around a Protocol list.  Points to the merhod list in the mach-o buffer, and can be used
// to find the other fields of the method list.
struct ProtocolList
{
    ProtocolList(std::optional<metadata_visitor::ResolvedValue> protocolListPos) : protocolListPos(protocolListPos) { }

    uint64_t        numProtocols(const Visitor& objcVisitor) const;
    Protocol        getProtocol(const Visitor& objcVisitor, uint64_t i) const;

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    void            setProtocol(const Visitor& objcVisitor, uint64_t i, VMAddress vmAddr);
#endif

    __attribute__((used))
    void dump(const Visitor& objcVisitor) const;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    const void*                 getLocation() const;
    std::optional<VMAddress>    getVMAddress() const;
#endif

private:

    // Note a protocol list looks like this to libobjc:
    template<typename PtrTy>
    struct protocol_list_t {
        PtrTy       count;
        PtrTy       list[];
    };

    typedef protocol_list_t<uint32_t> protocol_list32_t;
    typedef protocol_list_t<uint64_t> protocol_list64_t;

    metadata_visitor::ResolvedValue getProtocolField(const Visitor& objcVisitor, uint64_t i) const;

    const std::optional<metadata_visitor::ResolvedValue> protocolListPos;
};

// A wrapped around an individual objc IVar in a IVar list.
struct IVar
{
    IVar(metadata_visitor::ResolvedValue ivarPos) : ivarPos(ivarPos) { }

    std::optional<uint32_t> getOffset(const Visitor& objcVisitor) const;
    void                    setOffset(const Visitor& objcVisitor, uint32_t offset) const;
    const char*             getName(const Visitor& objcVisitor) const;
    uint32_t                getAlignment(const Visitor& objcVisitor) const;
    bool                    elided(const Visitor& objcVisitor) const;   // size == 0

private:

    template<typename PtrTy>
    struct ivar_t {
        PtrTy       offsetVMAddr;  // uint32_t*
        PtrTy       nameVMAddr;    // const char *
        PtrTy       typeVMAddr;    // const char *
        uint32_t    alignment;
        uint32_t    size;
    };

    typedef ivar_t<uint32_t> ivar32_t;
    typedef ivar_t<uint64_t> ivar64_t;

    enum class Field
    {
        offset,
        name,
        type,
        alignment,
        size
    };

    const void* getFieldPos(const Visitor& objcVisitor, Field field) const;

    const metadata_visitor::ResolvedValue ivarPos;
};

// A wrapper around an IVar list.  Points to the ivar list in the mach-o buffer, and can be used
// to find the other fields of the ivar list.
struct IVarList
{
    IVarList(std::optional<metadata_visitor::ResolvedValue> ivarListPos) : ivarListPos(ivarListPos) { }

    // Note a ivar list looks like this to libobjc:
    struct ivar_list_t {

        uint32_t getElementSize() const
        {
            return this->entsize;
        }

        uint32_t getCount() const
        {
            return this->count;
        }

        const uint8_t* ivarBase() const {
            return &this->ivarArrayBase[0];
        }

    private:
        uint32_t    entsize;
        uint32_t    count;
        uint8_t     ivarArrayBase[]; // Note this is the start the array ivar_t[0]
    };

    uint32_t numIVars() const;
    IVar    getIVar(const Visitor& objcVisitor, uint32_t i) const;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    const void*                 getLocation() const;
    std::optional<VMAddress>    getVMAddress() const;
#endif

private:
    const std::optional<metadata_visitor::ResolvedValue> ivarListPos;
};

struct ClassData
{
    ClassData(metadata_visitor::ResolvedValue classDataPos) : classDataPos(classDataPos) { }

    // Data is a pointer to the read only data for the class, ie, one of these according to objc:
    template<typename PtrTy>
    struct data_t {
        uint32_t flags;
        uint32_t instanceStart;
        // Note there is 4-bytes of alignment padding between instanceSize and ivarLayout
        // on 64-bit archs, but no padding on 32-bit archs.
        // This union is a way to model that.
        union {
            uint32_t    instanceSize;
            PtrTy       pad;
        } instanceSize;
        PtrTy ivarLayoutVMAddr;
        PtrTy nameVMAddr;
        PtrTy baseMethodsVMAddr;
        PtrTy baseProtocolsVMAddr;
        PtrTy ivarsVMAddr;
        PtrTy weakIvarLayoutVMAddr;
        PtrTy basePropertiesVMAddr;
    };

    typedef data_t<uint32_t> data32_t;
    typedef data_t<uint64_t> data64_t;

    enum class Field
    {
        flags,
        instanceStart,
        instanceSize,
        ivarLayout,
        name,
        baseMethods,
        baseProtocols,
        ivars,
        weakIvarLayout,
        baseProperties
    };

    const void*                         getFieldPos(const Visitor& objcVisitor, Field field) const;
    metadata_visitor::ResolvedValue     getField(const Visitor& objcVisitor, Field field) const;

    const void* getLocation() const;
    VMAddress   getVMAddress() const;

private:
    metadata_visitor::ResolvedValue classDataPos;
};

// A wrapper around a Class.  Points to the class in the mach-o buffer, and can be used
// to find the other fields of the class.
struct Class
{
    Class(metadata_visitor::ResolvedValue classPos, bool isMetaClass, bool isPatchable)
        : isMetaClass(isMetaClass), isPatchable(isPatchable), classPos(classPos) { }

    metadata_visitor::ResolvedValue                 getISA(const Visitor& objcVisitor, bool& isPatchableClass) const;
    std::optional<metadata_visitor::ResolvedValue>  getSuperclass(const Visitor& objcVisitor) const;
    metadata_visitor::ResolvedValue                 getMethodCache(const Visitor& objcVisitor) const;
    std::optional<metadata_visitor::ResolvedValue>  getMethodCacheProperties(const Visitor& objcVisitor) const;
    ClassData                                       getClassData(const Visitor& objcVisitor) const;
    std::optional<uint32_t>                         swiftClassFlags(const Visitor& objcVisitor) const;

    std::optional<VMAddress>                    getSuperclassVMAddr(const Visitor& objcVisitor) const;
    std::optional<VMAddress>                    getMethodCachePropertiesVMAddr(const Visitor& objcVisitor) const;
    void                                        setMethodCachePropertiesVMAddr(const Visitor& objcVisitor, VMAddress vmAddr);

    // Returns the superclass field itself, ie the value of &this->superclass
    metadata_visitor::ResolvedValue             getSuperclassField(const Visitor& objcVisitor) const;

    // Returns the vtable field itself, ie the value of &this->vtable
    metadata_visitor::ResolvedValue             getMethodCachePropertiesField(const Visitor& objcVisitor) const;

    // Returns the data field itself, ie the value of &this->data
    metadata_visitor::ResolvedValue             getDataField(const Visitor& objcVisitor) const;

    // Gets the raw fixup and chained pointer format for the super class fixup
    void withSuperclass(const Visitor& objcVisitor,
                        void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const;

    // Note this is taken from the objc runtime
    bool isSwiftLegacy(const Visitor& objcVisitor) const;
    bool isSwiftStable(const Visitor& objcVisitor) const;
    bool isSwift(const Visitor& objcVisitor) const;
    bool isUnfixedBackwardDeployingStableSwift(const Visitor& objcVisitor) const;

    bool                isRootClass(const Visitor& objcVisitor) const;
    uint32_t            getInstanceStart(const Visitor& objcVisitor) const;
    void                setInstanceStart(const Visitor& objcVisitor, uint32_t value) const;
    uint32_t            getInstanceSize(const Visitor& objcVisitor) const;
    void                setInstanceSize(const Visitor& objcVisitor, uint32_t value) const;
    const char*         getName(const Visitor& objcVisitor) const;
    VMAddress           getNameVMAddr(const Visitor& objcVisitor) const;
    MethodList          getBaseMethods(const Visitor& objcVisitor) const;
    ProtocolList        getBaseProtocols(const Visitor& objcVisitor) const;
    IVarList            getIVars(const Visitor& objcVisitor) const;

    const void* getLocation() const;
    VMAddress   getVMAddress() const;

    const bool          isMetaClass;
    const bool          isPatchable;

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    // We need everything public to write tests
public:
#else
private:
#endif

    // Note a class looks like this to libobjc:
    template<typename PtrTy>
    struct class_t {
        PtrTy isaVMAddr;
        PtrTy superclassVMAddr;
        PtrTy methodCacheBuckets;
        PtrTy methodCacheProperties; // aka vtable
        PtrTy dataVMAddrAndFastFlags;

        // This field is only present if this is a Swift object, ie, has the Swift
        // fast bits set
        uint32_t swiftClassFlags;
    };

    typedef class_t<uint32_t> class32_t;
    typedef class_t<uint64_t> class64_t;

    enum : uint32_t {
        FAST_DATA_MASK32 = 0xfffffffcUL
    };
    enum : uint64_t {
        FAST_DATA_MASK64 = 0x00007ffffffffff8ULL
    };

    // These are embedded in the Mach-O itself by the compiler
    enum FastDataBits {
        FAST_IS_SWIFT_LEGACY    = 0x1,
        FAST_IS_SWIFT_STABLE    = 0x2
    };

    // These are embedded by the Swift compiler in the swiftClassFlags field
    enum SwiftClassFlags {
        isSwiftPreStableABI     = 0x1
    };

    enum class Field
    {
        isa,
        superclass,
        methodCacheBuckets,
        methodCacheProperties,
        data,
        swiftClassFlags
    };

    const void* getFieldPos(const Visitor& objcVisitor, Field field) const;

    const metadata_visitor::ResolvedValue classPos;
};

// A wrapper around a Category.  Points to the category in the mach-o buffer, and can be used
// to find the other fields of the category.
struct Category
{
    Category(metadata_visitor::ResolvedValue categoryPos) : categoryPos(categoryPos) { }

    const char*         getName(const Visitor& objcVisitor) const;
    MethodList          getInstanceMethods(const Visitor& objcVisitor) const;
    MethodList          getClassMethods(const Visitor& objcVisitor) const;
    ProtocolList        getProtocols(const Visitor& objcVisitor) const;

    // Gets the raw fixup and chained pointer format for the class fixup
    void withClass(const Visitor& objcVisitor,
                   void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const;

private:

    // Note a category looks like this to libobjc:
    template<typename PtrTy>
    struct category_t {
        PtrTy nameVMAddr;
        PtrTy clsVMAddr;
        PtrTy instanceMethodsVMAddr;
        PtrTy classMethodsVMAddr;
        PtrTy protocolsVMAddr;
        PtrTy instancePropertiesVMAddr;
    };

    typedef category_t<uint32_t> category32_t;
    typedef category_t<uint64_t> category64_t;

    enum class Field
    {
        name,
        cls,
        instanceMethods,
        classMethods,
        protocols,
        instanceProperties
    };

    const void* getFieldPos(const Visitor& objcVisitor, Field field) const;

    const metadata_visitor::ResolvedValue categoryPos;
};

// A wrapper around a Protocol.  Points to the protocol in the mach-o buffer, and can be used
// to find the other fields of the protocol.
struct Protocol
{
    Protocol(metadata_visitor::ResolvedValue protocolPos) : protocolPos(protocolPos) { }

    std::optional<VMAddress>                    getISAVMAddr(const Visitor& objcVisitor) const;
    const char*                                 getName(const Visitor& objcVisitor) const;
    VMAddress                                   getNameVMAddr(const Visitor& objcVisitor) const;
    ProtocolList                                getProtocols(const Visitor& objcVisitor) const;
    MethodList                                  getInstanceMethods(const Visitor& objcVisitor) const;
    MethodList                                  getClassMethods(const Visitor& objcVisitor) const;
    MethodList                                  getOptionalInstanceMethods(const Visitor& objcVisitor) const;
    MethodList                                  getOptionalClassMethods(const Visitor& objcVisitor) const;
    uint32_t                                        getSize(const Visitor& objcVisitor) const;
    void                                            setSize(const Visitor& objcVisitor, uint32_t size);
    void                                            setFixedUp(const Visitor& objcVisitor);
    void                                            setIsCanonical(const Visitor& objcVisitor);
    std::optional<metadata_visitor::ResolvedValue>  getExtendedMethodTypes(const Visitor& objcVisitor) const;
    std::optional<const char*>                  getDemangledName(const Visitor& objcVisitor) const;
    void                                        setDemangledName(const Visitor& objcVisitor, VMAddress nameVMAddr);

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
    void                                        setISA(const Visitor& objcVisitor, VMAddress vmAddr,
                                                       const dyld3::MachOFile::PointerMetaData& PMD);

    // Used by the cache builder to construct fixup chains.  We need to find all fields of the protocol which have values
    void                                        addFixups(const Visitor& objcVisitor, std::vector<void*>& fixups) const;
    std::optional<uint16_t>                     chainedPointerFormat() const;
#endif

    const void* getLocation() const;
    VMAddress   getVMAddress() const;

    static uint32_t getSize(bool is64);

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
    // We need everything public to write tests
public:
#else
private:
#endif

    // Note a protocol looks like this to libobjc:
    template<typename PtrTy>
    struct protocol_t {
        PtrTy       isaVMAddr;
        PtrTy       nameVMAddr;
        PtrTy       protocolsVMAddr;
        PtrTy       instanceMethodsVMAddr;
        PtrTy       classMethodsVMAddr;
        PtrTy       optionalInstanceMethodsVMAddr;
        PtrTy       optionalClassMethodsVMAddr;
        PtrTy       instancePropertiesVMAddr;
        uint32_t    size;
        uint32_t    flags;
        // Fields below this point are not always present on disk.
        PtrTy       extendedMethodTypesVMAddr;
        PtrTy       demangledNameVMAddr;
        PtrTy       classPropertiesVMAddr;
    };

    typedef protocol_t<uint32_t> protocol32_t;
    typedef protocol_t<uint64_t> protocol64_t;

    enum class Field
    {
        isa,
        name,
        protocols,
        instanceMethods,
        classMethods,
        optionalInstanceMethods,
        optionalClassMethods,
        instanceProperties,
        size,
        flags,
        // Fields below this point are not always present on disk.
        extendedMethodTypes,
        demangledName,
        classProperties,
    };

    const void* getFieldPos(const Visitor& objcVisitor, Field field) const;

    const metadata_visitor::ResolvedValue protocolPos;
};

} // namespace objc_visitor

#endif // ObjCVisitor_h
