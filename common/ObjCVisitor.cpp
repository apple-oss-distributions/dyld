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

#include "ObjCVisitor.h"

#if SUPPORT_VM_LAYOUT
#include "MachOAnalyzer.h"
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
#include "ASLRTracker.h"
#include <unordered_set>
#endif

using namespace objc_visitor;
using ResolvedValue = metadata_visitor::ResolvedValue;
using mach_o::Header;

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
void Class::setMethodCachePropertiesVMAddr(const Visitor& objcVisitor, VMAddress vmAddr,
                                           const dyld3::MachOFile::PointerMetaData& PMD)
{
    ResolvedValue field = objcVisitor.getField(this->classPos, this->getFieldPos(objcVisitor, Field::methodCacheProperties));
    objcVisitor.setTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()), PMD);
}
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL
void Class::withSuperclass(const Visitor& objcVisitor,
                           void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const
{
    // HACK: The visitor classes need to be refactored to handle cache util. For now just force the caller of this method in
    // cache util to have the chain format
#if BUILDING_SHARED_CACHE_UTIL
    uint16_t chainedPointerFormat = 0;
#else
    uint16_t chainedPointerFormat = this->classPos.chainedPointerFormat().value();
#endif

    const void* fieldPos = this->getFieldPos(objcVisitor, Field::superclass);
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

VMAddress Class::getClassDataVMAddr(const Visitor& objcVisitor) const
{
    return getClassData(objcVisitor).getVMAddress();
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

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
ResolvedValue Class::setBaseMethodsVMAddr(const Visitor& objcVisitor, VMAddress vmAddr,
                                 const dyld3::MachOFile::PointerMetaData& PMD)
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseMethods);;
    objcVisitor.setTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()), PMD);
    return field;
}
#endif

ProtocolList Class::getBaseProtocols(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseProtocols);
    return objcVisitor.resolveOptionalRebase(field);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
ResolvedValue Class::setBaseProtocolsVMAddr(const Visitor& objcVisitor, VMAddress vmAddr)
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseProtocols);
    objcVisitor.updateTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()));
    return field;
}
#endif

IVarList Class::getIVars(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::ivars);
    return objcVisitor.resolveOptionalRebase(field);
}

PropertyList Class::getBaseProperties(const Visitor& objcVisitor) const
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseProperties);
    return objcVisitor.resolveOptionalRebase(field);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
ResolvedValue Class::setBasePropertiesVMAddr(const Visitor& objcVisitor, VMAddress vmAddr)
{
    ClassData classData = getClassData(objcVisitor);
    ResolvedValue field = classData.getField(objcVisitor, ClassData::Field::baseProperties);
    objcVisitor.updateTargetVMAddress(field, CacheVMAddress(vmAddr.rawValue()));
    return field;
}
#endif

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
            case Field::classProperties:
                return &category32->classPropertiesVMAddr;
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
            case Field::classProperties:
                return &category64->classPropertiesVMAddr;
        }
    }
}

const char* Category::getName(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::name));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

VMAddress Category::getVMAddress() const
{
    return this->categoryPos.vmAddress();
}

const void* Category::getLocation() const
{
    return this->categoryPos.value();
}

VMAddress Category::getNameVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::name));
    return objcVisitor.resolveRebase(field).vmAddress();
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

PropertyList Category::getInstanceProperties(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::instanceProperties));
    return objcVisitor.resolveOptionalRebase(field);
}

PropertyList Category::getClassProperties(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::classProperties));
    return objcVisitor.resolveOptionalRebase(field);
}

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS || BUILDING_SHARED_CACHE_UTIL
void Category::withClass(const Visitor& objcVisitor,
                         void (^handler)(const dyld3::MachOFile::ChainedFixupPointerOnDisk* fixup, uint16_t pointerFormat)) const
{
    // HACK: The visitor classes need to be refactored to handle cache util. For now just force the caller of this method in
    // cache util to have the chain format
#if BUILDING_SHARED_CACHE_UTIL
    uint16_t chainedPointerFormat = 0;
#else
    assert(objcVisitor.isOnDiskBinary());
    uint16_t chainedPointerFormat = this->categoryPos.chainedPointerFormat().value();
#endif

    const void* fieldPos = nullptr;
    if ( objcVisitor.pointerSize == 8 ) {
        fieldPos = &((const category64_t*)this->categoryPos.value())->clsVMAddr;
    } else if (objcVisitor.pointerSize == 4 ){
        fieldPos = &((const category32_t*)this->categoryPos.value())->clsVMAddr;
    }
    assert(fieldPos != nullptr);

    dyld3::MachOFile::ChainedFixupPointerOnDisk* fieldFixup = (dyld3::MachOFile::ChainedFixupPointerOnDisk*)fieldPos;
    handler(fieldFixup, chainedPointerFormat);
}
#endif

