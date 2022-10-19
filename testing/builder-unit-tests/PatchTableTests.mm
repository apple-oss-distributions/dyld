
/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2021 Apple Inc. All rights reserved.
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

#include "NewSharedCacheBuilder.h"
#include "BuilderFileSystem.h"

using namespace cache_builder;
using error::Error;

@interface PatchTableTests : XCTestCase

@end

@implementation PatchTableTests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
    self.continueAfterFailure = NO;
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
    self.continueAfterFailure = YES;
}

static uint32_t addBind(CacheDylib& fromDylib, PatchInfo& fromDylibPatchInfo,
                        const CacheDylib& toDylib,
                        VMOffset runtimeOffset, std::string_view symbolName)
{
    CacheDylib::BindTarget::CacheImage cacheImage = {
        .targetRuntimeOffset = runtimeOffset,
        .targetDylib         = &toDylib,
        .isWeak              = false
    };
    CacheDylib::BindTarget bindTarget = {
        .kind = CacheDylib::BindTarget::Kind::cacheImage,
        .addend = 0,
        .cacheImage = cacheImage
    };

    uint32_t bindIndex = (uint32_t)fromDylib.bindTargets.size();
    fromDylib.bindTargets.push_back(std::move(bindTarget));

    // Add the patch info entries too, as they need to match the size of the bindTargets array
    fromDylibPatchInfo.bindUses.emplace_back();
    fromDylibPatchInfo.bindTargetNames.emplace_back(symbolName);
    fromDylibPatchInfo.bindGOTUses.emplace_back();
    fromDylibPatchInfo.bindAuthGOTUses.emplace_back();

    return bindIndex;
}

static void addUse(CacheDylib& fromDylib, PatchInfo& fromDylibPatchInfo,
                   uint32_t bindIndex, CacheVMAddress cacheVMAddr)
{
    dyld3::MachOFile::PointerMetaData pmd = { };
    uint64_t addend = 0;
    dyld_cache_patchable_location loc(cacheVMAddr, pmd, addend);
    fromDylibPatchInfo.bindUses[bindIndex].push_back(std::move(loc));
}

static void addGOTUse(CacheDylib& fromDylib, PatchInfo& fromDylibPatchInfo,
                      uint32_t bindIndex, CacheVMAddress cacheVMAddr)
{
    dyld3::MachOFile::PointerMetaData pmd = { };
    uint64_t addend = 0;
    dyld_cache_patchable_location loc(cacheVMAddr, pmd, addend);
    PatchInfo::GOTInfo gotInfo = {
        .patchInfo = loc,
        .targetValue = VMOffset(),
    };
    fromDylibPatchInfo.bindGOTUses[bindIndex].push_back(std::move(gotInfo));
}

struct FoundExport
{
    uint32_t    dylibVMOffsetOfImpl;
    const char* exportName;
    PatchKind   patchKind;
};

static std::vector<FoundExport> getExports(const PatchTable& patchTable, uint32_t dylibIndex)
{
    __block std::vector<FoundExport> foundExports;
    patchTable.forEachPatchableExport(dylibIndex,
                                      ^(uint32_t dylibVMOffsetOfImpl, const char *exportName,
                                        PatchKind patchKind) {
        foundExports.push_back({ dylibVMOffsetOfImpl, exportName, patchKind });
    });
    return foundExports;
}

struct FoundExportUse
{
    uint32_t userImageIndex;
    uint32_t userVMOffset;
    dyld3::MachOFile::PointerMetaData pmd;
    uint64_t addend;
};

static std::vector<FoundExportUse> getExportUses(const PatchTable& patchTable, uint32_t dylibIndex,
                                                 VMOffset dylibVMOffsetOfImpl)
{
    __block std::vector<FoundExportUse> uses;
    patchTable.forEachPatchableUseOfExport(dylibIndex, (uint32_t)dylibVMOffsetOfImpl.rawValue(),
                                           ^(uint32_t userImageIndex, uint32_t userVMOffset,
                                             dyld3::MachOFile::PointerMetaData pmd, uint64_t addend) {
        uses.push_back({ userImageIndex, userVMOffset, pmd, addend });
    });
    return uses;
}

struct FoundExportCacheUse
{
    uint64_t cacheOffset;
    dyld3::MachOFile::PointerMetaData pmd;
    uint64_t addend;
};

static std::vector<FoundExportCacheUse> getExportCacheUses(const PatchTable& patchTable,
                                                           uint32_t dylibIndex,
                                                           VMOffset dylibVMOffsetOfImpl,
                                                           CacheVMAddress cacheUnslidAddress,
                                                           std::span<CacheVMAddress> cacheDylibAddresses)
{
    auto getDylibAddressHandler = ^(uint32_t dylibCacheIndex) {
        return cacheDylibAddresses[dylibCacheIndex].rawValue();
    };

    __block std::vector<FoundExportCacheUse> uses;
    patchTable.forEachPatchableCacheUseOfExport(dylibIndex, (uint32_t)dylibVMOffsetOfImpl.rawValue(),
                                                cacheUnslidAddress.rawValue(),
                                                getDylibAddressHandler,
                                                ^(uint64_t cacheVMOffset,
                                                  dyld3::MachOFile::PointerMetaData pmd, uint64_t addend) {
        uses.push_back({ cacheVMOffset, pmd, addend });
    });
    return uses;
}

