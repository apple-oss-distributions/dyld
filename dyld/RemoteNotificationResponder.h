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

#ifndef RemoteNotificationResponder_h
#define RemoteNotificationResponder_h

#if !TARGET_OS_SIMULATOR
#include <mach/mach.h>

namespace dyld4 {
struct RemoteNotificationResponder {
    RemoteNotificationResponder(const RemoteNotificationResponder&) = delete;
    RemoteNotificationResponder(RemoteNotificationResponder&&) = delete;
    RemoteNotificationResponder();
    ~RemoteNotificationResponder();
    void sendMessage(mach_msg_id_t msgId, mach_msg_size_t sendSize, mach_msg_header_t* buffer);
    bool const active() const;
    void blockOnSynchronousEvent(uint32_t event);
private:
    mach_port_t             _namesArray[8] = {0};
    mach_port_name_array_t  _names = (mach_port_name_array_t)&_namesArray[0];
    mach_msg_type_number_t  _namesCnt = 8;
    vm_size_t               _namesSize = 0;
};

}; /* namespace dyld4 */

#endif /* !TARGET_OS_SIMULATOR */
#endif /* RemoteNotificationResponder_h */
