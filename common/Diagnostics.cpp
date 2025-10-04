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


#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <TargetConditionals.h>
#include "Defines.h"
#if TARGET_OS_EXCLAVEKIT
  extern "C" void abort_report_np(const char* format, ...) __attribute__((noreturn,format(printf, 1, 2)));
#else
  #include <_simple.h>
  #include <libc_private.h>
#endif
#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
  #include <mach/mach_time.h> // mach_absolute_time()
  #include <iostream>
#endif

#if !TARGET_OS_EXCLAVEKIT
#include "BitUtils.h"
#include <mach/mach.h>
#endif

#include "Diagnostics.h"

#if !BUILDING_DYLD
#define _simple_vsnprintf(str, size, fmt, ap) vsnprintf(str, size, fmt, ap)
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
  #include <dispatch/dispatch.h>
  dispatch_queue_t sWarningQueue = dispatch_queue_create("com.apple.dyld.cache-builder.warnings", NULL);
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
Diagnostics::Diagnostics(bool verbose)
    : _prefix(""), _verbose(verbose)
{
}
#endif

Diagnostics::Diagnostics()
{
#if TARGET_OS_EXCLAVEKIT
    this->_strBuf[0] = '\0';
#endif
}

Diagnostics::Diagnostics(Diagnostics&& other)
{
#if TARGET_OS_EXCLAVEKIT
    strlcpy(this->_strBuf, other._strBuf, sizeof(this->_strBuf));
    other._strBuf[0] = '\0';
#else
    this->_buffer       = other._buffer;
    this->_bufferSize   = other._bufferSize;
    this->_bufferUsed   = other._bufferUsed;

    other._buffer       = nullptr;
    other._bufferSize   = 0;
    other._bufferUsed   = 0;
#endif
}

Diagnostics& Diagnostics::operator=(Diagnostics&& other)
{
#if TARGET_OS_EXCLAVEKIT
    strlcpy(this->_strBuf, other._strBuf, sizeof(this->_strBuf));
    other._strBuf[0] = '\0';
#else
    this->_buffer       = other._buffer;
    this->_bufferSize   = other._bufferSize;
    this->_bufferUsed   = other._bufferUsed;

    other._buffer       = nullptr;
    other._bufferSize   = 0;
    other._bufferUsed   = 0;
#endif

    return *this;
}

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
Diagnostics::Diagnostics(const std::string& prefix, bool verbose)
    : _prefix(prefix), _verbose(verbose)
{
}
#endif

Diagnostics::~Diagnostics()
{
    clearError();
}

void Diagnostics::error(const char* format, ...)
{
    va_list    list;
    va_start(list, format);
    error(format, va_list_wrap(list));
    va_end(list);
}

void Diagnostics::error(const char* format, va_list_wrap vaWrap)
{
#if TARGET_OS_EXCLAVEKIT
    vsnprintf(_strBuf, sizeof(_strBuf), format, vaWrap.list);
#else
    clearError();

    va_list list2;
    va_copy(list2, vaWrap.list);

    // Get the size
    char smallBuffer[1] = { '\0' };
    int expectedSize = _simple_vsnprintf(smallBuffer, sizeof(smallBuffer), format, vaWrap.list) + 1;
    assert(expectedSize >= 0);

    const uint64_t alignedSize = lsl::roundToNextAligned(PAGE_SIZE, (uint64_t)expectedSize);

    vm_address_t newBuffer = 0;
    int kr = ::vm_allocate(mach_task_self(), &newBuffer, (vm_size_t)alignedSize,
                           VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_DYLD));
    assert(kr == KERN_SUCCESS);

    this->_buffer = (char*)newBuffer;

    int actualSize = _simple_vsnprintf(this->_buffer, (size_t)alignedSize, format, list2);
    assert(actualSize >= 0);
    assert((actualSize + 1) == expectedSize);

    this->_bufferUsed = actualSize + 1;
    this->_bufferSize = (uint32_t)alignedSize;

    va_end(list2);
