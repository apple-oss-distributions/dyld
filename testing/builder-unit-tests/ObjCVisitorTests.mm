/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

#import <XCTest/XCTest.h>

#include "ASLRTracker.h"
#include "ObjCVisitor.h"

using metadata_visitor::ResolvedValue;
using metadata_visitor::Segment;
using namespace objc_visitor;

typedef objc_visitor::Visitor::DataSection DataSection;
typedef objc_visitor::Class::class32_t class32_t;
typedef objc_visitor::Class::class64_t class64_t;
typedef objc_visitor::ClassData::data32_t data32_t;
typedef objc_visitor::ClassData::data64_t data64_t;
typedef objc_visitor::Class::FastDataBits FastDataBits;
typedef objc_visitor::Protocol::protocol32_t protocol32_t;
typedef objc_visitor::Protocol::protocol64_t protocol64_t;

@interface ObjCVisitorTests : XCTestCase

@end

@implementation ObjCVisitorTests
{
@public
    dyld3::MachOFile MachO_x86_64;
    dyld3::MachOFile MachO_x86_64h;
    dyld3::MachOFile MachO_arm64;
    dyld3::MachOFile MachO_arm64e;
    dyld3::MachOFile MachO_arm64_32;

    // Parameters Visitor
    CacheVMAddress              cacheBaseAddress32;
    CacheVMAddress              cacheBaseAddress64;
    std::optional<VMAddress>    selectorStringsBaseAddress;

    // To make it easier to mock segments, keep track o what VMAddress we've reached so far
    VMAddress                   maxVMAddress32;
    VMAddress                   maxVMAddress64;
}

// Note minAlignment here is the alignment in bytes, not a shifted value.  Eg, 0x4000 for 16k alignment, not 14
static inline uint64_t alignTo(uint64_t value, uint64_t minAlignment)
{
    return (value + (minAlignment - 1)) & (-minAlignment);
}

static inline VMAddress alignTo(VMAddress value, uint64_t minAlignment)
{
    return VMAddress(alignTo(value.rawValue(), minAlignment));
}

// Add N values to a new segment, and returns an array of the ResolvedValue's for those new values
static std::vector<ResolvedValue> addValues(ObjCVisitorTests* state, Visitor& visitor,
                                            uint64_t numValues, uint32_t valueSize)
{
    assert(numValues > 0);

    uint64_t size = numValues * valueSize;

    VMAddress& maxVMAddress = (visitor.pointerSize == 4) ? state->maxVMAddress32 : state->maxVMAddress64;

    VMAddress startVMAddr = maxVMAddress;
    VMAddress endVMAddr = maxVMAddress + CacheVMSize(size);

    Segment segment;
    segment.startVMAddr                     = startVMAddr;
    segment.endVMAddr                       = endVMAddr;
    segment.bufferStart                     = (uint8_t*)calloc(1, size);
    segment.segIndex                        = (uint32_t)visitor.segments.size();
    segment.onDiskDylibChainedPointerFormat = std::nullopt;

    // HACK: If this is the first segment we're adding, then reserve space for a lot more, as
    // ResolvedValue actually points in to this array, so iterator invalidation is a problem
    if ( visitor.segments.empty() )
        visitor.segments.reserve(200);

    visitor.segments.push_back(std::move(segment));

    std::vector<ResolvedValue> values;
    for ( uint64_t i = 0; i != numValues; ++i ) {
        VMAddress vmAddr = startVMAddr + CacheVMSize(i * valueSize);
        values.push_back(visitor.getValueFor(vmAddr));
    }

    // Bump max so that the next value we add will start at a higher address
    maxVMAddress = alignTo(endVMAddr, 16);

    return values;
}

template<typename T>
static std::vector<ResolvedValue> addValues(ObjCVisitorTests* state, Visitor& visitor,
                                            uint64_t numValues)
{
    return addValues(state, visitor, numValues, sizeof(T));
}

// Add N pointers to a new segment, and returns an array of the ResolvedValue's for those new pointers
static std::vector<ResolvedValue> addPointerSegment(ObjCVisitorTests* state, Visitor& visitor,
                                                    uint64_t numPointers)
{
    return addValues(state, visitor, numPointers, visitor.pointerSize);
}

