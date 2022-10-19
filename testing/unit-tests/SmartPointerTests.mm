//
//  SmartPointerTests.m
//  SmartPointerTests
//
//  Created by Louis Gerbarg on 10/2/21.
//

#import "DyldTestCase.h"
#import "Allocator.h"
#import "Vector.h"

using namespace lsl;

@interface SmartPointerTests : DyldTestCase

@end

@implementation SmartPointerTests

struct TestSmartPointerVirtualCastsA {
    static uint64_t count;
    enum State {
        ConstructingA,
        ConstructingB,
        DestructingB,
        DestructingA
    };
    uintptr_t a;
    virtual bool test() = 0;
    bool testWrapper() {
        return test();
    }
    virtual ~TestSmartPointerVirtualCastsA() {
        assert(state == DestructingB);
        state = DestructingA;
        ++count;
    }
    State state;
    TestSmartPointerVirtualCastsA() {
        state = ConstructingA;
    }
    static uint64_t getDestructCount() {
        return count;
    }
};

uint64_t TestSmartPointerVirtualCastsA::count = 0;

struct TestSmartPointerVirtualCastsB : TestSmartPointerVirtualCastsA {
    uintptr_t b;
    bool test() override {
        return true;
    }
    virtual ~TestSmartPointerVirtualCastsB() {
        assert(state == ConstructingB);
        state = DestructingB;
    }
    TestSmartPointerVirtualCastsB() {
        state = ConstructingB;
    }
};

static bool testWrapperUnique(UniquePtr<TestSmartPointerVirtualCastsA>& a) {
    return a->testWrapper();
}

static bool testWrapperShared(SharedPtr<TestSmartPointerVirtualCastsA>& a) {
    return a->testWrapper();
}

- (void) testSmartPointerVirtualCasts {
    auto allocator = EphemeralAllocator();
    {
        UniquePtr<TestSmartPointerVirtualCastsB> b = allocator.makeUnique<TestSmartPointerVirtualCastsB>();
        UniquePtr<TestSmartPointerVirtualCastsA> a = std::move(b);
        XCTAssertTrue(testWrapperUnique(a), "Virtual disptch should work through type coercions");
    }
    XCTAssertEqual(TestSmartPointerVirtualCastsA::getDestructCount(), 1);
    {
        SharedPtr<TestSmartPointerVirtualCastsB> b = allocator.makeShared<TestSmartPointerVirtualCastsB>();
        SharedPtr<TestSmartPointerVirtualCastsA> a;
        a = b;
        SharedPtr<TestSmartPointerVirtualCastsA> c;
        c = a;
        c = b;
        XCTAssertTrue(testWrapperShared(a), "Virtual disptch should work through type coercions");
    }
    XCTAssertEqual(TestSmartPointerVirtualCastsA::getDestructCount(), 2);
}

struct TestStruct {
    TestStruct(bool& D) : destructed(D) {}
    TestStruct(bool& DE, uint32_t A, uint32_t B, uint32_t C, uint32_t D) :  destructed(DE), a(A), b(B), c(C), d(D) {}
    ~TestStruct() {
        destructed = true;
    }
    bool& destructed;
    uint32_t a = 0;
    uint32_t b = 1;
    uint32_t c = 2;
    uint32_t d = 3;
};

