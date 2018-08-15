/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2016 Apple Inc. All rights reserved.
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

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <mach/shared_region.h>
#include <mach/mach_vm.h>
#include <libkern/OSAtomic.h>

#include "dyld_process_info.h"
#include "dyld_process_info_internal.h"
#include "dyld_images.h"
#include "dyld_priv.h"

#include "LaunchCache.h"
#include "Loading.h"
#include "AllImages.h"


typedef void (^Notify)(bool unload, uint64_t timestamp, uint64_t machHeader, const uuid_t uuid, const char* path);
typedef void (^NotifyExit)();
typedef void (^NotifyMain)();


//
// Object used for monitoring another processes dyld loads
//
struct __attribute__((visibility("hidden"))) dyld_process_info_notify_base
{
    static dyld_process_info_notify_base* make(task_t task, dispatch_queue_t queue, Notify notify, NotifyExit notifyExit, kern_return_t* kr);
    										~dyld_process_info_notify_base();

    bool                incRetainCount() const;
    bool                decRetainCount() const;

	void				setNotifyMain(NotifyMain notifyMain) const { _notifyMain = notifyMain; }

    // override new and delete so we don't need to link with libc++
    static void*        operator new(size_t sz) { return malloc(sz); }
    static void         operator delete(void* p) { return free(p); }

private:
                        dyld_process_info_notify_base(dispatch_queue_t queue, Notify notify, NotifyExit notifyExit, task_t task);
    kern_return_t       makePorts();
    kern_return_t       pokeSendPortIntoTarget();
	kern_return_t		unpokeSendPortInTarget();
    void				setMachSourceOnQueue();

	mutable int32_t 	_retainCount;
    dispatch_queue_t    _queue;
    Notify              _notify;
    NotifyExit          _notifyExit;
	mutable NotifyMain	_notifyMain;
	task_t				_targetTask;
	dispatch_source_t	_machSource;
    uint64_t            _portAddressInTarget;
    mach_port_t         _sendPortInTarget;          // target is process being watched for image loading/unloading
    mach_port_t         _receivePortInMonitor;      // monitor is process being notified of image loading/unloading
};


dyld_process_info_notify_base::dyld_process_info_notify_base(dispatch_queue_t queue, Notify notify, NotifyExit notifyExit, task_t task)
    : _retainCount(1), _queue(queue), _notify(notify), _notifyExit(notifyExit), _notifyMain(NULL), _targetTask(task), _machSource(NULL), _portAddressInTarget(0), _sendPortInTarget(0), _receivePortInMonitor(0)
{
    dispatch_retain(_queue);
}

dyld_process_info_notify_base::~dyld_process_info_notify_base()
{
	if ( _machSource ) {
        dispatch_source_cancel(_machSource);
		dispatch_release(_machSource);
		_machSource = NULL;
	}
	if ( _portAddressInTarget ) {
		unpokeSendPortInTarget();
		_portAddressInTarget = 0;
	}
	if ( _sendPortInTarget ) {
		_sendPortInTarget = 0;
	}
    dispatch_release(_queue);
	if ( _receivePortInMonitor != 0 ) {
		mach_port_deallocate(mach_task_self(), _receivePortInMonitor);
		_receivePortInMonitor = 0;
	}
}

bool dyld_process_info_notify_base::incRetainCount() const
{
    int32_t newCount = OSAtomicIncrement32(&_retainCount);
    return ( newCount == 1 );
}

bool dyld_process_info_notify_base::decRetainCount() const
{
    int32_t newCount = OSAtomicDecrement32(&_retainCount);
    return ( newCount == 0 );
}


dyld_process_info_notify_base* dyld_process_info_notify_base::make(task_t task, dispatch_queue_t queue, Notify notify, NotifyExit notifyExit, kern_return_t* kr)
{
    dyld_process_info_notify_base* obj = new dyld_process_info_notify_base(queue, notify, notifyExit, task);

    if ( kern_return_t r = obj->makePorts() ) {
		if ( kr != NULL )
			*kr = r;
        goto fail;
	}

    obj->setMachSourceOnQueue();

    if ( kern_return_t r = obj->pokeSendPortIntoTarget() ) {
		if ( kr != NULL )
			*kr = r;
        goto fail;
	}

	if ( kr != NULL )
		*kr = KERN_SUCCESS;
    return obj;

fail:
    delete obj;
    return NULL;
}