// Sets a location to a 32-bit vmAddress encoded like a shared cache builder internal value
static void setCache32(ObjCVisitorTests* state, ResolvedValue loc, VMAddress vmAddr)
{
    cache_builder::Fixup::Cache32::setLocation(state->cacheBaseAddress32, loc.value(),
                                               CacheVMAddress(vmAddr.rawValue()));
}

// Sets a location to a 64-bit vmAddress encoded like a shared cache builder internal value
static void setCache64(ObjCVisitorTests* state, ResolvedValue loc, VMAddress vmAddr)
{
    dyld3::MachOFile::PointerMetaData pmd;
    cache_builder::Fixup::Cache64::setLocation(state->cacheBaseAddress64, loc.value(),
                                               CacheVMAddress(vmAddr.rawValue()),
                                               pmd.high8, pmd.diversity, pmd.usesAddrDiversity,
                                               pmd.key, pmd.authenticated);
}

typedef __typeof(&setCache32) SetCacheType;

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
    MachO_x86_64.magic      = MH_MAGIC_64;
    MachO_x86_64.cputype    = CPU_TYPE_X86_64;
    MachO_x86_64.cpusubtype = CPU_SUBTYPE_X86_ALL;
    MachO_x86_64.filetype   = MH_EXECUTE;
    MachO_x86_64.ncmds      = 0;
    MachO_x86_64.sizeofcmds = 0;
    MachO_x86_64.flags      = 0;

    MachO_x86_64h.magic      = MH_MAGIC_64;
    MachO_x86_64h.cputype    = CPU_TYPE_X86_64;
    MachO_x86_64h.cpusubtype = CPU_SUBTYPE_X86_64_H;
    MachO_x86_64h.filetype   = MH_EXECUTE;
    MachO_x86_64h.ncmds      = 0;
    MachO_x86_64h.sizeofcmds = 0;
    MachO_x86_64h.flags      = 0;

    MachO_arm64.magic      = MH_MAGIC_64;
    MachO_arm64.cputype    = CPU_TYPE_ARM64;
    MachO_arm64.cpusubtype = CPU_SUBTYPE_ARM64_ALL;
    MachO_arm64.filetype   = MH_EXECUTE;
    MachO_arm64.ncmds      = 0;
    MachO_arm64.sizeofcmds = 0;
    MachO_arm64.flags      = 0;

    MachO_arm64e.magic      = MH_MAGIC_64;
    MachO_arm64e.cputype    = CPU_TYPE_ARM64;
    MachO_arm64e.cpusubtype = CPU_SUBTYPE_ARM64E;
    MachO_arm64e.filetype   = MH_EXECUTE;
    MachO_arm64e.ncmds      = 0;
    MachO_arm64e.sizeofcmds = 0;
    MachO_arm64e.flags      = 0;

    MachO_arm64_32.magic      = MH_MAGIC;
    MachO_arm64_32.cputype    = CPU_TYPE_ARM64_32;
    MachO_arm64_32.cpusubtype = CPU_SUBTYPE_ARM64_V8;
    MachO_arm64_32.filetype   = MH_EXECUTE;
    MachO_arm64_32.ncmds      = 0;
    MachO_arm64_32.sizeofcmds = 0;
    MachO_arm64_32.flags      = 0;

    cacheBaseAddress32            = CacheVMAddress(0x1A000000ULL);
    cacheBaseAddress64            = CacheVMAddress(0x100000000ULL);
    selectorStringsBaseAddress  = std::nullopt;

    // Start the objc info somewhere in to the cache, but at a non-zero offset from the atart
    // of the cache.  The objc parsers assume pointing to the cache header is a null value, not
    // a relative rebase with an offset of 0
    maxVMAddress32 = VMAddress(cacheBaseAddress32.rawValue() + 0x4000ULL);
    maxVMAddress64 = VMAddress(cacheBaseAddress64.rawValue() + 0x4000ULL);
}

- (void)tearDown {
}

//
// MARK: --- Class methods ---
//

template<typename ClassType, typename PointerType>
static void testForEachClass(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                             SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add a class
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 1);

    // Add a classlist, and set it to point to the class
    std::vector<ResolvedValue> classListValues = addPointerSegment(state, visitor, 1);
    setCache(state, classListValues[0], classValues[0].vmAddress());
    DataSection classListSection(classListValues.front(), classListValues.size() * sizeof(PointerType));

    // Act:
    bool visitMetaClasses = false;
    __block std::vector<ResolvedValue> foundClasses;
    visitor.forEachClass(visitMetaClasses, classListSection,
                         ^(objc_visitor::Class& objcClass, bool isMetaClass, bool& stopClass) {
        foundClasses.push_back(objcClass.classPos);
    });

    // Assert:
    XCTAssertTrue(foundClasses.size() == 1);
    XCTAssertTrue(foundClasses[0].value() == classValues[0].value());
    XCTAssertTrue(foundClasses[0].vmAddress() == classValues[0].vmAddress());
}