static std::vector<FoundExportUse> getExportUsesIn(const PatchTable& patchTable, uint32_t dylibIndex,
                                                   VMOffset dylibVMOffsetOfImpl, uint32_t userImageIndex)
{
    __block std::vector<FoundExportUse> uses;
    patchTable.forEachPatchableUseOfExportInImage(dylibIndex, (uint32_t)dylibVMOffsetOfImpl.rawValue(),
                                                  userImageIndex,
                                                  ^(uint32_t userVMOffset,
                                                    dyld3::MachOFile::PointerMetaData pmd, uint64_t addend) {
        uses.push_back({ userImageIndex, userVMOffset, pmd, addend });
    });
    return uses;
}

struct FoundGOTUse
{
    uint64_t userCacheVMOffset;
    dyld3::MachOFile::PointerMetaData pmd;
    uint64_t addend;
};

static std::vector<FoundGOTUse> getGOTUsesIn(const PatchTable& patchTable, uint32_t dylibIndex,
                                             VMOffset dylibVMOffsetOfImpl)
{
    __block std::vector<FoundGOTUse> uses;
    patchTable.forEachPatchableGOTUseOfExport(dylibIndex, (uint32_t)dylibVMOffsetOfImpl.rawValue(),
                                              ^(uint64_t cacheVMOffset, dyld3::MachOFile::PointerMetaData pmd,
                                                uint64_t addend) {
        uses.push_back({ cacheVMOffset, pmd, addend });
    });
    return uses;
}

// A patch table with no elements
- (void)testEmptyPatchTable
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build({ }, { }, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 0);
}

// A patch table with 1 dylib, but no uses of that dylib
- (void)testOneDylibPatchTable
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    PatchInfo patchInfo;
    CacheDylib cacheDylib;
    cacheDylib.cacheIndex = 0;

    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build({ &cacheDylib, 1}, { &patchInfo, 1},
                               patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 1);
    XCTAssertTrue(numExports0 == 0);
}

// A patch table with 2 dylibs, but no uses of those dylibs
- (void)testTwoDylibPatchTable
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 0);
    XCTAssertTrue(numExports1 == 0);
}

// A patch table with 2 dylibs, where one dylib export a symbol but no-one uses it
- (void)testNoUsedExports
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    // Dylib 1 binds to dylib 0, but doesn't use that bind.  Weird
    addBind(cacheDylibs[1], patchInfos[1], cacheDylibs[0], VMOffset(0x1000ULL), "symbol0");

    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 0);
    XCTAssertTrue(numExports1 == 0);
}

