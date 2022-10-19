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

#include "ObjCVisitor.h"

#if SUPPORT_VM_LAYOUT
#include "MachOAnalyzer.h"
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "ASLRTracker.h"
#endif

using namespace objc_visitor;
using metadata_visitor::ResolvedValue;

#if !SUPPORT_VM_LAYOUT
using metadata_visitor::Segment;
#endif

//
// MARK: --- Class methods ---
//

const void* Class::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const class32_t* class32 = (const class32_t*)this->classPos.value();
        switch ( field ) {
            case Field::isa:
                return &class32->isaVMAddr;
            case Field::superclass:
                return &class32->superclassVMAddr;
            case Field::methodCacheBuckets:
                return &class32->methodCacheBuckets;
            case Field::methodCacheProperties:
                return &class32->methodCacheProperties;
            case Field::data:
                return &class32->dataVMAddrAndFastFlags;
            case Field::swiftClassFlags:
                return &class32->swiftClassFlags;
        }
    } else {
        const class64_t* class64 = (const class64_t*)this->classPos.value();
        switch ( field ) {
            case Field::isa:
                return &class64->isaVMAddr;
            case Field::superclass:
                return &class64->superclassVMAddr;
            case Field::methodCacheBuckets:
                return &class64->methodCacheBuckets;
            case Field::methodCacheProperties:
                return &class64->methodCacheProperties;
            case Field::data:
                return &class64->dataVMAddrAndFastFlags;
            case Field::swiftClassFlags:
                return &class64->swiftClassFlags;
        }
    }
}

ResolvedValue Class::getISA(const Visitor& objcVisitor, bool& isPatchableClass) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::isa));
    return objcVisitor.resolveBindOrRebase(field, isPatchableClass);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL
std::optional<ResolvedValue> Class::getSuperclass(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::superclass));
    return objcVisitor.resolveOptionalRebase(field);
}

std::optional<VMAddress> Class::getSuperclassVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::superclass));
    std::optional<VMAddress> targetAddress = objcVisitor.resolveOptionalRebaseToVMAddress(field);

    if ( !targetAddress.has_value() )
        return std::nullopt;

    return targetAddress;
}
#endif

ResolvedValue Class::getSuperclassField(const Visitor& objcVisitor) const
{
    return objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::superclass));
}

ResolvedValue Class::getMethodCache(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheBuckets));

    bool unusedIsPatchableClass;
    return objcVisitor.resolveBindOrRebase(field, unusedIsPatchableClass);
}

std::optional<ResolvedValue> Class::getMethodCacheProperties(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheProperties));
    return objcVisitor.resolveOptionalRebase(field);
}

ResolvedValue Class::getMethodCachePropertiesField(const Visitor& objcVisitor) const
{
    return objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheProperties));
}

std::optional<VMAddress> Class::getMethodCachePropertiesVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheProperties));

    // The field might be null.  Get the target value, then see if it has a value
    std::optional<ResolvedValue> targetValue = objcVisitor.resolveOptionalRebase(field);
    if ( targetValue.has_value() )
        return targetValue->vmAddress();

    return { };
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Class::setMethodCachePropertiesVMAddr(const Visitor& objcVisitor, VMAddress vmAddr)
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheProperties));
    objcVisitor.updateTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()));
}
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Class::withSuperclass(const Visitor& objcVisitor,
                           void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const
{
    assert(objcVisitor.pointerSize == 8);

    assert(objcVisitor.isOnDiskBinary());
    uint16_t chainedPointerFormat = this->classPos.chainedPointerFormat().value();

    const void* fieldPos = &((const class64_t*)this->classPos.value())->superclassVMAddr;
    dyld3::MachOFile::ChainedFixupPointerOnDisk* fieldFixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)fieldPos;
    handler(fieldFixup, chainedPointerFormat);
}
#endif

bool Class::isUnfixedBackwardDeployingStableSwift(const Visitor& objcVisitor) const
{
    // Only classes marked as Swift legacy need apply.
    if ( !this->isSwiftLegacy(objcVisitor) )
        return false;

    std::optional<uint32_t> swiftClassFlags = this->swiftClassFlags(objcVisitor);
    if ( swiftClassFlags.has_value() ) {
        // Check the true legacy vs stable distinguisher.
        // The low bit of Swift's ClassFlags is SET for true legacy
        // and UNSET for stable pretending to be legacy.
        bool isActuallySwiftLegacy = (swiftClassFlags.value() & isSwiftPreStableABI) != 0;
        return !isActuallySwiftLegacy;
    } else {
        // Is this possible?  We were a legacy class but had no flags?
        return false;
    }
}

bool Class::isSwiftLegacy(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::data));
    ResolvedValue fieldValue = objcVisitor.resolveRebase(field);

    // Mask out the flags from the data value
    uint64_t rawDataVMAddr = fieldValue.vmAddress().rawValue();
    return (rawDataVMAddr & FAST_IS_SWIFT_LEGACY) != 0;
}

bool Class::isSwiftStable(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::data));
    ResolvedValue fieldValue = objcVisitor.resolveRebase(field);

    // Mask out the flags from the data value
    uint64_t rawDataVMAddr = fieldValue.vmAddress().rawValue();
    return (rawDataVMAddr & FAST_IS_SWIFT_STABLE) != 0;
}

bool Class::isSwift(const Visitor& objcVisitor) const
{
    return this->isSwiftStable(objcVisitor) || this->isSwiftLegacy(objcVisitor);
}

std::optional<uint32_t> Class::swiftClassFlags(const Visitor& objcVisitor) const
{
    if ( !this->isSwift(objcVisitor) )
        return { };

    return *(uint32_t*)this->getFieldPos(objcVisitor, Field::swiftClassFlags);
}

ResolvedValue Class::getDataField(const Visitor& objcVisitor) const
{
    return objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::data));
}