uint32_t Category::getSize(bool is64)
{
    return is64 ? sizeof(category64_t) : sizeof(category32_t);
}


#if BUILDING_SHARED_CACHE_UTIL
std::optional<VMAddress> Category::getClassVMAddr(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->categoryPos, this->getFieldPos(objcVisitor, Field::cls));
    std::optional<ResolvedValue> targetValue = objcVisitor.resolveOptionalRebase(field);
    if ( targetValue )
        return targetValue->vmAddress();

    return { };
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

uint32_t MethodList::listSize() const
{
    if ( !methodListPos.has_value() )
        return 0;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    uint32_t size = sizeof(uint32_t) * 2;
    size += methodList->getMethodCount() * methodList->getMethodSize();
    return size;
}

uint32_t MethodList::methodSize() const
{
    if ( !methodListPos.has_value() )
        return 0;

    const ResolvedValue& methodListValue = this->methodListPos.value();

    const method_list_t* methodList = (const method_list_t*)methodListValue.value();
    assert(methodList != nullptr);

    return methodList->getMethodSize();
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

size_t MethodList::makeEmptyMethodList(void* buffer)
{
    assert(buffer != nullptr);
    method_list_t* methodList = (method_list_t*)buffer;
    bzero(methodList, sizeof(method_list_t));

    methodList->setIsUniqued();
    methodList->setIsSorted();

    return sizeof(method_list_t);
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

bool MethodList::isListOfLists() const
{
    if ( !methodListPos.has_value() )
        return false;

    const ResolvedValue& methodListValue = this->methodListPos.value();
    return methodListValue.vmAddress().rawValue() & 1;
}

const void* MethodList::getLocation() const
{
    if ( !this->methodListPos.has_value() )
        return nullptr;
    return this->methodListPos->value();
}

std::optional<VMAddress> MethodList::getVMAddress() const
{
    if ( !this->methodListPos.has_value() )
        return { };
    return this->methodListPos->vmAddress();
}

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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress nameSelRefVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue nameSelRefValue = objcVisitor.getValueFor(nameSelRefVMAddr);
            return (const char*)objcVisitor.resolveRebase(nameSelRefValue).value();
        }
        case Kind::relativeDirect: {
#if BUILDING_SHARED_CACHE_UTIL
            const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->nameOffset;
            uint32_t nameOffsetInBuffer = *(uint32_t*)fieldPos;

            VMAddress nameVMAddr = objcVisitor.sharedCacheSelectorStringsBaseAddress() + VMOffset((uint64_t)nameOffsetInBuffer);
            ResolvedValue nameValue = objcVisitor.getValueFor(nameVMAddr);
            return (const char*)nameValue.value();
#else
            // dyld should never walk direct methods as the objc closure optimizations skip cache dylibs
            assert(0);
#endif
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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, typesOffset) + relativeOffsetFromField);

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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, impOffset) + relativeOffsetFromField);

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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

            VMAddress methodVMAddr = this->methodPos.vmAddress();
            VMAddress nameSelRefVMAddr = methodVMAddr + relativeOffsetFromMethod;

            ResolvedValue nameSelRefValue = objcVisitor.getValueFor(nameSelRefVMAddr);
            return objcVisitor.resolveRebase(nameSelRefValue).vmAddress();
        }
        case Kind::relativeDirect: {
#if BUILDING_DYLD || BUILDING_CLOSURE_UTIL || BUILDING_UNIT_TESTS
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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, typesOffset) + relativeOffsetFromField);

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

            // protocols have null impls
            if ( relativeOffsetFromField == 0 )
                return std::nullopt;

            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, impOffset) + relativeOffsetFromField);

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
            VMOffset relativeOffsetFromMethod((uint64_t)offsetof(relative_method_t, nameOffset) + relativeOffsetFromField);

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
            VMAddress typesFieldVMAddr = methodVMAddr + VMOffset((uint64_t)offsetof(relative_method_t, typesOffset));

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
            if ( !impVMAddr.has_value() ) {
                // A NULL imp is probably a protocol, and is expected.  Every other IMP in the
                // protocol is also going to be NULL, so just make sure this one matches
                assert(!this->getIMPVMAddr(objcVisitor).has_value());
            } else {
                VMAddress methodVMAddr = this->methodPos.vmAddress();
                VMAddress impFieldVMAddr = methodVMAddr + VMOffset((uint64_t)offsetof(relative_method_t, impOffset));

                VMOffset impRelativeOffset = impVMAddr.value() - impFieldVMAddr;
                int64_t relativeOffset = (int64_t)impRelativeOffset.rawValue();

                const uint8_t* fieldPos = (const uint8_t*)&((const relative_method_t*)this->methodPos.value())->impOffset;
                assert((int32_t)relativeOffset == relativeOffset);
                *(int32_t*)fieldPos = (int32_t)relativeOffset;
            }
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

