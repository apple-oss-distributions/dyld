//
//  CRC32cTestCase.m
//  CRC32cTestCase
//
//  Created by Louis Gerbarg on 11/11/21.
//

#include "CRC32c.h"
#import "DyldTestCase.h"

using lsl::CRC32c;

@interface CRC32cTestCase : DyldTestCase

@end

@implementation CRC32cTestCase

/*
 // Test vectors for CRC32c
 00000000​00000000​00000000​00000000​00000000​00000000​00000000​00000000​ AA36918A
 FFFFFFFF​FFFFFFFF​FFFFFFFF​FFFFFFFF​FFFFFFFF​FFFFFFFF​FFFFFFFF​FFFFFFFF​ 43ABA862
 00010203​04050607​08090A0B​0C0D0E0F​10111213​14151617​18191A1B​1C1D1E1F​ 4E79DD46
 */

- (void) checkTestVectors:(CRC32c&)checksumer {
    std::array<std::byte,40> data = {(std::byte)0};
    checksumer.reset();
    for (auto i = 0; i < 32; ++i) { checksumer((uint8_t)0); }
    XCTAssertEqual((uint32_t)checksumer, 0x8A9136AA);
    checksumer.reset();
    for (auto i = 0; i < 32; ++i) { checksumer((uint8_t)0xff); }
    XCTAssertEqual((uint32_t)checksumer, 0x62A8AB43);
    checksumer.reset();
    for (auto i = 0; i < 32; ++i) { checksumer((uint8_t)i); }
    XCTAssertEqual((uint32_t)checksumer, 0x46DD794E);
    checksumer.reset();
    checksumer(std::span(&data[0], 32));
    XCTAssertEqual((uint32_t)checksumer, 0x8A9136AA);
    checksumer.reset();
    checksumer(std::span(&data[1], 32));
    XCTAssertEqual((uint32_t)checksumer, 0x8A9136AA);
    checksumer.reset();
    checksumer(std::span(&data[7], 32));
    XCTAssertEqual((uint32_t)checksumer, 0x8A9136AA);
}

//#if TARGET_OS_OSX && __arm64__
- (void)testHWChecksummer {
    auto checksumer = CRC32c::hardwareChecksumer();
    [self checkTestVectors:checksumer];
}
//#endif

- (void)testSWChecksummer {
    auto checksumer = CRC32c::softwareChecksumer();
    [self checkTestVectors:checksumer];
}

- (void)testChecksummer {
    CRC32c checksumer;
    [self checkTestVectors:checksumer];
}

@end
