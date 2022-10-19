/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
* Copyright (c) 2017 Apple Inc. All rights reserved.
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

#include "Optimizers.h"

using namespace cache_builder;


//
// MARK: --- StubOptimizer methods ---
//

// These are functions that are interposed by Instruments.app or ASan
const char* const neverStubEliminateSymbols[] = {
    "___bzero",
    "___cxa_atexit",
    "___cxa_throw",
    "__longjmp",
    "__objc_autoreleasePoolPop",
    "_accept",
    "_access",
    "_asctime",
    "_asctime_r",
    "_asprintf",
    "_atoi",
    "_atol",
    "_atoll",
    "_calloc",
    "_chmod",
    "_chown",
    "_close",
    "_confstr",
    "_cp_drawable_present",
    "_cp_drawable_encode_present",
    "_cp_drawable_enqueue_present",
    "_ctime",
    "_ctime_r",
    "_dispatch_after",
    "_dispatch_after_f",
    "_dispatch_async",
    "_dispatch_async_f",
    "_dispatch_barrier_async_f",
    "_dispatch_group_async",
    "_dispatch_group_async_f",
    "_dispatch_source_set_cancel_handler",
    "_dispatch_source_set_event_handler",
    "_dispatch_sync_f",
    "_dlclose",
    "_dlopen",
    "_dup",
    "_dup2",
    "_endgrent",
    "_endpwent",
    "_ether_aton",
    "_ether_hostton",
    "_ether_line",
    "_ether_ntoa",
    "_ether_ntohost",
    "_fchmod",
    "_fchown",
    "_fclose",
    "_fdopen",
    "_fflush",
    "_fopen",
    "_fork",
    "_fprintf",
    "_free",
    "_freopen",
    "_frexp",
    "_frexpf",
    "_frexpl",
    "_fscanf",
    "_fstat",
    "_fstatfs",
    "_fstatfs64",
    "_fsync",
    "_ftime",
    "_getaddrinfo",
    "_getattrlist",
    "_getcwd",
    "_getgrent",
    "_getgrgid",
    "_getgrgid_r",
    "_getgrnam",
    "_getgrnam_r",
    "_getgroups",
    "_gethostbyaddr",
    "_gethostbyname",
    "_gethostbyname2",
    "_gethostent",
    "_getifaddrs",
    "_getitimer",
    "_getnameinfo",
    "_getpass",
    "_getpeername",
    "_getpwent",
    "_getpwnam",
    "_getpwnam_r",
    "_getpwuid",
    "_getpwuid_r",
    "_getsockname",
    "_getsockopt",
    "_gmtime",
    "_gmtime_r",
    "_if_indextoname",
    "_if_nametoindex",
    "_index",
    "_inet_aton",
    "_inet_ntop",
    "_inet_pton",
    "_initgroups",
    "_ioctl",
    "_lchown",
    "_lgamma",
    "_lgammaf",
    "_lgammal",
    "_link",
    "_listxattr",
    "_localtime",
    "_localtime_r",
    "_longjmp",
    "_lseek",
    "_lstat",
    "_malloc",
    "_malloc_create_zone",
    "_malloc_default_purgeable_zone",
    "_malloc_default_zone",
    "_malloc_destroy_zone",
    "_malloc_good_size",
    "_malloc_make_nonpurgeable",
    "_malloc_make_purgeable",
    "_malloc_set_zone_name",
    "_malloc_zone_from_ptr",
    "_mbsnrtowcs",
    "_mbsrtowcs",
    "_mbstowcs",
    "_memchr",
    "_memcmp",
    "_memcpy",
    "_memmove",
    "_memset",
    "_mktime",
    "_mlock",
    "_mlockall",
    "_modf",
    "_modff",
    "_modfl",
    "_munlock",
    "_munlockall",
    "_objc_autoreleasePoolPop",
    "_objc_setProperty",
    "_objc_setProperty_atomic",
    "_objc_setProperty_atomic_copy",
    "_objc_setProperty_nonatomic",
    "_objc_setProperty_nonatomic_copy",
    "_objc_storeStrong",
    "_open",
    "_opendir",
    "_poll",
    "_posix_memalign",
    "_pread",
    "_printf",
    "_pthread_attr_getdetachstate",
    "_pthread_attr_getguardsize",
    "_pthread_attr_getinheritsched",
    "_pthread_attr_getschedparam",
    "_pthread_attr_getschedpolicy",
    "_pthread_attr_getscope",
    "_pthread_attr_getstack",
    "_pthread_attr_getstacksize",
    "_pthread_cond_broadcast",
    "_pthread_condattr_getpshared",
    "_pthread_cond_signal",
    "_pthread_cond_signal_thread_np",
    "_pthread_cond_timedwait_relative_np",
    "_pthread_cond_timedwait",
    "_pthread_cond_wait",
    "_pthread_create",
    "_pthread_getschedparam",
    "_pthread_join",
    "_pthread_mutex_lock",
    "_pthread_mutex_unlock",
    "_pthread_mutexattr_getprioceiling",
    "_pthread_mutexattr_getprotocol",
    "_pthread_mutexattr_getpshared",
    "_pthread_mutexattr_gettype",
    "_pthread_rwlockattr_getpshared",
    "_pthread_rwlock_rdlock",
    "_pthread_rwlock_wrlock",
    "_pthread_rwlock_unlock",
    "_pwrite",
    "_rand_r",
    "_read",
    "_readdir",
    "_readdir_r",
    "_readv",
    "_readv$UNIX2003",
    "_realloc",
    "_realpath",
    "_recv",
    "_recvfrom",
    "_recvmsg",
    "_remquo",
    "_remquof",
    "_remquol",
    "_scanf",
    "_send",
    "_sendmsg",
    "_sendto",
    "_setattrlist",
    "_setgrent",
    "_setitimer",
    "_setlocale",
    "_setpwent",
    "_shm_open",
    "_shm_unlink",
    "_sigaction",
    "_sigemptyset",
    "_sigfillset",
    "_siglongjmp",
    "_signal",
    "_sigpending",
    "_sigprocmask",
    "_sigwait",
    "_snprintf",
    "_sprintf",
    "_sscanf",
    "_stat",
    "_statfs",
    "_statfs64",
    "_strcasecmp",
    "_strcat",
    "_strchr",
    "_strcmp",
    "_strcpy",
    "_strdup",
    "_strerror",
    "_strerror_r",
    "_strlen",
    "_strncasecmp",
    "_strncat",
    "_strncmp",
    "_strncpy",
    "_strptime",
    "_strtoimax",
    "_strtol",
    "_strtoll",
    "_strtoumax",
    "_tempnam",
    "_time",
    "_times",
    "_tmpnam",
    "_tsearch",
    "_unlink",
    "_valloc",
    "_vasprintf",
    "_vfprintf",
    "_vfscanf",
    "_vprintf",
    "_vscanf",
    "_vsnprintf",
    "_vsprintf",
    "_vsscanf",
    "_wait",
    "_wait$UNIX2003",
    "_wait3",
    "_wait4",
    "_waitid",
    "_waitid$UNIX2003",
    "_waitpid",
    "_waitpid$UNIX2003",
    "_wcslen",
    "_wcsnrtombs",
    "_wcsrtombs",
    "_wcstombs",
    "_wordexp",
    "_write",
    "_writev",
    "_writev$UNIX2003",
    "_xpc_connection_send_message_with_reply_sync",
    // <rdar://problem/22050956> always use stubs for C++ symbols that can be overridden
    "__ZdaPv",
    "__ZdlPv",
    "__Znam",
    "__Znwm",

    nullptr
};