kern_return_t dyld_process_info_notify_base::makePorts()
{
    // Allocate a port to listen on in this monitoring task
    if ( kern_return_t r = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_receivePortInMonitor) )
        return r;

    // Add send rights for replying
    if ( kern_return_t r = mach_port_insert_right(mach_task_self(), _receivePortInMonitor, _receivePortInMonitor, MACH_MSG_TYPE_MAKE_SEND) )
        return r;

    // Allocate a name in the target. We need a new name to add send rights to
    if ( kern_return_t r = mach_port_allocate(_targetTask, MACH_PORT_RIGHT_DEAD_NAME, &_sendPortInTarget) )
        return r;

    // Deallocate the dead name
    if ( kern_return_t r = mach_port_mod_refs(_targetTask, _sendPortInTarget, MACH_PORT_RIGHT_DEAD_NAME, -1) )
        return r;

    // Make the dead name a send right to our listening port
    if ( kern_return_t r = mach_port_insert_right(_targetTask, _sendPortInTarget, _receivePortInMonitor, MACH_MSG_TYPE_MAKE_SEND) )
        return r;

    // Notify us if the target dies
    mach_port_t previous = MACH_PORT_NULL;
    if ( kern_return_t r = mach_port_request_notification(_targetTask, _sendPortInTarget, MACH_NOTIFY_DEAD_NAME, 0, _receivePortInMonitor, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous))
        return r;

    //fprintf(stderr, "_sendPortInTarget=%d, _receivePortInMonitor=%d\n", _sendPortInTarget, _receivePortInMonitor);
    return KERN_SUCCESS;
}