// Walk classes but not metaclasses
- (void)testForEachClass
{
    testForEachClass<class32_t, uint32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testForEachClass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testForEachClass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testForEachClass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testForEachClass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

template<typename ClassType, typename PointerType>
static void testForEachClassAndMetaclass(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                                         SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 2 classes and 2 metaclasses
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 4);
    ResolvedValue class1 = classValues[0];
    ResolvedValue metaclass1 = classValues[1];
    ResolvedValue class2 = classValues[2];
    ResolvedValue metaclass2 = classValues[3];

    // Set the classes to point to their metaclasses
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->isaVMAddr),
             metaclass1.vmAddress());
    setCache(state, visitor.getField(class2, &((ClassType*)class2.value())->isaVMAddr),
             metaclass2.vmAddress());

    // Add a classlist, and set it to point to the class
    std::vector<ResolvedValue> classListValues = addPointerSegment(state, visitor, 2);
    setCache(state, classListValues[0], class1.vmAddress());
    setCache(state, classListValues[1], class2.vmAddress());
    DataSection classListSection(classListValues.front(), classListValues.size() * sizeof(PointerType));

    // Act:
    bool visitMetaClasses = true;
    __block std::vector<objc_visitor::Class> foundClasses;
    visitor.forEachClass(visitMetaClasses, classListSection,
                         ^(objc_visitor::Class& objcClass, bool isMetaClass, bool& stopClass) {
        foundClasses.push_back(objcClass);
    });

    // Assert:
    XCTAssertTrue(foundClasses.size() == 4);
    XCTAssertTrue(foundClasses[0].getLocation() == class1.value());
    XCTAssertTrue(foundClasses[0].getVMAddress() == class1.vmAddress());
    XCTAssertFalse(foundClasses[0].isMetaClass);
    XCTAssertTrue(foundClasses[1].getLocation() == metaclass1.value());
    XCTAssertTrue(foundClasses[1].getVMAddress() == metaclass1.vmAddress());
    XCTAssertTrue(foundClasses[1].isMetaClass);
    XCTAssertTrue(foundClasses[2].getLocation() == class2.value());
    XCTAssertTrue(foundClasses[2].getVMAddress() == class2.vmAddress());
    XCTAssertFalse(foundClasses[2].isMetaClass);
    XCTAssertTrue(foundClasses[3].getLocation() == metaclass2.value());
    XCTAssertTrue(foundClasses[3].getVMAddress() == metaclass2.vmAddress());
    XCTAssertTrue(foundClasses[3].isMetaClass);
}