void StubOptimizer::addDefaultSymbols()
{
    for (const char* const* p=neverStubEliminateSymbols; *p != nullptr; ++p)
        neverStubEliminate.insert(*p);
}


uint64_t StubOptimizer::gotAddrFromArm64Stub(Diagnostics& diag, std::string_view dylibID,
                                             const uint8_t* stubInstructions, uint64_t stubVMAddr)
{
    uint32_t stubInstr1 = *(uint32_t*)stubInstructions;
    if ( (stubInstr1 & 0x9F00001F) != 0x90000010 ) {
        diag.warning("first instruction of stub (0x%08X) is not ADRP for stub at addr 0x%0llX in %s",
                     stubInstr1, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    int32_t adrpValue = ((stubInstr1 & 0x00FFFFE0) >> 3) | ((stubInstr1 & 0x60000000) >> 29);
    if ( stubInstr1 & 0x00800000 )
        adrpValue |= 0xFFF00000;
    uint32_t stubInstr2 = *(uint32_t*)(stubInstructions + 4);
    if ( (stubInstr2 & 0xFFC003FF) != 0xF9400210 ) {
        diag.warning("second instruction of stub (0x%08X) is not LDR for stub at addr 0x%0llX in %s",
                     stubInstr2, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    uint32_t ldrValue = ((stubInstr2 >> 10) & 0x00000FFF);
    return (stubVMAddr & (-4096)) + adrpValue*4096 + ldrValue*8;
}

void StubOptimizer::generateArm64StubTo(uint8_t* stubBuffer,
                                        uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000010) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF);
    uint32_t newADD  = (0x91000210) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     //      ADRP   X16, target@page
    stubInstructions[1] = newADD;      //      ADD    X16, X16, target@pageoff
    stubInstructions[2] = 0xD61F0200;  //      BR     X16
}

void StubOptimizer::generateArm64StubToGOT(uint8_t* stubBuffer,
                                           uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000010) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF) >> 3;
    uint32_t newLDR  = (0xF9400210) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     // ADRP  X16, lazy_pointer@page
    stubInstructions[1] = newLDR;      // LDR   X16, [X16, lazy_pointer@pageoff]
    stubInstructions[2] = 0xD61F0200;  // BR    X16
}