// A patch table with 2 dylibs, where one dylib uses one export from the other
- (void)testOneUsedExport
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);
    CacheVMAddress dylibAddresses[2] = {
        cacheBaseAddress + VMOffset(0x10000ULL),
        cacheBaseAddress + VMOffset(0x20000ULL)
    };

    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheLoadAddress = dylibAddresses[0];
    cacheDylibs[1].cacheLoadAddress = dylibAddresses[1];
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    // Dylib 1 should use an export from dylib 0
    VMOffset exportVMOffset(0x1000ULL);
    VMOffset useVMOffset(0x2000ULL);
    CacheVMAddress useVMAddr = dylibAddresses[1] + useVMOffset;
    uint32_t bindIndex0 = addBind(cacheDylibs[1], patchInfos[1],
                                  cacheDylibs[0], exportVMOffset, "symbol0");
    addUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Walk the exports.  Dylib 0 should have 1 export, and dylib 1 should have none
    std::vector<FoundExport> foundExports0 = getExports(patchTable, 0);
    std::vector<FoundExport> foundExports1 = getExports(patchTable, 1);

    // Walk the uses via different forEach methods
    std::vector<FoundExportUse> uses0 = getExportUses(patchTable, 0, exportVMOffset);
    std::vector<FoundExportUse> uses1 = getExportUses(patchTable, 1, exportVMOffset);
    std::vector<FoundExportUse> usesWrongOffset = getExportUses(patchTable, 0, VMOffset(0x0ULL));
    std::vector<FoundExportUse> usesOfZeroInOne = getExportUsesIn(patchTable, 0, exportVMOffset, 1);
    std::vector<FoundExportUse> usesOfOneInZero = getExportUsesIn(patchTable, 1, exportVMOffset, 0);

    // Walk the uses as cache offsets
    std::vector<FoundExportCacheUse> cacheUses = getExportCacheUses(patchTable, 0, exportVMOffset,
                                                                    cacheBaseAddress, dylibAddresses);

    // Dylib 0 should have dylib 1 as a client, and dylib 1 should have no clients
    bool zeroIsClientOfZero = patchTable.imageHasClient(0, 0);
    bool oneIsClientOfZero = patchTable.imageHasClient(0, 1);
    bool twoIsClientOfZero = patchTable.imageHasClient(0, 2);
    bool zeroIsClientOfOne = patchTable.imageHasClient(1, 0);
    bool oneIsClientOfOne = patchTable.imageHasClient(1, 1);
    bool twoIsClientOfOne = patchTable.imageHasClient(1, 2);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 1);
    XCTAssertTrue(numExports1 == 0);
    XCTAssertTrue(numExports0 == foundExports0.size());
    XCTAssertTrue(numExports1 == foundExports1.size());

    // Should find 1 used export from dylib 0, and none from dylib 1
    XCTAssertTrue(foundExports0[0].dylibVMOffsetOfImpl == exportVMOffset.rawValue());
    XCTAssertTrue(!strcmp(foundExports0[0].exportName, "symbol0"));
    XCTAssertTrue(foundExports0[0].patchKind == PatchKind::regular);

    // Should find 1 use in dylib 1, of a value in dylib 0
    XCTAssertTrue(uses0.size() == 1);
    XCTAssertTrue(uses0[0].userImageIndex == 1);
    XCTAssertTrue(uses0[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(uses0[0].pmd.diversity            == 0);
    XCTAssertTrue(uses0[0].pmd.high8                == 0);
    XCTAssertTrue(uses0[0].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[0].pmd.key                  == 0);
    XCTAssertTrue(uses0[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[0].addend == 0);

    // Also look in the "in image" version of the callback
    XCTAssertTrue(usesOfZeroInOne.size() == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userImageIndex == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(usesOfZeroInOne[0].pmd.diversity            == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.high8                == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.authenticated        == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.key                  == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(usesOfZeroInOne[0].addend == 0);

    // Also look in the cache offset version of the callback
    XCTAssertTrue(cacheUses.size() == 1);
    VMOffset useCacheVMOffset = useVMAddr - cacheBaseAddress;
    XCTAssertTrue(cacheUses[0].cacheOffset == useCacheVMOffset.rawValue());
    XCTAssertTrue(cacheUses[0].pmd.diversity            == 0);
    XCTAssertTrue(cacheUses[0].pmd.high8                == 0);
    XCTAssertTrue(cacheUses[0].pmd.authenticated        == 0);
    XCTAssertTrue(cacheUses[0].pmd.key                  == 0);
    XCTAssertTrue(cacheUses[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(cacheUses[0].addend == 0);

    // Should find no other uses
    XCTAssertTrue(uses1.empty());
    XCTAssertTrue(usesWrongOffset.empty());
    XCTAssertTrue(usesOfOneInZero.size() == 0);

    // Only dylib 1 should be a client of dylib 0
    XCTAssertTrue(oneIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfZero);
    XCTAssertFalse(twoIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfOne);
    XCTAssertFalse(oneIsClientOfOne);
    XCTAssertFalse(twoIsClientOfOne);
}

// A patch table with 2 dylibs, where one dylib uses one ObjC export from the other
- (void)testOneUsedExport_ObjC
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);
    CacheVMAddress dylibAddresses[2] = {
        cacheBaseAddress + VMOffset(0x10000ULL),
        cacheBaseAddress + VMOffset(0x20000ULL)
    };

    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheLoadAddress = dylibAddresses[0];
    cacheDylibs[1].cacheLoadAddress = dylibAddresses[1];
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    // Dylib 1 should use an export from dylib 0
    VMOffset exportVMOffset(0x1000ULL);
    VMOffset useVMOffset(0x2000ULL);
    CacheVMAddress exportVMAddr = dylibAddresses[0] + exportVMOffset;
    CacheVMAddress useVMAddr = dylibAddresses[1] + useVMOffset;
    uint32_t bindIndex0 = addBind(cacheDylibs[1], patchInfos[1],
                                  cacheDylibs[0], exportVMOffset, "symbol0");
    addUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr);

    // The export should be an objc class
    patchableClasses.insert(exportVMAddr);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Walk the exports.  Dylib 0 should have 1 export, and dylib 1 should have none
    std::vector<FoundExport> foundExports0 = getExports(patchTable, 0);
    std::vector<FoundExport> foundExports1 = getExports(patchTable, 1);

    // Walk the uses via different forEach methods
    std::vector<FoundExportUse> uses0 = getExportUses(patchTable, 0, exportVMOffset);
    std::vector<FoundExportUse> uses1 = getExportUses(patchTable, 1, exportVMOffset);
    std::vector<FoundExportUse> usesWrongOffset = getExportUses(patchTable, 0, VMOffset(0x0ULL));
    std::vector<FoundExportUse> usesOfZeroInOne = getExportUsesIn(patchTable, 0, exportVMOffset, 1);
    std::vector<FoundExportUse> usesOfOneInZero = getExportUsesIn(patchTable, 1, exportVMOffset, 0);

    // Walk the uses as cache offsets
    std::vector<FoundExportCacheUse> cacheUses = getExportCacheUses(patchTable, 0, exportVMOffset,
                                                                    cacheBaseAddress, dylibAddresses);

    // Dylib 0 should have dylib 1 as a client, and dylib 1 should have no clients
    bool zeroIsClientOfZero = patchTable.imageHasClient(0, 0);
    bool oneIsClientOfZero = patchTable.imageHasClient(0, 1);
    bool twoIsClientOfZero = patchTable.imageHasClient(0, 2);
    bool zeroIsClientOfOne = patchTable.imageHasClient(1, 0);
    bool oneIsClientOfOne = patchTable.imageHasClient(1, 1);
    bool twoIsClientOfOne = patchTable.imageHasClient(1, 2);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 1);
    XCTAssertTrue(numExports1 == 0);
    XCTAssertTrue(numExports0 == foundExports0.size());
    XCTAssertTrue(numExports1 == foundExports1.size());

    // Should find 1 used export from dylib 0, and none from dylib 1
    XCTAssertTrue(foundExports0[0].dylibVMOffsetOfImpl == exportVMOffset.rawValue());
    XCTAssertTrue(!strcmp(foundExports0[0].exportName, "symbol0"));
    XCTAssertTrue(foundExports0[0].patchKind == PatchKind::objcClass);

    // Should find 1 use in dylib 1, of a value in dylib 0
    XCTAssertTrue(uses0.size() == 1);
    XCTAssertTrue(uses0[0].userImageIndex == 1);
    XCTAssertTrue(uses0[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(uses0[0].pmd.diversity            == 0);
    XCTAssertTrue(uses0[0].pmd.high8                == 0);
    XCTAssertTrue(uses0[0].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[0].pmd.key                  == 0);
    XCTAssertTrue(uses0[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[0].addend == 0);

    // Also look in the "in image" version of the callback
    XCTAssertTrue(usesOfZeroInOne.size() == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userImageIndex == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(usesOfZeroInOne[0].pmd.diversity            == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.high8                == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.authenticated        == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.key                  == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(usesOfZeroInOne[0].addend == 0);

    // Also look in the cache offset version of the callback
    XCTAssertTrue(cacheUses.size() == 1);
    VMOffset useCacheVMOffset = useVMAddr - cacheBaseAddress;
    XCTAssertTrue(cacheUses[0].cacheOffset == useCacheVMOffset.rawValue());
    XCTAssertTrue(cacheUses[0].pmd.diversity            == 0);
    XCTAssertTrue(cacheUses[0].pmd.high8                == 0);
    XCTAssertTrue(cacheUses[0].pmd.authenticated        == 0);
    XCTAssertTrue(cacheUses[0].pmd.key                  == 0);
    XCTAssertTrue(cacheUses[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(cacheUses[0].addend == 0);

    // Should find no other uses
    XCTAssertTrue(uses1.empty());
    XCTAssertTrue(usesWrongOffset.empty());
    XCTAssertTrue(usesOfOneInZero.size() == 0);

    // Only dylib 1 should be a client of dylib 0
    XCTAssertTrue(oneIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfZero);
    XCTAssertFalse(twoIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfOne);
    XCTAssertFalse(oneIsClientOfOne);
    XCTAssertFalse(twoIsClientOfOne);
}

// A patch table with 2 dylibs, where one dylib uses one CFObj2 export from the other
- (void)testOneUsedExport_CFObj2
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);
    CacheVMAddress dylibAddresses[2] = {
        cacheBaseAddress + VMOffset(0x10000ULL),
        cacheBaseAddress + VMOffset(0x20000ULL)
    };

    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheLoadAddress = dylibAddresses[0];
    cacheDylibs[1].cacheLoadAddress = dylibAddresses[1];
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    // Dylib 1 should use an export from dylib 0
    VMOffset exportVMOffset(0x1000ULL);
    VMOffset useVMOffset(0x2000ULL);
    CacheVMAddress exportVMAddr = dylibAddresses[0] + exportVMOffset;
    CacheVMAddress useVMAddr = dylibAddresses[1] + useVMOffset;
    uint32_t bindIndex0 = addBind(cacheDylibs[1], patchInfos[1],
                                  cacheDylibs[0], exportVMOffset, "symbol0");
    addUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr);

    // The export should be a CF object
    patchableCFObj2.insert(exportVMAddr);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Walk the exports.  Dylib 0 should have 1 export, and dylib 1 should have none
    std::vector<FoundExport> foundExports0 = getExports(patchTable, 0);
    std::vector<FoundExport> foundExports1 = getExports(patchTable, 1);

    // Walk the uses via different forEach methods
    std::vector<FoundExportUse> uses0 = getExportUses(patchTable, 0, exportVMOffset);
    std::vector<FoundExportUse> uses1 = getExportUses(patchTable, 1, exportVMOffset);
    std::vector<FoundExportUse> usesWrongOffset = getExportUses(patchTable, 0, VMOffset(0x0ULL));
    std::vector<FoundExportUse> usesOfZeroInOne = getExportUsesIn(patchTable, 0, exportVMOffset, 1);
    std::vector<FoundExportUse> usesOfOneInZero = getExportUsesIn(patchTable, 1, exportVMOffset, 0);

    // Walk the uses as cache offsets
    std::vector<FoundExportCacheUse> cacheUses = getExportCacheUses(patchTable, 0, exportVMOffset,
                                                                    cacheBaseAddress, dylibAddresses);

    // Dylib 0 should have dylib 1 as a client, and dylib 1 should have no clients
    bool zeroIsClientOfZero = patchTable.imageHasClient(0, 0);
    bool oneIsClientOfZero = patchTable.imageHasClient(0, 1);
    bool twoIsClientOfZero = patchTable.imageHasClient(0, 2);
    bool zeroIsClientOfOne = patchTable.imageHasClient(1, 0);
    bool oneIsClientOfOne = patchTable.imageHasClient(1, 1);
    bool twoIsClientOfOne = patchTable.imageHasClient(1, 2);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 1);
    XCTAssertTrue(numExports1 == 0);
    XCTAssertTrue(numExports0 == foundExports0.size());
    XCTAssertTrue(numExports1 == foundExports1.size());

    // Should find 1 used export from dylib 0, and none from dylib 1
    XCTAssertTrue(foundExports0[0].dylibVMOffsetOfImpl == exportVMOffset.rawValue());
    XCTAssertTrue(!strcmp(foundExports0[0].exportName, "symbol0"));
    XCTAssertTrue(foundExports0[0].patchKind == PatchKind::cfObj2);

    // Should find 1 use in dylib 1, of a value in dylib 0
    XCTAssertTrue(uses0.size() == 1);
    XCTAssertTrue(uses0[0].userImageIndex == 1);
    XCTAssertTrue(uses0[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(uses0[0].pmd.diversity            == 0);
    XCTAssertTrue(uses0[0].pmd.high8                == 0);
    XCTAssertTrue(uses0[0].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[0].pmd.key                  == 0);
    XCTAssertTrue(uses0[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[0].addend == 0);

    // Also look in the "in image" version of the callback
    XCTAssertTrue(usesOfZeroInOne.size() == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userImageIndex == 1);
    XCTAssertTrue(usesOfZeroInOne[0].userVMOffset == useVMOffset.rawValue());
    XCTAssertTrue(usesOfZeroInOne[0].pmd.diversity            == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.high8                == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.authenticated        == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.key                  == 0);
    XCTAssertTrue(usesOfZeroInOne[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(usesOfZeroInOne[0].addend == 0);

    // Also look in the cache offset version of the callback
    XCTAssertTrue(cacheUses.size() == 1);
    VMOffset useCacheVMOffset = useVMAddr - cacheBaseAddress;
    XCTAssertTrue(cacheUses[0].cacheOffset == useCacheVMOffset.rawValue());
    XCTAssertTrue(cacheUses[0].pmd.diversity            == 0);
    XCTAssertTrue(cacheUses[0].pmd.high8                == 0);
    XCTAssertTrue(cacheUses[0].pmd.authenticated        == 0);
    XCTAssertTrue(cacheUses[0].pmd.key                  == 0);
    XCTAssertTrue(cacheUses[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(cacheUses[0].addend == 0);

    // Should find no other uses
    XCTAssertTrue(uses1.empty());
    XCTAssertTrue(usesWrongOffset.empty());
    XCTAssertTrue(usesOfOneInZero.size() == 0);

    // Only dylib 1 should be a client of dylib 0
    XCTAssertTrue(oneIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfZero);
    XCTAssertFalse(twoIsClientOfZero);
    XCTAssertFalse(zeroIsClientOfOne);
    XCTAssertFalse(oneIsClientOfOne);
    XCTAssertFalse(twoIsClientOfOne);
}

// Test GOT patches for an image
- (void)testGOTPatches
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);
    CacheVMAddress dylibAddress0 = cacheBaseAddress + VMOffset(0x10000ULL);
    CacheVMAddress dylibAddress1 = cacheBaseAddress + VMOffset(0x20000ULL);

    PatchInfo patchInfos[2];
    CacheDylib cacheDylibs[2];
    cacheDylibs[0].cacheLoadAddress = dylibAddress0;
    cacheDylibs[1].cacheLoadAddress = dylibAddress1;
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;

    // Dylib 1 should use an export from dylib 0
    VMOffset exportVMOffset(0x1000ULL);
    VMOffset useVMOffset0(0x2000ULL);
    VMOffset useVMOffset1(0x2001ULL);
    VMOffset useVMOffset2(0x2002ULL);
    uint32_t bindIndex0 = addBind(cacheDylibs[1], patchInfos[1], cacheDylibs[0], exportVMOffset, "symbol0");
    addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, dylibAddress1 + useVMOffset0);
    addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, dylibAddress1 + useVMOffset1);
    addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, dylibAddress1 + useVMOffset2);

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);

    // Walk the exports.  Dylib 0 should have 1 export, and dylib 1 should have none
    std::vector<FoundExport> foundExports0 = getExports(patchTable, 0);
    std::vector<FoundExport> foundExports1 = getExports(patchTable, 1);

    // Walk the got uses in each dylib
    std::vector<FoundGOTUse> uses0 = getGOTUsesIn(patchTable, 0, exportVMOffset);
    std::vector<FoundGOTUse> uses1 = getGOTUsesIn(patchTable, 1, exportVMOffset);

    // Assert:
    XCTAssertFalse(err1.hasError());
    XCTAssertFalse(err2.hasError());
    XCTAssertTrue(numImages == 2);
    XCTAssertTrue(numExports0 == 1);
    XCTAssertTrue(numExports1 == 0);
    XCTAssertTrue(numExports0 == foundExports0.size());
    XCTAssertTrue(numExports1 == foundExports1.size());

    // Should find 1 used export from dylib 0, and none from dylib 1
    XCTAssertTrue(foundExports0[0].dylibVMOffsetOfImpl == exportVMOffset.rawValue());
    XCTAssertTrue(!strcmp(foundExports0[0].exportName, "symbol0"));
    XCTAssertTrue(foundExports0[0].patchKind == PatchKind::regular);

    // Should have multiple GOT uses for dylib 0
    VMOffset vmOffset0 = (dylibAddress1 + useVMOffset0) - cacheBaseAddress;
    VMOffset vmOffset1 = (dylibAddress1 + useVMOffset1) - cacheBaseAddress;
    VMOffset vmOffset2 = (dylibAddress1 + useVMOffset2) - cacheBaseAddress;
    XCTAssertTrue(uses0.size() == 3);
    XCTAssertTrue(uses0[0].userCacheVMOffset == vmOffset0.rawValue());
    XCTAssertTrue(uses0[0].pmd.diversity            == 0);
    XCTAssertTrue(uses0[0].pmd.high8                == 0);
    XCTAssertTrue(uses0[0].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[0].pmd.key                  == 0);
    XCTAssertTrue(uses0[0].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[0].addend == 0);
    XCTAssertTrue(uses0[1].userCacheVMOffset == vmOffset1.rawValue());
    XCTAssertTrue(uses0[1].pmd.diversity            == 0);
    XCTAssertTrue(uses0[1].pmd.high8                == 0);
    XCTAssertTrue(uses0[1].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[1].pmd.key                  == 0);
    XCTAssertTrue(uses0[1].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[1].addend == 0);
    XCTAssertTrue(uses0[2].userCacheVMOffset == vmOffset2.rawValue());
    XCTAssertTrue(uses0[2].pmd.diversity            == 0);
    XCTAssertTrue(uses0[2].pmd.high8                == 0);
    XCTAssertTrue(uses0[2].pmd.authenticated        == 0);
    XCTAssertTrue(uses0[2].pmd.key                  == 0);
    XCTAssertTrue(uses0[2].pmd.usesAddrDiversity    == 0);
    XCTAssertTrue(uses0[2].addend == 0);

    // Should have no uses of dylib 1
    XCTAssertTrue(uses1.empty());
}

// To improve the speed of GOT patching, we stop the search once we find the dylibOffset we want
// We also use a binary search to find the image, as they are sorted.  This tests that GOTs added
// in an arbitrary order are in fact sorted
- (void)testManyGOTPatches
{
    // Arrange: mock up dylibs and binds
    PatchTableBuilder::PatchableClassesSet patchableClasses;
    PatchTableBuilder::PatchableSingletonsSet patchableCFObj2;
    CacheVMAddress cacheBaseAddress(0x180000000ULL);
    CacheVMAddress patchTableAddress(0x200000000ULL);
    CacheVMAddress dylibAddress0 = cacheBaseAddress + VMOffset(0x1000000ULL);
    CacheVMAddress dylibAddress1 = cacheBaseAddress + VMOffset(0x2000000ULL);
    CacheVMAddress dylibAddress2 = cacheBaseAddress + VMOffset(0x3000000ULL);

    PatchInfo patchInfos[3];
    CacheDylib cacheDylibs[3];
    cacheDylibs[0].cacheLoadAddress = dylibAddress0;
    cacheDylibs[1].cacheLoadAddress = dylibAddress1;
    cacheDylibs[2].cacheLoadAddress = dylibAddress2;
    cacheDylibs[0].cacheIndex = 0;
    cacheDylibs[1].cacheIndex = 1;
    cacheDylibs[2].cacheIndex = 2;

    // Add multiple exports, in various orders
    // Uses of dylib 0 from dylib 1
    VMOffset exportVMOffset0_0(0x3000ULL);
    VMOffset exportVMOffset0_1(0x2000ULL);
    VMOffset exportVMOffset0_2(0x1000ULL);
    {
        CacheVMAddress useVMAddr1_0 = dylibAddress1 + VMOffset(0x10000ULL);
        CacheVMAddress useVMAddr1_1 = dylibAddress1 + VMOffset(0x20000ULL);
        CacheVMAddress useVMAddr1_2 = dylibAddress1 + VMOffset(0x30000ULL);
        CacheVMAddress useVMAddr1_3 = dylibAddress1 + VMOffset(0x40000ULL);
        CacheVMAddress useVMAddr1_4 = dylibAddress1 + VMOffset(0x50000ULL);
        CacheVMAddress useVMAddr1_5 = dylibAddress1 + VMOffset(0x60000ULL);
        uint32_t bindIndex0 = addBind(cacheDylibs[1], patchInfos[1], cacheDylibs[0], exportVMOffset0_0, "symbol0");
        uint32_t bindIndex1 = addBind(cacheDylibs[1], patchInfos[1], cacheDylibs[0], exportVMOffset0_1, "symbol1");
        uint32_t bindIndex2 = addBind(cacheDylibs[1], patchInfos[1], cacheDylibs[0], exportVMOffset0_2, "symbol2");
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr1_0);
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr1_1);
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex0, useVMAddr1_2);
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex1, useVMAddr1_3);
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex1, useVMAddr1_4);
        addGOTUse(cacheDylibs[1], patchInfos[1], bindIndex2, useVMAddr1_5);
    }
    // Uses of dylib 0 from dylib 2
    {
        CacheVMAddress useVMAddr2_0 = dylibAddress2 + VMOffset(0x10000ULL);
        CacheVMAddress useVMAddr2_1 = dylibAddress2 + VMOffset(0x20000ULL);
        CacheVMAddress useVMAddr2_2 = dylibAddress2 + VMOffset(0x30000ULL);
        CacheVMAddress useVMAddr2_3 = dylibAddress2 + VMOffset(0x40000ULL);
        CacheVMAddress useVMAddr2_4 = dylibAddress2 + VMOffset(0x50000ULL);
        CacheVMAddress useVMAddr2_5 = dylibAddress2 + VMOffset(0x60000ULL);
        uint32_t bindIndex0 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[0], exportVMOffset0_0, "symbol0");
        uint32_t bindIndex1 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[0], exportVMOffset0_1, "symbol1");
        uint32_t bindIndex2 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[0], exportVMOffset0_2, "symbol3");
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_0);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_1);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_2);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex1, useVMAddr2_3);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex1, useVMAddr2_4);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex2, useVMAddr2_5);
    }
    // Uses of dylib 1 from dylib 2
    VMOffset exportVMOffset1_0(0x13000ULL);
    VMOffset exportVMOffset1_1(0x12000ULL);
    VMOffset exportVMOffset1_2(0x1000ULL);
    {
        CacheVMAddress useVMAddr2_0 = dylibAddress2 + VMOffset(0x110000ULL);
        CacheVMAddress useVMAddr2_1 = dylibAddress2 + VMOffset(0x120000ULL);
        CacheVMAddress useVMAddr2_2 = dylibAddress2 + VMOffset(0x130000ULL);
        CacheVMAddress useVMAddr2_3 = dylibAddress2 + VMOffset(0x140000ULL);
        CacheVMAddress useVMAddr2_4 = dylibAddress2 + VMOffset(0x150000ULL);
        CacheVMAddress useVMAddr2_5 = dylibAddress2 + VMOffset(0x160000ULL);
        uint32_t bindIndex0 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[1], exportVMOffset1_0, "symbol0");
        uint32_t bindIndex1 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[1], exportVMOffset1_1, "symbol1");
        uint32_t bindIndex2 = addBind(cacheDylibs[2], patchInfos[2], cacheDylibs[1], exportVMOffset1_2, "symbol3");
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_0);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_1);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex0, useVMAddr2_2);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex1, useVMAddr2_3);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex1, useVMAddr2_4);
        addGOTUse(cacheDylibs[2], patchInfos[2], bindIndex2, useVMAddr2_5);
    }

    // Act:
    PatchTableBuilder builder;
    Error err1 = builder.build(cacheDylibs, patchInfos, patchableClasses, patchableCFObj2, cacheBaseAddress);

    std::vector<uint8_t> buffer(builder.getPatchTableSize());
    Error err2 = builder.write(buffer.data(), buffer.size(), patchTableAddress.rawValue());
    const PatchTable patchTable(buffer.data(), patchTableAddress.rawValue());
    uint64_t numImages = patchTable.numImages();
    uint32_t numExports0 = patchTable.patchableExportCount(0);
    uint32_t numExports1 = patchTable.patchableExportCount(1);
    uint32_t numExports2 = patchTable.patchableExportCount(2);

    // Walk the got uses in each dylib
    std::vector<FoundGOTUse> usesDylib0Export0 = getGOTUsesIn(patchTable, 0, exportVMOffset0_0);
    std::vector<FoundGOTUse> usesDylib0Export1 = getGOTUsesIn(patchTable, 0, exportVMOffset0_1);
    std::vector<FoundGOTUse> usesDylib0Export2 = getGOTUsesIn(patchTable, 0, exportVMOffset0_2);
    std::vector<FoundGOTUse> usesDylib1Export0 = getGOTUsesIn(patchTable, 1, exportVMOffset1_0);
    std::vector<FoundGOTUse> usesDylib1Export1 = getGOTUsesIn(patchTable, 1, exportVMOffset1_1);
    std::vector<FoundGOTUse> usesDylib1Export2 = getGOTUsesIn(patchTable, 1, exportVMOffset1_2);
    std::vector<FoundGOTUse> usesDylib1ExportN = getGOTUsesIn(patchTable, 1, VMOffset(0x8000000ULL));

    // Check the number of exports from each dylib
    XCTAssertTrue(numImages == 3);
    XCTAssertTrue(numExports0 == 3);
    XCTAssertTrue(numExports1 == 3);
    XCTAssertTrue(numExports2 == 0);

    // Dylib 0 export 0 is used 3 times from dylib 1 and 3 times from dylib 2
    {
        XCTAssertTrue(usesDylib0Export0.size() == 6);
        CacheVMAddress useVMAddr1_0 = dylibAddress1 + VMOffset(0x10000ULL);
        CacheVMAddress useVMAddr1_1 = dylibAddress1 + VMOffset(0x20000ULL);
        CacheVMAddress useVMAddr1_2 = dylibAddress1 + VMOffset(0x30000ULL);
        CacheVMAddress useVMAddr2_0 = dylibAddress2 + VMOffset(0x10000ULL);
        CacheVMAddress useVMAddr2_1 = dylibAddress2 + VMOffset(0x20000ULL);
        CacheVMAddress useVMAddr2_2 = dylibAddress2 + VMOffset(0x30000ULL);
        VMOffset useVMOffset1_0 = useVMAddr1_0 - cacheBaseAddress;
        VMOffset useVMOffset1_1 = useVMAddr1_1 - cacheBaseAddress;
        VMOffset useVMOffset1_2 = useVMAddr1_2 - cacheBaseAddress;
        VMOffset useVMOffset2_0 = useVMAddr2_0 - cacheBaseAddress;
        VMOffset useVMOffset2_1 = useVMAddr2_1 - cacheBaseAddress;
        VMOffset useVMOffset2_2 = useVMAddr2_2 - cacheBaseAddress;
        XCTAssertTrue(usesDylib0Export0[0].userCacheVMOffset == useVMOffset1_0.rawValue());
        XCTAssertTrue(usesDylib0Export0[1].userCacheVMOffset == useVMOffset1_1.rawValue());
        XCTAssertTrue(usesDylib0Export0[2].userCacheVMOffset == useVMOffset1_2.rawValue());
        XCTAssertTrue(usesDylib0Export0[3].userCacheVMOffset == useVMOffset2_0.rawValue());
        XCTAssertTrue(usesDylib0Export0[4].userCacheVMOffset == useVMOffset2_1.rawValue());
        XCTAssertTrue(usesDylib0Export0[5].userCacheVMOffset == useVMOffset2_2.rawValue());
    }

    // Dylib 0 export 1 is used 2 times from dylib 1 and 2 times from dylib 2
    {
        XCTAssertTrue(usesDylib0Export1.size() == 4);
        CacheVMAddress useVMAddr1_3 = dylibAddress1 + VMOffset(0x40000ULL);
        CacheVMAddress useVMAddr1_4 = dylibAddress1 + VMOffset(0x50000ULL);
        CacheVMAddress useVMAddr2_3 = dylibAddress2 + VMOffset(0x40000ULL);
        CacheVMAddress useVMAddr2_4 = dylibAddress2 + VMOffset(0x50000ULL);
        VMOffset useVMOffset1_3 = useVMAddr1_3 - cacheBaseAddress;
        VMOffset useVMOffset1_4 = useVMAddr1_4 - cacheBaseAddress;
        VMOffset useVMOffset2_3 = useVMAddr2_3 - cacheBaseAddress;
        VMOffset useVMOffset2_4 = useVMAddr2_4 - cacheBaseAddress;
        XCTAssertTrue(usesDylib0Export1[0].userCacheVMOffset == useVMOffset1_3.rawValue());
        XCTAssertTrue(usesDylib0Export1[1].userCacheVMOffset == useVMOffset1_4.rawValue());
        XCTAssertTrue(usesDylib0Export1[2].userCacheVMOffset == useVMOffset2_3.rawValue());
        XCTAssertTrue(usesDylib0Export1[3].userCacheVMOffset == useVMOffset2_4.rawValue());
    }

    // Dylib 0 export 2 is used 1 time from dylib 1 and 1 time from dylib 2
    {
        XCTAssertTrue(usesDylib0Export2.size() == 2);
        CacheVMAddress useVMAddr1_5 = dylibAddress1 + VMOffset(0x60000ULL);
        CacheVMAddress useVMAddr2_5 = dylibAddress2 + VMOffset(0x60000ULL);
        VMOffset useVMOffset1_5 = useVMAddr1_5 - cacheBaseAddress;
        VMOffset useVMOffset2_5 = useVMAddr2_5 - cacheBaseAddress;
        XCTAssertTrue(usesDylib0Export2[0].userCacheVMOffset == useVMOffset1_5.rawValue());
        XCTAssertTrue(usesDylib0Export2[1].userCacheVMOffset == useVMOffset2_5.rawValue());
    }

    // Dylib 1 export 0 is used 0 times from dylib 0 and 3 times from dylib 2
    {
        XCTAssertTrue(usesDylib1Export0.size() == 3);
        CacheVMAddress useVMAddr2_0 = dylibAddress2 + VMOffset(0x110000ULL);
        CacheVMAddress useVMAddr2_1 = dylibAddress2 + VMOffset(0x120000ULL);
        CacheVMAddress useVMAddr2_2 = dylibAddress2 + VMOffset(0x130000ULL);
        VMOffset useVMOffset2_0 = useVMAddr2_0 - cacheBaseAddress;
        VMOffset useVMOffset2_1 = useVMAddr2_1 - cacheBaseAddress;
        VMOffset useVMOffset2_2 = useVMAddr2_2 - cacheBaseAddress;
        XCTAssertTrue(usesDylib1Export0[0].userCacheVMOffset == useVMOffset2_0.rawValue());
        XCTAssertTrue(usesDylib1Export0[1].userCacheVMOffset == useVMOffset2_1.rawValue());
        XCTAssertTrue(usesDylib1Export0[2].userCacheVMOffset == useVMOffset2_2.rawValue());
    }

    // Dylib 1 export 1 is used 0 times from dylib 0 and 2 times from dylib 2
    {
        XCTAssertTrue(usesDylib1Export1.size() == 2);
        CacheVMAddress useVMAddr2_3 = dylibAddress2 + VMOffset(0x140000ULL);
        CacheVMAddress useVMAddr2_4 = dylibAddress2 + VMOffset(0x150000ULL);
        VMOffset useVMOffset2_3 = useVMAddr2_3 - cacheBaseAddress;
        VMOffset useVMOffset2_4 = useVMAddr2_4 - cacheBaseAddress;
        XCTAssertTrue(usesDylib1Export1[0].userCacheVMOffset == useVMOffset2_3.rawValue());
        XCTAssertTrue(usesDylib1Export1[1].userCacheVMOffset == useVMOffset2_4.rawValue());
    }

    // Dylib 1 export 2 is used 0 times from dylib 0 and 1 time from dylib 2
    {
        XCTAssertTrue(usesDylib1Export2.size() == 1);
        CacheVMAddress useVMAddr2_5 = dylibAddress2 + VMOffset(0x160000ULL);
        VMOffset useVMOffset2_5 = useVMAddr2_5 - cacheBaseAddress;
        XCTAssertTrue(usesDylib1Export2[0].userCacheVMOffset == useVMOffset2_5.rawValue());
    }

    // Should have no uses of the usesDylib1ExportN list, as it was too high
    // This list also verifies that the binary search for GOTs works when the address passed in is
    // higher than all GOT offsets
    XCTAssertTrue(usesDylib1ExportN.empty());
}

@end