void dyld_process_info_notify_base::setMachSourceOnQueue()
{
	NotifyExit exitHandler = _notifyExit;
	_machSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, _receivePortInMonitor, 0, _queue);
    dispatch_source_set_event_handler(_machSource, ^{
        // This event handler block has an implicit reference to "this"
        // if incrementing the count goes to one, that means the object may have already been destroyed
        if ( incRetainCount() )
            return;
        uint8_t messageBuffer[DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE];
        mach_msg_header_t* h = (mach_msg_header_t*)messageBuffer;

        kern_return_t r = mach_msg(h, MACH_RCV_MSG, 0, sizeof(messageBuffer), _receivePortInMonitor, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if ( r == KERN_SUCCESS ) {
            //fprintf(stderr, "received message id=0x%X, size=%d\n", h->msgh_id, h->msgh_size);
			if ( h->msgh_id == DYLD_PROCESS_INFO_NOTIFY_LOAD_ID || h->msgh_id == DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID ) {
				// run notifier block for each [un]load image
				const dyld_process_info_notify_header* header = (dyld_process_info_notify_header*)messageBuffer;
				const dyld_process_info_image_entry* entries = (dyld_process_info_image_entry*)&messageBuffer[header->imagesOffset];
				const char* const stringPool = (char*)&messageBuffer[header->stringsOffset];
				for (unsigned i=0; i < header->imageCount; ++i) {
					bool isUnload = (h->msgh_id == DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID);
					_notify(isUnload, header->timestamp, entries[i].loadAddress, entries[i].uuid, stringPool + entries[i].pathStringOffset);
				}
				// reply to dyld, so it can continue
				mach_msg_header_t replyHeader;
				replyHeader.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
				replyHeader.msgh_id          = 0;
				replyHeader.msgh_local_port  = MACH_PORT_NULL;
				replyHeader.msgh_remote_port = h->msgh_remote_port;
				replyHeader.msgh_reserved    = 0;
				replyHeader.msgh_size        = sizeof(replyHeader);
				mach_msg(&replyHeader, MACH_SEND_MSG | MACH_SEND_TIMEOUT, replyHeader.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
			}
			else if ( h->msgh_id == DYLD_PROCESS_INFO_NOTIFY_MAIN_ID ) {
				if ( _notifyMain != NULL )  {
					_notifyMain();
				}
				// reply to dyld, so it can continue
				mach_msg_header_t replyHeader;
				replyHeader.msgh_bits        = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
				replyHeader.msgh_id          = 0;
				replyHeader.msgh_local_port  = MACH_PORT_NULL;
				replyHeader.msgh_remote_port = h->msgh_remote_port;
				replyHeader.msgh_reserved    = 0;
				replyHeader.msgh_size        = sizeof(replyHeader);
				mach_msg(&replyHeader, MACH_SEND_MSG | MACH_SEND_TIMEOUT, replyHeader.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
			}
			else if ( h->msgh_id == MACH_NOTIFY_PORT_DELETED ) {
				mach_port_t deadPort = ((mach_port_deleted_notification_t *)h)->not_port;
				//fprintf(stderr, "received message id=MACH_NOTIFY_PORT_DELETED, size=%d, deadPort=%d\n", h->msgh_size, deadPort);
				if ( deadPort == _sendPortInTarget ) {
					// target process died.  Clean up ports
					_sendPortInTarget = 0;
					mach_port_deallocate(mach_task_self(), _receivePortInMonitor);
					_receivePortInMonitor = 0;
					_portAddressInTarget = 0;
					// notify that target is gone
					exitHandler();
				}
			}
			else {
				fprintf(stderr, "received unknown message id=0x%X, size=%d\n", h->msgh_id, h->msgh_size);
			}
        }
        if ( decRetainCount() )
            delete this;
   });
    dispatch_resume(_machSource);
}


kern_return_t dyld_process_info_notify_base::pokeSendPortIntoTarget()
{
    // get location on all_image_infos in target task
    task_dyld_info_data_t taskDyldInfo;
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t r = task_info(_targetTask, TASK_DYLD_INFO, (task_info_t)&taskDyldInfo, &count);
    if ( r )
        return  r;

    // remap the page containing all_image_infos into this process r/w
    mach_vm_address_t mappedAddress = 0;
    mach_vm_size_t    mappedSize = taskDyldInfo.all_image_info_size;
    vm_prot_t curProt = VM_PROT_NONE;
    vm_prot_t maxProt = VM_PROT_NONE;
    r = mach_vm_remap(mach_task_self(), &mappedAddress, mappedSize, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_RETURN_DATA_ADDR,
    					_targetTask, taskDyldInfo.all_image_info_addr, false, &curProt, &maxProt, VM_INHERIT_NONE);
    if ( r )
        return r;
    if ( curProt != (VM_PROT_READ|VM_PROT_WRITE) )
        return KERN_PROTECTION_FAILURE;

    // atomically set port into all_image_info_struct
    static_assert(sizeof(mach_port_t) == sizeof(uint32_t), "machport size not 32-bits");

    mach_vm_address_t mappedAddressToPokePort = 0;
    if ( taskDyldInfo.all_image_info_format == TASK_DYLD_ALL_IMAGE_INFO_32 )
        mappedAddressToPokePort = mappedAddress + offsetof(dyld_all_image_infos_32,notifyMachPorts);
    else
        mappedAddressToPokePort = mappedAddress + offsetof(dyld_all_image_infos_64,notifyMachPorts);

    // use first available slot
	bool slotFound = false;
	for (int slotIndex=0; slotIndex < DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT; ++slotIndex) {
		if ( OSAtomicCompareAndSwap32Barrier(0, _sendPortInTarget, (volatile int32_t*)mappedAddressToPokePort) ) {
			slotFound = true;
			break;
		}
		mappedAddressToPokePort += sizeof(uint32_t);
     }
	if ( !slotFound ) {
		mach_vm_deallocate(mach_task_self(), mappedAddress, mappedSize);
		return KERN_UREFS_OVERFLOW;
	}
    _portAddressInTarget = taskDyldInfo.all_image_info_addr + mappedAddressToPokePort - mappedAddress;
    //fprintf(stderr, "poked port %d into target at address 0x%llX\n", _sendPortInTarget, _portAddressInTarget);
    mach_vm_deallocate(mach_task_self(), mappedAddress, mappedSize);
    return r;
}



kern_return_t dyld_process_info_notify_base::unpokeSendPortInTarget()
{
    // remap the page containing all_image_infos into this process r/w
    mach_vm_address_t mappedAddress = 0;
    mach_vm_size_t    mappedSize = sizeof(mach_port_t);
    vm_prot_t curProt = VM_PROT_NONE;
    vm_prot_t maxProt = VM_PROT_NONE;
    kern_return_t r = mach_vm_remap(mach_task_self(), &mappedAddress, mappedSize, 0, VM_FLAGS_ANYWHERE | VM_FLAGS_RETURN_DATA_ADDR,
									_targetTask, _portAddressInTarget, false, &curProt, &maxProt, VM_INHERIT_NONE);
    if ( r )
        return r;
    if ( curProt != (VM_PROT_READ|VM_PROT_WRITE) )
        return KERN_PROTECTION_FAILURE;

    OSAtomicCompareAndSwap32Barrier(_sendPortInTarget, 0, (volatile int32_t*)mappedAddress);

    //fprintf(stderr, "cleared port %d from target\n", _sendPortInTarget);
    mach_vm_deallocate(mach_task_self(), mappedAddress, mappedSize);
    return r;
}



dyld_process_info_notify _dyld_process_info_notify(task_t task, dispatch_queue_t queue,
                                                   void (^notify)(bool unload, uint64_t timestamp, uint64_t machHeader, const uuid_t uuid, const char* path),
                                                   void (^notifyExit)(),
                                                   kern_return_t* kr)
{
    return dyld_process_info_notify_base::make(task, queue, notify, notifyExit, kr);
}

void _dyld_process_info_notify_main(dyld_process_info_notify object, void (^notifyMain)())
{
	object->setNotifyMain(notifyMain);
}

void _dyld_process_info_notify_retain(dyld_process_info_notify object)
{
    object->incRetainCount();
}

void _dyld_process_info_notify_release(dyld_process_info_notify object)
{
    // Note if _machSource is currently handling a message, the retain count will not be zero
    // and object will instead be deleted when handling is done.
    if ( object->decRetainCount() )
        delete object;
}







namespace dyld3 {


static mach_port_t sNotifyReplyPorts[DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT];
static bool        sZombieNotifiers[DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT];

static void notifyMonitoringDyld(bool unloading, unsigned portSlot, const launch_cache::DynArray<loader::ImageInfo>& imageInfos)
{
    if ( sZombieNotifiers[portSlot] )
        return;

    unsigned entriesSize = (unsigned)imageInfos.count()*sizeof(dyld_process_info_image_entry);
    unsigned pathsSize = 0;
    for (uintptr_t i=0; i < imageInfos.count(); ++i) {
        launch_cache::Image image(imageInfos[i].imageData);
        pathsSize += (strlen(image.path()) + 1);
    }
    unsigned totalSize = (sizeof(dyld_process_info_notify_header) + MAX_TRAILER_SIZE + entriesSize + pathsSize + 127) & -128;   // align
    if ( totalSize > DYLD_PROCESS_INFO_NOTIFY_MAX_BUFFER_SIZE ) {
        // Putting all image paths into one message would make buffer too big.
        // Instead split into two messages.  Recurse as needed until paths fit in buffer.
        unsigned imageHalfCount = (unsigned)imageInfos.count()/2;
        const launch_cache::DynArray<loader::ImageInfo> firstHalf(imageHalfCount, (loader::ImageInfo*)&imageInfos[0]);
        const launch_cache::DynArray<loader::ImageInfo> secondHalf(imageInfos.count() - imageHalfCount, (loader::ImageInfo*)&imageInfos[imageHalfCount]);
        notifyMonitoringDyld(unloading, portSlot, firstHalf);
        notifyMonitoringDyld(unloading, portSlot, secondHalf);
        return;
    }
    // build buffer to send
    dyld_all_image_infos*  allImageInfo = gAllImages.oldAllImageInfo();
    uint8_t    buffer[totalSize];
    dyld_process_info_notify_header* header = (dyld_process_info_notify_header*)buffer;
    header->version          = 1;
    header->imageCount       = (uint32_t)imageInfos.count();
    header->imagesOffset     = sizeof(dyld_process_info_notify_header);
    header->stringsOffset    = sizeof(dyld_process_info_notify_header) + entriesSize;
    header->timestamp        = allImageInfo->infoArrayChangeTimestamp;
    dyld_process_info_image_entry* entries = (dyld_process_info_image_entry*)&buffer[header->imagesOffset];
    char* const pathPoolStart = (char*)&buffer[header->stringsOffset];
    char* pathPool = pathPoolStart;
    for (uintptr_t i=0; i < imageInfos.count(); ++i) {
        launch_cache::Image image(imageInfos[i].imageData);
        strcpy(pathPool, image.path());
        uint32_t len = (uint32_t)strlen(pathPool);
        memcpy(entries->uuid, image.uuid(), sizeof(uuid_t));
        entries->loadAddress = (uint64_t)imageInfos[i].loadAddress;
        entries->pathStringOffset = (uint32_t)(pathPool - pathPoolStart);
        entries->pathLength  = len;
        pathPool += (len +1);
        ++entries;
    }
    // lazily alloc reply port
    if ( sNotifyReplyPorts[portSlot] == 0 ) {
        if ( !mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &sNotifyReplyPorts[portSlot]) )
            mach_port_insert_right(mach_task_self(), sNotifyReplyPorts[portSlot], sNotifyReplyPorts[portSlot], MACH_MSG_TYPE_MAKE_SEND);
        //log("allocated reply port %d\n", sNotifyReplyPorts[portSlot]);
    }
    //log("found port to send to\n");
    mach_msg_header_t* h = (mach_msg_header_t*)buffer;
    h->msgh_bits         = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND); // MACH_MSG_TYPE_MAKE_SEND_ONCE
    h->msgh_id           = unloading ? DYLD_PROCESS_INFO_NOTIFY_UNLOAD_ID : DYLD_PROCESS_INFO_NOTIFY_LOAD_ID;
    h->msgh_local_port   = sNotifyReplyPorts[portSlot];
    h->msgh_remote_port  = allImageInfo->notifyPorts[portSlot];
    h->msgh_reserved     = 0;
    h->msgh_size         = (mach_msg_size_t)sizeof(buffer);
    //log("sending to port[%d]=%d, size=%d, reply port=%d, id=0x%X\n", portSlot, allImageInfo->notifyPorts[portSlot], h->msgh_size, sNotifyReplyPorts[portSlot], h->msgh_id);
    kern_return_t sendResult = mach_msg(h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, h->msgh_size, h->msgh_size, sNotifyReplyPorts[portSlot], 2000, MACH_PORT_NULL);
    //log("send result = 0x%X, msg_id=%d, msg_size=%d\n", sendResult, h->msgh_id, h->msgh_size);
    if ( sendResult == MACH_SEND_INVALID_DEST ) {
        // sender is not responding, detatch
        //log("process requesting notification gone. deallocation send port %d and receive port %d\n", allImageInfo->notifyPorts[portSlot], sNotifyReplyPorts[portSlot]);
        mach_port_deallocate(mach_task_self(), allImageInfo->notifyPorts[portSlot]);
        mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[portSlot]);
        allImageInfo->notifyPorts[portSlot] = 0;
        sNotifyReplyPorts[portSlot] = 0;
    }
    else if ( sendResult == MACH_RCV_TIMED_OUT ) {
        // client took too long, ignore him from now on
        sZombieNotifiers[portSlot] = true;
        mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[portSlot]);
        sNotifyReplyPorts[portSlot] = 0;
    }
}