#endif

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
    if ( !_verbose )
        return;

    if (_prefix.empty()) {
        fprintf(stderr, "%s\n", this->_buffer);
    } else {
        fprintf(stderr, "[%s] %s\n", _prefix.c_str(), this->_buffer);
    }
#endif
}

bool Diagnostics::empty() const
{
#if TARGET_OS_EXCLAVEKIT
    return (this->_strBuf[0] == '\0');
#else
    if ( (_buffer == nullptr) || (_bufferUsed == 0) || (_bufferSize == 0) )
        return true;
    return false;
#endif
}

void Diagnostics::appendError(const char* format, ...)
{
#if TARGET_OS_EXCLAVEKIT
    size_t len = strlen(_strBuf);
    va_list list;
    va_start(list, format);
    vsnprintf(&_strBuf[len], sizeof(_strBuf)-len, format, list);
    va_end(list);
#else
    if ( empty() ) {
        va_list list;
        va_start(list, format);
        error(format, va_list_wrap(list));
        va_end(list);
        return;
     }

     // Existing buffer, so need to actually append
     va_list list, list2;
     va_start(list, format);
     va_copy(list2, list);

     // Get the size
     char smallBuffer[1] = { '\0' };
     int expectedSize = _simple_vsnprintf(smallBuffer, sizeof(smallBuffer), format, list) + 1;
     assert(expectedSize >= 0);

     uint64_t remainingSize = this->_bufferSize - this->_bufferUsed;
     if ( remainingSize < expectedSize ) {
         // Grow the buffer to make space
         const uint64_t alignedSize = lsl::roundToNextAligned(PAGE_SIZE, (uint64_t)expectedSize + this->_bufferUsed);

         vm_address_t newBuffer = 0;
         int kr = ::vm_allocate(mach_task_self(), &newBuffer, (vm_size_t)alignedSize,
                                VM_FLAGS_ANYWHERE | VM_MAKE_TAG(VM_MEMORY_DYLD));
         assert(kr == KERN_SUCCESS);

         // Copy the old buffer over to the new one
         memcpy((void*)newBuffer, this->_buffer, this->_bufferSize);

         // Free the existing buffer
         ::vm_deallocate(mach_task_self(), (vm_address_t)this->_buffer, (vm_size_t)this->_bufferSize);

         // Set the buffer to the new one
         this->_buffer = (char*)newBuffer;
         this->_bufferSize = (uint32_t)alignedSize;

         remainingSize = this->_bufferSize - this->_bufferUsed;
     }

     // Write in to the existing buffer
     int actualSize = _simple_vsnprintf(&this->_buffer[this->_bufferUsed - 1], (size_t)remainingSize, format, list2);
     assert(actualSize >= 0);
     assert((actualSize + 1) == expectedSize);

     this->_bufferUsed += actualSize;

     va_end(list);
     va_end(list2);
#endif
}

bool Diagnostics::hasError() const
{
#if TARGET_OS_EXCLAVEKIT
    return (this->_strBuf[0] != '\0');
#else
    return _buffer != nullptr;
#endif
}

bool Diagnostics::noError() const
{
#if TARGET_OS_EXCLAVEKIT
    return (this->_strBuf[0] == '\0');
#else
    return _buffer == nullptr;
#endif
}

void Diagnostics::clearError()
{
#if TARGET_OS_EXCLAVEKIT
    this->_strBuf[0] = '\0';
#else
    if ( _buffer ) {
        ::vm_deallocate(mach_task_self(), (vm_address_t)this->_buffer, (vm_size_t)this->_bufferSize);
    }
    _buffer = nullptr;
    _bufferSize = 0;
    _bufferUsed = 0;
#endif
}

void Diagnostics::assertNoError() const
{
    if ( hasError() )
        abort_report_np("%s", errorMessageCStr());
}

bool Diagnostics::errorMessageContains(const char* subString) const
{
    if ( noError() )
        return false;
    return (strstr(errorMessageCStr(), subString) != nullptr);
}