ClassData Class::getClassData(const Visitor& objcVisitor) const
{
    ResolvedValue field = this->getDataField(objcVisitor);
    ResolvedValue targetValue = objcVisitor.resolveRebase(field);
    // Mask out the low bits, if they are set
    VMAddress vmAddr = targetValue.vmAddress();

    uint64_t mask = (objcVisitor.pointerSize == 4) ? (uint64_t)FAST_DATA_MASK32 : FAST_DATA_MASK64;
    uint64_t rawVMAddr = vmAddr.rawValue();
    uint64_t maskedVMAddr = rawVMAddr & mask;
    if ( maskedVMAddr != rawVMAddr ) {
        // Adjust the pointer as we have bits to remove
        uint64_t adjust = rawVMAddr - maskedVMAddr;
        const uint8_t* unadjustedValue = (const uint8_t*)targetValue.value();
        const uint8_t* adjustedValue = unadjustedValue - adjust;
        //return ClassData(ResolvedValue(targetValue, adjustedValue));

        // We can't just construct a new ResolvedValue here with the adjusted value as we don't
        // have the right constructors when building for dyld.  Instead we'll pretend we are
        // just accessing a field of a struct, where the value we have is the struct and we are
        // resolving to a value before it.  Eg, "struct foo ... return &foo - 2"
        ResolvedValue adjustedField = objcVisitor.getField(targetValue, adjustedValue);
        return ClassData(adjustedField);
    }
    return ClassData(targetValue);
}

bool Class::isRootClass(const Visitor& objcVisitor) const
{
    ClassData data = getClassData(objcVisitor);
    uint32_t flags = *(uint32_t*)data.getFieldPos(objcVisitor, ClassData::Field::flags);

    const uint32_t RO_ROOT = (1 << 1);
    return (flags & RO_ROOT) != 0;
}

const char* Class::getName(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::name);
    return (const char*)objcVisitor.resolveRebase(field).value();
}

VMAddress Class::getNameVMAddr(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::name);
    return objcVisitor.resolveRebase(field).vmAddress();
}

MethodList Class::getBaseMethods(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseMethods);
    return objcVisitor.resolveOptionalRebase(field);
}

ProtocolList Class::getBaseProtocols(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseProtocols);
    return objcVisitor.resolveOptionalRebase(field);
}

IVarList Class::getIVars(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::ivars);
    return objcVisitor.resolveOptionalRebase(field);
}

uint32_t Class::getInstanceStart(const Visitor& objcVisitor) const
{
    ClassData data = getClassData(objcVisitor);
    return *(uint32_t*)data.getFieldPos(objcVisitor, ClassData::Field::instanceStart);
}

void Class::setInstanceStart(const Visitor& objcVisitor, uint32_t value) const
{
    ClassData data = getClassData(objcVisitor);
    *(uint32_t*)data.getFieldPos(objcVisitor, ClassData::Field::instanceStart) = value;
}

uint32_t Class::getInstanceSize(const Visitor& objcVisitor) const
{
    ClassData data = getClassData(objcVisitor);
    return *(uint32_t*)data.getFieldPos(objcVisitor, ClassData::Field::instanceSize);
}

void Class::setInstanceSize(const Visitor& objcVisitor, uint32_t value) const
{
    ClassData data = getClassData(objcVisitor);
    *(uint32_t*)data.getFieldPos(objcVisitor, ClassData::Field::instanceSize) = value;
}

const void* Class::getLocation() const
{
    return this->classPos.value();
}

VMAddress Class::getVMAddress() const
{
    return this->classPos.vmAddress();
}

//
// MARK: --- ClassData methods ---
//

const void* ClassData::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const data32_t* data32 = (const data32_t*)this->classDataPos.value();
        switch ( field ) {
            case Field::flags:
                return &data32->flags;
            case Field::instanceStart:
                return &data32->instanceStart;
            case Field::instanceSize:
                return &data32->instanceSize;
            case Field::ivarLayout:
                return &data32->ivarLayoutVMAddr;
            case Field::name:
                return &data32->nameVMAddr;
            case Field::baseMethods:
                return &data32->baseMethodsVMAddr;
            case Field::baseProtocols:
                return &data32->baseProtocolsVMAddr;
            case Field::ivars:
                return &data32->ivarsVMAddr;
            case Field::weakIvarLayout:
                return &data32->weakIvarLayoutVMAddr;
            case Field::baseProperties:
                return &data32->basePropertiesVMAddr;
        }
    } else {
        const data64_t* data64 = (const data64_t*)this->classDataPos.value();
        switch ( field ) {
            case Field::flags:
                return &data64->flags;
            case Field::instanceStart:
                return &data64->instanceStart;
            case Field::instanceSize:
                return &data64->instanceSize;
            case Field::ivarLayout:
                return &data64->ivarLayoutVMAddr;
            case Field::name:
                return &data64->nameVMAddr;
            case Field::baseMethods:
                return &data64->baseMethodsVMAddr;
            case Field::baseProtocols:
                return &data64->baseProtocolsVMAddr;
            case Field::ivars:
                return &data64->ivarsVMAddr;
            case Field::weakIvarLayout:
                return &data64->weakIvarLayoutVMAddr;
            case Field::baseProperties:
                return &data64->basePropertiesVMAddr;
        }
    }
}

ResolvedValue ClassData::getField(const Visitor& objcVisitor, Field field) const
{
    return objcVisitor.getField(this->classDataPos, this->getFieldPos(objcVisitor, field));
}

const void* ClassData::getLocation() const
{
    return this->classDataPos.value();
}

VMAddress ClassData::getVMAddress() const
{
    return this->classDataPos.vmAddress();
}

//
// MARK: --- Category methods ---
//

const void* Category::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const category32_t* category32 = (const category32_t*)this->categoryPos.value();
        switch ( field ) {
            case Field::name:
                return &category32->nameVMAddr;
            case Field::cls:
                return &category32->clsVMAddr;
            case Field::instanceMethods:
                return &category32->instanceMethodsVMAddr;
            case Field::classMethods:
                return &category32->classMethodsVMAddr;
            case Field::protocols:
                return &category32->protocolsVMAddr;
            case Field::instanceProperties:
                return &category32->instancePropertiesVMAddr;
        }
    } else {
        const category64_t* category64 = (const category64_t*)this->categoryPos.value();
        switch ( field ) {
            case Field::name:
                return &category64->nameVMAddr;
            case Field::cls:
                return &category64->clsVMAddr;
            case Field::instanceMethods:
                return &category64->instanceMethodsVMAddr;
            case Field::classMethods:
                return &category64->classMethodsVMAddr;
            case Field::protocols:
                return &category64->protocolsVMAddr;
            case Field::instanceProperties:
                return &category64->instancePropertiesVMAddr;
        }
    }
}

const char* Category::getName(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::name));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

MethodList Category::getInstanceMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::instanceMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

MethodList Category::getClassMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::classMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

