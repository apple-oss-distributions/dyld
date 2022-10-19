/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
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

#import <XCTest/XCTest.h>

#include "BumpAllocator.h"
#include "DyldRuntimeState.h"
#include "JustInTimeLoader.h"
#include "PrebuiltSwift.h"
#include "OptimizerSwift.h"
#include "Map.h"
#include "MockO.h"

using dyld4::PrebuiltSwift;
using dyld4::TypeProtocolMap;
using dyld4::MetadataProtocolMap;
using dyld4::ForeignProtocolMap;

using dyld4::EqualTypeConformanceKey;
using dyld4::EqualTypeConformanceLookupKey;
using dyld4::EqualForeignConformanceLookupKey;

extern const dyld3::MachOAnalyzer __dso_handle;

@interface PreBuiltSwiftTests : XCTestCase
@end

//
// The PreBuiltSwiftTester is a utility for wrapping up the KernArgs and delegate needed
// to test the PreBuiltSwift class.
//
class PreBuiltSwiftTester
{

public:
    PreBuiltSwiftTester(const std::vector<const char*>& envp={});

    dyld4::RuntimeState                         _state;

private:
    MockO                                       _mockO;
    dyld4::SyscallDelegate                      _osDelegate;
    dyld4::KernelArgs                           _kernArgs;
    lsl::Allocator&                             _allocator;
    dyld4::ProcessConfig                        _config;
};

PreBuiltSwiftTester::PreBuiltSwiftTester(const std::vector<const char*>& envp)
    : _state(_config),
      _mockO(MH_EXECUTE, "arm64"), _kernArgs(_mockO.header(), {"test.exe"}, envp, {"executable_path=/foo/test.exe"}),
      _allocator(dyld4::Allocator::persistentAllocator()), _config(&_kernArgs, _osDelegate, _allocator)
{
    
}

@implementation PreBuiltSwiftTests

- (void)setUp {
    // Put setup code here. This method is called before the invocation of each test method in the class.
    self.continueAfterFailure = false;
}

- (void)tearDown {
    // Put teardown code here. This method is called after the invocation of each test method in the class.
}