uint64_t StubOptimizer::gotAddrFromArm64_32Stub(Diagnostics& diag, std::string_view dylibID,
                                                const uint8_t* stubInstructions, uint64_t stubVMAddr)
{
    uint32_t stubInstr1 = *(uint32_t*)stubInstructions;
    if ( (stubInstr1 & 0x9F00001F) != 0x90000010 ) {
        diag.warning("first instruction of stub (0x%08X) is not ADRP for stub at addr 0x%0llX in %s",
                     stubInstr1, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    int32_t adrpValue = ((stubInstr1 & 0x00FFFFE0) >> 3) | ((stubInstr1 & 0x60000000) >> 29);
    if ( stubInstr1 & 0x00800000 )
        adrpValue |= 0xFFF00000;
    uint32_t stubInstr2 = *(uint32_t*)(stubInstructions + 4);
    if ( (stubInstr2 & 0xFFC003FF) != 0xB9400210 ) {
        diag.warning("second instruction of stub (0x%08X) is not LDR for stub at addr 0x%0llX in %s",
                     stubInstr2, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    uint32_t ldrValue = ((stubInstr2 >> 10) & 0x00000FFF);
    return (stubVMAddr & (-4096)) + adrpValue*4096 + ldrValue*4; // LDR Wn has a scale factor of 4

}

void StubOptimizer::generateArm64_32StubTo(uint8_t* stubBuffer,
                                        uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000010) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF);
    uint32_t newADD  = (0x91000210) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     //      ADRP   X16, target@page
    stubInstructions[1] = newADD;      //      ADD    X16, X16, target@pageoff
    stubInstructions[2] = 0xD61F0200;  //      BR     X16
}

void StubOptimizer::generateArm64_32StubToGOT(uint8_t* stubBuffer,
                                              uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000010) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF) >> 2;
    uint32_t newLDR  = (0xB9400210) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     // ADRP  X16, lazy_pointer@page
    stubInstructions[1] = newLDR;      // LDR   W16, [X16, lazy_pointer@pageoff]
    stubInstructions[2] = 0xD61F0200;  // BR    X16
}

uint64_t StubOptimizer::gotAddrFromArm64eStub(Diagnostics& diag, std::string_view dylibID,
                                              const uint8_t* stubInstructions, uint64_t stubVMAddr)
{
    uint32_t stubInstr1 = *(uint32_t*)stubInstructions;
    // ADRP  X17, dyld_ImageLoaderCache@page
    if ( (stubInstr1 & 0x9F00001F) != 0x90000011 ) {
        diag.warning("first instruction of stub (0x%08X) is not ADRP for stub at addr 0x%0llX in %s",
                     stubInstr1, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    int32_t adrpValue = ((stubInstr1 & 0x00FFFFE0) >> 3) | ((stubInstr1 & 0x60000000) >> 29);
    if ( stubInstr1 & 0x00800000 )
        adrpValue |= 0xFFF00000;

    // ADD     X17, X17, dyld_ImageLoaderCache@pageoff
    uint32_t stubInstr2 = *(uint32_t*)(stubInstructions + 4);
    if ( (stubInstr2 & 0xFFC003FF) != 0x91000231 ) {
        diag.warning("second instruction of stub (0x%08X) is not ADD for stub at addr 0x%0llX in %s",
                     stubInstr2, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    uint32_t addValue = ((stubInstr2 & 0x003FFC00) >> 10);

    // LDR   X16, [X17]
    uint32_t stubInstr3 = *(uint32_t*)(stubInstructions + 8);
    if ( stubInstr3 != 0xF9400230 ) {
        diag.warning("second instruction of stub (0x%08X) is not LDR for stub at addr 0x%0llX in %s",
                     stubInstr2, (uint64_t)stubVMAddr, dylibID.data());
        return 0;
    }
    return (stubVMAddr & (-4096)) + adrpValue*4096 + addValue;
}

void StubOptimizer::generateArm64eStubTo(uint8_t* stubBuffer,
                                         uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000010) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF);
    uint32_t newADD  = (0x91000210) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     //      ADRP   X16, target@page
    stubInstructions[1] = newADD;      //      ADD    X16, X16, target@pageoff
    stubInstructions[2] = 0xD61F0200;  //      BR     X16
    stubInstructions[3] = 0xD4200020;  //      TRAP
}

void StubOptimizer::generateArm64eStubToGOT(uint8_t* stubBuffer,
                                            uint64_t stubVMAddr, uint64_t targetVMAddr)
{
    int64_t adrpDelta = (targetVMAddr & -4096) - (stubVMAddr & -4096);

    uint32_t immhi   = (adrpDelta >> 9) & (0x00FFFFE0);
    uint32_t immlo   = (adrpDelta << 17) & (0x60000000);
    uint32_t newADRP = (0x90000011) | immlo | immhi;
    uint32_t off12   = (targetVMAddr & 0xFFF);
    uint32_t newADD  = (0x91000231) | (off12 << 10);

    uint32_t* stubInstructions = (uint32_t*)stubBuffer;
    stubInstructions[0] = newADRP;     // ADRP  X17, lazy_pointer@page
    stubInstructions[1] = newADD;      // ADD   X17, X17, lazy_pointer@pageoff
    stubInstructions[2] = 0xF9400230;  // LDR   X16, [X17]
    stubInstructions[3] = 0xD71F0A11;  // BRAA  X16, X17
}