ProtocolList Category::getProtocols(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::protocols));
    return objcVisitor.resolveOptionalRebase(field);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Category::withClass(const Visitor& objcVisitor,
                         void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const
{
    assert(objcVisitor.pointerSize == 8);

    assert(objcVisitor.isOnDiskBinary());
    uint16_t chainedPointerFormat = this->categoryPos.chainedPointerFormat().value();

    const void* fieldPos = &((const category64_t*)this->categoryPos.value())->clsVMAddr;
    dyld3::MachOFile::ChainedFixupPointerOnDisk* fieldFixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)fieldPos;
    handler(fieldFixup, chainedPointerFormat);
}
#endif

//
// MARK: --- Protocol methods ---
//

const void* Protocol::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const protocol32_t* protocol32 = (const protocol32_t*)this->protocolPos.value();
        switch ( field ) {
            case Field::isa:
                return &protocol32->isaVMAddr;
            case Field::name:
                return &protocol32->nameVMAddr;
            case Field::protocols:
                return &protocol32->protocolsVMAddr;
            case Field::instanceMethods:
                return &protocol32->instanceMethodsVMAddr;
            case Field::classMethods:
                return &protocol32->classMethodsVMAddr;
            case Field::optionalInstanceMethods:
                return &protocol32->optionalInstanceMethodsVMAddr;
            case Field::optionalClassMethods:
                return &protocol32->optionalClassMethodsVMAddr;
            case Field::instanceProperties:
                return &protocol32->instancePropertiesVMAddr;
            case Field::size:
                return &protocol32->size;
            case Field::flags:
                return &protocol32->flags;
            case Field::extendedMethodTypes:
                return &protocol32->extendedMethodTypesVMAddr;
            case Field::demangledName:
                return &protocol32->demangledNameVMAddr;
            case Field::classProperties:
                return &protocol32->classPropertiesVMAddr;
        }
    } else {
        const protocol64_t* protocol64 = (const protocol64_t*)this->protocolPos.value();
        switch ( field ) {
            case Field::isa:
                return &protocol64->isaVMAddr;
            case Field::name:
                return &protocol64->nameVMAddr;
            case Field::protocols:
                return &protocol64->protocolsVMAddr;
            case Field::instanceMethods:
                return &protocol64->instanceMethodsVMAddr;
            case Field::classMethods:
                return &protocol64->classMethodsVMAddr;
            case Field::optionalInstanceMethods:
                return &protocol64->optionalInstanceMethodsVMAddr;
            case Field::optionalClassMethods:
                return &protocol64->optionalClassMethodsVMAddr;
            case Field::instanceProperties:
                return &protocol64->instancePropertiesVMAddr;
            case Field::size:
                return &protocol64->size;
            case Field::flags:
                return &protocol64->flags;
            case Field::extendedMethodTypes:
                return &protocol64->extendedMethodTypesVMAddr;
            case Field::demangledName:
                return &protocol64->demangledNameVMAddr;
            case Field::classProperties:
                return &protocol64->classPropertiesVMAddr;
        }
    }
}

std::optional<VMAddress> Protocol::getISAVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::isa));
    std::optional<ResolvedValue> value = objcVisitor.resolveOptionalRebase(field);
    if ( value )
        return value->vmAddress();

    return { };
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Protocol::setISA(const Visitor& objcVisitor, VMAddress vmAddr, const dyld3::MachOFile::PointerMetaData& PMD)
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::isa));
    objcVisitor.setTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()), PMD);
}
#endif

const char* Protocol::getName(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::name));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

VMAddress Protocol::getNameVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::name));
    return objcVisitor.resolveRebase(field).vmAddress();
}

ProtocolList Protocol::getProtocols(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::protocols));
    return objcVisitor.resolveOptionalRebase(field);
}

MethodList Protocol::getInstanceMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::instanceMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

MethodList Protocol::getClassMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::classMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

MethodList Protocol::getOptionalInstanceMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::optionalInstanceMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

MethodList Protocol::getOptionalClassMethods(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::optionalClassMethods));
    return objcVisitor.resolveOptionalRebase(field);
}

uint32_t Protocol::getSize(const Visitor& objcVisitor) const
{
    return *(uint32_t*)this->getFieldPos(objcVisitor, Field::size);
}

void Protocol::setSize(const Visitor& objcVisitor, uint32_t size)
{
    *(uint32_t*)this->getFieldPos(objcVisitor, Field::size) = size;
}

void Protocol::setFixedUp(const Visitor& objcVisitor)
{
    uint32_t& flags = *(uint32_t*)this->getFieldPos(objcVisitor, Field::flags);

    assert((flags & (1<<30)) == 0);
    flags = flags | (1<<30);
}

void Protocol::setIsCanonical(const Visitor& objcVisitor)
{
    uint32_t& flags = *(uint32_t*)this->getFieldPos(objcVisitor, Field::flags);

    assert((flags & (1<<29)) == 0);
    flags = flags | (1<<29);
}

std::optional<ResolvedValue> Protocol::getExtendedMethodTypes(const Visitor& objcVisitor) const
{
    // extendedMethodTypes is not always present on disk.
    uint32_t structSize = this->getSize(objcVisitor);
    if ( objcVisitor.pointerSize == 4 ) {
        if ( structSize < (offsetof(protocol32_t, extendedMethodTypesVMAddr) + sizeof(protocol32_t::extendedMethodTypesVMAddr)) )
            return std::nullopt;
    } else {
        if ( structSize < (offsetof(protocol64_t, extendedMethodTypesVMAddr) + sizeof(protocol64_t::extendedMethodTypesVMAddr)) )
            return std::nullopt;
    }

    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::extendedMethodTypes));
    return objcVisitor.resolveOptionalRebase(field);
}

