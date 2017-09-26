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


#include <assert.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <bootstrap.h>

#include "ClosureBuffer.h"
#include "PathOverrides.h"


namespace dyld3 {

TypedContentBuffer::TypedContentBuffer(size_t elementsCount, size_t elementsTotalSize)
{
    _size = elementsTotalSize + (elementsCount+1)*(sizeof(Element)+4);  // worst case padding, plus "end" element
    vm_address_t bufferAddress = 0;
    assert(::vm_allocate(mach_task_self(), &bufferAddress, _size, VM_FLAGS_ANYWHERE) == 0);
    _buffer = (Element*)bufferAddress;
    _currentEnd = _buffer;
    _readOnly = false;
}

void TypedContentBuffer::free()
{
    if ( _buffer != nullptr )
        vm_deallocate(mach_task_self(), (long)_buffer, _size);
    _buffer = nullptr;
}

void TypedContentBuffer::addItem(uint32_t k, const void* content, size_t len)
{
    assert(!_readOnly);
    assert(((char*)_currentEnd + len) < ((char*)_buffer + _size));
    _currentEnd->kind = k;
    _currentEnd->contentLength = (uint32_t)len;
    if ( len != 0 )
        memmove(&(_currentEnd->content), content, len);
    size_t delta = (sizeof(Element) + len + 3) & (-4);
    _currentEnd = (Element*)((char*)_currentEnd + delta);
}

vm_address_t TypedContentBuffer::vmBuffer() const
{
    assert(_readOnly);
    return (vm_address_t)_buffer;
}

uint32_t TypedContentBuffer::vmBufferSize() const
{
    assert(_readOnly);
    return (uint32_t)_size;
}

void TypedContentBuffer::doneBuilding()
{
    _readOnly = true;
}


const TypedContentBuffer::Element* TypedContentBuffer::Element::next() const
{
   return (Element*)((char*)this + sizeof(Element) + ((contentLength + 3) & -4));
}

TypedContentBuffer::TypedContentBuffer(const void* buff, size_t buffSize)
    : _size(buffSize), _buffer((Element*)buff), _currentEnd((Element*)((char*)buff+buffSize)), _readOnly(true)
{
}

unsigned TypedContentBuffer::count(uint32_t kind) const
{
    assert(_readOnly);
    unsigned count = 0;
    for (const Element* e = _buffer; e->kind != 0; e = e->next()) {
        if ( e->kind == kind )
            ++count;
    }
    return count;
}

void TypedContentBuffer::forEach(uint32_t kind, void (^callback)(const void* content, size_t length)) const
{
    assert(_readOnly);
    for (const Element* e = _buffer; e->kind != 0; e = e->next()) {
        if ( e->kind == kind ) {
            callback(&(e->content), e->contentLength);
        }
    }
}

#if !BUILDING_CLOSURED

ClosureBuffer::ClosureBuffer(const CacheIdent& cacheIdent, const char* path, const launch_cache::ImageGroupList& groups, const PathOverrides& envVars)
    : TypedContentBuffer(2 + envVars.envVarCount() + groups.count(), computeSize(path, groups, envVars))
{
    addItem(kindCacheIdent, &cacheIdent, sizeof(CacheIdent));
    addItem(kindTargetPath, path, strlen(path)+1);
    envVars.forEachEnvVar(^(const char* envVar) {
        addItem(kindEnvVar, envVar, strlen(envVar)+1);
    });
    for (size_t i=0; i < groups.count(); ++i) {
        launch_cache::ImageGroup group(groups[i]);
        addItem(kindImageGroup, group.binaryData(), group.size());
    }
    addItem(kindEnd, nullptr, 0);
    doneBuilding();
}

size_t ClosureBuffer::computeSize(const char* path, const launch_cache::ImageGroupList& groups, const PathOverrides& envVars)
{
    __block size_t result = sizeof(CacheIdent);
    result += (strlen(path) + 1);
    envVars.forEachEnvVar(^(const char* envVar) {
        result += (strlen(envVar) + 1);
    });
    for (size_t i=0; i < groups.count(); ++i) {
        launch_cache::ImageGroup group(groups[i]);
        result += group.size();
    }
    return result;
}

#endif

ClosureBuffer::ClosureBuffer(const char* errorMessage)
    : TypedContentBuffer(1, strlen(errorMessage+2))
{
    addItem(kindErrorMessage, errorMessage, strlen(errorMessage)+1);
    doneBuilding();
}

ClosureBuffer::ClosureBuffer(const launch_cache::BinaryImageGroupData* imageGroup)
    : TypedContentBuffer(1, launch_cache::ImageGroup(imageGroup).size())
{
    addItem(kindImageGroup, imageGroup, launch_cache::ImageGroup(imageGroup).size());
    doneBuilding();
}

ClosureBuffer::ClosureBuffer(const launch_cache::BinaryClosureData* closure)
    : TypedContentBuffer(1, launch_cache::Closure(closure).size())
{
    addItem(kindClosure, closure, launch_cache::Closure(closure).size());
    doneBuilding();
}


ClosureBuffer::ClosureBuffer(const void* buff, size_t buffSize)
    : TypedContentBuffer(buff, buffSize)
{
}

const ClosureBuffer::CacheIdent& ClosureBuffer::cacheIndent() const
{
    __block CacheIdent* ident = nullptr;
    forEach(kindCacheIdent, ^(const void* content, size_t length) {
        ident = (CacheIdent*)content;
        assert(length == sizeof(CacheIdent));
    });
    assert(ident != nullptr);
    return *ident;
}

const char* ClosureBuffer::targetPath() const
{
    __block char* path = nullptr;
    forEach(kindTargetPath, ^(const void* content, size_t length) {
        path = (char*)content;
    });
    assert(path != nullptr);
    return path;
}

uint32_t ClosureBuffer::envVarCount() const
{
    __block uint32_t count = 0;
    forEach(kindEnvVar, ^(const void* content, size_t length) {
        ++count;
    });
    return count;
}

void ClosureBuffer::copyImageGroups(const char* envVars[]) const
{
    __block uint32_t index = 0;
    forEach(kindEnvVar, ^(const void* content, size_t length) {
        envVars[index] = (char*)content;
        ++index;
    });
}

uint32_t ClosureBuffer::imageGroupCount() const
{
    __block uint32_t count = 0;
    forEach(kindImageGroup, ^(const void* content, size_t length) {
        ++count;
    });
    return count;
}

void ClosureBuffer::copyImageGroups(const launch_cache::BinaryImageGroupData* imageGroups[]) const
{
    __block uint32_t index = 0;
    forEach(kindImageGroup, ^(const void* content, size_t length) {
        imageGroups[index] = (launch_cache::BinaryImageGroupData*)content;
        ++index;
    });
}

bool ClosureBuffer::isError() const
{
    return ( errorMessage() != nullptr );
}

const char* ClosureBuffer::errorMessage() const
{
    __block char* message = nullptr;
    forEach(kindErrorMessage, ^(const void* content, size_t length) {
        message = (char*)content;
    });
    return message;
}

const launch_cache::BinaryClosureData* ClosureBuffer::closure() const
{
   __block const launch_cache::BinaryClosureData* result = nullptr;
    forEach(kindClosure, ^(const void* content, size_t length) {
        result = (const launch_cache::BinaryClosureData*)content;
    });
    assert(result != nullptr);
    return result;
}


const launch_cache::BinaryImageGroupData* ClosureBuffer::imageGroup() const
{
    __block const launch_cache::BinaryImageGroupData* result = nullptr;
    forEach(kindImageGroup, ^(const void* content, size_t length) {
        result = (const launch_cache::BinaryImageGroupData*)content;
    });
    assert(result != nullptr);
    return result;
}






} // namespace dyld3

