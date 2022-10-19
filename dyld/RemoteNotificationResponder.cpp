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

#include "DebuggerSupport.h"
#include "dyld_process_info_internal.h"

#include "RemoteNotificationResponder.h"

#if !TARGET_OS_SIMULATOR
#define DYLD_PROCESS_INFO_NOTIFY_MAGIC 0x49414E46

namespace dyld4 {

RemoteNotificationResponder::RemoteNotificationResponder() {
    if (gProcessInfo->notifyPorts[0] != DYLD_PROCESS_INFO_NOTIFY_MAGIC) {
        // No notifier found, early out
        _namesCnt = 0;
        return;
    }
    kern_return_t kr = task_dyld_process_info_notify_get(_names, &_namesCnt);
    while (kr == KERN_NO_SPACE) {
        // In the future the SPI may return the size we need, but for now we just double the count. Since we don't want to depend on the
        // return value in _nameCnt we set it to have a minimm of 16, double the inline storage value
        _namesCnt = std::max<uint32_t>(16, 2*_namesCnt);
        _namesSize = _namesCnt*sizeof(mach_port_t);
        kr = vm_allocate(mach_task_self(), (vm_address_t*)&_names, _namesSize, VM_FLAGS_ANYWHERE);
        if (kr != KERN_SUCCESS) {
            // We could not allocate memory, time to error out
            break;
        }
        kr = task_dyld_process_info_notify_get(_names, &_namesCnt);
        if (kr != KERN_SUCCESS) {
            // We failed, so deallocate the memory. If the failures was KERN_NO_SPACE we will loop back and try again
            (void)vm_deallocate(mach_task_self(), (vm_address_t)_names, _namesSize);
            _namesSize = 0;
        }
    }
    if (kr != KERN_SUCCESS) {
        // We failed, set _namesCnt to 0 so nothing else will happen
        _namesCnt = 0;
    }
}

RemoteNotificationResponder::~RemoteNotificationResponder() {
    if (_namesCnt) {
        for (auto i = 0; i < _namesCnt; ++i) {
            (void)mach_port_deallocate(mach_task_self(), _names[i]);
        }
        if (_namesSize != 0) {
            // We are not using inline memory, we need to free it
            (void)vm_deallocate(mach_task_self(), (vm_address_t)_names, _namesSize);
        }
    }
}
void RemoteNotificationResponder::sendMessage(mach_msg_id_t msgId, mach_msg_size_t sendSize, mach_msg_header_t* buffer) {
    if (_namesCnt == 0) { return; }
    // Allocate a port to listen on in this monitoring task
    mach_port_t replyPort = MACH_PORT_NULL;
    mach_port_options_t options = { .flags = MPO_CONTEXT_AS_GUARD | MPO_STRICT, .mpl = { 1 }};
    kern_return_t kr = mach_port_construct(mach_task_self(), &options, (mach_port_context_t)&replyPort, &replyPort);
    if (kr != KERN_SUCCESS) {
        return;
    }
    for (auto i = 0; i < _namesCnt; ++i) {
//            fprintf(stderr, "Sending: %u\n", _names[i]);
        if (_names[i] == MACH_PORT_NULL) { continue; }
        // Assemble a message
        uint8_t replyBuffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
        mach_msg_header_t*     msg = buffer;
        msg->msgh_bits         = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
        msg->msgh_id           = msgId;
        msg->msgh_local_port   = replyPort;
        msg->msgh_remote_port  = _names[i];
        msg->msgh_reserved     = 0;
        msg->msgh_size         = sendSize;
        kr = mach_msg_overwrite(msg, MACH_SEND_MSG | MACH_RCV_MSG, msg->msgh_size, sizeof(replyBuffer), replyPort, 0, MACH_PORT_NULL,
                                 (mach_msg_header_t*)&replyBuffer[0], 0);
        if (kr != KERN_SUCCESS) {
            // Send failed, we may have been psuedo recieved. destroy the message
            (void)mach_msg_destroy(msg);
            // Mark the port as null. It does not matter why we failed... if it is s single message we will not retry, if it
            // is a fragmented message then subsequent messages will not decode correctly
            _names[i] = MACH_PORT_NULL;
        }
    }
    (void)mach_port_destruct(mach_task_self(), replyPort, 0, (mach_port_context_t)&replyPort);
}

bool const RemoteNotificationResponder::active() const {
    for (auto i = 0; i < _namesCnt; ++i) {
        if (_names[i] != MACH_PORT_NULL) {
            return true;
        }
    }
    return false;
}

void RemoteNotificationResponder::blockOnSynchronousEvent(uint32_t event) {
//        fprintf(stderr, "Blocking: %u\n", event);
    uint8_t buffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
    sendMessage(DYLD_PROCESS_EVENT_ID_BASE + event, sizeof(mach_msg_header_t), (mach_msg_header_t*)buffer);
}

//FIXME: Remove this once we drop support for iOS 11 simulators
// This is an enormous hack to keep remote introspection of older simulators working
//   It works by interposing mach_msg, and redirecting message sent to a special port name. Messages to that portname will trigger a full set
//   of sends to all kernel registered notifiers. In this mode mach_msg_sim_interposed() must return KERN_SUCCESS or the older dyld_sim may
//   try to cleanup the notifer array.

kern_return_t mach_msg_sim_interposed(    mach_msg_header_t* msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                                      mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    if (msg->msgh_remote_port != DYLD_PROCESS_INFO_NOTIFY_MAGIC) {
        // Not the magic port, so just pass through to the real mach_msg()
        return mach_msg(msg, option, send_size, rcv_size, rcv_name, timeout, notify);
    }

    // The magic port. We know dyld_sim is trying to message observers, so lets call into our messaging code directly.
    // This is kind of weird since we effectively built a buffer in dyld_sim, then pass it to mach_msg, which we interpose, unpack, and then
    // pass to send_message which then sends the buffer back out vis mach_message_overwrite(), but it should work at least as well as the old
    // way.
    RemoteNotificationResponder responder;
    responder.sendMessage(msg->msgh_id, send_size, msg);

    // We always return KERN_SUCCESS, otherwise old dyld_sims might clear the port
    return KERN_SUCCESS;
}
};

#endif /* !TARGET_OS_SIMULATOR */