std::optional<const char*> Protocol::getDemangledName(const Visitor& objcVisitor) const
{
    // demangledName is not always present on disk.
    uint32_t structSize = this->getSize(objcVisitor);
    if ( objcVisitor.pointerSize == 4 ) {
        if ( structSize < (offsetof(protocol32_t, demangledNameVMAddr) + sizeof(protocol32_t::demangledNameVMAddr)) )
            return std::nullopt;
    } else {
        if ( structSize < (offsetof(protocol64_t, demangledNameVMAddr) + sizeof(protocol64_t::demangledNameVMAddr)) )
            return std::nullopt;
    }

    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::demangledName));
    std::optional<ResolvedValue> value = objcVisitor.resolveOptionalRebase(field);
    if ( value.has_value() )
        return (const char*)value->value();

    return std::nullopt;
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Protocol::setDemangledName(const Visitor& objcVisitor, VMAddress vmAddr)
{
    uint32_t structSize = this->getSize(objcVisitor);
    if ( objcVisitor.pointerSize == 4 ) {
        assert(structSize >= (offsetof(protocol32_t, demangledNameVMAddr) + sizeof(protocol32_t::demangledNameVMAddr)));
    } else {
        assert(structSize >= (offsetof(protocol64_t, demangledNameVMAddr) + sizeof(protocol64_t::demangledNameVMAddr)));
    }

    ResolvedValue field = objcVisitor.getField(this->protocolPos, this->getFieldPos(objcVisitor, Field::demangledName));
    objcVisitor.updateTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()));
}
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Protocol::addFixups(const Visitor& objcVisitor, std::vector<void*>& fixups) const
{

    if ( objcVisitor.pointerSize == 4 ) {
        protocol32_t* protocol = (protocol32_t*)this->protocolPos.value();
        if ( protocol->isaVMAddr != 0 )
            fixups.push_back(&protocol->isaVMAddr);
        if ( protocol->nameVMAddr != 0 )
            fixups.push_back(&protocol->nameVMAddr);
        if ( protocol->protocolsVMAddr != 0 )
            fixups.push_back(&protocol->protocolsVMAddr);
        if ( protocol->instanceMethodsVMAddr != 0 )
            fixups.push_back(&protocol->instanceMethodsVMAddr);
        if ( protocol->classMethodsVMAddr != 0 )
            fixups.push_back(&protocol->classMethodsVMAddr);
        if ( protocol->optionalInstanceMethodsVMAddr != 0 )
            fixups.push_back(&protocol->optionalInstanceMethodsVMAddr);
        if ( protocol->optionalClassMethodsVMAddr != 0 )
            fixups.push_back(&protocol->optionalClassMethodsVMAddr);
        if ( protocol->instancePropertiesVMAddr != 0 )
            fixups.push_back(&protocol->instancePropertiesVMAddr);

        uint32_t structSize = ((const protocol32_t*)this->protocolPos.value())->size;
        if ( structSize >= (offsetof(protocol32_t, extendedMethodTypesVMAddr) + sizeof(protocol32_t::extendedMethodTypesVMAddr)) ) {
            if ( protocol->extendedMethodTypesVMAddr != 0 )
                fixups.push_back(&protocol->extendedMethodTypesVMAddr);
        }
        if ( structSize >= (offsetof(protocol32_t, demangledNameVMAddr) + sizeof(protocol32_t::demangledNameVMAddr)) ) {
            if ( protocol->demangledNameVMAddr != 0 )
                fixups.push_back(&protocol->demangledNameVMAddr);
        }
        if ( structSize >= (offsetof(protocol32_t, classPropertiesVMAddr) + sizeof(protocol32_t::classPropertiesVMAddr)) ) {
            if ( protocol->classPropertiesVMAddr != 0 )
                fixups.push_back(&protocol->classPropertiesVMAddr);
        }
    } else {
        protocol64_t* protocol = (protocol64_t*)this->protocolPos.value();
        if ( protocol->isaVMAddr != 0 )
            fixups.push_back(&protocol->isaVMAddr);
        if ( protocol->nameVMAddr != 0 )
            fixups.push_back(&protocol->nameVMAddr);
        if ( protocol->protocolsVMAddr != 0 )
            fixups.push_back(&protocol->protocolsVMAddr);
        if ( protocol->instanceMethodsVMAddr != 0 )
            fixups.push_back(&protocol->instanceMethodsVMAddr);
        if ( protocol->classMethodsVMAddr != 0 )
            fixups.push_back(&protocol->classMethodsVMAddr);
        if ( protocol->optionalInstanceMethodsVMAddr != 0 )
            fixups.push_back(&protocol->optionalInstanceMethodsVMAddr);
        if ( protocol->optionalClassMethodsVMAddr != 0 )
            fixups.push_back(&protocol->optionalClassMethodsVMAddr);
        if ( protocol->instancePropertiesVMAddr != 0 )
            fixups.push_back(&protocol->instancePropertiesVMAddr);

        uint32_t structSize = ((const protocol64_t*)this->protocolPos.value())->size;
        if ( structSize >= (offsetof(protocol64_t, extendedMethodTypesVMAddr) + sizeof(protocol64_t::extendedMethodTypesVMAddr)) ) {
            if ( protocol->extendedMethodTypesVMAddr != 0 )
                fixups.push_back(&protocol->extendedMethodTypesVMAddr);
        }
        if ( structSize >= (offsetof(protocol64_t, demangledNameVMAddr) + sizeof(protocol64_t::demangledNameVMAddr)) ) {
            if ( protocol->demangledNameVMAddr != 0 )
                fixups.push_back(&protocol->demangledNameVMAddr);
        }
        if ( structSize >= (offsetof(protocol64_t, classPropertiesVMAddr) + sizeof(protocol64_t::classPropertiesVMAddr)) ) {
            if ( protocol->classPropertiesVMAddr != 0 )
                fixups.push_back(&protocol->classPropertiesVMAddr);
        }
    }
}

std::optional<uint16_t> Protocol::chainedPointerFormat() const
{
    return this->protocolPos.chainedPointerFormat();
}
#endif

const void* Protocol::getLocation() const
{
    return this->protocolPos.value();
}

VMAddress Protocol::getVMAddress() const
{
    return this->protocolPos.vmAddress();
}

uint32_t Protocol::getSize(bool is64)
{
    return is64 ? sizeof(protocol64_t) : sizeof(protocol32_t);
}

//
// MARK: --- MethodList methods ---
//

uint32_t MethodList::numMethods() const
{
    if ( !methodListPos.has_value() )
        return 0;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    return methodList->getMethodCount();
}

bool MethodList::usesRelativeOffsets() const
{
    if ( !methodListPos.has_value() )
        return false;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    return methodList->usesRelativeOffsets();
}

bool MethodList::usesOffsetsFromSelectorBuffer() const
{
    if ( !methodListPos.has_value() )
        return false;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    return methodList->usesOffsetsFromSelectorBuffer();
}