// Walk classes and metaclasses
- (void)testForEachClassAndMetaclass64
{
    testForEachClassAndMetaclass<class32_t, uint32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testForEachClassAndMetaclass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testForEachClassAndMetaclass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testForEachClassAndMetaclass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testForEachClassAndMetaclass<class64_t, uint64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

template<typename ClassType, typename ClassDataType>
static void testClassFields(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                            SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 2 classes and 2 metaclasses
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 4);
    ResolvedValue class1 = classValues[0];
    ResolvedValue metaclass1 = classValues[1];
    ResolvedValue class2 = classValues[2];
    ResolvedValue metaclass2 = classValues[3];

    // Add 4 class RO data
    std::vector<ResolvedValue> classROValues = addValues<ClassDataType>(state, visitor, 4);
    ResolvedValue classData1 = classROValues[0];
    //ResolvedValue metaclassData1 = classROValues[1];
    ResolvedValue classData2 = classROValues[2];
    //ResolvedValue metaclassData2 = classROValues[3];

    // Just some random data we need to populate fields. The contents don't matter
    std::vector<ResolvedValue> scratchData = addValues<ClassDataType>(state, visitor, 2);
    ResolvedValue scratchData1 = scratchData[0];
    ResolvedValue scratchData2 = scratchData[1];

    // Set class1 fields
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->isaVMAddr),
             metaclass1.vmAddress());
    // Leave the superclass as null.  We'll set the superclass on class 2 instead
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->methodCacheBuckets),
             scratchData1.vmAddress());
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->methodCacheProperties),
             scratchData2.vmAddress());
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->dataVMAddrAndFastFlags),
             classData1.vmAddress());

    // Set class2 fields
    setCache(state, visitor.getField(class2, &((ClassType*)class2.value())->isaVMAddr),
             metaclass2.vmAddress());
    setCache(state, visitor.getField(class2, &((ClassType*)class2.value())->superclassVMAddr),
             class1.vmAddress());
    // Don't set methods.  We'll test that elsewhere
    // Don't set vtable.  It's unused
    setCache(state, visitor.getField(class2, &((ClassType*)class2.value())->dataVMAddrAndFastFlags),
             classData2.vmAddress());

    objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);
    objc_visitor::Class metaclassObject1(metaclass1, /* isMetaClass */ true, /* isPatchable */ false);
    objc_visitor::Class classObject2(class2, /* isMetaClass */ false, /* isPatchable */ false);
    objc_visitor::Class metaclassObject3(metaclass1, /* isMetaClass */ true, /* isPatchable */ false);

    // Act:
    // Class 1
    bool class1Patchable = false;
    ResolvedValue class1ISA = classObject1.getISA(visitor, class1Patchable);
    std::optional<ResolvedValue> class1Superclass = classObject1.getSuperclass(visitor);
    ResolvedValue class1MethodCache = classObject1.getMethodCache(visitor);
    std::optional<ResolvedValue> class1VTable = classObject1.getMethodCacheProperties(visitor);
    ClassData class1Data = classObject1.getClassData(visitor);
    std::optional<uint32_t> class1SwiftFlags = classObject1.swiftClassFlags(visitor);
    std::optional<VMAddress> class1SuperclassVMAddr = classObject1.getSuperclassVMAddr(visitor);
    std::optional<VMAddress> class1VTableVMAddr = classObject1.getMethodCachePropertiesVMAddr(visitor);

    // Class 2
    bool class2Patchable = false;
    ResolvedValue class2ISA = classObject2.getISA(visitor, class2Patchable);
    std::optional<ResolvedValue> class2Superclass = classObject2.getSuperclass(visitor);
    std::optional<ResolvedValue> class2VTable = classObject2.getMethodCacheProperties(visitor);
    ClassData class2Data = classObject2.getClassData(visitor);
    std::optional<uint32_t> class2SwiftFlags = classObject2.swiftClassFlags(visitor);
    std::optional<VMAddress> class2SuperclassVMAddr = classObject2.getSuperclassVMAddr(visitor);
    std::optional<VMAddress> class2VTableVMAddr = classObject2.getMethodCachePropertiesVMAddr(visitor);

    // Assert:
    // Class 1
    XCTAssertTrue(class1ISA.value() == metaclass1.value());
    XCTAssertTrue(class1ISA.vmAddress() == metaclass1.vmAddress());
    XCTAssertFalse(class1Superclass.has_value());
    XCTAssertTrue(class1MethodCache.value() == scratchData1.value());
    XCTAssertTrue(class1MethodCache.vmAddress() == scratchData1.vmAddress());
    XCTAssertTrue(class1VTable.has_value());
    XCTAssertTrue(class1VTable->value() == scratchData2.value());
    XCTAssertTrue(class1VTable->vmAddress() == scratchData2.vmAddress());
    XCTAssertTrue(class1Data.getLocation() == classData1.value());
    XCTAssertTrue(class1Data.getVMAddress() == classData1.vmAddress());
    XCTAssertTrue(!class1SwiftFlags.has_value());
    XCTAssertFalse(class1SuperclassVMAddr.has_value());
    XCTAssertTrue(class1VTableVMAddr.has_value());
    XCTAssertTrue(class1VTableVMAddr.value() == scratchData2.vmAddress());

    // Class 2
    XCTAssertTrue(class2ISA.value() == metaclass2.value());
    XCTAssertTrue(class2ISA.vmAddress() == metaclass2.vmAddress());
    XCTAssertTrue(class2Superclass.has_value());
    XCTAssertTrue(class2Superclass->value() == class1.value());
    XCTAssertTrue(class2Superclass->vmAddress() == class1.vmAddress());
    XCTAssertFalse(class2VTable.has_value());
    XCTAssertTrue(class2Data.getLocation() == classData2.value());
    XCTAssertTrue(class2Data.getVMAddress() == classData2.vmAddress());
    XCTAssertTrue(!class2SwiftFlags.has_value());
    XCTAssertTrue(class2SuperclassVMAddr.has_value());
    XCTAssertTrue(class2SuperclassVMAddr.value() == class1.vmAddress());
    XCTAssertFalse(class2VTableVMAddr.has_value());
}