void AllImages::notifyMonitorMain()
{
    dyld_all_image_infos* allImageInfo = gAllImages.oldAllImageInfo();
    for (int slot=0; slot < DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT; ++slot) {
        if ( (allImageInfo->notifyPorts[slot] != 0 ) && !sZombieNotifiers[slot] ) {
            if ( sNotifyReplyPorts[slot] == 0 ) {
                if ( !mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &sNotifyReplyPorts[slot]) )
                    mach_port_insert_right(mach_task_self(), sNotifyReplyPorts[slot], sNotifyReplyPorts[slot], MACH_MSG_TYPE_MAKE_SEND);
                //dyld::log("allocated reply port %d\n", sNotifyReplyPorts[slot]);
            }
            //dyld::log("found port to send to\n");
            uint8_t messageBuffer[sizeof(mach_msg_header_t) + MAX_TRAILER_SIZE];
            mach_msg_header_t* h = (mach_msg_header_t*)messageBuffer;
            h->msgh_bits         = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND); // MACH_MSG_TYPE_MAKE_SEND_ONCE
            h->msgh_id           = DYLD_PROCESS_INFO_NOTIFY_MAIN_ID;
            h->msgh_local_port   = sNotifyReplyPorts[slot];
            h->msgh_remote_port  = allImageInfo->notifyPorts[slot];
            h->msgh_reserved     = 0;
            h->msgh_size         = (mach_msg_size_t)sizeof(messageBuffer);
            //dyld::log("sending to port[%d]=%d, size=%d, reply port=%d, id=0x%X\n", slot, allImageInfo->notifyPorts[slot], h->msgh_size, sNotifyReplyPorts[slot], h->msgh_id);
            kern_return_t sendResult = mach_msg(h, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, h->msgh_size, h->msgh_size, sNotifyReplyPorts[slot], 2000, MACH_PORT_NULL);
            //dyld::log("send result = 0x%X, msg_id=%d, msg_size=%d\n", sendResult, h->msgh_id, h->msgh_size);
            if ( sendResult == MACH_SEND_INVALID_DEST ) {
                // sender is not responding, detatch
                //dyld::log("process requesting notification gone. deallocation send port %d and receive port %d\n", allImageInfo->notifyPorts[slot], sNotifyReplyPorts[slot]);
                mach_port_deallocate(mach_task_self(), allImageInfo->notifyPorts[slot]);
                mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[slot]);
                allImageInfo->notifyPorts[slot] = 0;
                sNotifyReplyPorts[slot] = 0;
            }
            else if ( sendResult == MACH_RCV_TIMED_OUT ) {
                // client took too long, ignore him from now on
                sZombieNotifiers[slot] = true;
                mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[slot]);
                sNotifyReplyPorts[slot] = 0;
            }
        }
    }
}