- (void) testUniquePtr {
    auto allocator      = EphemeralAllocator(alloca(1024),1024);
    bool destructed1    = false;
    bool destructed2    = false;
    bool destructed3    = false;
    {
        UniquePtr<TestStruct> purposefullyUnusedToTestNullHandling;
        auto test1 = allocator.makeUnique<TestStruct>(destructed1);
        auto test2 = allocator.makeUnique<TestStruct>(destructed2, 4, 5, 6, 7);
        XCTAssertEqual(test1->a, 0);
        XCTAssertEqual(test1->b, 1);
        XCTAssertEqual(test1->c, 2);
        XCTAssertEqual(test1->d, 3);
        XCTAssertEqual(test2->a, 4);
        XCTAssertEqual(test2->b, 5);
        XCTAssertEqual(test2->c, 6);
        XCTAssertEqual(test2->d, 7);

        test1->a = 8;
        test1->b = 9;
        test1->c = 10;
        test1->d = 11;

        XCTAssertEqual(test1->a, 8);
        XCTAssertEqual(test1->b, 9);
        XCTAssertEqual(test1->c, 10);
        XCTAssertEqual(test1->d, 11);

        UniquePtr<TestStruct> test0;
        test0 = std::move(test1);

        XCTAssert((bool)test1 == false);
        XCTAssertEqual(test0->a, 8);
        XCTAssertEqual(test0->b, 9);
        XCTAssertEqual(test0->c, 10);
        XCTAssertEqual(test0->d, 11);
        XCTAssertFalse(destructed2);
        test2 = allocator.makeUnique<TestStruct>(destructed3);
        XCTAssertTrue(destructed2);
        XCTAssertFalse(destructed3);
        test2 = nullptr;
        XCTAssertTrue(destructed3);
        XCTAssertFalse(destructed1);
    }
    XCTAssertTrue(destructed1);
    XCTAssertTrue(destructed2);
    XCTAssertTrue(destructed3);
    XCTAssertEqual(allocator.allocated_bytes(), 0);
}

- (void) testSharedPtrTraffic {
    auto allocator = EphemeralAllocator();
    auto value      = allocator.makeShared<uint64_t>(0);
    auto pointers   = Vector<SharedPtr<uint64_t>>(allocator);
    pointers.reserve(1024*1024);
    for (auto i = 0; i < 1024*1024; ++i) {
        pointers.push_back(value);
    }
}

- (void) testSharedPtr {
    auto allocator = EphemeralAllocator();
    bool destructed1 = false;
    bool destructed2 = false;
    bool destructed3 = false;
    {
        SharedPtr<TestStruct> purposefullyUnusedToTestNullHandling;
        auto testU = purposefullyUnusedToTestNullHandling;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
        testU = testU;
#pragma clang diagnostic pop
        auto test1 = allocator.makeShared<TestStruct>(destructed1);
        auto test2 = allocator.makeShared<TestStruct>(destructed2, 4, 5, 6, 7);
        XCTAssertEqual(test1->a, 0);
        XCTAssertEqual(test1->b, 1);
        XCTAssertEqual(test1->c, 2);
        XCTAssertEqual(test1->d, 3);
        XCTAssertEqual(test2->a, 4);
        XCTAssertEqual(test2->b, 5);
        XCTAssertEqual(test2->c, 6);
        XCTAssertEqual(test2->d, 7);

        test1->a = 8;
        test1->b = 9;
        test1->c = 10;
        test1->d = 11;

        XCTAssertEqual(test1->a, 8);
        XCTAssertEqual(test1->b, 9);
        XCTAssertEqual(test1->c, 10);
        XCTAssertEqual(test1->d, 11);

        SharedPtr<TestStruct> test0;
        test0 = std::move(test1);

        XCTAssert((bool)test1 == false);
        XCTAssertEqual(test0->a, 8);
        XCTAssertEqual(test0->b, 9);
        XCTAssertEqual(test0->c, 10);
        XCTAssertEqual(test0->d, 11);

        test1 = test0;

        XCTAssertEqual(test1->a, 8);
        XCTAssertEqual(test1->b, 9);
        XCTAssertEqual(test1->c, 10);
        XCTAssertEqual(test1->d, 11);

        test1.withUnsafe([&](auto x) {
            test1.withUnsafe([&](auto y) {
                XCTAssertEqual(x, y);
            });
        });
        XCTAssertFalse(destructed2);
        test2 = allocator.makeShared<TestStruct>(destructed3);
        XCTAssertTrue(destructed2);
        XCTAssertFalse(destructed1);
        XCTAssertFalse(destructed3);
        test0 = nullptr;
        XCTAssertFalse(destructed1);
        test1 = nullptr;
        XCTAssertTrue(destructed1);
    }
    XCTAssertTrue(destructed1);
    XCTAssertTrue(destructed2);
    XCTAssertTrue(destructed3);
}

- (void) testAllocatorNullHandling {
    // There are no XCTAsserts here, if test does not crash it passed
    auto allocator = EphemeralAllocator();
    UniquePtr<char> nullUnique = nullptr;
    SharedPtr<char> nullShared = nullptr;
}
@end