// Class fields
- (void)testClassFields
{
    testClassFields<class32_t, data32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

template<typename ClassType, typename ClassDataType>
static void testClassFields2(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                             SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 1 class
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 1);
    ResolvedValue class1 = classValues[0];

    const objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);

    // Act:
    ClassType* class1Value = ((ClassType*)class1.value());
    ResolvedValue superclassField1 = visitor.getField(class1, &class1Value->superclassVMAddr);
    ResolvedValue superclassField2 = classObject1.getSuperclassField(visitor);
    ResolvedValue propertiesField1 = visitor.getField(class1, &class1Value->methodCacheProperties);
    ResolvedValue propertiesField2 = classObject1.getMethodCachePropertiesField(visitor);
    ResolvedValue dataField1 = visitor.getField(class1, &class1Value->dataVMAddrAndFastFlags);
    ResolvedValue dataField2 = classObject1.getDataField(visitor);

    // Assert:
    XCTAssertTrue(superclassField1.value() == superclassField2.value());
    XCTAssertTrue(superclassField1.vmAddress() == superclassField2.vmAddress());
    XCTAssertTrue(propertiesField1.value() == propertiesField2.value());
    XCTAssertTrue(propertiesField1.vmAddress() == propertiesField2.vmAddress());
    XCTAssertTrue(dataField1.value() == dataField2.value());
    XCTAssertTrue(dataField1.vmAddress() == dataField2.vmAddress());
}

// Class fields
- (void)testClassFields2
{
    testClassFields2<class32_t, data32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testClassFields2<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testClassFields2<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testClassFields2<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testClassFields2<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

template<typename ClassType, typename ClassDataType>
static void testSetClassFields(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                               SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 1 class
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 1);
    ResolvedValue class1 = classValues[0];

    // We need a valid VMAddress as scratch data
    std::vector<ResolvedValue> scratchData = addValues<ClassDataType>(state, visitor, 1);
    ResolvedValue scratchData1 = scratchData[0];

    objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);

    // Act:
    std::optional<VMAddress> oldMethodCachePropertiesVMAddr = classObject1.getMethodCachePropertiesVMAddr(visitor);
    VMAddress newVMAddr = scratchData1.vmAddress();
    classObject1.setMethodCachePropertiesVMAddr(visitor, newVMAddr);
    std::optional<VMAddress> newMethodCachePropertiesVMAddr = classObject1.getMethodCachePropertiesVMAddr(visitor);

    // Assert:
    XCTAssertFalse(oldMethodCachePropertiesVMAddr.has_value());
    XCTAssertTrue(newMethodCachePropertiesVMAddr.has_value());
    XCTAssertTrue(newMethodCachePropertiesVMAddr.value() == newVMAddr);
}

// Class fields
- (void)testSetClassFields
{
    testSetClassFields<class32_t, data32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testSetClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testSetClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testSetClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testSetClassFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

// Class withSuperClass
- (void)testClassWithSuperclass
{
    // Arrange: mock up visitor, segments, etc
    VMAddress onDiskDylibBaseAddress(0x4000ULL);
    Visitor visitor(onDiskDylibBaseAddress, &MachO_arm64, { }, selectorStringsBaseAddress, { });

    // Add 1 class
    std::vector<ResolvedValue> classValues = addValues<class64_t>(self, visitor, 1);
    ResolvedValue class1 = classValues[0];

    // Set the chained pointer format
    for ( Segment& segment : visitor.segments )
        segment.onDiskDylibChainedPointerFormat = DYLD_CHAINED_PTR_64;

    objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);

    // Act:
    uint64_t superclassValue = 0x1234;
    ResolvedValue superclassField = classObject1.getSuperclassField(visitor);
    *(uint64_t*)superclassField.value() = superclassValue;

    __block uint64_t rawFixup = 0;
    classObject1.withSuperclass(visitor, ^(const dyld3::MachOFile::ChainedFixupPointerOnDisk *fixup, uint16_t pointerFormat) {
        rawFixup = fixup->raw64;
    });

    // Assert:
    XCTAssertTrue(rawFixup == superclassValue);
}