- (void)testProtocolConformancesMaps
{
    // Arrange: mock up start up config
    PreBuiltSwiftTester tester;

    // Note we don't use the mach_header here.  It's just to give us a non-null parameter
    dyld4::JustInTimeLoader* testLoader = dyld4::JustInTimeLoader::makeJustInTimeLoader(tester._state, &__dso_handle, "");
    testLoader->ref.app = true;
    testLoader->ref.index = 0;

    // Act: add protocol conformances to be optimized
    dyld4::PrebuiltSwift testPrebuiltSwift;

    SwiftTypeProtocolConformanceDiskLocationKey protoLocKeyA = {
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xA300),
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xA400)
    };
    SwiftTypeProtocolConformanceDiskLocation protoLocA{PrebuiltLoader::BindTargetRef({testLoader, 0xA200})};
    testPrebuiltSwift.typeProtocolConformances.insert({ protoLocKeyA, protoLocA });


    SwiftTypeProtocolConformanceDiskLocationKey protoLocKeyB = {
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xB400),
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xB600)
    };
    SwiftTypeProtocolConformanceDiskLocation protoLocB{PrebuiltLoader::BindTargetRef({nullptr, 0xB100})};
    testPrebuiltSwift.typeProtocolConformances.insert({ protoLocKeyB, protoLocB });


    SwiftTypeProtocolConformanceDiskLocationKey protoLocKeyADup = {
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xD300),
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xD400)
    };
    SwiftTypeProtocolConformanceDiskLocation protoLocADup{PrebuiltLoader::BindTargetRef({testLoader, 0xD200})};
    testPrebuiltSwift.typeProtocolConformances.insert({ protoLocKeyADup, protoLocADup });


    SwiftTypeProtocolConformanceDiskLocationKey protoLocKeyATrueDup = {
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xA300),
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xA400)
    };
    SwiftTypeProtocolConformanceDiskLocation protoLocATrueDup{PrebuiltLoader::BindTargetRef({testLoader, 0xA200})};
    testPrebuiltSwift.typeProtocolConformances.insert({ protoLocKeyATrueDup, protoLocATrueDup });

    dyld4::BumpAllocator allocator;
    dyld4::BumpAllocatorPtr<uint64_t> allocatorPtr(allocator, 0);
    testPrebuiltSwift.typeProtocolConformances.serialize(allocator);
    allocator.align(8);
    dyld4::TypeProtocolMap deserializedMap(&tester._state, allocatorPtr.get());


    // Foreign descriptors
    std::string strA("STR");
    std::string strADup("STR");
    SwiftForeignTypeProtocolConformanceDiskLocationKey foreignKeyA = {
        (uint64_t)strA.c_str(),
        PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)strA.c_str()),
        3,
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xA400)
    };
    SwiftForeignTypeProtocolConformanceDiskLocationKey foreignKeyDupA = {
        (uint64_t)nullptr,
        PrebuiltLoader::BindTargetRef::makeAbsolute((uint64_t)strADup.c_str()),
        3,
        PrebuiltLoader::BindTargetRef::makeAbsolute(0xB400)
    };

    SwiftForeignTypeProtocolConformanceDiskLocation foreignLoc{PrebuiltLoader::BindTargetRef({testLoader, 0xE100})};
    testPrebuiltSwift.foreignProtocolConformances.insert({ foreignKeyA, foreignLoc});

    dyld4::BumpAllocator foreignAllocator;
    dyld4::BumpAllocatorPtr<uint64_t> foreignAllocatorPtr(foreignAllocator, 0);
    testPrebuiltSwift.foreignProtocolConformances.serialize(foreignAllocator);
    foreignAllocator.align(8);
    dyld4::ForeignProtocolMap deserializedForeignMap(&tester._state, foreignAllocatorPtr.get());
    // Assert
    XCTAssertEqual(deserializedMap.array().count(), 4);
    XCTAssertEqual(deserializedMap.array()[0].value.protocolConformance.offset(), 0xA200);
    XCTAssertEqual(deserializedMap.array()[0].value.protocolConformance.loaderRef().index, 0);
    XCTAssertEqual(deserializedMap.array()[0].key.typeDescriptor.offset(), 0xA300);
    XCTAssertEqual(deserializedMap.array()[0].key.protocol.offset(), 0xA400);

    XCTAssertEqual(deserializedMap.array()[1].value.protocolConformance.offset(), 0xB100);
    XCTAssertEqual(deserializedMap.array()[1].value.protocolConformance.loaderRef().index, INT16_MAX);
    XCTAssertEqual(deserializedMap.array()[1].key.typeDescriptor.offset(), 0xB400);
    XCTAssertEqual(deserializedMap.array()[1].key.protocol.offset(), 0xB600);

    XCTAssertEqual(deserializedMap.array()[2].value.protocolConformance.offset(), 0xD200);
    XCTAssertEqual(deserializedMap.array()[2].value.protocolConformance.loaderRef().index, 0);
    XCTAssertEqual(deserializedMap.array()[2].key.typeDescriptor.offset(), 0xD300);
    XCTAssertEqual(deserializedMap.array()[2].key.protocol.offset(), 0xD400);

    XCTAssertEqual(deserializedMap.array()[3].value.protocolConformance.offset(), 0xA200);
    XCTAssertEqual(deserializedMap.array()[3].value.protocolConformance.loaderRef().index, 0);
    XCTAssertEqual(deserializedMap.array()[3].key.typeDescriptor.offset(), 0xA300);
    XCTAssertEqual(deserializedMap.array()[3].key.protocol.offset(), 0xA400);

    auto* foundA = deserializedMap.find(protoLocKeyA);
    XCTAssertNotEqual(foundA, deserializedMap.end());

    auto* nextA = deserializedMap.nextDuplicate(foundA);
    XCTAssertNotEqual(nextA, deserializedMap.end());

    auto* foundADup = deserializedMap.find(protoLocKeyADup);
    auto* nextADup = deserializedMap.nextDuplicate(foundADup);
    XCTAssertNotEqual(nextADup, deserializedMap.end());

    auto* lastADup = deserializedMap.nextDuplicate(nextADup);
    XCTAssertNotEqual(lastADup, deserializedMap.end());

    XCTAssertEqual(deserializedMap.nextDuplicate(lastADup), deserializedMap.end());

    XCTAssertFalse(EqualTypeConformanceLookupKey::equal(foundA->key, nextA->key.typeDescriptor.offset(), nextA->key.protocol.offset(), &tester._state));
    XCTAssertTrue(EqualTypeConformanceKey::equal(foundA->key, nextA->key, &tester._state));
    XCTAssertTrue(EqualTypeConformanceLookupKey::equal(foundA->key, lastADup->key.typeDescriptor.offset(), lastADup->key.protocol.offset(), &tester._state));

    XCTAssertEqual(deserializedMap.nextDuplicate(deserializedMap.find(protoLocKeyB)), deserializedMap.end());

    XCTAssertNotEqual(deserializedForeignMap.find(foreignKeyA), deserializedForeignMap.end());
    XCTAssertNotEqual(deserializedForeignMap.find(foreignKeyDupA), deserializedForeignMap.end());
    XCTAssertFalse(EqualForeignConformanceLookupKey::equal(foreignKeyDupA, "STR", 3, 0xA400, &tester._state));
    XCTAssertFalse(EqualForeignConformanceLookupKey::equal(foreignKeyDupA, "STR", 4, 0xA400, &tester._state));
    XCTAssertTrue(EqualForeignConformanceLookupKey::equal(foreignKeyDupA, "STR", 3, 0xB400, &tester._state));
    XCTAssertFalse(EqualForeignConformanceLookupKey::equal(foreignKeyA, "STR", 3, 0xB400, &tester._state));
}

@end