void MethodList::setIsUniqued()
{
    if ( !methodListPos.has_value() )
        return;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    method_list_t* methodList = (method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    methodList->setIsUniqued();
}

void MethodList::setIsSorted()
{
    if ( !methodListPos.has_value() )
        return;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    method_list_t* methodList = (method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    methodList->setIsSorted();
}

void MethodList::setUsesOffsetsFromSelectorBuffer()
{
    if ( !methodListPos.has_value() )
        return;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    method_list_t* methodList = (method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    methodList->setUsesOffsetsFromSelectorBuffer();
}

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
const void* MethodList::getLocation() const
{
    if ( !this->methodListPos.has_value() )
        return nullptr;
    return this->methodListPos->value();
}

std::optional<VMAddress> MethodList::getVMAddress() const
{
    return this->methodListPos->vmAddress();
}
#endif

static Method::Kind getKind(const MethodList::method_list_t* methodList)
{
    typedef Method::Kind Kind;
    if ( methodList->usesRelativeOffsets() )
        return methodList->usesOffsetsFromSelectorBuffer() ? Kind::relativeDirect : Kind::relativeIndirect;
    else
        return Kind::pointer;
}

Method MethodList::getMethod(const Visitor& objcVisitor, uint32_t i) const
{
    assert(methodListPos.has_value());

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    const uint8_t* methodListBase = methodList->methodBase();
    const uint8_t* method = methodListBase + (i * methodList->getMethodSize());

    ResolvedValue methodValue = objcVisitor.getField(methodListValue, method);

    return Method(getKind(methodList), methodValue);
}

//
// MARK: --- Method methods ---
//

const void* Method::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const method32_t* method32 = (const method32_t*)this->methodPos.value();
        switch ( field ) {
            case Field::name:
                return &method32->nameVMAddr;
            case Field::types:
                return &method32->typesVMAddr;
            case Field::imp:
                return &method32->impVMAddr;
        }
    } else {
        const method64_t* method64 = (const method64_t*)this->methodPos.value();
        switch ( field ) {
            case Field::name:
                return &method64->nameVMAddr;
            case Field::types:
                return &method64->typesVMAddr;
            case Field::imp:
                return &method64->impVMAddr;
        }
    }
}

ResolvedValue Method::getNameField(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            assert(0);
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            return objcVisitor.getField(this->methodPos, this->getFieldPos(objcVisitor, Field::name));
        }
    }
}

ResolvedValue Method::getTypesField(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            assert(0);
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            return objcVisitor.getField(this->methodPos, this->getFieldPos(objcVisitor, Field::types));
        }
    }
}

ResolvedValue Method::getIMPField(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            assert(0);
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            return objcVisitor.getField(this->methodPos, this->getFieldPos(objcVisitor, Field::imp));
        }
    }
}

const char* Method::getName(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            // The uint32_t name field is an offset from itself to a selref.  The selref then points to the selector string
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress nameSelRefVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue nameSelRefValue = objcVisitor.getValueFor(nameSelRefVMAddr);
            return (const char*)objcVisitor.resolveRebase(nameSelRefValue).value();
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            ResolvedValue nameField = this->getNameField(objcVisitor);
            return (const char*)objcVisitor.resolveRebase(nameField).value();
        }
    }
}

const char* Method::getTypes(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->typesOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, typesOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress typeVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue typeValue = objcVisitor.getValueFor(typeVMAddr);
            return (const char*)typeValue.value();
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            ResolvedValue typesField = this->getTypesField(objcVisitor);
            return (const char*)objcVisitor.resolveRebase(typesField).value();
        }
    }
}

const void* Method::getIMP(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->impOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, impOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress impVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue impValue = objcVisitor.getValueFor(impVMAddr);
            return (const char*)impValue.value();
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            ResolvedValue impField = this->getIMPField(objcVisitor);
            return (const char*)objcVisitor.resolveRebase(impField).value();
        }
    }
}

// Get the selector string name.  A method often indirects via a selector reference.  This returns
// the vmAddr of the final selector string, not the selector reference.
VMAddress Method::getNameVMAddr(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            // The uint32_t name field is an offset from itself to a selref.  The selref then points to the selector string
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress nameSelRefVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue nameSelRefValue = objcVisitor.getValueFor(nameSelRefVMAddr);
            return objcVisitor.resolveRebase(nameSelRefValue).vmAddress();
        }
        case Kind::relativeDirect: {
#if BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_SHARED_CACHE_UTIL || BUILDING_UNIT_TESTS
            // dyld should never walk direct methods as the objc closure optimizations skip cache dylibs
            assert(0);
#else
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            uint32_t nameOffsetInBuffer = *(uint32_t*)fieldPos;

            return objcVisitor.sharedCacheSelectorStringsBaseAddress() + VMOffset((uint64_t)nameOffsetInBuffer);
#endif
        }
        case Kind::pointer: {
            ResolvedValue nameSelRefValue = this->getNameField(objcVisitor);
            return objcVisitor.resolveRebase(nameSelRefValue).vmAddress();
        }
    }
}

VMAddress Method::getTypesVMAddr(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect:
        case Kind::relativeDirect: {
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->typesOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, typesOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress typeVMAddr = methodVMAddr + relativeOffsetFromMethod;
            return typeVMAddr;
        }
        case Kind::pointer: {
            ResolvedValue typesRefValue = this->getTypesField(objcVisitor);
            return objcVisitor.resolveRebase(typesRefValue).vmAddress();
        }
    }
}

std::optional<VMAddress> Method::getIMPVMAddr(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect:
        case Kind::relativeDirect:  {
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->impOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, impOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress impVMAddr = methodVMAddr + relativeOffsetFromMethod;
            return impVMAddr;
        }
        case Kind::pointer: {
            ResolvedValue impRefValue = this->getIMPField(objcVisitor);
            return objcVisitor.resolveOptionalRebaseToVMAddress(impRefValue);
        }
    }
}