template<typename ClassType, typename ClassDataType>
static void testSwiftClass(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                           SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add classes
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 4);
    ResolvedValue class1 = classValues[0];
    ResolvedValue class2 = classValues[1];
    ResolvedValue class3 = classValues[2];
    ResolvedValue class4 = classValues[3];

    // Add class RO datas
    std::vector<ResolvedValue> classROValues = addValues<ClassDataType>(state, visitor, 4);
    ResolvedValue classData1 = classROValues[0];
    ResolvedValue classData2 = classROValues[1];
    ResolvedValue classData3 = classROValues[2];
    ResolvedValue classData4 = classROValues[3];

    // Set class data bits, which includes if its Swift
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->dataVMAddrAndFastFlags),
             classData1.vmAddress() | 0);
    setCache(state, visitor.getField(class2, &((ClassType*)class2.value())->dataVMAddrAndFastFlags),
             classData2.vmAddress() | FastDataBits::FAST_IS_SWIFT_LEGACY);
    setCache(state, visitor.getField(class3, &((ClassType*)class3.value())->dataVMAddrAndFastFlags),
             classData3.vmAddress() | FastDataBits::FAST_IS_SWIFT_STABLE);
    setCache(state, visitor.getField(class4, &((ClassType*)class4.value())->dataVMAddrAndFastFlags),
             classData4.vmAddress() | FastDataBits::FAST_IS_SWIFT_LEGACY | FastDataBits::FAST_IS_SWIFT_STABLE);
    ((ClassType*)class1.value())->swiftClassFlags = objc_visitor::Class::isSwiftPreStableABI;
    ((ClassType*)class2.value())->swiftClassFlags = objc_visitor::Class::isSwiftPreStableABI;
    ((ClassType*)class3.value())->swiftClassFlags = 0;
    ((ClassType*)class4.value())->swiftClassFlags = 0;

    objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);
    objc_visitor::Class classObject2(class2, /* isMetaClass */ false, /* isPatchable */ false);
    objc_visitor::Class classObject3(class3, /* isMetaClass */ false, /* isPatchable */ false);
    objc_visitor::Class classObject4(class4, /* isMetaClass */ false, /* isPatchable */ false);

    // Act:
    // Class 1
    ClassData class1Data = classObject1.getClassData(visitor);
    std::optional<uint32_t> class1SwiftFlags = classObject1.swiftClassFlags(visitor);
    bool isSwiftLegacy1 = classObject1.isSwiftLegacy(visitor);
    bool isSwiftStable1 = classObject1.isSwiftStable(visitor);
    bool isSwift1 = classObject1.isSwift(visitor);
    bool isUnfixedBackwardDeployingStableSwift1 = classObject1.isUnfixedBackwardDeployingStableSwift(visitor);
    // Class 2
    ClassData class2Data = classObject2.getClassData(visitor);
    std::optional<uint32_t> class2SwiftFlags = classObject2.swiftClassFlags(visitor);
    bool isSwiftLegacy2 = classObject2.isSwiftLegacy(visitor);
    bool isSwiftStable2 = classObject2.isSwiftStable(visitor);
    bool isSwift2 = classObject2.isSwift(visitor);
    bool isUnfixedBackwardDeployingStableSwift2 = classObject2.isUnfixedBackwardDeployingStableSwift(visitor);
    // Class 3
    ClassData class3Data = classObject3.getClassData(visitor);
    std::optional<uint32_t> class3SwiftFlags = classObject3.swiftClassFlags(visitor);
    bool isSwiftLegacy3 = classObject3.isSwiftLegacy(visitor);
    bool isSwiftStable3 = classObject3.isSwiftStable(visitor);
    bool isSwift3 = classObject3.isSwift(visitor);
    bool isUnfixedBackwardDeployingStableSwift3 = classObject3.isUnfixedBackwardDeployingStableSwift(visitor);
    // Class 4
    ClassData class4Data = classObject4.getClassData(visitor);
    std::optional<uint32_t> class4SwiftFlags = classObject4.swiftClassFlags(visitor);
    bool isSwiftLegacy4 = classObject4.isSwiftLegacy(visitor);
    bool isSwiftStable4 = classObject4.isSwiftStable(visitor);
    bool isSwift4 = classObject4.isSwift(visitor);
    bool isUnfixedBackwardDeployingStableSwift4 = classObject4.isUnfixedBackwardDeployingStableSwift(visitor);

    // Assert:
    // Class 1
    XCTAssertTrue(class1Data.getLocation() == classData1.value());
    XCTAssertTrue(class1Data.getVMAddress() == classData1.vmAddress());
    XCTAssertFalse(class1SwiftFlags.has_value());
    XCTAssertFalse(isSwiftLegacy1);
    XCTAssertFalse(isSwiftStable1);
    XCTAssertFalse(isSwift1);
    XCTAssertFalse(isUnfixedBackwardDeployingStableSwift1);
    // Class 2
    XCTAssertTrue(class2Data.getLocation() == classData2.value());
    XCTAssertTrue(class2Data.getVMAddress() == classData2.vmAddress());
    XCTAssertTrue(class2SwiftFlags.has_value());
    XCTAssertTrue(class2SwiftFlags.value() == objc_visitor::Class::isSwiftPreStableABI);
    XCTAssertTrue(isSwiftLegacy2);
    XCTAssertFalse(isSwiftStable2);
    XCTAssertTrue(isSwift2);
    XCTAssertFalse(isUnfixedBackwardDeployingStableSwift2);
    // Class 3
    XCTAssertTrue(class3Data.getLocation() == classData3.value());
    XCTAssertTrue(class3Data.getVMAddress() == classData3.vmAddress());
    XCTAssertTrue(class3SwiftFlags.has_value());
    XCTAssertTrue(class3SwiftFlags.value() == 0);
    XCTAssertFalse(isSwiftLegacy3);
    XCTAssertTrue(isSwiftStable3);
    XCTAssertTrue(isSwift3);
    XCTAssertFalse(isUnfixedBackwardDeployingStableSwift3);
    // Class 4
    XCTAssertTrue(class4Data.getLocation() == classData4.value());
    XCTAssertTrue(class4Data.getVMAddress() == classData4.vmAddress());
    XCTAssertTrue(class4SwiftFlags.has_value());
    XCTAssertTrue(class4SwiftFlags.value() == 0);
    XCTAssertTrue(isSwiftLegacy4);
    XCTAssertTrue(isSwiftStable4);
    XCTAssertTrue(isSwift4);
    XCTAssertTrue(isUnfixedBackwardDeployingStableSwift4);
}

