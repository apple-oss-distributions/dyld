/*
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


#include <atomic>

#include <assert.h>
#include <mach/mach.h>

#include "Tracing.h"

namespace {
VIS_HIDDEN
static uint64_t elapsed(const time_value_t start, const time_value_t end) {
    uint64_t duration;
    duration = 1000000*(end.seconds - start.seconds);
    duration += (end.microseconds - start.microseconds);
    return duration;
}
}

namespace dyld3 {

VIS_HIDDEN
void kdebug_trace_dyld_image(const uint32_t code,
                       const uuid_t* uuid_bytes,
                       const fsobj_id_t fsobjid,
                       const fsid_t fsid,
                       const mach_header* load_addr)
{
#if __LP64__
    uint64_t *uuid = (uint64_t *)uuid_bytes[0];
    kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, code), uuid[0],
                 uuid[1], (uint64_t)load_addr,
                 (uint64_t)fsid.val[0] | ((uint64_t)fsid.val[1] << 32));
    kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, code + 1),
                 (uint64_t)fsobjid.fid_objno |
                 ((uint64_t)fsobjid.fid_generation << 32),
                 0, 0, 0);
#else /* __LP64__ */
    uint32_t *uuid = (uint32_t *)uuid_bytes[0];
    kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, code + 2), uuid[0],
                 uuid[1], uuid[2], uuid[3]);
    kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, code + 3),
                 (uint32_t)load_addr, fsid.val[0], fsid.val[1],
                 fsobjid.fid_objno);
    kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_UUID, code + 4),
                 fsobjid.fid_generation, 0, 0, 0);
#endif /* __LP64__ */
}

VIS_HIDDEN
void kdebug_trace_dyld_signpost(const uint32_t code, uint64_t data1, uint64_t data2) {
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_SIGNPOST, code))) {
        task_thread_times_info info;
        mach_msg_type_number_t infoSize = sizeof(task_thread_times_info);
        (void)task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t)&info, &infoSize);
        uint64_t user_duration = elapsed({0,0}, info.user_time);
        uint64_t system_duration = elapsed({0,0}, info.system_time);
        kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_SIGNPOST, code), user_duration, system_duration, data1, data2);
    }
}

static std::atomic<uint64_t> trace_pair_id(0);

VIS_HIDDEN
void kdebug_trace_dyld_duration(const uint32_t code, uint64_t data1, uint64_t data2, void (^block)()) {
    //FIXME: We should assert here, but it is verified on our current platforms
    //Re-enabled when we move to C++17 and can use constexpr is_lock_always_free()
    //assert(std::atomic<uint64_t>{}.is_lock_free());
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_TIMING, code))) {
        uint64_t current_trace_id = trace_pair_id++;
        kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_TIMING, code) | DBG_FUNC_START, current_trace_id, 0, data1, data2);
        block();
        kdebug_trace(KDBG_CODE(DBG_DYLD, DBG_DYLD_TIMING, code) | DBG_FUNC_END, current_trace_id, 0, data1, data2);
    } else {
        block();
    }
}

void kdebug_trace_print(const uint32_t code, const char *string) {
    if (kdebug_is_enabled(KDBG_CODE(DBG_DYLD, DBG_DYLD_PRINT, code))) {
        kdebug_trace_string(KDBG_CODE(DBG_DYLD, DBG_DYLD_PRINT, code), 0, string);
    }
}
};
