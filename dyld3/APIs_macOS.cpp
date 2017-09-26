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


#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <_simple.h>
#include <sys/errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <TargetConditionals.h>
#include <malloc/malloc.h>

#include <algorithm>

#include "dlfcn.h"
#include "dyld_priv.h"

#include "AllImages.h"
#include "MachOParser.h"
#include "Loading.h"
#include "Logging.h"
#include "Diagnostics.h"
#include "DyldSharedCache.h"
#include "APIs.h"



typedef dyld3::launch_cache::binary_format::Image BinaryImage;


namespace dyld3 {

// from APIs.cpp
void                                        parseDlHandle(void* h, const mach_header** mh, bool* dontContinue);
const mach_header*                          loadImageAndDependents(Diagnostics& diag, const launch_cache::binary_format::Image* imageToLoad, bool bumpDlopenCount);


// only in macOS and deprecated 
#if __MAC_OS_X_VERSION_MIN_REQUIRED

// macOS needs to support an old API that only works with fileype==MH_BUNDLE.
// In this deprecated API (unlike dlopen), loading and linking are separate steps.
// NSCreateObjectFileImageFrom*() just maps in the bundle mach-o file.
// NSLinkModule() does the load of dependent modules and rebasing/binding.
// To unload one of these, you must call NSUnLinkModule() and NSDestroyObjectFileImage() in any order!
//

NSObjectFileImageReturnCode NSCreateObjectFileImageFromFile(const char* path, NSObjectFileImage* ofi)
{
    log_apis("NSCreateObjectFileImageFromFile(\"%s\", %p)\n", path, ofi);

    // verify path exists
     struct stat statbuf;
    if ( ::stat(path, &statbuf) == -1 )
        return NSObjectFileImageFailure;

    // create ofi that just contains path. NSLinkModule does all the work
    __NSObjectFileImage* result = gAllImages.addNSObjectFileImage();
    result->path        = strdup(path);
    result->memSource   = nullptr;
    result->memLength   = 0;
    result->loadAddress = nullptr;
    result->binImage    = nullptr;
    *ofi = result;

    log_apis("NSCreateObjectFileImageFromFile() => %p\n", result);

    return NSObjectFileImageSuccess;
}

NSObjectFileImageReturnCode NSCreateObjectFileImageFromMemory(const void* memImage, size_t memImageSize, NSObjectFileImage *ofi)
{
    log_apis("NSCreateObjectFileImageFromMemory(%p, 0x%0lX, %p)\n", memImage, memImageSize, ofi);

    // sanity check the buffer is a mach-o file
    __block Diagnostics diag;
    __block const mach_header* foundMH = nullptr;
    if ( MachOParser::isMachO(diag, memImage, memImageSize) ) {
        foundMH = (mach_header*)memImage;
    }
    else {
        FatUtil::forEachSlice(diag, memImage, memImageSize, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, size_t sliceSize, bool& stop) {
            if ( MachOParser::isMachO(diag, sliceStart, sliceSize) ) {
                foundMH = (mach_header*)sliceStart;
                stop = true;
            }
        });
    }
    if ( foundMH == nullptr ) {
        log_apis("NSCreateObjectFileImageFromMemory() not mach-o\n");
        return NSObjectFileImageFailure;
    }

    // this API can only be used with bundles
    if ( foundMH->filetype != MH_BUNDLE ) {
        log_apis("NSCreateObjectFileImageFromMemory() not a bundle, filetype=%d\n", foundMH->filetype);
        return NSObjectFileImageInappropriateFile;
    }

    // allocate ofi that just lists the memory range
    __NSObjectFileImage* result = gAllImages.addNSObjectFileImage();
    result->path        = nullptr;
    result->memSource   = memImage;
    result->memLength   = memImageSize;
    result->loadAddress = nullptr;
    result->binImage    = nullptr;
    *ofi = result;

    log_apis("NSCreateObjectFileImageFromMemory() => %p\n", result);

    return NSObjectFileImageSuccess;
}