// Swift class
- (void)testSwiftClass
{
    testSwiftClass<class32_t, data32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testSwiftClass<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testSwiftClass<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testSwiftClass<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testSwiftClass<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

template<typename ClassType, typename ClassDataType>
static void testClassDataFields(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                                SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 1 class and data
    std::vector<ResolvedValue> classValues = addValues<ClassType>(state, visitor, 1);
    ResolvedValue class1 = classValues[0];
    std::vector<ResolvedValue> classDataValues = addValues<ClassDataType>(state, visitor, 1);
    ResolvedValue classData1 = classDataValues[0];

    // We need some scratch locations.  The values don't matter
    std::vector<ResolvedValue> scratchData = addValues<uint8_t>(state, visitor, 10);
    ResolvedValue scratchIVarLayout = scratchData[0];
    ResolvedValue scratchName = scratchData[1];
    ResolvedValue scratchBaseMethods = scratchData[2];
    ResolvedValue scratchBaseProtocols = scratchData[3];
    ResolvedValue scratchIVars = scratchData[4];
    ResolvedValue scratchWeakIVarLayout = scratchData[5];
    ResolvedValue scratchBaseProperties = scratchData[6];

    // Set the class to point to the data
    setCache(state, visitor.getField(class1, &((ClassType*)class1.value())->dataVMAddrAndFastFlags),
             classData1.vmAddress());

    // Set data fields
    ClassDataType* data1 = (ClassDataType*)classData1.value();
    data1->flags            = (1 << 1);
    data1->instanceStart    = 100;
    data1->instanceSize.instanceSize     = 200;
    setCache(state, visitor.getField(classData1, &data1->ivarLayoutVMAddr), scratchIVarLayout.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->nameVMAddr), scratchName.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->baseMethodsVMAddr), scratchBaseMethods.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->baseProtocolsVMAddr), scratchBaseProtocols.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->ivarsVMAddr), scratchIVars.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->weakIvarLayoutVMAddr), scratchWeakIVarLayout.vmAddress());
    setCache(state, visitor.getField(classData1, &data1->basePropertiesVMAddr), scratchBaseProperties.vmAddress());

    const objc_visitor::Class classObject1(class1, /* isMetaClass */ false, /* isPatchable */ false);
    const objc_visitor::ClassData classDataObject1(classData1);

    // Act:
    bool isRootClass = classObject1.isRootClass(visitor);
    uint32_t instanceStart = classObject1.getInstanceStart(visitor);
    uint32_t instanceSize = classObject1.getInstanceSize(visitor);
    ResolvedValue ivarLayout = visitor.resolveRebase(classDataObject1.getField(visitor, ClassData::Field::ivarLayout));
    const char* name = classObject1.getName(visitor);
    VMAddress nameVMAddr = classObject1.getNameVMAddr(visitor);
    MethodList baseMethods = classObject1.getBaseMethods(visitor);
    ProtocolList baseProtocols = classObject1.getBaseProtocols(visitor);
    IVarList ivars = classObject1.getIVars(visitor);

    // Assert:
    XCTAssertTrue(isRootClass);
    XCTAssertTrue(instanceStart == 100);
    XCTAssertTrue(instanceSize == 200);
    XCTAssertTrue(ivarLayout.value() == scratchIVarLayout.value());
    XCTAssertTrue(ivarLayout.vmAddress() == scratchIVarLayout.vmAddress());
    XCTAssertTrue(name == scratchName.value());
    XCTAssertTrue(nameVMAddr == scratchName.vmAddress());
    XCTAssertTrue(baseMethods.getLocation() == scratchBaseMethods.value());
    XCTAssertTrue(baseMethods.getVMAddress().has_value());
    XCTAssertTrue(baseMethods.getVMAddress().value() == scratchBaseMethods.vmAddress());
    XCTAssertTrue(baseProtocols.getLocation() == scratchBaseProtocols.value());
    XCTAssertTrue(baseProtocols.getVMAddress().has_value());
    XCTAssertTrue(baseProtocols.getVMAddress().value() == scratchBaseProtocols.vmAddress());
    XCTAssertTrue(ivars.getLocation() == scratchIVars.value());
    XCTAssertTrue(ivars.getVMAddress().has_value());
    XCTAssertTrue(ivars.getVMAddress().value() == scratchIVars.vmAddress());
}


