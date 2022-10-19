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
#include <random>

#include "DyldRuntimeState.h"
#include "DyldProcessConfig.h"
#include "DyldAPIs.h"
#include "MockO.h"


NS_ASSUME_NONNULL_BEGIN

using lsl::Allocator;
using dyld4::RuntimeState;
using dyld4::FileManager;
using dyld4::KernelArgs;
using dyld4::ProcessConfig;
using dyld4::APIs;
using dyld4::SyscallDelegate;
using dyld3::Platform;
using dyld3::MachOFile;

//
// This tests methods in the APIs class.  Thus, by calling tester.apis.dlopen()
// it is calling the dlopen linked into the unit test, and not the OS dlopen().
//
class TestState {
public:
                         TestState(SyscallDelegate& sys, MockO& main, const std::vector<const char*>& envp={},
                                   const std::vector<const char*>& apple={"executable_path=/foo/test.exe"});
                         TestState(SyscallDelegate& sys, const std::vector<const char*>& envp, const std::vector<const char*>& apple);
                         TestState(MockO& main, const std::vector<const char*>& envp={},
                                   const std::vector<const char*>& apple={"executable_path=/foo/test.exe"});
                         TestState(const std::vector<const char*>& envp={});
    SyscallDelegate&     osDelegate()   { return _osDelegate; }

private:
    MockO                _defaultMain;
    SyscallDelegate      _osDelegate;
    KernelArgs           _kernArgs;
    Allocator&           _allocator;
    ProcessConfig        _config;
public:
    APIs                 apis;
};

@interface DyldTestCase : XCTestCase

- (void) setRandomSeed:(uint64_t)seed;
- (bool) randomBool;
- (uint64_t) uniformRandomFrom:(uint64_t)lowerBound to:(uint64_t)upperBound;
@end

NS_ASSUME_NONNULL_END