NSModule NSLinkModule(NSObjectFileImage ofi, const char* moduleName, uint32_t options)
{
    log_apis("NSLinkModule(%p, \"%s\", 0x%08X)\n", ofi, moduleName, options);

    // ofi is invalid if not in list
    if ( !gAllImages.hasNSObjectFileImage(ofi) ) {
        log_apis("NSLinkModule() => NULL (invalid NSObjectFileImage)\n");
        return nullptr;
    }

    // if this is memory based image, write to temp file, then use file based loading
    const BinaryImage* imageToLoad = nullptr;
    if ( ofi->memSource != nullptr ) {
        // make temp file with content of memory buffer
        bool successfullyWritten = false;
        ofi->path = ::tempnam(nullptr, "NSCreateObjectFileImageFromMemory-");
        if ( ofi->path != nullptr ) {
            int fd = ::open(ofi->path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if ( fd != -1 ) {
                ssize_t writtenSize = ::pwrite(fd, ofi->memSource, ofi->memLength, 0);
                if ( writtenSize == ofi->memLength )
                    successfullyWritten = true;
                ::close(fd);
            }
        }
        if ( !successfullyWritten ) {
            if ( ofi->path != nullptr ) {
                free((void*)ofi->path);
                ofi->path = nullptr;
            }
            log_apis("NSLinkModule() => NULL (could not save memory image to temp file)\n");
            return nullptr;
        }
    }
    else {
        // check if image is in a known ImageGroup, but not loaded. if so, load using existing closure info
        log_apis("   NSLinkModule: checking for pre-built closure for path: %s\n", ofi->path);
        imageToLoad = gAllImages.findImageInKnownGroups(ofi->path);
        // TODO: check symlinks, realpath
    }

    // if no existing closure, RPC to closured to create one
    if ( imageToLoad == nullptr ) {
        const char* closuredErrorMessages[3];
        int closuredErrorMessagesCount = 0;
        if ( imageToLoad == nullptr ) {
            imageToLoad = gAllImages.messageClosured(ofi->path, "NSLinkModule", closuredErrorMessages, closuredErrorMessagesCount);
        }
        for (int i=0; i < closuredErrorMessagesCount; ++i) {
            log_apis("   NSLinkModule: failed: %s\n", closuredErrorMessages[i]);
            free((void*)closuredErrorMessages[i]);
        }
    }

    // use Image info to load and fixup image and all its dependents
    if ( imageToLoad != nullptr ) {
        Diagnostics diag;
        ofi->loadAddress = loadImageAndDependents(diag, imageToLoad, true);
        if ( diag.hasError() )
            log_apis("   NSLinkModule: failed: %s\n", diag.errorMessage());
   }

    // if memory based load, delete temp file
    if ( ofi->memSource != nullptr ) {
        log_apis("   NSLinkModule: delete temp file: %s\n", ofi->path);
        ::unlink(ofi->path);
    }

    log_apis("NSLinkModule() => %p\n", ofi->loadAddress);
    return (NSModule)ofi->loadAddress;
}

// NSUnLinkModule unmaps the image, but does not release the NSObjectFileImage
bool NSUnLinkModule(NSModule module, uint32_t options)
{
    log_apis("NSUnLinkModule(%p, 0x%08X)\n", module, options);

    bool result = false;
    const mach_header*  mh = (mach_header*)module;
    launch_cache::Image image = gAllImages.findByLoadAddress(mh);
    if ( image.valid() ) {
        // removes image if reference count went to zero
        gAllImages.decRefCount(mh);
        result = true;
    }

    log_apis("NSUnLinkModule() => %d\n", result);

    return result;
}

// NSDestroyObjectFileImage releases the NSObjectFileImage, but the mapped image may remain in use
bool NSDestroyObjectFileImage(NSObjectFileImage ofi)
{
    log_apis("NSDestroyObjectFileImage(%p)\n", ofi);

    // ofi is invalid if not in list
    if ( !gAllImages.hasNSObjectFileImage(ofi) )
        return false;

    // keep copy of info
    const void* memSource = ofi->memSource;
    size_t      memLength = ofi->memLength;
    const char* path      = ofi->path;

    // remove from list
    gAllImages.removeNSObjectFileImage(ofi);

    // if object was created from a memory, release that memory
    // NOTE: this is the way dyld has always done this. NSCreateObjectFileImageFromMemory() hands ownership of the memory to dyld
    if ( memSource != nullptr ) {
        // we don't know if memory came from malloc or vm_allocate, so ask malloc
        if ( malloc_size(memSource) != 0 )
            free((void*)(memSource));
        else
            vm_deallocate(mach_task_self(), (vm_address_t)memSource, memLength);
    }
    free((void*)path);

    return true;
}

uint32_t NSSymbolDefinitionCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
    halt("NSSymbolDefinitionCountInObjectFileImage() is obsolete");
}

const char* NSSymbolDefinitionNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal)
{
    halt("NSSymbolDefinitionNameInObjectFileImage() is obsolete");
}

uint32_t NSSymbolReferenceCountInObjectFileImage(NSObjectFileImage objectFileImage)
{
    halt("NSSymbolReferenceCountInObjectFileImage() is obsolete");
}

const char* NSSymbolReferenceNameInObjectFileImage(NSObjectFileImage objectFileImage, uint32_t ordinal, bool *tentative_definition)
{
    halt("NSSymbolReferenceNameInObjectFileImage() is obsolete");
}

bool NSIsSymbolDefinedInObjectFileImage(NSObjectFileImage ofi, const char* symbolName)
{
    log_apis("NSIsSymbolDefinedInObjectFileImage(%p, %s)\n", ofi, symbolName);

    // ofi is invalid if not in list
    if ( !gAllImages.hasNSObjectFileImage(ofi) )
        return false;

    void* addr;
    MachOParser parser(ofi->loadAddress);
    return parser.hasExportedSymbol(symbolName, ^(uint32_t , const char*, void*, const mach_header**, void**) {
        return false;
    }, &addr);
}

void* NSGetSectionDataInObjectFileImage(NSObjectFileImage ofi, const char* segmentName, const char* sectionName, size_t* size)
{
    // ofi is invalid if not in list
    if ( !gAllImages.hasNSObjectFileImage(ofi) )
        return nullptr;

    __block void* result = nullptr;
    MachOParser parser(ofi->loadAddress);
    parser.forEachSection(^(const char* aSegName, const char* aSectName, uint32_t flags, const void* content, size_t aSize, bool illegalSectionSize, bool& stop) {
        if ( (strcmp(sectionName, aSectName) == 0) && (strcmp(segmentName, aSegName) == 0) ) {
            result = (void*)content;
            if ( size != nullptr )
                *size = aSize;
            stop = true;
        }
    });
    return result;
}

const char* NSNameOfModule(NSModule m)
{
    log_apis("NSNameOfModule(%p)\n", m);

    const mach_header* foundInLoadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(m, &foundInLoadAddress);
    if ( image.valid() ) {
        return gAllImages.imagePath(image.binaryData());
    }
    return nullptr;
}

const char* NSLibraryNameForModule(NSModule m)
{
    log_apis("NSLibraryNameForModule(%p)\n", m);

    const mach_header* foundInLoadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(m, &foundInLoadAddress);
    if ( image.valid() ) {
        return gAllImages.imagePath(image.binaryData());
    }
    return nullptr;
}


static bool flatFindSymbol(const char* symbolName, void** symbolAddress, const mach_header** foundInImageAtLoadAddress)
{
    for (uint32_t index=0; index < gAllImages.count(); ++index) {
        const mach_header* loadAddress;
        launch_cache::Image image = gAllImages.findByLoadOrder(index, &loadAddress);
        if ( image.valid() ) {
            MachOParser parser(loadAddress);
            if ( parser.hasExportedSymbol(symbolName, ^(uint32_t , const char* , void* , const mach_header** , void**) { return false; }, symbolAddress) ) {
                *foundInImageAtLoadAddress = loadAddress;
                return true;
            }
        }
    }
    return false;
}

bool NSIsSymbolNameDefined(const char* symbolName)
{
    log_apis("NSIsSymbolNameDefined(%s)\n", symbolName);

    const mach_header* foundInImageAtLoadAddress;
    void* address;
    return flatFindSymbol(symbolName, &address, &foundInImageAtLoadAddress);
}

