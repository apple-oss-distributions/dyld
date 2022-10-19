//
//  PVLEInt64Test.m
//  PVLEInt64Test
//
//  Created by Louis Gerbarg on 11/13/21.
//

#include "Vector.h"
#include "PVLEInt64.h"

#import "DyldTestCase.h"

using lsl::Vector;
using lsl::Allocator;
using lsl::EphemeralAllocator;

@interface PVLEInt64Test : DyldTestCase

@end

@implementation PVLEInt64Test

- (void) testPVLEUInt64 {
    EphemeralAllocator allocator;
    Vector<uint64_t>    ints(allocator);
    Vector<std::byte>     intStream(allocator);
    for (auto i = 0; i < 1000000; ++i) {
        //        auto value = [self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()];
        auto value = [self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()];

        ints.push_back(value);
        lsl::emitPVLEUInt64(value, intStream);
    }
    std::span<std::byte> i = intStream;
    for (auto uint64 : ints) {
        auto value = lsl::readPVLEUInt64(i);
        XCTAssertEqual(value, uint64);
    }
}

- (void) testPVLEInt64 {
    EphemeralAllocator allocator;
    Vector<int64_t>     ints(allocator);
    Vector<std::byte>   intStream(allocator);
    for (auto i = 0; i < 1000000; ++i) {
        //        auto value = [self uniformRandomFrom:0 to:std::numeric_limits<uint64_t>::max()];
        auto value = [self uniformRandomFrom:std::numeric_limits<int64_t>::min() to:std::numeric_limits<int64_t>::max()];

        ints.push_back(value);
        lsl::emitPVLEInt64(value, intStream);
    }
    std::span<std::byte> i = intStream;
    for (auto int64 : ints) {
        auto value = lsl::readPVLEInt64(i);
        XCTAssertEqual(value, int64);
    }
}

@end
