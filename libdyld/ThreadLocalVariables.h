/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
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


#ifndef ThreadLocalVariables_h
#define ThreadLocalVariables_h

#include <stdint.h>

#include "Defines.h"
#include "Error.h"
#include "Header.h"

// cannot include LibSystemHelpers.h because that will introduce a cycle
namespace dyld4 {
    struct LibSystemHelpers;
}

class DyldSharedCache;

namespace dyld {

using mach_o::Header;

/*
              * * * How thread-local variables work on Apple platforms * * *

    A thread local variable (TLV) is a per-thread variable. It is not statically allocated in
    __DATA segment, nor is it stack allocated.  Instead, on first use of a TLV, malloc() is used
    to allocate space for the variable and its address is stored in a thread-specific way.  This
    allocation is lazy, so that a thread that does not access a TLV does not have space malloc()ed.

    When C (__thread) or C++ (thread_local) source code define a TLV, the compiler emits a
    thunk in the __DATA,__thread_vars section. The first pointer in the thunk is to a function.
    When code uses a TLV, the compiler emits code to materialize the address of the thunk, then
    calls the first pointer in the thunk, passing the thunk's address as a parameter, and the
    function returns the address of the TLV for the current thread.  The thunk func has special
    calling conventions where all registers are preserved (other than the result register). That
    means the compiler does not need to spill registers to the stack when "computing" the address
    of a TLV.

    In a .o file a thunk for "myvar" looks like:

            .section __DATA,__thread_vars,thread_local_variables
            .globl _myvar
 _myvar:    .quad  __tlv_bootstrap
            .quad  0
            .quad  _myvar$tlv$init

    A thunk is always three pointers in size.  The first points to a bootstrapping function.
    The second is always zero.  The third is a pointer to the initial content for when the TLV
    is instantiated at runtime.

    The linker does a minor transformation of these thunks.  The linker finds all TLV defined in the
    linkage unit and co-locates their initial content blobs (e.g. _myvar$tlv$init).  Thus making
    one contiguous run of initial content.  This is so that the runtime can do just one malloc() for all
    TLVs in the image on first use, and then do one copy of the initial content into the malloc()ed space.

    At runtime dyld needs to do some load time processing of images with TLVs. Dyld needs to allocate
    a pthread_key and stuff the pthread_key into the second slot of each thunk.  Once they are set up,
    when the code calls the thunk func, it uses the key and pthread_getspecific() to get the
    address of the malloc()ed space, then add the third field (offset) to return the address of
    the specific TLV in the image.  If pthread_getspecific() returns NULL, that means this is the first
    use of any TLV in this image on this thread. In that case, dyld needs to determine the overall
    size to malloc() and the initial content bytes to set that to.

    It turns out the slow path (first use of a TLV) required taking a lock and walking dyld data
    structures to find the image containing the TLV and the initial content for it.

    In Spring 2025 releases, an optimization was made to how TLVs work to optimize the slow path.
    The code below implements this optimization which repacks the fields in the thunk after the
    func pointer to contain all the info needed for the fast and slow paths.  That means dyld
    does not need to maintain a side table and there is no need for a lock.  Each TLV is self
    contained once set up.

    There is also an optimization done in the dyld cache builder for dylibs in the dyld cache which
    have TLVs.  Instead of have dyld setup the TLVs at runtime (which dirties pages), the dyld
    cache builder does the setup.  It does this by using a range of reserved static pthread keys.


 */

//
// Class for managing thread-local variables in mach-o files at runtime
//
class VIS_HIDDEN ThreadLocalVariables
{
public:
    // on disk format of a thread-local variable
    struct Thunk
    {
        void*       func;   // really void* (*ThunkFunc)(Thunk*);
        size_t      key;
        size_t      offset;
    };

    typedef void  (*TermFunc)(void* objAddr);

    // called during libSystem initializer
    void                    initialize();

    // called by _tlv_atexit() to register a callback to be called when a thread terminates
    void                    addTermFunc(TermFunc func, void* objAddr);

    // called by libc's exit() to run all terminators
    void                    exit();

    // called by dyld when image with thread-locals is first loaded
    mach_o::Error           setUpImage(const DyldSharedCache* cache, const Header* hdr);

    // called by pthreads when a thread goes away
    void                    finalizeList(void* list);

    // called on first use of a thread local in a thread to allocate and initialize thread locals for current thread
#if BUILDING_UNIT_TESTS
    void*                   instantiateVariable(const Thunk&);
#else
    static void*            instantiateVariable(const Thunk&);
#endif

    // internal routines to prepare the thunks in an image
    mach_o::Error           initializeThunksFromDisk(const Header* hdr);
    mach_o::Error           initializeThunksInDyldCache(const DyldSharedCache* cache, const Header* hdr);

    // runtime structure of 64-bit arch thread-local thunk
    struct TLV_Thunkv2
    {
        void*        func;
        uint32_t     key;
        uint32_t     offset;
        int32_t      initialContentDelta;   // if zero, then content is all zeros
        uint32_t     initialContentSize;
    };

    // runtime structure of 32-bit arch thread-local thunk
    struct TLV_Thunkv2_32
    {
        void*        func;
        uint16_t     key;
        uint16_t     offset;
        int32_t      machHeaderDelta; // if < 0, content is found by walking load commands. If > 0, then it is size and content is all zeros
    };

#if BUILDING_UNIT_TESTS
    void                    setMock(int tlvKey, std::span<Thunk> thunks, std::span<const uint8_t> content);
#endif

private:
#if BUILDING_UNIT_TESTS
    void                            findInitialContent(const Header* hdr, std::span<const uint8_t>& initialContent, bool& allZeroFill);
#else
    static void                     findInitialContent(const Header* hdr, std::span<const uint8_t>& initialContent, bool& allZeroFill);
#endif
    mach_o::Error                   forEachThunkSpan(const Header* hdr, mach_o::Error (^visit)(std::span<Thunk>));

    // used to record _tlv_atexit() entries to clean up on thread exit
    struct Terminator
    {
        TermFunc      termFunc;
        void*         objAddr;
    };

    struct TerminatorList {
        TerminatorList* next  = nullptr;
        uintptr_t       count = 0;
        Terminator      elements[7];
        void                reverseWalkChain(void (^work)(TerminatorList*));
    };

    static const bool verbose = false;

    dyld_thread_key_t               _terminatorsKey      = 0;
#if BUILDING_UNIT_TESTS
    dyld_thread_key_t               _key;
    std::span<Thunk>                _thunks;
    std::span<const uint8_t>        _initialContent;
    bool                            _allZeroFillContent;
#endif
};


#if BUILDING_LIBDYLD
extern ThreadLocalVariables sThreadLocalVariables;
#endif


} // namespace dyld

#endif /* ThreadLocalVariables_h */