bool NSIsSymbolNameDefinedWithHint(const char* symbolName, const char* libraryNameHint)
{
    log_apis("NSIsSymbolNameDefinedWithHint(%s, %s)\n", symbolName, libraryNameHint);

    const mach_header* foundInImageAtLoadAddress;
    void* address;
    return flatFindSymbol(symbolName, &address, &foundInImageAtLoadAddress);
}

bool NSIsSymbolNameDefinedInImage(const struct mach_header* mh, const char* symbolName)
{
    log_apis("NSIsSymbolNameDefinedInImage(%p, %s)\n", mh, symbolName);

    MachOParser::DependentFinder reExportFollower = ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        *foundMH = gAllImages.alreadyLoaded(depLoadPath, false);
        return (*foundMH != nullptr);
    };

    MachOParser parser(mh);
    void* result;
    return parser.hasExportedSymbol(symbolName, reExportFollower, &result);
}

NSSymbol NSLookupAndBindSymbol(const char* symbolName)
{
    log_apis("NSLookupAndBindSymbol(%s)\n", symbolName);

    const mach_header* foundInImageAtLoadAddress;
    void* symbolAddress;
    if ( flatFindSymbol(symbolName, &symbolAddress, &foundInImageAtLoadAddress) ) {
        return (NSSymbol)symbolAddress;
    }
    return nullptr;
}

NSSymbol NSLookupAndBindSymbolWithHint(const char* symbolName, const char* libraryNameHint)
{
    log_apis("NSLookupAndBindSymbolWithHint(%s, %s)\n", symbolName, libraryNameHint);

    const mach_header* foundInImageAtLoadAddress;
    void* symbolAddress;
    if ( flatFindSymbol(symbolName, &symbolAddress, &foundInImageAtLoadAddress) ) {
        return (NSSymbol)symbolAddress;
    }
    return nullptr;
}

NSSymbol NSLookupSymbolInModule(NSModule module, const char* symbolName)
{
    log_apis("NSLookupSymbolInModule(%p. %s)\n", module, symbolName);

    MachOParser::DependentFinder reExportFollower = ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        *foundMH = gAllImages.alreadyLoaded(depLoadPath, false);
        return (*foundMH != nullptr);
    };

    const mach_header* mh = (const mach_header*)module;
    uint32_t loadIndex;
    if ( gAllImages.findIndexForLoadAddress(mh, loadIndex) ) {
        MachOParser parser(mh);
        void* symAddress;
        if ( parser.hasExportedSymbol(symbolName, reExportFollower, &symAddress) ) {
            return (NSSymbol)symAddress;
        }
    }
    return nullptr;
}

NSSymbol NSLookupSymbolInImage(const struct mach_header* mh, const char* symbolName, uint32_t options)
{
    log_apis("NSLookupSymbolInImage(%p, \"%s\", 0x%08X)\n", mh, symbolName, options);

    MachOParser::DependentFinder reExportFollower = ^(uint32_t depIndex, const char* depLoadPath, void* extra, const mach_header** foundMH, void** foundExtra) {
        *foundMH = gAllImages.alreadyLoaded(depLoadPath, false);
        return (*foundMH != nullptr);
    };

    MachOParser parser(mh);
    void* result;
    if ( parser.hasExportedSymbol(symbolName, reExportFollower, &result) ) {
        log_apis("   NSLookupSymbolInImage() => %p\n", result);
        return (NSSymbol)result;
    }

    if ( options & NSLOOKUPSYMBOLINIMAGE_OPTION_RETURN_ON_ERROR ) {
        log_apis("   NSLookupSymbolInImage() => NULL\n");
        return nullptr;
    }
    return nullptr;
}

const char* NSNameOfSymbol(NSSymbol symbol)
{
    halt("NSNameOfSymbol() is obsolete");
}

void* NSAddressOfSymbol(NSSymbol symbol)
{
    log_apis("NSAddressOfSymbol(%p)\n", symbol);

    // in dyld 1.0, NSSymbol was a pointer to the nlist entry in the symbol table
    return (void*)symbol;
}

NSModule NSModuleForSymbol(NSSymbol symbol)
{
    log_apis("NSModuleForSymbol(%p)\n", symbol);

    const mach_header* foundInLoadAddress;
    launch_cache::Image image = gAllImages.findByOwnedAddress(symbol, &foundInLoadAddress);
    if ( image.valid() ) {
        return (NSModule)foundInLoadAddress;
    }
    return nullptr;
}