// Get the selector string name.  A method often indirects via a selector reference.  This returns
// the vmAddr of the selector reference, not the final selector string
VMAddress Method::getNameSelRefVMAddr(const Visitor& objcVisitor) const
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            // The uint32_t name field is an offset from itself to a selref.  The selref then points to the selector string
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            int32_t relativeOffsetFromField = *(int32_t*)fieldPos;
            VMOffset relativeOffsetFromMethod((uint64_t)__offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress nameSelRefVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue nameSelRefValue = objcVisitor.getValueFor(nameSelRefVMAddr);
            return nameSelRefValue.vmAddress();
        }
        case Kind::relativeDirect: {
            assert(0);
        }
        case Kind::pointer: {
            assert(0);
        }
    }
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Method::setName(const Visitor& objcVisitor, VMAddress nameVMAddr)
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            assert(0);
        }
        case Kind::relativeDirect: {
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;

            VMOffset nameOffsetInBuffer = nameVMAddr - objcVisitor.sharedCacheSelectorStringsBaseAddress();
            uint64_t relativeOffset = (uint64_t)nameOffsetInBuffer.rawValue();

            assert((uint32_t)relativeOffset == relativeOffset);
            *(uint32_t*)fieldPos = (uint32_t)relativeOffset;
            break;
        }
        case Kind::pointer: {
            ResolvedValue selRefValue = this->getNameField(objcVisitor);
            objcVisitor.updateTargetVMAddress(selRefValue, CacheVMAddress(nameVMAddr.rawValue()));
        }
    }
}

void Method::setTypes(const Visitor& objcVisitor, VMAddress typesVMAddr)
{
    switch ( this->kind ) {
        case Kind::relativeIndirect:
        case Kind::relativeDirect: {
            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress typesFieldVMAddr = methodVMAddr + VMOffset((uint64_t)__offsetof(relative_method_t, typesOffset));

            VMOffset typesRelativeOffset = typesVMAddr - typesFieldVMAddr;
            int64_t relativeOffset = (int64_t)typesRelativeOffset.rawValue();

            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->typesOffset;
            assert((int32_t)relativeOffset == relativeOffset);
            *(int32_t*)fieldPos = (int32_t)relativeOffset;
            break;
        }
        case Kind::pointer: {
            ResolvedValue refValue = this->getTypesField(objcVisitor);
            objcVisitor.updateTargetVMAddress(refValue, CacheVMAddress(typesVMAddr.rawValue()));
        }
    }
}

void Method::setIMP(const Visitor& objcVisitor, std::optional<VMAddress> impVMAddr)
{
    switch ( this->kind ) {
        case Kind::relativeIndirect:
        case Kind::relativeDirect: {
            // We don't support NULL imp's with relative method lists.
            assert(impVMAddr.has_value());

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress impFieldVMAddr = methodVMAddr + VMOffset((uint64_t)__offsetof(relative_method_t, impOffset));

            VMOffset impRelativeOffset = impVMAddr.value() - impFieldVMAddr;
            int64_t relativeOffset = (int64_t)impRelativeOffset.rawValue();

            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->impOffset;
            assert((int32_t)relativeOffset == relativeOffset);
            *(int32_t*)fieldPos = (int32_t)relativeOffset;
            break;
        }
        case Kind::pointer: {
            if ( !impVMAddr.has_value() ) {
                // A NULL imp is probably a protocol, and is expected.  Every other IMP in the
                // protocol is also going to be NULL, so just make sure this one matches
                assert(!this->getIMPVMAddr(objcVisitor).has_value());
            } else {
                ResolvedValue refValue = this->getIMPField(objcVisitor);
                objcVisitor.updateTargetVMAddress(refValue, CacheVMAddress(impVMAddr->rawValue()));
            }
        }
    }
}
#endif

void Method::convertNameToOffset(const Visitor& objcVisitor, uint32_t nameOffset)
{
    switch ( this->kind ) {
        case Kind::relativeIndirect: {
            // We are always looking at an indirect method when converting a name to an offset
            uint8_t* fieldPos = (uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            *(uint32_t*)fieldPos = (uint32_t)nameOffset;

            // FIXME: Should we convert the kind field on this method to relativeDirect?
            break;
        }
        case Kind::relativeDirect: {
            // This shouldn't happen
            assert(0);
        }
        case Kind::pointer: {
            // This shouldn't happen
            assert(0);
        }
    }
}

//
// MARK: --- IVarList methods ---
//

uint32_t IVarList::numIVars() const
{
    if ( !ivarListPos.has_value() )
        return 0;

    const ResolvedValue& ivarListValue = this->ivarListPos.value();

    const ivar_list_t* ivarList = (const ivar_list_t*)ivarListValue.value();
    assert(ivarList != nullptr);

    return ivarList->getCount();
}

IVar IVarList::getIVar(const Visitor& objcVisitor, uint32_t i) const
{
    assert(ivarListPos.has_value());

    const ResolvedValue& ivarListValue = this->ivarListPos.value();

    const ivar_list_t* ivarList = (const ivar_list_t*)ivarListValue.value();
    assert(ivarList != nullptr);

    const uint8_t* ivarListBase = ivarList->ivarBase();
    const uint8_t* ivar = ivarListBase + (i * ivarList->getElementSize());

    ResolvedValue ivarValue = objcVisitor.getField(ivarListValue, ivar);
    return IVar(ivarValue);
}

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
const void* IVarList::getLocation() const
{
    if ( !this->ivarListPos.has_value() )
        return nullptr;
    return this->ivarListPos->value();
}

std::optional<VMAddress> IVarList::getVMAddress() const
{
    return this->ivarListPos->vmAddress();
}
#endif

//
// MARK: --- ProtocolList methods ---
//

uint64_t ProtocolList::numProtocols(const Visitor& objcVisitor) const
{
    if ( !this->protocolListPos.has_value() )
        return 0;

    const ResolvedValue& protocolListValue = this->protocolListPos.value();
    const void* protocolList = protocolListValue.value();
    assert(protocolList != nullptr);

    if ( objcVisitor.pointerSize == 4 ) {
        return ((const protocol_list32_t*)protocolList)->count;
    } else {
        return ((const protocol_list64_t*)protocolList)->count;
    }
}

ResolvedValue ProtocolList::getProtocolField(const Visitor& objcVisitor, uint64_t i) const
{
    assert(this->protocolListPos.has_value());
    assert(i < this->numProtocols(objcVisitor));

    const ResolvedValue& protocolListValue = this->protocolListPos.value();
    const void* protocolList = protocolListValue.value();
    assert(protocolList != nullptr);

    const void* protocolFixupLoc = nullptr;
    if ( objcVisitor.pointerSize == 4 ) {
        protocolFixupLoc = &((const protocol_list32_t*)protocolList)->list[i];
    } else {
        protocolFixupLoc = &((const protocol_list64_t*)protocolList)->list[i];
    }

    return objcVisitor.getField(protocolListValue, protocolFixupLoc);
}

Protocol ProtocolList::getProtocol(const Visitor& objcVisitor, uint64_t i) const
{
    ResolvedValue field = this->getProtocolField(objcVisitor, i);
    ResolvedValue protocolValue = objcVisitor.resolveRebase(field);
    return Protocol(protocolValue);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void ProtocolList::setProtocol(const Visitor& objcVisitor, uint64_t i, VMAddress vmAddr)
{
    ResolvedValue field = this->getProtocolField(objcVisitor, i);
    objcVisitor.updateTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()));
}
#endif