// Class Data fields
- (void)testClassDataFields
{
    testClassDataFields<class32_t, data32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testClassDataFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testClassDataFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testClassDataFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testClassDataFields<class64_t, data64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

//
// MARK: --- Protocol methods ---
//

// FIXME: Add other fields and tests for protocols which are too small to have all fields
template<typename ProtocolType>
static void testProtocolAddFixups(ObjCVisitorTests* state, CacheVMAddress cacheBaseAddress,
                                  SetCacheType setCache, const dyld3::MachOFile* mf)
{
    // Arrange: mock up visitor, segments, etc
    Visitor visitor(cacheBaseAddress, mf, { }, state->selectorStringsBaseAddress, { });

    // Add 1 protocol and data
    std::vector<ResolvedValue> protocolValues = addValues<ProtocolType>(state, visitor, 1);
    ResolvedValue protocolValue1 = protocolValues[0];

    // Set data fields
    ProtocolType* protocol1 = (ProtocolType*)protocolValue1.value();
    protocol1->size         = sizeof(ProtocolType);

    // Act:
    const objc_visitor::Protocol protocolObject(protocolValue1);
    std::vector<void*> fixups;
    protocolObject.addFixups(visitor, fixups);

    // Assert:
    XCTAssertTrue(fixups.empty());
}

- (void)testProtocolAddFixups
{
    testProtocolAddFixups<protocol32_t>(self, cacheBaseAddress32, &setCache32, &MachO_arm64_32);
    testProtocolAddFixups<protocol64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64);
    testProtocolAddFixups<protocol64_t>(self, cacheBaseAddress64, &setCache64, &MachO_x86_64h);
    testProtocolAddFixups<protocol64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64);
    testProtocolAddFixups<protocol64_t>(self, cacheBaseAddress64, &setCache64, &MachO_arm64e);
}

@end