uint32_t Method::getSize(bool is64)
{
    return is64 ? sizeof(method64_t) : sizeof(method32_t);
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
// MARK: --- PropertyList methods ---
//
uint32_t PropertyList::numProperties() const
{
    if ( !this->propertyListPos.has_value() )
        return 0;

    const ResolvedValue& propertyListValue = this->propertyListPos.value();

    const property_list_t* propertyList = (const property_list_t*)propertyListValue.value();
    assert(propertyList != nullptr);

    return propertyList->getCount();
}

Property PropertyList::getProperty(const Visitor& objcVisitor, uint32_t i) const
{
    assert(this->propertyListPos.has_value());

    const ResolvedValue& propertyListValue = this->propertyListPos.value();

    const property_list_t* propertyList = (const property_list_t*)propertyListValue.value();
    assert(propertyList != nullptr);

    const uint8_t* propertyListBase = propertyList->propertyBase();
    const uint8_t* property = propertyListBase + (i * propertyList->getElementSize());

    ResolvedValue propertyValue = objcVisitor.getField(propertyListValue, property);
    return Property(propertyValue);
}

const void* PropertyList::getLocation() const
{
    if ( !this->propertyListPos.has_value() )
        return nullptr;
    return this->propertyListPos->value();
}

std::optional<VMAddress> PropertyList::getVMAddress() const
{
    if ( !this->propertyListPos.has_value() )
        return { };
    return this->propertyListPos->vmAddress();
}

bool PropertyList::isListOfLists() const
{
    if ( !propertyListPos.has_value() )
        return false;

    const ResolvedValue& propertyListValue = this->propertyListPos.value();
    return propertyListValue.vmAddress().rawValue() & 1;
}

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

const void* ProtocolList::getLocation() const
{
    if ( !this->protocolListPos.has_value() )
        return nullptr;
    return this->protocolListPos->value();
}

std::optional<VMAddress> ProtocolList::getVMAddress() const
{
    if ( !this->protocolListPos.has_value() )
        return { };
    return this->protocolListPos->vmAddress();
}

bool ProtocolList::isListOfLists() const
{
    if ( !protocolListPos.has_value() )
        return false;

    const ResolvedValue& protocolListValue = this->protocolListPos.value();
    return protocolListValue.vmAddress().rawValue() & 1;
}


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
// MARK: --- Property methods ---
//

const void* Property::getFieldPos(const Visitor& objcVisitor, Field field) const
{
    if ( objcVisitor.pointerSize == 4 ) {
        const property32_t* property32 = (const property32_t*)this->propertyPos.value();
        switch ( field ) {
            case Field::name:
                return &property32->nameVMAddr;
            case Field::attributes:
                return &property32->attributesVMAddr;
        }
    } else {
        const property64_t* property64 = (const property64_t*)this->propertyPos.value();
        switch ( field ) {
            case Field::name:
                return &property64->nameVMAddr;
            case Field::attributes:
                return &property64->attributesVMAddr;
        }
    }
}

const char* Property::getName(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->propertyPos, this->getFieldPos(objcVisitor, Field::name));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

const char* Property::getAttributes(const Visitor& objcVisitor) const
{
    ResolvedValue field = objcVisitor.getField(this->propertyPos, this->getFieldPos(objcVisitor, Field::attributes));
    return (const char*)objcVisitor.resolveRebase(field).value();
}

//
// MARK: --- Visitor::Section methods ---
//

std::optional<Visitor::Section> Visitor::findSection(std::span<const char*> altSegNames, const char *sectionName) const
{
#if SUPPORT_VM_LAYOUT
    const dyld3::MachOFile* mf = this->dylibMA;
#else
    const dyld3::MachOFile* mf = this->dylibMF;
#endif

    __block std::optional<Visitor::Section> objcDataSection;
    ((const Header*)mf)->forEachSection(^(const Header::SegmentInfo& segInfo, const Header::SectionInfo& sectInfo, bool& stop) {
        bool segMatch = std::any_of(altSegNames.begin(), altSegNames.end(), [&sectInfo](const char* segName) {
            return sectInfo.segmentName == segName;
        });
        if ( !segMatch )
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
        objcDataSection.emplace(std::move(target), sectInfo.size);

        stop = true;
    });
    return objcDataSection;
}

std::optional<Visitor::Section> Visitor::findObjCDataSection(const char *sectionName) const
{
    static const char* objcDataSegments[] = {
        "__DATA", "__DATA_CONST", "__DATA_DIRTY"
    };
    return findSection(objcDataSegments, sectionName);
}

std::optional<Visitor::Section> Visitor::findObjCTextSection(const char *sectionName) const
{
    static const char* objcTextSegments[] = {
        "__TEXT"
    };
    return findSection(objcTextSegments, sectionName);
}

//
// MARK: --- Visitor methods ---
//

void Visitor::forEachClass(bool visitMetaClasses, const Visitor::Section& classListSection,
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
    std::optional<Section> classListSection = this->findObjCDataSection("__objc_classlist");
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
    for ( bool isCatlist2 : { false, true }) {
        const char* listSection = isCatlist2 ? "__objc_catlist2" : "__objc_catlist";
        std::optional<Section> categoryListSection = findObjCDataSection(listSection);
        if ( !categoryListSection.has_value() )
            continue;

        assert((categoryListSection->sectSize % pointerSize) == 0);
        uint64_t numCategories = categoryListSection->sectSize / pointerSize;

        const ResolvedValue& sectionValue = categoryListSection->sectionBase;
        const uint8_t* sectionBase = (const uint8_t*)sectionValue.value();
        for ( uint64_t categoryIndex = 0; categoryIndex != numCategories; ++categoryIndex ) {
            const uint8_t* categoryRefPos = sectionBase + (categoryIndex * pointerSize);
            ResolvedValue categoryRefValue = this->getField(sectionValue, categoryRefPos);

            // Follow the category reference to get to the actual category
            ResolvedValue categoryPos = resolveRebase(categoryRefValue);
            Category objcCategory(categoryPos, isCatlist2);
            bool stopCategory = false;
            callback(objcCategory, stopCategory);
            if ( stopCategory )
                break;
        }
    }
}

void Visitor::forEachProtocol(void (^callback)(const Protocol& objcProtocol, bool& stopProtocol))
{
    std::optional<Section> protocolListSection = findObjCDataSection("__objc_protolist");
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
    std::optional<Section> selRefsSection = findObjCDataSection("__objc_selrefs");
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
    std::optional<Section> protocolRefsSection = findObjCDataSection("__objc_protorefs");
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

#if BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Visitor::forEachMethodList(void (^callback)(MethodList& objcMethodList, std::optional<metadata_visitor::ResolvedValue> extendedMethodTypes))
{
    __block std::unordered_set<const void*> visitedLists;

    forEachClassAndMetaClass(^(const objc_visitor::Class& objcClass, bool&) {
        objc_visitor::MethodList objcMethodList = objcClass.getBaseMethods(*this);

        callback(objcMethodList, std::nullopt);
        visitedLists.insert(objcMethodList.getLocation());
    });

    forEachCategory(^(const objc_visitor::Category& objcCategory, bool&) {
        objc_visitor::MethodList instanceMethodList = objcCategory.getInstanceMethods(*this);
        objc_visitor::MethodList classMethodList    = objcCategory.getClassMethods(*this);

        callback(instanceMethodList, std::nullopt);
        visitedLists.insert(instanceMethodList.getLocation());

        callback(classMethodList, std::nullopt);
        visitedLists.insert(classMethodList.getLocation());
    });

    forEachProtocol(^(const objc_visitor::Protocol& objcProtocol, bool&) {
        objc_visitor::MethodList instanceMethodList         = objcProtocol.getInstanceMethods(*this);
        objc_visitor::MethodList classMethodList            = objcProtocol.getClassMethods(*this);
        objc_visitor::MethodList optionalInstanceMethodList = objcProtocol.getOptionalInstanceMethods(*this);
        objc_visitor::MethodList optionalClassMethodList    = objcProtocol.getOptionalClassMethods(*this);

        // This is an optional flat array with entries for all method lists.
        // Each method list of length N has N char* entries in this list, if its present
        std::optional<metadata_visitor::ResolvedValue> extendedMethodTypes = objcProtocol.getExtendedMethodTypes(*this);
        const uint8_t* currentMethodTypes = extendedMethodTypes.has_value() ? (const uint8_t*)extendedMethodTypes->value() : nullptr;

        callback(instanceMethodList, extendedMethodTypes);
        visitedLists.insert(instanceMethodList.getLocation());
        if ( extendedMethodTypes.has_value() ) {
            currentMethodTypes += (instanceMethodList.numMethods() * pointerSize);
            extendedMethodTypes.emplace(metadata_visitor::ResolvedValue(extendedMethodTypes.value(), currentMethodTypes));
        }

        callback(classMethodList, extendedMethodTypes);
        visitedLists.insert(classMethodList.getLocation());
        if ( extendedMethodTypes.has_value() ) {
            currentMethodTypes += (classMethodList.numMethods() * pointerSize);
            extendedMethodTypes.emplace(metadata_visitor::ResolvedValue(extendedMethodTypes.value(), currentMethodTypes));
        }

        callback(optionalInstanceMethodList, extendedMethodTypes);
        visitedLists.insert(optionalInstanceMethodList.getLocation());
        if ( extendedMethodTypes.has_value() ) {
            currentMethodTypes += (optionalInstanceMethodList.numMethods() * pointerSize);
            extendedMethodTypes.emplace(metadata_visitor::ResolvedValue(extendedMethodTypes.value(), currentMethodTypes));
        }

        callback(optionalClassMethodList, extendedMethodTypes);
        visitedLists.insert(optionalClassMethodList.getLocation());
        if ( extendedMethodTypes.has_value() ) {
            currentMethodTypes += (optionalClassMethodList.numMethods() * pointerSize);
            extendedMethodTypes.emplace(metadata_visitor::ResolvedValue(extendedMethodTypes.value(), currentMethodTypes));
        }
    });

    // rdar://129304028 (dyld cache builder support for relative method lists in Swift generic classes)
    // Also scan the entire __objc_methlist section looking for other method lists that
    // aren't referenced through the regular ObjC metadata.
    std::optional<Section> methodListSection = findObjCTextSection("__objc_methlist");
    if ( !methodListSection.has_value() )
        return;
    assert((methodListSection->sectSize % 4) == 0);

    const ResolvedValue& sectionValue = methodListSection->sectionBase;
    const uint8_t* sectionPos = (const uint8_t*)sectionValue.value();
    const uint8_t* sectionEnd = (const uint8_t*)sectionValue.value() + methodListSection->sectSize;

    while ( sectionPos < sectionEnd ) {
        ResolvedValue methodListValue = this->getField(sectionValue, sectionPos);

        // method lists are 8-byte alligned, a valid method list can never start
        // with a 0 because that's where the method size entry and flags are encoded
        if ( *(uint32_t*)methodListValue.value() == 0 ) {
            sectionPos += sizeof(uint32_t);
            continue;
        }

        MethodList methodList(methodListValue);

        // sanity check entry - all lists in __objc_methlist are relative and
        // a relative method list entry is 12 bytes large
        assert(methodList.usesRelativeOffsets() && methodList.methodSize() == 12
                && "not a relative method list");

        // skip method lists that were visited through classes etc.
        if ( !visitedLists.contains(methodList.getLocation()) ) {
            callback(methodList, std::nullopt);
        }

        uint32_t size = methodList.listSize();
        assert(size != 0 && "method list can't be empty");
        sectionPos += size;
    }
    assert(sectionPos == sectionEnd && "malformed __objc_methlist section");
}
#endif

void Visitor::withImageInfo(void (^callback)(const uint32_t version, const uint32_t flags)) const
{
    std::optional<Section> imageInfoSection = findObjCDataSection("__objc_imageinfo");
    if ( !imageInfoSection.has_value() )
        return;

    assert((imageInfoSection->sectSize % pointerSize) == 0);
    const ResolvedValue& sectionValue = imageInfoSection->sectionBase;

    struct objc_image_info {
        int32_t version;
        uint32_t flags;
    };
    const objc_image_info* sectionBase = (const objc_image_info*)sectionValue.value();
    callback(sectionBase->version, sectionBase->flags);
}

#endif // !TARGET_OS_EXCLAVEKIT