#if BUILDING_CACHE_BUILDER_UNIT_TESTS
const void* ProtocolList::getLocation() const
{
    if ( !this->protocolListPos.has_value() )
        return nullptr;
    return this->protocolListPos->value();
}

std::optional<VMAddress> ProtocolList::getVMAddress() const
{
    return this->protocolListPos->vmAddress();
}
#endif

void ProtocolList::dump(const Visitor& objcVisitor) const
{
    if ( !this->protocolListPos.has_value() ) {
        fprintf(stdout, "no value\n");
        return;
    }

    const ResolvedValue& protocolListValue = this->protocolListPos.value();
    uint64_t count = this->numProtocols(objcVisitor);
    fprintf(stdout, "Protocol list (count %lld): vmAddr 0x%llx at %p\n",
            count, protocolListValue.vmAddress().rawValue(), protocolListValue.value());
    for ( uint64_t i = 0; i != count; ++i ) {
        Protocol objCProtocol = this->getProtocol(objcVisitor, i);
        fprintf(stdout, "  Protocol[%lld]: vmAddr 0x%llx at %p\n", i,
                objCProtocol.getVMAddress().rawValue(),
                objCProtocol.getLocation());
    }
}

//
// MARK: --- IVar methods ---
//

const void* IVar::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const ivar32_t* ivar32 = (const ivar32_t*)this->ivarPos.value();
        switch ( field ) {
            case Field::offset:
                return &ivar32->offsetVMAddr;
            case Field::name:
                return &ivar32->nameVMAddr;
            case Field::type:
                return &ivar32->typeVMAddr;
            case Field::alignment:
                return &ivar32->alignment;
            case Field::size:
                return &ivar32->size;
        }
    } else {
        const ivar64_t* ivar64 = (const ivar64_t*)this->ivarPos.value();
        switch ( field ) {
            case Field::offset:
                return &ivar64->offsetVMAddr;
            case Field::name:
                return &ivar64->nameVMAddr;
            case Field::type:
                return &ivar64->typeVMAddr;
            case Field::alignment:
                return &ivar64->alignment;
            case Field::size:
                return &ivar64->size;
        }
    }
}

std::optional<uint32_t> IVar::getOffset(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->ivarPos, this->getFieldPos(objcVisitor, Field::offset));

    // The offset might not be set, if it points to 0
    std::optional<ResolvedValue> targetValue = objcVisitor.resolveOptionalRebase(field);
    if ( targetValue.has_value() )
        return *(uint32_t*)targetValue->value();

    return std::nullopt;
}

void IVar::setOffset(const Visitor& objcVisitor, uint32_t offset) const
{
    ResolvedValue field = objcVisitor.getField(this->ivarPos, this->getFieldPos(objcVisitor, Field::offset));
    ResolvedValue targetValue = objcVisitor.resolveRebase(field);
    *(uint32_t*)targetValue.value() = offset;
}

const char* IVar::getName(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->ivarPos, this->getFieldPos(objcVisitor, Field::name));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

uint32_t IVar::getAlignment(const Visitor& objcVisitor) const
{
    return *(uint32_t*)this->getFieldPos(objcVisitor, Field::alignment);
}

bool IVar::elided(const Visitor& objcVisitor) const
{
    uint32_t size = *(uint32_t*)this->getFieldPos(objcVisitor, Field::size);
    // swift can optimize away ivars.  It leaves the meta data about them, but they have no ivar offset to update
    return (size == 0);
}

//
// MARK: --- Visitor::DataSection methods ---
//

std::optional<Visitor::DataSection> Visitor::findObjCDataSection(const char *sectionName) const
{
#if SUPPORT_VM_LAYOUT
    const dyld3::MachOFile* mf = this->dylibMA;
#else
    const dyld3::MachOFile* mf = this->dylibMF;
#endif

    __block std::optional<Visitor::DataSection> objcDataSection;
    mf->forEachSection(^(const dyld3::MachOFile::SectionInfo& sectInfo, bool malformedSectionRange, bool& stop) {
        if ( (strcmp(sectInfo.segInfo.segName, "__DATA") != 0) &&
             (strcmp(sectInfo.segInfo.segName, "__DATA_CONST") != 0) &&
             (strcmp(sectInfo.segInfo.segName, "__DATA_DIRTY") != 0) )
            return;
        if ( strcmp(sectInfo.sectName, sectionName) != 0 )
            return;

#if SUPPORT_VM_LAYOUT
        const void* targetValue = (const void*)(sectInfo.sectAddr + this->dylibMA->getSlide());
        ResolvedValue target(targetValue, VMAddress(sectInfo.sectAddr));
#else
        VMOffset offsetInSegment(sectInfo.sectAddr - sectInfo.segInfo.vmAddr);
        ResolvedValue target(this->segments[sectInfo.segInfo.segIndex], offsetInSegment);
#endif
        objcDataSection.emplace(std::move(target), sectInfo.sectSize);

        stop = true;
    });
    return objcDataSection;
}

//
// MARK: --- Visitor methods ---
//

void Visitor::forEachClass(bool visitMetaClasses, const Visitor::DataSection& classListSection,
                           void (^callback)(Class& objcClass, bool isMetaClass, bool& stopClass))
{
    assert((classListSection.sectSize % pointerSize) == 0);
    uint64_t numClasses = classListSection.sectSize / pointerSize;

    // Use the segment index to find the corresponding cache segment
    const ResolvedValue& sectionValue = classListSection.sectionBase;
    const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
    for ( uint64_t classIndex = 0; classIndex != numClasses; ++classIndex ) {
        const uint8_t* classRefPos = sectionBase + (classIndex * pointerSize);

        ResolvedValue classRefValue = this->getField(sectionValue, classRefPos);

        bool isPatchableClass = false;
        ResolvedValue classPos = this->resolveBindOrRebase(classRefValue, isPatchableClass);

        Class objcClass(classPos, false, isPatchableClass);
        bool stopClass = false;
        callback(objcClass, false, stopClass);
        if ( stopClass )
            return;

        // If we don't want the metaclass then skip to the next class
        if ( !visitMetaClasses)
            continue;

        bool isPatchableMetaClass = false;
        ResolvedValue objcClassISA = objcClass.getISA(*this, isPatchableMetaClass);
        Class objcMetaClass(objcClassISA, true, isPatchableMetaClass);
        callback(objcMetaClass, true, stopClass);
        if ( stopClass )
            return;
    }
}