void NSLinkEditError(NSLinkEditErrors *c, int *errorNumber, const char** fileName, const char** errorString)
{
    log_apis("NSLinkEditError(%p, %p, %p, %p)\n", c, errorNumber, fileName, errorString);
    *c = NSLinkEditOtherError;
    *errorNumber = 0;
    *fileName = NULL;
    *errorString = NULL;
}

bool NSAddLibrary(const char* pathName)
{
    log_apis("NSAddLibrary(%s)\n", pathName);

    return ( dlopen(pathName, 0) != nullptr);
}

bool NSAddLibraryWithSearching(const char* pathName)
{
    log_apis("NSAddLibraryWithSearching(%s)\n", pathName);

    return ( dlopen(pathName, 0) != nullptr);
}

const mach_header* NSAddImage(const char* imageName, uint32_t options)
{
    log_apis("NSAddImage(\"%s\", 0x%08X)\n", imageName, options);

    // Note: this is a quick and dirty implementation that just uses dlopen() and ignores some option flags
    uint32_t dloptions = 0;
    if ( (options & NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED) != 0 )
        dloptions |= RTLD_NOLOAD;

    void* h = dlopen(imageName, dloptions);
    if ( h != nullptr ) {
        const mach_header* mh;
        bool dontContinue;
        parseDlHandle(h, &mh, &dontContinue);
        return mh;
    }

    if ( (options & (NSADDIMAGE_OPTION_RETURN_ON_ERROR|NSADDIMAGE_OPTION_RETURN_ONLY_IF_LOADED)) == 0 ) {
        halt("NSAddImage() image not found");
    }
    return nullptr;
}

void NSInstallLinkEditErrorHandlers(const NSLinkEditErrorHandlers *handlers)
{
    halt("NSInstallLinkEditErrorHandlers() is obsolete");
}

bool _dyld_present(void)
{
    log_apis("_dyld_present()\n");

    return true;
}

bool _dyld_launched_prebound(void)  
{
    halt("_dyld_launched_prebound() is obsolete");
}

bool _dyld_all_twolevel_modules_prebound(void)
{
    halt("_dyld_all_twolevel_modules_prebound() is obsolete");
}

bool _dyld_bind_fully_image_containing_address(const void* address)
{
    log_apis("_dyld_bind_fully_image_containing_address(%p)\n", address);

    // in dyld3, everything is always fully bound
    return true;
}

bool _dyld_image_containing_address(const void* address)
{
    log_apis("_dyld_image_containing_address(%p)\n", address);

    return (dyld_image_header_containing_address(address) != nullptr);
}

void _dyld_lookup_and_bind(const char* symbolName, void **address, NSModule* module)
{
    log_apis("_dyld_lookup_and_bind(%s, %p, %p)\n", symbolName, address, module);

    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        *module = (NSModule)foundInImageAtLoadAddress;
        return;
    }

    *address = 0;
    *module = 0;
}

void _dyld_lookup_and_bind_with_hint(const char* symbolName, const char* libraryNameHint, void** address, NSModule* module)
{
    log_apis("_dyld_lookup_and_bind_with_hint(%s, %s, %p, %p)\n", symbolName, libraryNameHint, address, module);

    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        *module = (NSModule)foundInImageAtLoadAddress;
        return;
    }

    *address = 0;
    *module = 0;
}


void _dyld_lookup_and_bind_fully(const char* symbolName, void** address, NSModule* module)
{
    log_apis("_dyld_lookup_and_bind_fully(%s, %p, %p)\n", symbolName, address, module);

    const mach_header* foundInImageAtLoadAddress;
    if ( flatFindSymbol(symbolName, address, &foundInImageAtLoadAddress) ) {
        *module = (NSModule)foundInImageAtLoadAddress;
        return;
    }

    *address = 0;
    *module = 0;
}

const struct mach_header* _dyld_get_image_header_containing_address(const void* address)
{
    log_apis("_dyld_get_image_header_containing_address(%p)\n", address);

    return dyld_image_header_containing_address(address);
}

#endif


} // namespace dyld3

