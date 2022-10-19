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

#import "DyldTestCase.h"

TestState::TestState(SyscallDelegate& sys, MockO& main, const std::vector<const char*>& envp, const std::vector<const char*>& apple)
    : _defaultMain(MH_EXECUTE, "arm64"), _osDelegate(sys), _kernArgs(main.header(), {"test.exe"}, envp, apple),
      _allocator(Allocator::persistentAllocator()), _config(&_kernArgs, _osDelegate, _allocator), apis(_config, _allocator)
{
}

TestState::TestState(SyscallDelegate& sys, const std::vector<const char*>& envp, const std::vector<const char*>& apple)
    : _defaultMain(MH_EXECUTE, "arm64"), _osDelegate(sys), _kernArgs(_defaultMain.header(), {"test.exe"}, envp, apple),
      _allocator(Allocator::persistentAllocator()), _config(&_kernArgs, _osDelegate, _allocator), apis(_config, _allocator)
{
}

TestState::TestState(MockO& main, const std::vector<const char*>& envp, const std::vector<const char*>& apple)
    :  _defaultMain(MH_EXECUTE, "arm64"), _kernArgs(main.header(), {"test.exe"}, envp, apple),
      _allocator(Allocator::persistentAllocator()), _config(&_kernArgs, _osDelegate, _allocator), apis(_config, _allocator)
{
}

TestState::TestState(const std::vector<const char*>& envp)
    :  _defaultMain(MH_EXECUTE, "arm64"), _kernArgs(_defaultMain.header(), {"test.exe"}, envp, {"executable_path=/foo/test.exe"}),
      _allocator(Allocator::persistentAllocator()), _config(&_kernArgs, _osDelegate, _allocator), apis(_config, _allocator)
{
}

@implementation DyldTestCase {
    std::mt19937_64     _mt;
    uint64_t            _seed;
}

- (instancetype)initWithInvocation:(nullable NSInvocation *)invocation {
    self = [super initWithInvocation:invocation];
    
    if (self) {
        self.continueAfterFailure = false;
        _seed = clock_gettime_nsec_np(CLOCK_REALTIME);
    }
    
    return self;
}

- (void)recordIssue:(XCTIssue *)issue {
    XCTMutableIssue* newIssue = [issue mutableCopy];
    newIssue.compactDescription = [NSString stringWithFormat:@"%@ (randomSeed: %llu)", issue.compactDescription, _seed];
    [super recordIssue:newIssue];
}

- (void)performTest:(XCTestRun *)run  {
    std::uniform_int_distribution<uint64_t> dist(0,std::numeric_limits<uint64_t>::max());
    _mt.seed(_seed);
    [super performTest:run];
}

- (void) setRandomSeed:(uint64_t)seed {
    _seed = seed;
    _mt.seed(_seed);
}


- (bool) randomBool {
    std::uniform_int_distribution<bool> dist(0, 1);
    return dist(_mt);
}

- (uint64_t) uniformRandomFrom:(uint64_t)lowerBound to:(uint64_t)upperBound {
    std::uniform_int_distribution<uint64_t> dist(lowerBound, upperBound);
    return dist(_mt);
}

@end