void AllImages::notifyMonitorLoads(const launch_cache::DynArray<loader::ImageInfo>& newImages)
{
    // notify each monitoring process
    dyld_all_image_infos* allImageInfo = gAllImages.oldAllImageInfo();
    for (int slot=0; slot < DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT; ++slot) {
        if ( allImageInfo->notifyPorts[slot] != 0 ) {
             notifyMonitoringDyld(false, slot, newImages);
        }
        else if ( sNotifyReplyPorts[slot] != 0 ) {
            // monitoring process detached from this process, so release reply port
            //dyld::log("deallocated reply port %d\n", sNotifyReplyPorts[slot]);
            mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[slot]);
            sNotifyReplyPorts[slot] = 0;
            sZombieNotifiers[slot] = false;
        }
    }
}

void AllImages::notifyMonitorUnloads(const launch_cache::DynArray<loader::ImageInfo>& unloadingImages)
{
    // notify each monitoring process
    dyld_all_image_infos* allImageInfo = gAllImages.oldAllImageInfo();
    for (int slot=0; slot < DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT; ++slot) {
        if ( allImageInfo->notifyPorts[slot] != 0 ) {
             notifyMonitoringDyld(true, slot, unloadingImages);
        }
        else if ( sNotifyReplyPorts[slot] != 0 ) {
            // monitoring process detached from this process, so release reply port
            //dyld::log("deallocated reply port %d\n", sNotifyReplyPorts[slot]);
            mach_port_deallocate(mach_task_self(), sNotifyReplyPorts[slot]);
            sNotifyReplyPorts[slot] = 0;
            sZombieNotifiers[slot] = false;
        }
    }
}

} // namespace dyld3




