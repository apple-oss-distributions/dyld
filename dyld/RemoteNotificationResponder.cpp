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

#include <TargetConditionals.h>

#if !TARGET_OS_EXCLAVEKIT

#include "Header.h"
#include "dyld_process_info_internal.h"

#include "RemoteNotificationResponder.h"

#if !TARGET_OS_SIMULATOR

namespace dyld4 {

RemoteNotificationResponder::RemoteNotificationResponder(mach_port_t notifyPortValue) {
    if (notifyPortValue != DYLD_PROCESS_INFO_NOTIFY_MAGIC) {
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

void RemoteNotificationResponder::notifyMonitorOfImageListChanges(bool unloading, unsigned imageCount, const struct mach_header* loadAddresses[], const char* imagePaths[], uint64_t lastUpdateTime)
{
#if BUILDING_DYLD
    // Make sure there is at least enough room to hold a the largest single file entry that can exist.
    static_assert((PATH_MAX + sizeof(dyld_process_info_image_entry) + 1 + MAX_TRAILER_SIZE) <= DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE);

    unsigned entriesSize = imageCount*sizeof(dyld_process_info_image_entry);
    unsigned pathsSize = 0;
    for (unsigned j=0; j < imageCount; ++j)
        pathsSize += (strlen(imagePaths[j]) + 1);

    unsigned totalSize = (sizeof(struct dyld_process_info_notify_header) + entriesSize + pathsSize + 127) & -128;   // align
    // The reciever has a fixed buffer of DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE, whcih needs to hold both the message and a trailer.
    // If the total size exceeds that we need to fragment the message.
    if ( (totalSize + MAX_TRAILER_SIZE) > DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE ) {
        // Putting all image paths into one message would make buffer too big.
        // Instead split into two messages.  Recurse as needed until paths fit in buffer.
        unsigned imageHalfCount = imageCount/2;
        this->notifyMonitorOfImageListChanges(unloading, imageHalfCount, loadAddresses, imagePaths, lastUpdateTime);
        this->notifyMonitorOfImageListChanges(unloading, imageCount - imageHalfCount, &loadAddresses[imageHalfCount], &imagePaths[imageHalfCount], lastUpdateTime);
        return;
    }
    uint8_t buffer[totalSize + MAX_TRAILER_SIZE];
    dyld_process_info_notify_header* header = (dyld_process_info_notify_header*)buffer;
    header->version          = 1;
    header->imageCount       = imageCount;
    header->imagesOffset     = sizeof(dyld_process_info_notify_header);
    header->stringsOffset    = sizeof(dyld_process_info_notify_header) + entriesSize;
    header->timestamp        = lastUpdateTime;
    dyld_process_info_image_entry* entries = (dyld_process_info_image_entry*)&buffer[header->imagesOffset];
    char* const pathPoolStart = (char*)&buffer[header->stringsOffset];
    char* pathPool = pathPoolStart;
    for (unsigned j=0; j < imageCount; ++j) {
        strcpy(pathPool, imagePaths[j]);
        uint32_t len = (uint32_t)strlen(pathPool);
        bzero(entries->uuid, 16);
        mach_o::Header* mh = (mach_o::Header*)loadAddresses[j];
        mh->getUuid(entries->uuid);
        entries->loadAddress = (uint64_t)loadAddresses[j];
        entries->pathStringOffset = (uint32_t)(pathPool - pathPoolStart);
        entries->pathLength  = len;
        pathPool += (len +1);
        ++entries;
    }
    mach_msg_id_t msgID = unloading ? DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID : DYLD_PROCESS_INFO_NOTIFY_LOAD_ID;
    this->sendMessage(msgID, totalSize, (mach_msg_header_t*)buffer);
#endif /* BUILDING_DYLD */
}

void RemoteNotificationResponder::notifyMonitorOfMainCalled()
{
    uint8_t buffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
    this->sendMessage(DYLD_PROCESS_INFO_NOTIFY_MAIN_ID, sizeof(mach_msg_header_t), (mach_msg_header_t*)buffer);
    this->blockOnSynchronousEvent(DYLD_REMOTE_EVENT_MAIN);
}

void RemoteNotificationResponder::notifyMonitorOfDyldBeforeInitializers()
{
    this->blockOnSynchronousEvent(DYLD_REMOTE_EVENT_BEFORE_INITIALIZERS);
}


void RemoteNotificationResponder::blockOnSynchronousEvent(uint32_t event) {
//        fprintf(stderr, "Blocking: %u\n", event);
    uint8_t buffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
    sendMessage(DYLD_PROCESS_EVENT_ID_BASE + event, sizeof(mach_msg_header_t), (mach_msg_header_t*)buffer);
}

#if 0
//FIXME: Remove this once we drop support for iOS 11 simulators
// This is an enormous hack to keep remote introspection of older simulators working
//   It works by interposing mach_msg, and redirecting message sent to a special port name. Messages to that portname will trigger a full set
//   of sends to all kernel registered notifiers. In this mode mach_msg_sim_interposed() must return KERN_SUCCESS or the older dyld_sim may
//   try to cleanup the notifer array.

kern_return_t mach_msg_sim_interposed(    mach_msg_header_t* msg, mach_msg_option_t option, mach_msg_size_t send_size, mach_msg_size_t rcv_size,
                                      mach_port_name_t rcv_name, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    if (msg->msgh_remote_port != RemoteNotificationResponder::DYLD_PROCESS_INFO_NOTIFY_MAGIC) {
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
#endif // 0

};

#endif /* !TARGET_OS_SIMULATOR */

#endif // !TARGET_OS_EXCLAVEKIT