void Visitor::forEachClass(bool visitMetaClasses, void (^callback)(Class& objcClass, bool isMetaClass, bool& stopClass))
{
    std::optional<DataSection> classListSection = this->findObjCDataSection("__objc_classlist");
    if ( !classListSection.has_value() )
        return;

    this->forEachClass(visitMetaClasses, classListSection.value(), callback);
}

void Visitor::forEachClass(void (^callback)(const Class& objcClass, bool& stopClass))
{
    auto adaptor = ^(Class& objcClass, bool isMetaClass, bool& stopClass) {
        callback(objcClass, stopClass);
    };
    forEachClass(false, adaptor);
}

void Visitor::forEachClass(void (^callback)(Class& objcClass, bool& stopClass))
{
    auto adaptor = ^(Class& objcClass, bool isMetaClass, bool& stopClass) {
        callback(objcClass, stopClass);
    };
    forEachClass(false, adaptor);
}

void Visitor::forEachClassAndMetaClass(void (^callback)(const Class& objcClass, bool& stopClass))
{
    auto adaptor = ^(Class& objcClass, bool isMetaClass, bool& stopClass) {
        callback(objcClass, stopClass);
    };
    forEachClass(true, adaptor);
}

void Visitor::forEachClassAndMetaClass(void (^callback)(Class& objcClass, bool& stopClass))
{
    auto adaptor = ^(Class& objcClass, bool isMetaClass, bool& stopClass) {
        callback(objcClass, stopClass);
    };
    forEachClass(true, adaptor);
}

void Visitor::forEachCategory(void (^callback)(const Category& objcCategory, bool& stopCategory))
{
    std::optional<DataSection> categoryListSection = findObjCDataSection("__objc_catlist");
    if ( !categoryListSection.has_value() )
        return;

    assert((categoryListSection->sectSize % pointerSize) == 0);
    uint64_t numCategories = categoryListSection->sectSize / pointerSize;

    const ResolvedValue& sectionValue = categoryListSection->sectionBase;
    const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
    for ( uint64_t categoryIndex = 0; categoryIndex != numCategories; ++categoryIndex ) {
        const uint8_t* categoryRefPos = sectionBase + (categoryIndex * pointerSize);
        ResolvedValue categoryRefValue = this->getField(sectionValue, categoryRefPos);

        // Follow the category reference to get to the actual category
        ResolvedValue categoryPos = resolveRebase(categoryRefValue);
        Category objcCategory(categoryPos);
        bool stopCategory = false;
        callback(objcCategory, stopCategory);
        if ( stopCategory )
            break;
    }
}

void Visitor::forEachProtocol(void (^callback)(const Protocol& objcProtocol, bool& stopProtocol))
{
    std::optional<DataSection> protocolListSection = findObjCDataSection("__objc_protolist");
    if ( !protocolListSection.has_value() )
        return;

    assert((protocolListSection->sectSize % pointerSize) == 0);
    uint64_t numCategories = protocolListSection->sectSize / pointerSize;

    const ResolvedValue& sectionValue = protocolListSection->sectionBase;
    const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
    for ( uint64_t protocolIndex = 0; protocolIndex != numCategories; ++protocolIndex ) {
        const uint8_t* protocolRefPos = sectionBase + (protocolIndex * pointerSize);
        ResolvedValue protocolRefValue = this->getField(sectionValue, protocolRefPos);

        // Follow the protocol reference to get to the actual protocol
        ResolvedValue protocolPos = resolveRebase(protocolRefValue);
        Protocol objcProtocol(protocolPos);
        bool stopProtocol = false;
        callback(objcProtocol, stopProtocol);
        if ( stopProtocol )
            break;
    }
}

void Visitor::forEachSelectorReference(void (^callback)(ResolvedValue& value)) const
{
    std::optional<DataSection> selRefsSection = findObjCDataSection("__objc_selrefs");
    if ( !selRefsSection.has_value() )
        return;

    assert((selRefsSection->sectSize % pointerSize) == 0);
    uint64_t numSelRefs = selRefsSection->sectSize / pointerSize;

    const ResolvedValue& sectionValue = selRefsSection->sectionBase;
    const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
    for ( uint64_t selRefIndex = 0; selRefIndex != numSelRefs; ++selRefIndex ) {
        const uint8_t* selRefPos = sectionBase + (selRefIndex * pointerSize);
        ResolvedValue selRefValue = this->getField(sectionValue, selRefPos);

        callback(selRefValue);
    }
}

void Visitor::forEachSelectorReference(void (^callback)(VMAddress selRefVMAddr, VMAddress selRefTargetVMAddr,
                                                            const char* selectorString)) const
{
    this->forEachSelectorReference(^(ResolvedValue& selRefValue) {
        ResolvedValue selRefTarget = this->resolveRebase(selRefValue);

        VMAddress selRefVMAddr = selRefValue.vmAddress();
        VMAddress selRefTargetVMAddr = selRefTarget.vmAddress();
        const char* selectorString = (const char*)selRefTarget.value();

        callback(selRefVMAddr, selRefTargetVMAddr, selectorString);
    });
}

void Visitor::forEachProtocolReference(void (^callback)(ResolvedValue& value))
{
    std::optional<DataSection> protocolRefsSection = findObjCDataSection("__objc_protorefs");
    if ( !protocolRefsSection.has_value() )
        return;

    assert((protocolRefsSection->sectSize % pointerSize) == 0);
    uint64_t numProtocolRefs = protocolRefsSection->sectSize / pointerSize;

    const ResolvedValue& sectionValue = protocolRefsSection->sectionBase;
    const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
    for ( uint64_t protocolRefIndex = 0; protocolRefIndex != numProtocolRefs; ++protocolRefIndex ) {
        const uint8_t* protocolRefPos = sectionBase + (protocolRefIndex * pointerSize);
        ResolvedValue protocolRefValue = this->getField(sectionValue, protocolRefPos);

        callback(protocolRefValue);
    }
}