std::string_view Diagnostics::errorMessageSV() const
{
    return this->errorMessageCStr();
}

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
void Diagnostics::warning(const char* format, ...)
{
    _SIMPLE_STRING tmp = _simple_salloc();
    va_list    list;
    va_start(list, format);
    _simple_vsprintf(tmp, format, list);
    va_end(list);
    dispatch_sync(sWarningQueue, ^{
        _warnings.insert(_simple_string(tmp));
    });
    _simple_sfree(tmp);
}

void Diagnostics::verbose(const char* format, ...)
{
    if ( !_verbose )
        return;

    char*   output_string;
    va_list list;
    va_start(list, format);
    vasprintf(&output_string, format, list);
    va_end(list);

    if (_prefix.empty()) {
        fprintf(stderr, "%s", output_string);
    } else {
        fprintf(stderr, "[%s] %s", _prefix.c_str(), output_string);
    }
    free(output_string);
}

const std::string Diagnostics::prefix() const
{
    return _prefix;
}

void Diagnostics::copy(const Diagnostics& other)
{
    if ( other.hasError() )
        error("%s", other.errorMessage().c_str());
    for (const std::string& warn : other.warnings())
        warning("%s", warn.c_str());
}

std::string Diagnostics::errorMessage() const
{
    return errorMessageCStr();
}

const char* Diagnostics::errorMessageCStr() const
{
    if ( _buffer != nullptr )
        return this->_buffer;
    else
        return "";
}

const std::set<std::string> Diagnostics::warnings() const
{
    __block std::set<std::string> retval;
    dispatch_sync(sWarningQueue, ^{
        retval = _warnings;
    });
    return retval;
}

void Diagnostics::clearWarnings()
{
    dispatch_sync(sWarningQueue, ^{
        _warnings.clear();
    });
}

#else

const char* Diagnostics::errorMessage() const
{
    if ( noError() )
        return "";
#if TARGET_OS_EXCLAVEKIT
    return _strBuf;
#else
    return this->_buffer;
#endif
}

#endif

#if BUILDING_CACHE_BUILDER || BUILDING_UNIT_TESTS || BUILDING_CACHE_BUILDER_UNIT_TESTS
void TimeRecorder::pushTimedSection() {
    openTimings.push_back(mach_absolute_time());
}

void TimeRecorder::recordTime(const char* format, ...) {
    uint64_t t = mach_absolute_time();
    uint64_t previousTime = openTimings.back();
    openTimings.pop_back();

    char*   output_string = nullptr;
    va_list list;
    va_start(list, format);
    vasprintf(&output_string, format, list);
    va_end(list);

    if (output_string != nullptr) {
        timings.push_back(TimingEntry {
            .time = t - previousTime,
            .logMessage = std::string(output_string),
            .depth = (int)openTimings.size()
        });
    }

    free(output_string);

    openTimings.push_back(mach_absolute_time());
}

void TimeRecorder::popTimedSection() {
    openTimings.pop_back();
}

static inline uint32_t absolutetime_to_milliseconds(uint64_t abstime)
{
    return (uint32_t)(abstime/1000/1000);
}

void TimeRecorder::logTimings() {
    for (const TimingEntry& entry : timings) {
        for (int i = 0 ; i < entry.depth ; i++) {
            std::cerr << "  ";
        }
        std::cerr << "time to " << entry.logMessage << " " << absolutetime_to_milliseconds(entry.time) << "ms" << std::endl;
    }

    timings.clear();
}

#endif

#if BUILDING_LD || BUILDING_UNIT_TESTS || BUILDING_SHARED_CACHE_LINKER || BUILDING_CACHE_BUILDER || BUILDING_CACHE_BUILDER_UNIT_TESTS

mach_o::Error Diagnostics::toError() const
{
    if ( hasError() )
        return mach_o::Error("%s", errorMessageCStr());
    return mach_o::Error::none();
}

void Diagnostics::error(const mach_o::Error& err)
{
    if ( err.noError() )
        return;

    error("%s", err.message());
}

#endif
