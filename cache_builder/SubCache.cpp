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

#include "BuilderConfig.h"
#include "BuilderOptions.h"
#include "CacheDylib.h"
#include "CodeSigningTypes.h"
#include "DyldSharedCache.h"
#include "SubCache.h"
#include "JSONWriter.h"

#include "dyld_cache_format.h"

#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonDigestSPI.h>

using dyld3::GradedArchs;
using dyld3::MachOFile;

using error::Error;

using namespace cache_builder;

static inline uint64_t alignPage(uint64_t value)
{
    // Align to 16KB even on x86_64.  That makes it easier for arm64 machines to map in the cache.
    const uint64_t MinRegionAlignment = 0x4000;

    return ((value + MinRegionAlignment - 1) & (-MinRegionAlignment));
}

// MARK: --- SharedCacheBuilder::Region methods ---

uint32_t Region::initProt() const
{
    uint32_t maxProt = 0;
    switch ( this->kind ) {
        case Region::Kind::text:
            maxProt = VM_PROT_READ | VM_PROT_EXECUTE;
            break;
        case Region::Kind::data:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::dataConst:
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::auth:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::authConst:
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::linkedit:
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::dynamicConfig:
            // HACK: This is not actually mapped, but it is used in vm calculations
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::unmapped:
        case Region::Kind::codeSignature:
            // This isn't mapped, so we should never ask for its maxprot
            assert(0);
            break;
        case Region::Kind::numKinds:
            assert(0);
            break;
    }

    return maxProt;
}

uint32_t Region::maxProt() const
{
    uint32_t maxProt = 0;
    switch ( this->kind ) {
        case Region::Kind::text:
            maxProt = VM_PROT_READ | VM_PROT_EXECUTE;
            break;
        case Region::Kind::data:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::dataConst:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::auth:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::authConst:
            maxProt = VM_PROT_READ | VM_PROT_WRITE;
            break;
        case Region::Kind::linkedit:
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::dynamicConfig:
            // HACK: This is not actually mapped, but it is used in vm calculations
            maxProt = VM_PROT_READ;
            break;
        case Region::Kind::unmapped:
        case Region::Kind::codeSignature:
            // This isn't mapped, so we should never ask for its maxprot
            assert(0);
            break;
        case Region::Kind::numKinds:
            assert(0);
            break;
    }

    return maxProt;
}

bool Region::canContainAuthPointers() const
{
    bool result = false;
    switch ( this->kind ) {
        case Region::Kind::text:
            // We should never ask this region if it has auth content
            assert(0);
            break;
        case Region::Kind::data:
        case Region::Kind::dataConst:
            result = false;
            break;
        case Region::Kind::auth:
        case Region::Kind::authConst:
            result = true;
            break;
        case Region::Kind::linkedit:
        case Region::Kind::dynamicConfig:
            // We should never ask this region if it has auth content
            assert(0);
            break;
        case Region::Kind::unmapped:
        case Region::Kind::codeSignature:
            // We should never ask this region if it has auth content
            assert(0);
            break;
        case Region::Kind::numKinds:
            assert(0);
            break;
    }

    return result;
}

// Returns true if the given Region should be saved as a Mapping in the shared cache
bool Region::needsSharedCacheMapping() const
{
    switch ( this->kind ) {
        case Region::Kind::text:
        case Region::Kind::data:
        case Region::Kind::dataConst:
        case Region::Kind::auth:
        case Region::Kind::authConst:
        case Region::Kind::linkedit:
            return true;
        case Region::Kind::unmapped:
        case Region::Kind::dynamicConfig:
        case Region::Kind::codeSignature:
            return false;
        case Region::Kind::numKinds:
            assert(0);
    }
}

// Returns true if the given Region has content that requires reserved address space
bool Region::needsSharedCacheReserveAddressSpace() const
{
    switch ( this->kind ) {
        case Region::Kind::text:
        case Region::Kind::data:
        case Region::Kind::dataConst:
        case Region::Kind::auth:
        case Region::Kind::authConst:
        case Region::Kind::linkedit:
        case Region::Kind::dynamicConfig:
            return true;
        case Region::Kind::unmapped:
        case Region::Kind::codeSignature:
            return false;
        case Region::Kind::numKinds:
            assert(0);
    }
}

// Returns true if we need 32MB of padding between regions.  This benefits page tables
// where we don't want something like TEXT and DATA on the same pages, as then that needs extra
// page table entries.
bool Region::needsRegionPadding(const Region& next) const
{
    if ( !this->needsSharedCacheMapping() || !next.needsSharedCacheMapping() )
        return false;

    switch ( this->kind ) {
        case Region::Kind::text: {
            // Add padding if TEXT is adjacent to something that is mutable,
            // ie, next to DATA/etc.
            // Note we want TEXT to be adjacent to DATA_CONST as we don't expect DATA_CONST to change
            bool nextIsRW = (next.initProt() & VM_PROT_WRITE) != 0;
            return nextIsRW;
        }
        case Region::Kind::data:
        case Region::Kind::auth: {
            // HACK: Remove once we have rdar://96315050
            if ( (this->kind == Region::Kind::auth) && (next.kind == Region::Kind::authConst) )
                return false;

            // Add padding if DATA is adjacent to something immutable, eg TEXT/DATA_CONST
            bool nextIsRO = (next.initProt() & VM_PROT_WRITE) == 0;
            return nextIsRO;
        }
        case Region::Kind::dataConst:
        case Region::Kind::authConst: {
            // Don't add padding if *_CONST is next to *_CONST, otherwise add padding
            bool nextInitIsRO = (next.initProt() & VM_PROT_WRITE) == 0;
            bool nextMaxIsRW = (next.maxProt() & VM_PROT_WRITE) != 0;
            bool nextIsDataConst = nextInitIsRO & nextMaxIsRW;
            return !nextIsDataConst;
        }
        case Region::Kind::linkedit:
        case Region::Kind::dynamicConfig: {
            // Add padding if LINKEDIT is adjacent to something that is mutable,
            // ie, next to DATA/DATA_CONST/etc
            bool nextIsRW = (next.maxProt() & VM_PROT_WRITE) != 0;
            return nextIsRW;
        }
        case Region::Kind::unmapped:
        case Region::Kind::codeSignature:
            return false;
        case Region::Kind::numKinds:
            assert(0);
    }
}



// MARK: --- SubCache methods ---

SubCache::SubCache(Kind kind)
    : kind(kind)
{
    memset(&this->uuidString[0], '\0', sizeof(this->uuidString));

    // Start every subCache with all the regions it might need
    uint32_t maxSize = (uint32_t)Region::Kind::numKinds;
    this->regions.reserve(maxSize);
    for ( uint32_t i = 0; i != (uint32_t)Region::Kind::numKinds; ++i )
        this->regions.emplace_back((Region::Kind)i);
}

SubCache SubCache::makeMainCache(const BuilderOptions& options, bool isDevelopment)
{
    Kind kind = isDevelopment ? Kind::mainDevelopment : Kind::mainCustomer;
    SubCache subCache(kind);

    // If we are a universal cache, then .development actually gets a suffix
    if ( options.kind == CacheKind::universal ) {
        subCache.fileSuffix = isDevelopment ? ".development" : "";
    } else {
        subCache.fileSuffix = "";
    }

    return subCache;
}

SubCache SubCache::makeSubCache(const BuilderOptions& options)
{
    SubCache subCache(Kind::subUniversal);

    // We'll set this later, after subCaches have been split for universal caches
    subCache.fileSuffix = "unset";

    return subCache;
}

SubCache SubCache::makeStubsCache(const BuilderOptions& options, bool isDevelopment)
{
    Kind kind = isDevelopment ? Kind::stubsDevelopment : Kind::stubsCustomer;
    SubCache subCache(kind);

    // We'll set this later, after subCaches have been split for universal caches
    subCache.fileSuffix = "unset";

    return subCache;
}

SubCache SubCache::makeSymbolsCache()
{
    SubCache subCache(Kind::symbols);
    subCache.fileSuffix = ".symbols";
    return subCache;
}

static bool hasDataRegion(std::span<Region> regions)
{
    for ( const Region& region : regions ) {
        if ( region.chunks.empty() )
            continue;
        switch ( region.kind ) {
            case cache_builder::Region::Kind::text:
                break;
            case cache_builder::Region::Kind::dataConst:
            case cache_builder::Region::Kind::data:
            case cache_builder::Region::Kind::auth:
            case cache_builder::Region::Kind::authConst:
                return true;
            case cache_builder::Region::Kind::linkedit:
            case cache_builder::Region::Kind::unmapped:
            case cache_builder::Region::Kind::dynamicConfig:
            case cache_builder::Region::Kind::codeSignature:
            case cache_builder::Region::Kind::numKinds:
                break;
        }
    }
    return false;
}

static bool hasLinkeditRegion(std::span<Region> regions)
{
    for ( const Region& region : regions ) {
        if ( region.chunks.empty() )
            continue;
        switch ( region.kind ) {
            case cache_builder::Region::Kind::text:
            case cache_builder::Region::Kind::dataConst:
            case cache_builder::Region::Kind::data:
            case cache_builder::Region::Kind::auth:
            case cache_builder::Region::Kind::authConst:
                break;
            case cache_builder::Region::Kind::linkedit:
                return true;
            case cache_builder::Region::Kind::unmapped:
            case cache_builder::Region::Kind::dynamicConfig:
            case cache_builder::Region::Kind::codeSignature:
            case cache_builder::Region::Kind::numKinds:
                break;
        }
    }
    return false;
}

void SubCache::setSuffix(dyld3::Platform platform, bool forceDevelopmentSubCacheSuffix,
                         size_t subCacheIndex)
{
    assert(this->isSubCache() || this->isStubsCache());
    assert(subCacheIndex > 0);

    const char* dataSuffix = forceDevelopmentSubCacheSuffix ? ".development.dylddata" : ".dylddata";
    const char* linkeditSuffix = forceDevelopmentSubCacheSuffix ? ".development.dyldlinkedit" : ".dyldlinkedit";
    const char* subCacheSuffix = forceDevelopmentSubCacheSuffix ? ".development" : "";

    if ( platform == dyld3::Platform::macOS ) {
        // macOS never has a .development suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex);
    } else if ( platform == dyld3::Platform::driverKit ) {
        // driverKit never has a .development suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex);
    } else if ( this->isStubsDevelopmentCache() ) {
        // Dev stubs always have a suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex) + ".development";
    } else if ( this->isStubsCustomerCache() ) {
        // Customer stubs never have a suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex);
    } else if ( hasDataRegion(this->regions) ) {
        // Data only subcaches have their own suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex) + dataSuffix;
    } else if ( hasLinkeditRegion(this->regions) ) {
        // Linkedit only subcaches have their own suffix
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex) + linkeditSuffix;
    } else {
        this->fileSuffix = "." + dyld3::json::decimal(subCacheIndex) + subCacheSuffix;
    }
}

static std::string getCodeSigningIdentifier(const BuilderOptions& options)
{
    std::string cacheIdentifier = "com.apple.dyld.cache.";
    cacheIdentifier += options.archs.name();
    if ( options.dylibsRemovedFromDisk ) {
        switch ( options.kind ) {
            case CacheKind::development:
                cacheIdentifier += ".development";
                break;
            case CacheKind::universal:
                cacheIdentifier += ".universal";
                break;
        }
    }

    return cacheIdentifier;
}
struct CodeSignatureLayout
{
    // These fields configure the code signature
    bool     agile              = false;
    uint8_t  dscHashType        = 0;
    uint8_t  dscHashSize        = 0;
    uint32_t dscDigestFormat    = 0;

    // These describe the layout of the signature
    uint32_t blobCount          = 0;
    uint32_t slotCount          = 0;
    uint32_t xSlotCount         = 0;
    size_t   idOffset           = 0;
    size_t   hashOffset         = 0;
    size_t   hash256Offset      = 0;
    size_t   cdSize             = 0;
    size_t   cd256Size          = 0;
    size_t   reqsSize           = 0;
    size_t   cmsSize            = 0;
    size_t   cdOffset           = 0;
    size_t   cd256Offset        = 0;
    size_t   reqsOffset         = 0;
    size_t   cmsOffset          = 0;
    size_t   sbSize             = 0;
    size_t   sigSize            = 0;
};

static CodeSignatureLayout getCodeSignatureLayout(const BuilderOptions& options,
                                                  const BuilderConfig& config,
                                                  CacheFileSize subCacheSize)
{
    CodeSignatureLayout layout;

    const uint32_t pageSize = config.codeSign.pageSize;
    assert((subCacheSize.rawValue() % pageSize) == 0);

    layout.agile = false;

    // select which codesigning hash
    switch ( config.codeSign.mode ) {
        case CodeSign::Mode::agile:
            layout.agile = true;
            // Fall through to SHA1, because the main code directory remains SHA1 for compatibility.
            [[clang::fallthrough]];
        case CodeSign::Mode::onlySHA1:
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            layout.dscHashType     = CS_HASHTYPE_SHA1;
            layout.dscHashSize     = CS_HASH_SIZE_SHA1;
            layout.dscDigestFormat = kCCDigestSHA1;
#pragma clang diagnostic pop
            break;
        case CodeSign::Mode::onlySHA256:
            layout.dscHashType     = CS_HASHTYPE_SHA256;
            layout.dscHashSize     = CS_HASH_SIZE_SHA256;
            layout.dscDigestFormat = kCCDigestSHA256;
            break;
    }

    std::string cacheIdentifier = getCodeSigningIdentifier(options);

    // layout code signature contents
    size_t idSize            = cacheIdentifier.size()+1; // +1 for terminating 0
    layout.blobCount         = layout.agile ? 4 : 3;
    layout.slotCount         = (uint32_t)(subCacheSize.rawValue() / pageSize);
    layout.xSlotCount        = CSSLOT_REQUIREMENTS;
    layout.idOffset          = offsetof(CS_CodeDirectory, end_withExecSeg);
    layout.hashOffset        = layout.idOffset + idSize + layout.dscHashSize * layout.xSlotCount;
    layout.hash256Offset     = layout.idOffset + idSize + CS_HASH_SIZE_SHA256 * layout.xSlotCount;
    layout.cdSize            = layout.hashOffset + (layout.slotCount * layout.dscHashSize);
    layout.cd256Size         = layout.agile ? layout.hash256Offset + (layout.slotCount * CS_HASH_SIZE_SHA256) : 0;
    layout.reqsSize          = 12;
    layout.cmsSize           = sizeof(CS_Blob);
    layout.cdOffset          = sizeof(CS_SuperBlob) + layout.blobCount * sizeof(CS_BlobIndex);
    layout.cd256Offset       = layout.cdOffset + layout.cdSize;
    layout.reqsOffset        = layout.cd256Offset + layout.cd256Size; // equals cdOffset + cdSize if not agile
    layout.cmsOffset         = layout.reqsOffset + layout.reqsSize;
    layout.sbSize            = layout.cmsOffset + layout.cmsSize;
    layout.sigSize           = alignPage(layout.sbSize);       // keep whole cache 16KB aligned

    return layout;
}

void SubCache::setCodeSignatureSize(const BuilderOptions& options, const BuilderConfig& config,
                                    CacheFileSize estimatedSize)
{
    CodeSignatureLayout estimatedLayout = getCodeSignatureLayout(options, config, estimatedSize);

    this->codeSignature->cacheVMSize      = CacheVMSize(0ULL);
    this->codeSignature->subCacheFileSize = CacheFileSize((uint64_t)estimatedLayout.sigSize);
}

void SubCache::codeSign(Diagnostics& diag, const BuilderOptions& options, const BuilderConfig& config)
{
    CodeSignatureChunk& cacheCodeSignatureChunk   = *this->codeSignature.get();
    const uint32_t      pageSize                  = config.codeSign.pageSize;

    uint64_t subCacheBufferSize = 0;
    for ( const Region& region : this->regions ) {
        // Skip the code signature.  We don't need to measure it as we are computing it
        if ( region.kind == Region::Kind::codeSignature )
            continue;

        subCacheBufferSize = std::max(subCacheBufferSize, (region.subCacheFileOffset + region.subCacheFileSize).rawValue());
    }

    CodeSignatureLayout layout = getCodeSignatureLayout(options, config,
                                                        CacheFileSize(subCacheBufferSize));

    if ( layout.sigSize > cacheCodeSignatureChunk.subCacheFileSize.rawValue() ) {
        diag.error("Overflow in code signature size");
        return;
    }

    // create overall code signature which is a superblob
    CS_SuperBlob* sb    = reinterpret_cast<CS_SuperBlob*>(cacheCodeSignatureChunk.subCacheBuffer);
    sb->magic           = htonl(CSMAGIC_EMBEDDED_SIGNATURE);
    sb->length          = htonl(layout.sbSize);
    sb->count           = htonl(layout.blobCount);
    sb->index[0].type   = htonl(CSSLOT_CODEDIRECTORY);
    sb->index[0].offset = htonl(layout.cdOffset);
    sb->index[1].type   = htonl(CSSLOT_REQUIREMENTS);
    sb->index[1].offset = htonl(layout.reqsOffset);
    sb->index[2].type   = htonl(CSSLOT_CMS_SIGNATURE);
    sb->index[2].offset = htonl(layout.cmsOffset);
    if ( layout.agile ) {
        sb->index[3].type   = htonl(CSSLOT_ALTERNATE_CODEDIRECTORIES + 0);
        sb->index[3].offset = htonl(layout.cd256Offset);
    }

    // fill in empty requirements
    CS_RequirementsBlob* reqs = (CS_RequirementsBlob*)(((char*)sb) + layout.reqsOffset);
    reqs->magic               = htonl(CSMAGIC_REQUIREMENTS);
    reqs->length              = htonl(sizeof(CS_RequirementsBlob));
    reqs->data                = 0;

    // initialize fixed fields of Code Directory
    CS_CodeDirectory* cd = (CS_CodeDirectory*)(((char*)sb) + layout.cdOffset);
    cd->magic            = htonl(CSMAGIC_CODEDIRECTORY);
    cd->length           = htonl(layout.cdSize);
    cd->version          = htonl(0x20400); // supports exec segment
    cd->flags            = htonl(kSecCodeSignatureAdhoc);
    cd->hashOffset       = htonl(layout.hashOffset);
    cd->identOffset      = htonl(layout.idOffset);
    cd->nSpecialSlots    = htonl(layout.xSlotCount);
    cd->nCodeSlots       = htonl(layout.slotCount);
    cd->codeLimit        = htonl(subCacheBufferSize);
    cd->hashSize         = layout.dscHashSize;
    cd->hashType         = layout.dscHashType;
    cd->platform         = 0;                       // not platform binary
    cd->pageSize         = __builtin_ctz(pageSize); // log2(CS_PAGE_SIZE);
    cd->spare2           = 0;                       // unused (must be zero)
    cd->scatterOffset    = 0;                       // not supported anymore
    cd->teamOffset       = 0;                       // no team ID
    cd->spare3           = 0;                       // unused (must be zero)
    cd->codeLimit64      = 0;                       // falls back to codeLimit

    // executable segment info
    cd->execSegBase  = 0;
    cd->execSegLimit = 0;
    cd->execSegFlags = 0; // not a main binary

    for ( const Region& region : this->regions ) {
        if ( region.kind == Region::Kind::text ) {
            cd->execSegBase  = htonll(region.subCacheFileOffset.rawValue()); // base of TEXT segment
            cd->execSegLimit = htonll(region.subCacheFileSize.rawValue());   // size of TEXT segment
        }
    }

    std::string cacheIdentifier = getCodeSigningIdentifier(options);

    // initialize dynamic fields of Code Directory
    strcpy((char*)cd + layout.idOffset, cacheIdentifier.c_str());

    // add special slot hashes
    uint8_t* hashSlot     = (uint8_t*)cd + layout.hashOffset;
    uint8_t* reqsHashSlot = &hashSlot[-CSSLOT_REQUIREMENTS * layout.dscHashSize];
    CCDigest(layout.dscDigestFormat, (uint8_t*)reqs, sizeof(CS_RequirementsBlob), reqsHashSlot);

    CS_CodeDirectory* cd256;
    uint8_t*          hash256Slot;
    uint8_t*          reqsHash256Slot;
    if ( layout.agile ) {
        // Note that the assumption here is that the size up to the hashes is the same as for
        // sha1 code directory, and that they come last, after everything else.

        cd256                = (CS_CodeDirectory*)(((char*)sb) + layout.cd256Offset);
        cd256->magic         = htonl(CSMAGIC_CODEDIRECTORY);
        cd256->length        = htonl(layout.cd256Size);
        cd256->version       = htonl(0x20400); // supports exec segment
        cd256->flags         = htonl(kSecCodeSignatureAdhoc);
        cd256->hashOffset    = htonl(layout.hash256Offset);
        cd256->identOffset   = htonl(layout.idOffset);
        cd256->nSpecialSlots = htonl(layout.xSlotCount);
        cd256->nCodeSlots    = htonl(layout.slotCount);
        cd256->codeLimit     = htonl(subCacheBufferSize);
        cd256->hashSize      = CS_HASH_SIZE_SHA256;
        cd256->hashType      = CS_HASHTYPE_SHA256;
        cd256->platform      = 0;                       // not platform binary
        cd256->pageSize      = __builtin_ctz(pageSize); // log2(CS_PAGE_SIZE);
        cd256->spare2        = 0;                       // unused (must be zero)
        cd256->scatterOffset = 0;                       // not supported anymore
        cd256->teamOffset    = 0;                       // no team ID
        cd256->spare3        = 0;                       // unused (must be zero)
        cd256->codeLimit64   = 0;                       // falls back to codeLimit

        // executable segment info
        cd256->execSegBase  = cd->execSegBase;
        cd256->execSegLimit = cd->execSegLimit;
        cd256->execSegFlags = cd->execSegFlags;

        // initialize dynamic fields of Code Directory
        strcpy((char*)cd256 + layout.idOffset, cacheIdentifier.c_str());

        // add special slot hashes
        hash256Slot     = (uint8_t*)cd256 + layout.hash256Offset;
        reqsHash256Slot = &hash256Slot[-CSSLOT_REQUIREMENTS * CS_HASH_SIZE_SHA256];
        CCDigest(kCCDigestSHA256, (uint8_t*)reqs, sizeof(CS_RequirementsBlob), reqsHash256Slot);
    }
    else {
        cd256           = NULL;
        hash256Slot     = NULL;
        reqsHash256Slot = NULL;
    }

    // fill in empty CMS blob for ad-hoc signing
    CS_Blob* cms = (CS_Blob*)(((char*)sb) + layout.cmsOffset);
    cms->magic   = htonl(CSMAGIC_BLOBWRAPPER);
    cms->length  = htonl(sizeof(CS_Blob));

    // alter header of cache to record size and location of code signature
    // do this *before* hashing each page
    Chunk&             cacheHeaderChunk   = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader    = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;
    dyldCacheHeader->codeSignatureOffset  = cacheCodeSignatureChunk.subCacheFileOffset.rawValue();
    dyldCacheHeader->codeSignatureSize    = cacheCodeSignatureChunk.subCacheFileSize.rawValue();

    auto codeSignPage = ^(size_t pageIndex) {
        const uint8_t* code = this->buffer + (pageIndex * pageSize);

        CCDigest(layout.dscDigestFormat, code, pageSize, hashSlot + (pageIndex * layout.dscHashSize));

        if ( layout.agile ) {
            CCDigest(kCCDigestSHA256, code, pageSize, hash256Slot + (pageIndex * CS_HASH_SIZE_SHA256));
        }
    };

    // compute hashes
    dispatch_apply(layout.slotCount, DISPATCH_APPLY_AUTO, ^(size_t i) {
        codeSignPage(i);
    });

    // Now that we have a code signature, compute a cache UUID by hashing the code signature blob
    {
        uint8_t* uuidLoc = dyldCacheHeader->uuid;
        assert(uuid_is_null(uuidLoc));
        static_assert(offsetof(dyld_cache_header, uuid) / CS_PAGE_SIZE_4K == 0, "uuid is expected in the first page of the cache");
        uint8_t fullDigest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256((const void*)cd, (unsigned)layout.cdSize, fullDigest);
        memcpy(uuidLoc, fullDigest, 16);
        // <rdar://problem/6723729> uuids should conform to RFC 4122 UUID version 4 & UUID version 5 formats
        uuidLoc[6] = (uuidLoc[6] & 0x0F) | (3 << 4);
        uuidLoc[8] = (uuidLoc[8] & 0x3F) | 0x80;

        // Now codesign page 0 again, because we modified it by setting uuid in header
        codeSignPage(0);
    }

    // hash of entire code directory (cdHash) uses same hash as each page
    uint8_t fullCdHash[layout.dscHashSize];
    CCDigest(layout.dscDigestFormat, (const uint8_t*)cd, layout.cdSize, fullCdHash);
    // Note: cdHash is defined as first 20 bytes of hash
    memcpy(this->cdHash, fullCdHash, 20);

    // Set the UUID string in the subcache
    uuid_unparse_upper(dyldCacheHeader->uuid, this->uuidString);
}

void SubCache::addStubsChunk(Chunk* chunk)
{
    assert(chunk->isStubsChunk());
    this->regions[(uint32_t)Region::Kind::text].chunks.push_back(chunk);
}

void SubCache::addTextChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::text].chunks.push_back(chunk);
}

void SubCache::addDataChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::data].chunks.push_back(chunk);
}

void SubCache::addDataConstChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::dataConst].chunks.push_back(chunk);
}

void SubCache::addAuthChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::auth].chunks.push_back(chunk);
}

void SubCache::addAuthConstChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::authConst].chunks.push_back(chunk);
}

void SubCache::addLinkeditChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::linkedit].chunks.push_back(chunk);
}

void SubCache::addUnmappedChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::unmapped].chunks.push_back(chunk);
}

void SubCache::addCodeSignatureChunk(Chunk* chunk)
{
    this->regions[(uint32_t)Region::Kind::codeSignature].chunks.push_back(chunk);
}

// HACK: We need to insert the libobjc __TEXT first so that its before all the other OBJC_RO chunks
void SubCache::addObjCTextChunk(Chunk* chunk)
{
    std::vector<Chunk*>& chunks = this->regions[(uint32_t)Region::Kind::text].chunks;
    chunks.insert(chunks.begin(), chunk);
}

// ObjC optimizations need to add read-only chunks.  For now these are added to the start
// of TEXT, so that they are in the same subCache when split by One Cache.  In future we want to
// move these to LINKEDIT
void SubCache::addObjCReadOnlyChunk(Chunk* chunk)
{
    std::vector<Chunk*>& chunks = this->regions[(uint32_t)Region::Kind::text].chunks;
    chunks.insert(chunks.begin(), chunk);
}

// All objc optimizations need to be contiguous, but that means if any need AUTH then all do
void SubCache::addObjCReadWriteChunk(const BuilderConfig& config, Chunk* chunk)
{
    // Add canonical objc protocols
    if ( config.layout.hasAuthRegion ) {
        addAuthChunk(chunk);
    }
    else {
        addDataChunk(chunk);
    }
}

void SubCache::addDylib(CacheDylib& cacheDylib, bool addLinkedit)
{
    for ( DylibSegmentChunk& segmentInfo : cacheDylib.segments ) {
        switch ( segmentInfo.kind ) {
            case Chunk::Kind::dylibText:
                if ( cacheDylib.installName == "/usr/lib/libobjc.A.dylib" )
                    this->addObjCTextChunk(&segmentInfo);
                else
                    this->addTextChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibData:
            case Chunk::Kind::dylibDataConstWorkaround:
                this->addDataChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibDataConst:
                this->addDataConstChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibDataDirty:
                // On arm64e, dataDirty goes in to auth
                if ( cacheDylib.inputMF->isArch("arm64e") )
                    this->addAuthChunk(&segmentInfo);
                else
                    this->addDataChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibAuth:
            case Chunk::Kind::dylibAuthConstWorkaround:
                this->addAuthChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibAuthConst:
                this->addAuthConstChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibReadOnly:
                // FIXME: Read-only data should really be in a read-only mapping.
                this->addTextChunk(&segmentInfo);
                break;
            case Chunk::Kind::dylibLinkedit:
                // Skip adding here.  We'll do this in addLinkeditFromDylib()
                break;
            default:
                assert(0);
                break;
        }
    }

    if ( addLinkedit )
        this->addLinkeditFromDylib(cacheDylib);
}

// Linkedit is stored in Chunks in its own array on the dylib.  This adds it to the subCache.
void SubCache::addLinkeditFromDylib(CacheDylib& cacheDylib)
{
    for ( DylibSegmentChunk& segmentInfo : cacheDylib.segments ) {
        if ( segmentInfo.kind == Chunk::Kind::dylibLinkedit )
            this->addLinkeditChunk(&segmentInfo);
    }
    for ( LinkeditDataChunk& chunk : cacheDylib.linkeditChunks )
        this->addLinkeditChunk(&chunk);
}

void SubCache::addCacheHeaderChunk(const std::span<CacheDylib> cacheDylibs)
{
    // calculate size of header info and where first dylib's mach_header should start
    uint64_t numMappings = this->regions.size();
    uint64_t startOffset = sizeof(dyld_cache_header) + (numMappings * sizeof(dyld_cache_mapping_info));
    startOffset += numMappings * sizeof(dyld_cache_mapping_and_slide_info);

    // Only the main cache has a list of subCaches to write
    if ( this->isMainCache() ) {
        startOffset += sizeof(dyld_subcache_entry) * this->subCaches.size();
    }

    if ( this->needsCacheHeaderImageList() ) {
        startOffset += sizeof(dyld_cache_image_info) * cacheDylibs.size();
        startOffset += sizeof(dyld_cache_image_text_info) * cacheDylibs.size();
        for ( const CacheDylib& cacheDylib : cacheDylibs ) {
            startOffset += cacheDylib.installName.size() + 1;
        }
    }

    //fprintf(stderr, "%s total header size = 0x%08lX\n", _options.archName.c_str(), startOffset);
    startOffset = alignPage(startOffset);

    this->cacheHeader                         = std::make_unique<CacheHeaderChunk>();
    this->cacheHeader->cacheVMSize            = CacheVMSize(startOffset);
    this->cacheHeader->subCacheFileSize       = CacheFileSize(startOffset);

    // Add this to the correct region
    this->addTextChunk(this->cacheHeader.get());
}

void SubCache::addObjCHeaderInfoReadWriteChunk(const BuilderConfig& config, ObjCOptimizer& objcOptimizer)
{
    this->objcHeaderInfoRW = std::make_unique<ObjCHeaderInfoReadWriteChunk>();
    this->objcHeaderInfoRW->cacheVMSize = CacheVMSize(objcOptimizer.headerInfoReadWriteByteSize);
    this->objcHeaderInfoRW->subCacheFileSize = CacheFileSize(objcOptimizer.headerInfoReadWriteByteSize);

    objcOptimizer.headerInfoReadWriteChunk = this->objcHeaderInfoRW.get();

    addObjCReadWriteChunk(config, this->objcHeaderInfoRW.get());
}

void SubCache::addSlideInfoChunks()
{
    // We can't compute the size yet.  Due to alignment, the size of the RW Region can grow after
    // we sort the Chunk's.  For now just add placeholder(s) and we'll update the
    // size in calculateSlideInfoSize()

    // DATA
    if ( !this->regions[(uint32_t)Region::Kind::data].chunks.empty() ) {
        this->dataSlideInfo = std::make_unique<SlideInfoChunk>();
        addLinkeditChunk(this->dataSlideInfo.get());
    }

    // DATA_CONST
    if ( !this->regions[(uint32_t)Region::Kind::dataConst].chunks.empty() ) {
        this->dataConstSlideInfo = std::make_unique<SlideInfoChunk>();
        addLinkeditChunk(this->dataConstSlideInfo.get());
    }

    // AUTH
    if ( !this->regions[(uint32_t)Region::Kind::auth].chunks.empty() ) {
        this->authSlideInfo = std::make_unique<SlideInfoChunk>();
        addLinkeditChunk(this->authSlideInfo.get());
    }

    // AUTH_CONST
    if ( !this->regions[(uint32_t)Region::Kind::authConst].chunks.empty() ) {
        this->authConstSlideInfo = std::make_unique<SlideInfoChunk>();
        addLinkeditChunk(this->authConstSlideInfo.get());
    }
}

void SubCache::addCodeSignatureChunk()
{
    // We can't compute the size yet.  Due to alignment, the size of the Region's can grow after
    // we sort the Section's.  For now just add a placeholder and we'll update the size in calculateCodeSignatureSize()
    this->codeSignature = std::make_unique<CodeSignatureChunk>();

    addCodeSignatureChunk(this->codeSignature.get());
}

void SubCache::addObjCOptsHeaderChunk(ObjCOptimizer& objcOptimizer)
{
    this->objcOptsHeader = std::make_unique<ObjCOptsHeaderChunk>();
    this->objcOptsHeader->cacheVMSize       = CacheVMSize(objcOptimizer.optsHeaderByteSize);
    this->objcOptsHeader->subCacheFileSize  = CacheFileSize(objcOptimizer.optsHeaderByteSize);

    objcOptimizer.optsHeaderChunk = this->objcOptsHeader.get();

    this->addLinkeditChunk(this->objcOptsHeader.get());
}

void SubCache::addObjCHeaderInfoReadOnlyChunk(ObjCOptimizer& objcOptimizer)
{
    this->objcHeaderInfoRO = std::make_unique<ObjCHeaderInfoReadOnlyChunk>();
    this->objcHeaderInfoRO->cacheVMSize         = CacheVMSize(objcOptimizer.headerInfoReadOnlyByteSize);
    this->objcHeaderInfoRO->subCacheFileSize    = CacheFileSize(objcOptimizer.headerInfoReadOnlyByteSize);

    objcOptimizer.headerInfoReadOnlyChunk = this->objcHeaderInfoRO.get();

    this->addObjCReadOnlyChunk(this->objcHeaderInfoRO.get());
}

void SubCache::addObjCSelectorStringsChunk(ObjCSelectorOptimizer& objCSelectorOptimizer)
{
    this->objcSelectorStrings = std::make_unique<ObjCStringsChunk>();
    this->objcSelectorStrings->cacheVMSize      = CacheVMSize(objCSelectorOptimizer.selectorStringsTotalByteSize);
    this->objcSelectorStrings->subCacheFileSize = CacheFileSize(objCSelectorOptimizer.selectorStringsTotalByteSize);

    objCSelectorOptimizer.selectorStringsChunk = this->objcSelectorStrings.get();

    this->addObjCReadOnlyChunk(this->objcSelectorStrings.get());
}

void SubCache::addObjCSelectorHashTableChunk(ObjCSelectorOptimizer& objCSelectorOptimizer)
{
    this->objcSelectorsHashTable = std::make_unique<ObjCSelectorHashTableChunk>();
    this->objcSelectorsHashTable->cacheVMSize       = CacheVMSize(objCSelectorOptimizer.selectorHashTableTotalByteSize);
    this->objcSelectorsHashTable->subCacheFileSize  = CacheFileSize(objCSelectorOptimizer.selectorHashTableTotalByteSize);

    objCSelectorOptimizer.selectorHashTableChunk = this->objcSelectorsHashTable.get();

    this->addObjCReadOnlyChunk(this->objcSelectorsHashTable.get());
}

void SubCache::addObjCClassNameStringsChunk(ObjCClassOptimizer& objcClassOptimizer)
{
    this->objcClassNameStrings = std::make_unique<ObjCStringsChunk>();
    this->objcClassNameStrings->cacheVMSize         = CacheVMSize(objcClassOptimizer.nameStringsTotalByteSize);
    this->objcClassNameStrings->subCacheFileSize    = CacheFileSize(objcClassOptimizer.nameStringsTotalByteSize);

    objcClassOptimizer.classNameStringsChunk = this->objcClassNameStrings.get();

    this->addObjCReadOnlyChunk(this->objcClassNameStrings.get());
}

void SubCache::addObjCClassHashTableChunk(ObjCClassOptimizer& objcClassOptimizer)
{
    this->objcClassesHashTable = std::make_unique<ObjCClassHashTableChunk>();
    this->objcClassesHashTable->cacheVMSize         = CacheVMSize(objcClassOptimizer.classHashTableTotalByteSize);
    this->objcClassesHashTable->subCacheFileSize    = CacheFileSize(objcClassOptimizer.classHashTableTotalByteSize);

    objcClassOptimizer.classHashTableChunk = this->objcClassesHashTable.get();

    this->addObjCReadOnlyChunk(this->objcClassesHashTable.get());
}

void SubCache::addObjCProtocolNameStringsChunk(ObjCProtocolOptimizer& objcProtocolOptimizer)
{
    this->objcProtocolNameStrings = std::make_unique<ObjCStringsChunk>();
    this->objcProtocolNameStrings->cacheVMSize      = CacheVMSize(objcProtocolOptimizer.nameStringsTotalByteSize);
    this->objcProtocolNameStrings->subCacheFileSize = CacheFileSize(objcProtocolOptimizer.nameStringsTotalByteSize);

    objcProtocolOptimizer.protocolNameStringsChunk = this->objcProtocolNameStrings.get();

    this->addObjCReadOnlyChunk(this->objcProtocolNameStrings.get());
}

void SubCache::addObjCProtocolHashTableChunk(ObjCProtocolOptimizer& objcProtocolOptimizer)
{
    this->objcProtocolsHashTable = std::make_unique<ObjCProtocolHashTableChunk>();
    this->objcProtocolsHashTable->cacheVMSize       = CacheVMSize(objcProtocolOptimizer.protocolHashTableTotalByteSize);
    this->objcProtocolsHashTable->subCacheFileSize  = CacheFileSize(objcProtocolOptimizer.protocolHashTableTotalByteSize);

    objcProtocolOptimizer.protocolHashTableChunk = this->objcProtocolsHashTable.get();

    this->addObjCReadOnlyChunk(this->objcProtocolsHashTable.get());
}

void SubCache::addObjCProtocolSwiftDemangledNamesChunk(ObjCProtocolOptimizer& objcProtocolOptimizer)
{
    this->objcSwiftDemangledNameStrings = std::make_unique<ObjCStringsChunk>();
    this->objcSwiftDemangledNameStrings->cacheVMSize        = CacheVMSize(objcProtocolOptimizer.swiftDemangledNameStringsTotalByteSize);
    this->objcSwiftDemangledNameStrings->subCacheFileSize   = CacheFileSize(objcProtocolOptimizer.swiftDemangledNameStringsTotalByteSize);

    objcProtocolOptimizer.swiftDemangledNameStringsChunk = this->objcSwiftDemangledNameStrings.get();

    this->addObjCReadOnlyChunk(this->objcSwiftDemangledNameStrings.get());
}

void SubCache::addObjCIMPCachesChunk(ObjCIMPCachesOptimizer& objcIMPCachesOptimizer)
{
    this->objcIMPCaches = std::make_unique<ObjCIMPCachesChunk>();
    this->objcIMPCaches->cacheVMSize                        = CacheVMSize(objcIMPCachesOptimizer.impCachesTotalByteSize);
    this->objcIMPCaches->subCacheFileSize                   = CacheFileSize(objcIMPCachesOptimizer.impCachesTotalByteSize);

    objcIMPCachesOptimizer.impCachesChunk = this->objcIMPCaches.get();

    this->addLinkeditChunk(this->objcIMPCaches.get());
}

void SubCache::addObjCCanonicalProtocolsChunk(const BuilderConfig& config,
                                              ObjCProtocolOptimizer& objcProtocolOptimizer)
{
    this->objcCanonicalProtocols = std::make_unique<ObjCCanonicalProtocolsChunk>();
    this->objcCanonicalProtocols->cacheVMSize         = CacheVMSize(objcProtocolOptimizer.canonicalProtocolsTotalByteSize);
    this->objcCanonicalProtocols->subCacheFileSize    = CacheFileSize(objcProtocolOptimizer.canonicalProtocolsTotalByteSize);

    objcProtocolOptimizer.canonicalProtocolsChunk = this->objcCanonicalProtocols.get();

    // Add canonical objc protocols
    addObjCReadWriteChunk(config, this->objcCanonicalProtocols.get());
}

void SubCache::addCacheTrieChunk(DylibTrieOptimizer& dylibTrieOptimizer)
{
    this->cacheDylibsTrie = std::make_unique<CacheTrieChunk>(Chunk::Kind::cacheDylibsTrie);
    this->cacheDylibsTrie->cacheVMSize      = CacheVMSize((uint64_t)dylibTrieOptimizer.dylibsTrie.size());
    this->cacheDylibsTrie->subCacheFileSize = CacheFileSize((uint64_t)dylibTrieOptimizer.dylibsTrie.size());

    dylibTrieOptimizer.dylibsTrieChunk = this->cacheDylibsTrie.get();

    this->addLinkeditChunk(this->cacheDylibsTrie.get());
}

void SubCache::addPatchTableChunk(PatchTableOptimizer& patchTableOptimizer)
{
    // We can't compute the size yet.  We need to know how many fixups we have
    // And yet we have an estimate, so we'll use it

    this->patchTable = std::make_unique<PatchTableChunk>();
    this->patchTable->cacheVMSize       = CacheVMSize(patchTableOptimizer.patchTableTotalByteSize);
    this->patchTable->subCacheFileSize  = CacheFileSize(patchTableOptimizer.patchTableTotalByteSize);

    patchTableOptimizer.patchTableChunk = this->patchTable.get();

    this->addLinkeditChunk(this->patchTable.get());
}

void SubCache::addCacheDylibsLoaderChunk(PrebuiltLoaderBuilder& builder)
{
    // We can't compute the size yet.
    // And yet we have an estimate, so we'll use it

    this->cacheDylibsLoaders = std::make_unique<PrebuiltLoaderChunk>(Chunk::Kind::dylibPrebuiltLoaders);
    this->cacheDylibsLoaders->cacheVMSize       = CacheVMSize(builder.cacheDylibsLoaderSize);
    this->cacheDylibsLoaders->subCacheFileSize  = CacheFileSize(builder.cacheDylibsLoaderSize);

    builder.cacheDylibsLoaderChunk = this->cacheDylibsLoaders.get();

    this->addLinkeditChunk(this->cacheDylibsLoaders.get());
}

void SubCache::addExecutableLoaderChunk(PrebuiltLoaderBuilder& builder)
{
    // We can't compute the size yet.
    // And yet we have an estimate, so we'll use it

    this->executableLoaders = std::make_unique<PrebuiltLoaderChunk>(Chunk::Kind::executablePrebuiltLoaders);
    this->executableLoaders->cacheVMSize        = CacheVMSize(builder.executablesLoaderSize);
    this->executableLoaders->subCacheFileSize   = CacheFileSize(builder.executablesLoaderSize);

    builder.executablesLoaderChunk = this->executableLoaders.get();

    this->addLinkeditChunk(this->executableLoaders.get());
}

void SubCache::addExecutablesTrieChunk(PrebuiltLoaderBuilder& builder)
{
    this->executablesTrie = std::make_unique<CacheTrieChunk>(Chunk::Kind::cacheExecutablesTrie);
    this->executablesTrie->cacheVMSize      = CacheVMSize(builder.executablesTrieSize);
    this->executablesTrie->subCacheFileSize = CacheFileSize(builder.executablesTrieSize);

    builder.executableTrieChunk = this->executablesTrie.get();

    this->addLinkeditChunk(this->executablesTrie.get());
}

void SubCache::addSwiftOptsHeaderChunk(SwiftProtocolConformanceOptimizer& opt)
{
    this->swiftOptsHeader = std::make_unique<SwiftOptsHeaderChunk>();
    this->swiftOptsHeader->cacheVMSize      = CacheVMSize(opt.optsHeaderByteSize);
    this->swiftOptsHeader->subCacheFileSize = CacheFileSize(opt.optsHeaderByteSize);

    opt.optsHeaderChunk = this->swiftOptsHeader.get();

    this->addLinkeditChunk(this->swiftOptsHeader.get());
}

void SubCache::addSwiftTypeHashTableChunk(SwiftProtocolConformanceOptimizer& opt)
{
    this->swiftTypeHashTable = std::make_unique<SwiftProtocolConformancesHashTableChunk>();
    this->swiftTypeHashTable->cacheVMSize       = CacheVMSize(opt.typeConformancesHashTableSize);
    this->swiftTypeHashTable->subCacheFileSize  = CacheFileSize(opt.typeConformancesHashTableSize);

    opt.typeConformancesHashTable = this->swiftTypeHashTable.get();

    this->addLinkeditChunk(this->swiftTypeHashTable.get());
}

void SubCache::addSwiftMetadataHashTableChunk(SwiftProtocolConformanceOptimizer& opt)
{
    this->swiftMetadataHashTable = std::make_unique<SwiftProtocolConformancesHashTableChunk>();
    this->swiftMetadataHashTable->cacheVMSize       = CacheVMSize(opt.metadataConformancesHashTableSize);
    this->swiftMetadataHashTable->subCacheFileSize  = CacheFileSize(opt.metadataConformancesHashTableSize);

    opt.metadataConformancesHashTable = this->swiftMetadataHashTable.get();

    this->addLinkeditChunk(this->swiftMetadataHashTable.get());
}

void SubCache::addSwiftForeignHashTableChunk(SwiftProtocolConformanceOptimizer& opt)
{
    this->swiftForeignTypeHashTable = std::make_unique<SwiftProtocolConformancesHashTableChunk>();
    this->swiftForeignTypeHashTable->cacheVMSize        = CacheVMSize(opt.foreignTypeConformancesHashTableSize);
    this->swiftForeignTypeHashTable->subCacheFileSize   = CacheFileSize(opt.foreignTypeConformancesHashTableSize);

    opt.foreignTypeConformancesHashTable = this->swiftForeignTypeHashTable.get();

    this->addLinkeditChunk(this->swiftForeignTypeHashTable.get());
}

void SubCache::addUnmappedSymbols(const BuilderConfig& config, UnmappedSymbolsOptimizer& opt)
{
    assert(this->kind == Kind::symbols);

    // Add the unmapped symbol data
    uint64_t unmappedSymbolsFileSize = 0;
    unmappedSymbolsFileSize += sizeof(dyld_cache_local_symbols_info);
    unmappedSymbolsFileSize += sizeof(dyld_cache_local_symbols_entry_64) * opt.symbolInfos.size();
    opt.unmappedSymbolsChunk.cacheVMSize = CacheVMSize(0ULL);
    opt.unmappedSymbolsChunk.subCacheFileSize = CacheFileSize(unmappedSymbolsFileSize);

    uint64_t nlistFileSize = 0;
    if ( config.layout.is64 )
        nlistFileSize += sizeof(struct nlist_64) * opt.symbolNlistChunk.nlist64.size();
    else
        nlistFileSize += sizeof(struct nlist) * opt.symbolNlistChunk.nlist32.size();
    opt.symbolNlistChunk.cacheVMSize = CacheVMSize(0ULL);
    opt.symbolNlistChunk.subCacheFileSize = CacheFileSize(nlistFileSize);

    uint64_t symbolStringsSize = opt.stringBufferSize;
    opt.symbolStringsChunk.cacheVMSize = CacheVMSize(0ULL);
    opt.symbolStringsChunk.subCacheFileSize = CacheFileSize(symbolStringsSize);

    this->addUnmappedChunk(&opt.unmappedSymbolsChunk);
    this->addUnmappedChunk(&opt.symbolNlistChunk);
    this->addUnmappedChunk(&opt.symbolStringsChunk);
}

void SubCache::addDynamicConfigChunk()
{
    dynamicConfig                       = std::make_unique<DynamicConfigChunk>();
    dynamicConfig->cacheVMSize          = CacheVMSize(16_KB);
    dynamicConfig->subCacheFileSize     = CacheFileSize(0ULL);
    this->regions[(uint32_t)Region::Kind::dynamicConfig].chunks.push_back(dynamicConfig.get());
}

// When building SubCache's, we start off with all the Regions.  This removes any which didn't
// get any content
void SubCache::removeEmptyRegions()
{
    auto unusedRegion = [](const Region& region) {
        return region.chunks.empty();
    };
    this->regions.erase(std::remove_if(this->regions.begin(), this->regions.end(),
                                       unusedRegion), this->regions.end());
}

uint64_t SubCache::getCacheType(const BuilderOptions& options)
{
    switch ( options.kind ) {
        case CacheKind::development:
            return kDyldSharedCacheTypeDevelopment;
        case CacheKind::universal:
            return kDyldSharedCacheTypeUniversal;
    }
}

uint32_t SubCache::getCacheSubType() const
{
    switch ( this->kind ) {
        case Kind::mainDevelopment:
        case Kind::stubsDevelopment:
            return kDyldSharedCacheTypeDevelopment;
        case Kind::mainCustomer:
        case Kind::stubsCustomer:
            return kDyldSharedCacheTypeProduction;
        case Kind::subUniversal:
        case Kind::symbols:
            return kDyldSharedCacheTypeProduction;
    }
}

void SubCache::writeCacheHeaderMappings()
{
    Chunk& cacheHeaderChunk = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;

    assert(cacheHeaderChunk.subCacheFileOffset.rawValue() == 0);
    auto* mappings         = (dyld_cache_mapping_info*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->mappingOffset);
    auto* slidableMappings = (dyld_cache_mapping_and_slide_info*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->mappingWithSlideOffset);

    for ( const Region& region : this->regions ) {

        // Skip Region's like the code signature which doesn't get a mapping
        if ( !region.needsSharedCacheMapping() )
            continue;

        const uint32_t        initProt = region.initProt();
        const uint32_t        maxProt  = region.maxProt();
        uint32_t        flags    = 0;
        CacheFileOffset slideInfoFileOffset(0ULL);
        CacheFileSize   slideInfoFileSize(0ULL);

        switch ( region.kind ) {
            case Region::Kind::text:
                flags    = this->isStubsCache() ? DYLD_CACHE_MAPPING_TEXT_STUBS : 0;
                break;
            case Region::Kind::data:
                flags    = 0;

                // Get the slide info
                if ( this->dataSlideInfo ) {
                    slideInfoFileOffset = this->dataSlideInfo->subCacheFileOffset;
                    slideInfoFileSize   = this->dataSlideInfo->usedFileSize;
                }
                break;
            case Region::Kind::dataConst:
                flags    = DYLD_CACHE_MAPPING_CONST_DATA;

                // Get the slide info
                if ( this->dataConstSlideInfo ) {
                    slideInfoFileOffset = this->dataConstSlideInfo->subCacheFileOffset;
                    slideInfoFileSize   = this->dataConstSlideInfo->usedFileSize;
                }
                break;
            case Region::Kind::auth:
                flags    = DYLD_CACHE_MAPPING_AUTH_DATA;

                // Get the slide info
                if ( this->authSlideInfo ) {
                    slideInfoFileOffset = this->authSlideInfo->subCacheFileOffset;
                    slideInfoFileSize   = this->authSlideInfo->usedFileSize;
                }
                break;
            case Region::Kind::authConst:
                flags    = DYLD_CACHE_MAPPING_AUTH_DATA | DYLD_CACHE_MAPPING_CONST_DATA;

                // Get the slide info
                if ( this->authConstSlideInfo ) {
                    slideInfoFileOffset = this->authConstSlideInfo->subCacheFileOffset;
                    slideInfoFileSize   = this->authConstSlideInfo->usedFileSize;
                }
                break;
            case Region::Kind::linkedit:
                flags    = 0;
                break;
            case Region::Kind::dynamicConfig:
                flags    = DYLD_CACHE_DYNAMIC_CONFIG_DATA;
                break;
            case Region::Kind::unmapped:
            case Region::Kind::codeSignature:
                // This isn't mapped, so we should never ask for its maxprot
                assert(0);
                break;
            case Region::Kind::numKinds:
                assert(0);
                break;
        }

        mappings->address    = region.subCacheVMAddress.rawValue();
        mappings->fileOffset = region.subCacheFileOffset.rawValue();
        mappings->size       = region.subCacheFileSize.rawValue();
        mappings->maxProt    = maxProt;
        mappings->initProt   = initProt;

        slidableMappings->address             = region.subCacheVMAddress.rawValue();
        slidableMappings->fileOffset          = region.subCacheFileOffset.rawValue();
        slidableMappings->size                = region.subCacheFileSize.rawValue();
        slidableMappings->maxProt             = maxProt;
        slidableMappings->initProt            = initProt;
        slidableMappings->slideInfoFileOffset = slideInfoFileOffset.rawValue();
        slidableMappings->slideInfoFileSize   = slideInfoFileSize.rawValue();
        slidableMappings->flags               = flags;

        ++mappings;
        ++slidableMappings;
    }
}

void SubCache::writeCacheHeader(const BuilderOptions& options, const BuilderConfig& config,
                                const std::span<CacheDylib> cacheDylibs)
{
    Chunk& cacheHeaderChunk = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;

    // "dyld_v1" + spaces + archName(), with enough spaces to pad to 15 bytes
    std::string magic = "dyld_v1";
    magic.append(15 - magic.length() - strlen(options.archs.name()), ' ');
    magic.append(options.archs.name());
    assert(magic.length() == 15);

    // Num of mappings depends on cache layout.
    // For a regular/large cache its probably 1 each of TEXT, DATA, DATA_CONST, and LINKEDIT
    // For a split cache, most files are a single mapping.
    uint32_t mappingCount = 0;
    for ( const Region& region : this->regions ) {
        if ( region.needsSharedCacheMapping() )
            ++mappingCount;
    }
    assert(mappingCount <= DyldSharedCache::MaxMappings);

    // fill in header
    memcpy(dyldCacheHeader->magic, magic.c_str(), 16);
    dyldCacheHeader->mappingOffset          = sizeof(dyld_cache_header);
    dyldCacheHeader->mappingCount           = mappingCount;
    dyldCacheHeader->mappingWithSlideOffset = (uint32_t)(dyldCacheHeader->mappingOffset + mappingCount * sizeof(dyld_cache_mapping_and_slide_info));
    dyldCacheHeader->mappingWithSlideCount  = mappingCount;
    dyldCacheHeader->imagesOffsetOld        = 0; // no longer used
    dyldCacheHeader->imagesCountOld         = 0; // no longer used
    dyldCacheHeader->imagesOffset           = 0; // set later on all cache files
    dyldCacheHeader->imagesCount            = 0; // set later on all cache files
    dyldCacheHeader->dyldBaseAddress        = 0; // unused
    dyldCacheHeader->codeSignatureOffset    = 0; // set later on all cache files in codeSign()
    dyldCacheHeader->codeSignatureSize      = 0; // set later on all cache files in codeSign()
    dyldCacheHeader->slideInfoOffsetUnused  = 0; // no longer used
    dyldCacheHeader->slideInfoSizeUnused    = 0; // no longer used
    dyldCacheHeader->localSymbolsOffset     = 0;
    dyldCacheHeader->localSymbolsSize       = 0;
    dyldCacheHeader->cacheType              = getCacheType(options);
    dyldCacheHeader->dyldInCacheMH          = 0; // set later only on the main cache file
    dyldCacheHeader->dyldInCacheEntry       = 0; // set later only on the main cache file
    bzero(dyldCacheHeader->uuid, 16);                   // overwritten later by recomputeCacheUUID()
    dyldCacheHeader->branchPoolsOffset             = 0; // no longer used
    dyldCacheHeader->branchPoolsCount              = 0; // no longer used
    dyldCacheHeader->imagesTextOffset              = 0;
    dyldCacheHeader->imagesTextCount               = 0;
    dyldCacheHeader->patchInfoAddr                 = 0;
    dyldCacheHeader->patchInfoSize                 = 0;
    dyldCacheHeader->otherImageGroupAddrUnused     = 0; // no longer used
    dyldCacheHeader->otherImageGroupSizeUnused     = 0; // no longer used
    dyldCacheHeader->progClosuresAddr              = 0; // no longer used
    dyldCacheHeader->progClosuresSize              = 0; // no longer used
    dyldCacheHeader->progClosuresTrieAddr          = 0; // no longer used
    dyldCacheHeader->progClosuresTrieSize          = 0; // no longer used
    dyldCacheHeader->platform                      = (uint8_t)options.platform;
    dyldCacheHeader->formatVersion                 = 0; //dyld3::closure::kFormatVersion;
    dyldCacheHeader->dylibsExpectedOnDisk          = !options.dylibsRemovedFromDisk;
    dyldCacheHeader->simulator                     = options.isSimultor();
    dyldCacheHeader->locallyBuiltCache             = options.isLocallyBuiltCache;
    dyldCacheHeader->builtFromChainedFixups        = false; // no longer used
    dyldCacheHeader->sharedRegionStart             = this->subCacheVMAddress.rawValue();
    dyldCacheHeader->sharedRegionSize              = 0;
    dyldCacheHeader->maxSlide                      = 0; // overwritten later in build if the cache supports ASLR
    dyldCacheHeader->dylibsImageArrayAddr          = 0; // no longer used
    dyldCacheHeader->dylibsImageArraySize          = 0; // no longer used
    dyldCacheHeader->dylibsTrieAddr                = 0; // set later only on the main cache file
    dyldCacheHeader->dylibsTrieSize                = 0; // set later only on the main cache file
    dyldCacheHeader->otherImageArrayAddr           = 0; // no longer used
    dyldCacheHeader->otherImageArraySize           = 0; // no longer used
    dyldCacheHeader->otherTrieAddr                 = 0; // no longer used
    dyldCacheHeader->otherTrieSize                 = 0; // no longer used
    dyldCacheHeader->dylibsPBLStateArrayAddrUnused = 0; // no longer used
    dyldCacheHeader->dylibsPBLSetAddr              = 0; // set later only on the main cache file
    dyldCacheHeader->programsPBLSetPoolAddr        = 0; // set later only on the main cache file
    dyldCacheHeader->programsPBLSetPoolSize        = 0; // set later only on the main cache file
    dyldCacheHeader->programTrieAddr               = 0; // set later only on the main cache file
    dyldCacheHeader->programTrieSize               = 0; // set later only on the main cache file
    dyldCacheHeader->osVersion                     = 0; // set later only on the main cache file
    dyldCacheHeader->altPlatform                   = 0; // set later only on the main cache file
    dyldCacheHeader->altOsVersion                  = 0; // set later only on the main cache file
    dyldCacheHeader->swiftOptsOffset               = 0; // set later only on the main cache file
    dyldCacheHeader->swiftOptsSize                 = 0; // set later only on the main cache file
    dyldCacheHeader->subCacheArrayOffset           = 0;
    dyldCacheHeader->subCacheArrayCount            = 0;
    bzero(dyldCacheHeader->symbolFileUUID, 16); // overwritten later after measuring the local symbols file
    dyldCacheHeader->rosettaReadOnlyAddr           = this->rosettaReadOnlyAddr;
    dyldCacheHeader->rosettaReadOnlySize           = this->rosettaReadOnlySize;
    dyldCacheHeader->rosettaReadWriteAddr          = this->rosettaReadWriteAddr;
    dyldCacheHeader->rosettaReadWriteSize          = this->rosettaReadWriteSize;
    dyldCacheHeader->cacheSubType                  = getCacheSubType();
    dyldCacheHeader->objcOptsOffset                = 0; // set later only on the main cache file
    dyldCacheHeader->objcOptsSize                  = 0; // set later only on the main cache file
    dyldCacheHeader->cacheAtlasOffset              = 0; // set later only on the main cache file
    dyldCacheHeader->cacheAtlasSize                = 0; // set later only on the main cache file
    dyldCacheHeader->dynamicDataOffset             = 0; // set later only on the main cache file
    dyldCacheHeader->dynamicDataMaxSize            = 0; // set later only on the main cache file

    // Fill in old mappings
    // And new mappings which also have slide info
    this->writeCacheHeaderMappings();

    this->addCacheHeaderImageInfo(options, cacheDylibs);
}

void SubCache::addMainCacheHeaderInfo(const BuilderOptions& options, const BuilderConfig& config,
                                      const std::span<CacheDylib> cacheDylibs,
                                      CacheVMSize totalVMSize, uint64_t maxSlide,
                                      uint32_t osVersion, uint32_t altPlatform, uint32_t altOsVersion,
                                      CacheVMAddress dyldInCacheUnslidAddr,
                                      CacheVMAddress dyldInCacheEntryUnslidAddr,
                                      const DylibTrieOptimizer& dylibTrieOptimizer,
                                      const ObjCOptimizer& objcOptimizer,
                                      const SwiftProtocolConformanceOptimizer& swiftProtocolConformanceOpt,
                                      const PatchTableOptimizer& patchTableOptimizer,
                                      const PrebuiltLoaderBuilder& prebuiltLoaderBuilder)
{
    const CacheVMAddress cacheBaseAddress = config.layout.cacheBaseAddress;

    Chunk&             cacheHeaderChunk   = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader    = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;

    // The first subCache knows the size of buffer to allocate to contain all other subCaches
    dyldCacheHeader->sharedRegionSize = totalVMSize.rawValue();

    dyldCacheHeader->dylibsTrieAddr = dylibTrieOptimizer.dylibsTrieChunk->cacheVMAddress.rawValue();
    dyldCacheHeader->dylibsTrieSize = dylibTrieOptimizer.dylibsTrieChunk->subCacheFileSize.rawValue();

    if ( !objcOptimizer.objcDylibs.empty() ) {
        dyldCacheHeader->objcOptsOffset = (objcOptimizer.optsHeaderChunk->cacheVMAddress - cacheBaseAddress).rawValue();
        dyldCacheHeader->objcOptsSize   = objcOptimizer.optsHeaderChunk->subCacheFileSize.rawValue();
    }

    if ( !objcOptimizer.objcDylibs.empty() ) {
        const auto& opt = swiftProtocolConformanceOpt;
        dyldCacheHeader->swiftOptsOffset = (opt.optsHeaderChunk->cacheVMAddress - cacheBaseAddress).rawValue();
        dyldCacheHeader->objcOptsSize    = opt.optsHeaderChunk->subCacheFileSize.rawValue();
    }

    dyldCacheHeader->patchInfoAddr = patchTableOptimizer.patchTableChunk->cacheVMAddress.rawValue();
    dyldCacheHeader->patchInfoSize = patchTableOptimizer.patchTableChunk->subCacheFileSize.rawValue();

    dyldCacheHeader->dylibsPBLSetAddr = prebuiltLoaderBuilder.cacheDylibsLoaderChunk->cacheVMAddress.rawValue();
    dyldCacheHeader->programsPBLSetPoolAddr = prebuiltLoaderBuilder.executablesLoaderChunk->cacheVMAddress.rawValue();
    dyldCacheHeader->programsPBLSetPoolSize = prebuiltLoaderBuilder.executablesLoaderChunk->subCacheFileSize.rawValue();
    dyldCacheHeader->programTrieAddr        = prebuiltLoaderBuilder.executableTrieChunk->cacheVMAddress.rawValue();
    dyldCacheHeader->programTrieSize        = (uint32_t)prebuiltLoaderBuilder.executableTrieChunk->subCacheFileSize.rawValue();

    dyldCacheHeader->dyldInCacheMH      = dyldInCacheUnslidAddr.rawValue();
    dyldCacheHeader->dyldInCacheEntry   = dyldInCacheEntryUnslidAddr.rawValue();

    dyldCacheHeader->osVersion      = osVersion;
    dyldCacheHeader->altPlatform    = altPlatform;
    dyldCacheHeader->altOsVersion   = altOsVersion;

    // record max slide now that final size is established
    dyldCacheHeader->maxSlide           = maxSlide;

    // TODO: Build the atlas
    dyldCacheHeader->cacheAtlasOffset              = 0; // set later only on the main cache file
    dyldCacheHeader->cacheAtlasSize                = 0; // set later only on the main cache file

    // The main cache has offsets to all the caches
    if ( !this->subCaches.empty() ) {
        // The first subCache has an array of UUIDs for all other subCaches
        // This should run after addCacheHeaderImageInfo(), which sets up te offsets
        assert(dyldCacheHeader->subCacheArrayOffset != 0);
        assert(dyldCacheHeader->subCacheArrayCount == this->subCaches.size());
        auto* subCacheEntries = (dyld_subcache_entry*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->subCacheArrayOffset);
        for ( uint32_t index = 0; index != this->subCaches.size(); ++index ) {
            const SubCache* subCache = this->subCaches[index];
            subCacheEntries[index].cacheVMOffset = (subCache->subCacheVMAddress - cacheBaseAddress).rawValue();
            strncpy(subCacheEntries[index].fileSuffix, subCache->fileSuffix.data(),
                    sizeof(dyld_subcache_entry::fileSuffix));
        }
        dyldCacheHeader->dynamicDataOffset  = (this->subCaches.back()->dynamicConfig->cacheVMAddress - cacheBaseAddress).rawValue();
        dyldCacheHeader->dynamicDataMaxSize = this->subCaches.back()->dynamicConfig->cacheVMSize.rawValue();
    } else {
        dyldCacheHeader->dynamicDataOffset  = (dynamicConfig->cacheVMAddress - cacheBaseAddress).rawValue();
        dyldCacheHeader->dynamicDataMaxSize = dynamicConfig->cacheVMSize.rawValue();
    }
}

void SubCache::addSymbolsCacheHeaderInfo(const UnmappedSymbolsOptimizer& optimizer)
{
    Chunk&             cacheHeaderChunk   = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader    = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;

    assert(kind == SubCache::Kind::symbols);

    // In the symbols cache, fill in the offset to the symbol info
    // FIXME: The implicit order of these chunks is not ideal
    assert(optimizer.unmappedSymbolsChunk.kind < optimizer.symbolNlistChunk.kind);
    assert(optimizer.symbolNlistChunk.kind < optimizer.symbolStringsChunk.kind);

    CacheFileOffset start = optimizer.unmappedSymbolsChunk.subCacheFileOffset;
    CacheFileOffset end = optimizer.symbolStringsChunk.subCacheFileOffset + optimizer.symbolStringsChunk.subCacheFileSize;
    uint64_t size = end.rawValue() - start.rawValue();

    dyldCacheHeader->localSymbolsOffset = start.rawValue();
    dyldCacheHeader->localSymbolsSize   = size;
}

void SubCache::addCacheHeaderImageInfo(const BuilderOptions& options,
                                       const std::span<CacheDylib> cacheDylibs)
{
    if ( !this->needsCacheHeaderImageList() )
        return;

    Chunk&             cacheHeaderChunk   = *this->cacheHeader.get();
    dyld_cache_header* dyldCacheHeader    = (dyld_cache_header*)cacheHeaderChunk.subCacheBuffer;

    // Work out where everything will be in the header
    dyldCacheHeader->imagesOffset        = (uint32_t)(dyldCacheHeader->mappingWithSlideOffset + dyldCacheHeader->mappingWithSlideCount * sizeof(dyld_cache_mapping_and_slide_info));
    dyldCacheHeader->imagesCount         = (uint32_t)cacheDylibs.size();
    dyldCacheHeader->imagesTextOffset    = dyldCacheHeader->imagesOffset + sizeof(dyld_cache_image_info) * dyldCacheHeader->imagesCount;
    dyldCacheHeader->imagesTextCount     = cacheDylibs.size();

    dyldCacheHeader->subCacheArrayOffset = (uint32_t)(dyldCacheHeader->imagesTextOffset + sizeof(dyld_cache_image_text_info) * cacheDylibs.size());
    dyldCacheHeader->subCacheArrayCount  = (uint32_t)this->subCaches.size();

    // calculate start of text image array and trailing string pool
    auto*    textImages   = (dyld_cache_image_text_info*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->imagesTextOffset);
    uint32_t stringOffset = (uint32_t)(dyldCacheHeader->subCacheArrayOffset + sizeof(dyld_subcache_entry) * dyldCacheHeader->subCacheArrayCount);

    // write text image array and image names pool at same time
    for ( const CacheDylib& cacheDylib : cacheDylibs ) {
        cacheDylib.inputMF->getUuid(textImages->uuid);
        textImages->loadAddress     = cacheDylib.cacheLoadAddress.rawValue();
        textImages->textSegmentSize = (uint32_t)cacheDylib.segments.front().cacheVMSize.rawValue();
        textImages->pathOffset      = stringOffset;
        const char* installName     = cacheDylib.installName.data();
        ::strcpy((char*)dyldCacheHeader + stringOffset, installName);
        stringOffset += (uint32_t)cacheDylib.installName.size() + 1;
        ++textImages;
    }

    // fill in image table.  This has to be after the above loop so that the install names are within 32-bits of the first shared cache
    textImages                    = (dyld_cache_image_text_info*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->imagesTextOffset);
    dyld_cache_image_info* images = (dyld_cache_image_info*)((uint8_t*)dyldCacheHeader + dyldCacheHeader->imagesOffset);
    for ( const CacheDylib& cacheDylib : cacheDylibs ) {
        images->address = cacheDylib.cacheLoadAddress.rawValue();
        if ( options.dylibsRemovedFromDisk ) {
            images->modTime = 0;
            images->inode   = 0;
        }
        else {
            images->modTime = cacheDylib.inputFile->mtime;
            images->inode   = cacheDylib.inputFile->inode;
        }
        images->pathFileOffset = (uint32_t)textImages->pathOffset;
        ++images;
        ++textImages;
    }

    // make sure header did not overflow
    assert(stringOffset <= cacheHeaderChunk.cacheVMSize.rawValue());
}

bool SubCache::isMainCache() const
{
    switch ( this->kind ) {
        case Kind::mainDevelopment:
        case Kind::mainCustomer:
            return true;
        case Kind::stubsDevelopment:
        case Kind::stubsCustomer:
        case Kind::subUniversal:
        case Kind::symbols:
            return false;
    }
}

bool SubCache::isMainDevelopmentCache() const
{
    return this->kind == Kind::mainDevelopment;
}

bool SubCache::isMainCustomerCache() const
{
    return this->kind == Kind::mainCustomer;
}

bool SubCache::isSymbolsCache() const
{
    switch ( this->kind ) {
        case Kind::symbols:
            return true;
        case Kind::mainDevelopment:
        case Kind::mainCustomer:
        case Kind::stubsDevelopment:
        case Kind::stubsCustomer:
        case Kind::subUniversal:
            return false;
    }
}

bool SubCache::isSubCache() const
{
    switch ( this->kind ) {
        case Kind::subUniversal:
            return true;
        case Kind::mainDevelopment:
        case Kind::mainCustomer:
        case Kind::stubsDevelopment:
        case Kind::stubsCustomer:
        case Kind::symbols:
            return false;
    }
}

bool SubCache::isStubsCache() const
{
    switch ( this->kind ) {
        case Kind::stubsDevelopment:
        case Kind::stubsCustomer:
            return true;
        case Kind::mainDevelopment:
        case Kind::mainCustomer:
        case Kind::subUniversal:
        case Kind::symbols:
            return false;
    }
}

bool SubCache::isStubsDevelopmentCache() const
{
    return this->kind == Kind::stubsDevelopment;
}

bool SubCache::isStubsCustomerCache() const
{
    return this->kind == Kind::stubsCustomer;
}

bool SubCache::needsCacheHeaderImageList() const
{
    // Symbols and stubs files don't need an image list
    // We'd like to not add the image list to subcaches, only the main cache, but Rosetta needs
    // the image list on subCaches.
    switch ( this->kind ) {
        case Kind::mainDevelopment:
        case Kind::mainCustomer:
        case Kind::subUniversal:
            return true;
        case Kind::stubsDevelopment:
        case Kind::stubsCustomer:
        case Kind::symbols:
            return false;
    }
}

static void set_toc(dyld_cache_slide_info* info, unsigned index, uint16_t value)
{
    ((uint16_t*)(((uint8_t*)info) + info->toc_offset))[index] = value;
}

Error SubCache::computeSlideInfoV1(const BuilderConfig&           config,
                                   cache_builder::SlideInfoChunk* slideChunk,
                                   Region&                        region)
{
    // build one 512-byte bitmap per page (16384) of DATA

    // fill in fixed info
    assert((region.subCacheVMSize.rawValue() % config.slideInfo.slideInfoPageSize) == 0);

    // Create a bitmap for all pages in this region
    const long bitmapSize = (region.subCacheVMSize.rawValue()) / (4*8);
    uint8_t* bitmap = (uint8_t*)calloc(bitmapSize, 1);
    for ( Chunk* chunk : region.chunks ) {
        SlidChunk* slidChunk = chunk->isSlidChunk();

        VMOffset chunkOffsetInRegion = chunk->cacheVMAddress - region.subCacheVMAddress;
        slidChunk->tracker.forEachFixup(^(void *loc, bool& stop) {
            uint64_t offsetInChunk = (uint64_t)loc - (uint64_t)chunk->subCacheBuffer;
            uint64_t offsetInRegion = chunkOffsetInRegion.rawValue() + offsetInChunk;

            // Convert the location to the slide info format.  In this case its just an unslid vmAddr
            CacheVMAddress vmAddr = Fixup::Cache32::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress,
                                                                             loc);

            *(uint32_t*)loc = (uint32_t)vmAddr.rawValue();

            // Set the byte corresponding to this fixup
            long byteIndex = offsetInRegion / (4*8);
            long bitInByte =  (offsetInRegion % 32) >> 2;
            bitmap[byteIndex] |= (1 << bitInByte);
        });
    }

    // allocate worst case size block of all slide info
    const unsigned entry_size = 4096/(8*4); // 8 bits per byte, possible pointer every 4 bytes.
    const unsigned toc_count = (unsigned)bitmapSize/entry_size;
    dyld_cache_slide_info* slideInfo = (dyld_cache_slide_info*)slideChunk->subCacheBuffer;
    slideInfo->version          = 1;
    slideInfo->toc_offset       = sizeof(dyld_cache_slide_info);
    slideInfo->toc_count        = toc_count;
    slideInfo->entries_offset   = (slideInfo->toc_offset+2*toc_count+127)&(-128);
    slideInfo->entries_count    = 0;
    slideInfo->entries_size     = entry_size;
    // append each unique entry
    const dyld_cache_slide_info_entry* bitmapAsEntries = (dyld_cache_slide_info_entry*)bitmap;
    dyld_cache_slide_info_entry* const entriesInSlidInfo = (dyld_cache_slide_info_entry*)((char*)slideInfo+slideInfo->entries_offset);
    int entry_count = 0;
    for ( unsigned i = 0; i < toc_count; ++i ) {
        const dyld_cache_slide_info_entry* thisEntry = &bitmapAsEntries[i];
        // see if it is same as one already added
        bool found = false;
        for (int j=0; j < entry_count; ++j) {
            if ( memcmp(thisEntry, &entriesInSlidInfo[j], entry_size) == 0 ) {
                set_toc(slideInfo, i, j);
                found = true;
                break;
            }
        }
        if ( !found ) {
            // append to end
            memcpy(&entriesInSlidInfo[entry_count], thisEntry, entry_size);
            set_toc(slideInfo, i, entry_count++);
        }
    }
    slideInfo->entries_count  = entry_count;
    ::free((void*)bitmap);

    // Update slide info size as we may not need all the space we allocated
    CacheFileSize slideInfoSize((uint64_t)slideInfo->entries_offset + (entry_count * entry_size));
    if ( slideInfoSize > slideChunk->subCacheFileSize ) {
        return Error("kernel slide info overflow buffer");
    }

    slideChunk->usedFileSize = slideInfoSize;

    return Error();
}

Error SubCache::computeSlideInfoV2(const BuilderConfig&           config,
                                   cache_builder::SlideInfoChunk* slideChunk,
                                   Region&                        region)
{
    __block Diagnostics diag;

    assert((region.subCacheVMSize.rawValue() % config.slideInfo.slideInfoPageSize) == 0);
    dyld_cache_slide_info2* info = (dyld_cache_slide_info2*)slideChunk->subCacheBuffer;
    info->version                = 2;
    info->page_size              = config.slideInfo.slideInfoPageSize;
    info->page_starts_offset     = sizeof(dyld_cache_slide_info2);
    info->page_starts_count      = (uint32_t)region.subCacheVMSize.rawValue() / config.slideInfo.slideInfoPageSize;
    info->page_extras_offset     = 0;
    info->page_extras_count      = 0;
    info->delta_mask             = config.slideInfo.slideInfoDeltaMask;
    info->value_add              = config.slideInfo.slideInfoValueAdd.rawValue();

    assert((sizeof(dyld_cache_slide_info2) + (info->page_starts_count * sizeof(uint16_t))) <= slideChunk->cacheVMSize.rawValue());

    uint16_t* pageStartsBuffer = (uint16_t*)((char*)info + info->page_starts_offset);
    std::fill(&pageStartsBuffer[0], &pageStartsBuffer[info->page_starts_count], DYLD_CACHE_SLIDE_PAGE_ATTR_NO_REBASE);

    const uint64_t deltaMask  = info->delta_mask;
    const uint64_t valueMask  = ~deltaMask;
    const uint64_t valueAdd   = info->value_add;
    const unsigned deltaShift = __builtin_ctzll(deltaMask) - 2;
    const uint32_t maxDelta   = (uint32_t)(deltaMask >> deltaShift);

    // Walk each fixup in each segment.  Every time we cross a page, add a page start
    __block MachOFile::ChainedFixupPointerOnDisk* lastFixup = nullptr;
    __block uint64_t lastPageIndex = ~0ULL;
    for ( Chunk* chunk : region.chunks ) {
        SlidChunk* slidChunk = chunk->isSlidChunk();

        slidChunk->tracker.forEachFixup(^(void *loc, bool& stop) {
            VMOffset       vmOffsetInSegment((uint64_t)loc - (uint64_t)slidChunk->subCacheBuffer);
            assert((vmOffsetInSegment.rawValue() + 8) <= slidChunk->cacheVMSize.rawValue());
            CacheVMAddress fixupVMAddr = slidChunk->cacheVMAddress + vmOffsetInSegment;
            uint64_t       pageIndex   = (fixupVMAddr - region.subCacheVMAddress).rawValue() / info->page_size;

            // Make sure we never cross a page
            uint64_t highBytesPageIndex = ((fixupVMAddr + VMOffset(4ULL)) - region.subCacheVMAddress).rawValue() / info->page_size;
            if ( pageIndex != highBytesPageIndex ) {
                diag.error("Fixup crosses page boundary");
                stop = true;
                return;
            }

            // If we are on a new page, then start a new chain
            if ( pageIndex != lastPageIndex ) {
                uint64_t vmOffsetInPage     = fixupVMAddr.rawValue() % info->page_size;

                // Note first location in the page is a word offset, not a byte offset
                pageStartsBuffer[pageIndex] = vmOffsetInPage / 4;
            }
            else {
                // Patch the previous fixup on this page to point to this one
                uint64_t delta = (uint64_t)loc - (uint64_t)lastFixup;
                assert(delta <= maxDelta);
                lastFixup->raw64 |= (delta << deltaShift);
            }

            MachOFile::ChainedFixupPointerOnDisk* fixup = (MachOFile::ChainedFixupPointerOnDisk*)loc;

            // Convert this fixup from the chained format in the cache builder, to the version we want in the cache file
            CacheVMAddress vmAddr = Fixup::Cache64::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress,
                                                                                  loc);

            if ( (vmAddr.rawValue() - valueAdd) & deltaMask ) {
                std::string dylibName = "unknown dylib";
                std::string segName   = "unknown segment";
                //findDylibAndSegment((void*)pageContent, dylibName, segName);
                diag.error("rebase pointer (0x%0llX) does not point within cache. vmOffsetInSegment=0x%04llX, seg=%s, dylib=%s\n",
                           vmAddr.rawValue(), vmOffsetInSegment.rawValue(), segName.c_str(), dylibName.c_str());
                stop = true;
                return;
            }


            // Make sure we don't have an authenticated value.  V2 doesn't support them
            {
                uint16_t    authDiversity  = 0;
                bool        authIsAddr     = false;
                uint8_t     authKey        = 0;
                if ( Fixup::Cache64::hasAuthData(loc, authDiversity, authIsAddr, authKey) ) {
                    std::string dylibName = "unknown dylib";
                    std::string segName   = "unknown segment";
                    //findDylibAndSegment((void*)pageContent, dylibName, segName);
                    diag.error("rebase pointer (0x%0llX) is authenticated. vmOffsetInSegment=0x%04llX, seg=%s, dylib=%s\n",
                               vmAddr.rawValue(), vmOffsetInSegment.rawValue(), segName.c_str(), dylibName.c_str());
                    return;
                }
            }

            uint64_t targetValue = ((vmAddr.rawValue() - valueAdd) & valueMask);
            if ( uint8_t high8 = Fixup::Cache64::getHigh8(loc) ) {
                uint64_t tbi = (uint64_t)high8 << 56;
                targetValue |= tbi;
            }

            fixup->raw64 = targetValue;

            lastFixup     = fixup;
            lastPageIndex = pageIndex;
        });
    }

    if ( diag.hasError() )
        return Error("could not build slide info because: %s", diag.errorMessageCStr());

    // V2 doesn't deduplicate content like V1, so the used size is the original size too
    slideChunk->usedFileSize = slideChunk->subCacheFileSize;

    return Error();
}

Error SubCache::computeSlideInfoV3(const BuilderConfig&           config,
                                   cache_builder::SlideInfoChunk* slideChunk,
                                   Region&                        region)
{
    Diagnostics diag;

    bool canContainAuthPointers = region.canContainAuthPointers();

    assert((region.subCacheVMSize.rawValue() % config.slideInfo.slideInfoPageSize) == 0);
    dyld_cache_slide_info3* info = (dyld_cache_slide_info3*)slideChunk->subCacheBuffer;
    info->version                = 3;
    info->page_size              = config.slideInfo.slideInfoPageSize;
    info->page_starts_count      = (uint32_t)region.subCacheVMSize.rawValue() / config.slideInfo.slideInfoPageSize;
    info->auth_value_add         = config.layout.cacheBaseAddress.rawValue();

    assert((sizeof(dyld_cache_slide_info3) + (info->page_starts_count * sizeof(uint16_t))) <= slideChunk->cacheVMSize.rawValue());

    std::fill(&info->page_starts[0], &info->page_starts[info->page_starts_count], DYLD_CACHE_SLIDE_V3_PAGE_ATTR_NO_REBASE);

    // Walk each fixup in each segment.  Every time we cross a page, add a page start
    __block dyld_cache_slide_pointer3* lastFixup     = nullptr;
    __block uint64_t                   lastPageIndex = ~0ULL;
    for ( Chunk* chunk : region.chunks ) {
        SlidChunk* slidChunk = chunk->isSlidChunk();

        slidChunk->tracker.forEachFixup(^(void *loc, bool& stop) {
            // V3 fixups must be 8-byte aligned
            assert(((uint64_t)loc % 8) == 0);

            VMOffset       vmOffsetInSegment((uint64_t)loc - (uint64_t)slidChunk->subCacheBuffer);
            CacheVMAddress fixupVMAddr = slidChunk->cacheVMAddress + vmOffsetInSegment;
            uint64_t       pageIndex   = (fixupVMAddr - region.subCacheVMAddress).rawValue() / info->page_size;

            // If we are on a new page, then start a new chain
            if ( pageIndex != lastPageIndex ) {
                uint64_t vmOffsetInPage      = fixupVMAddr.rawValue() % info->page_size;
                info->page_starts[pageIndex] = vmOffsetInPage;
            }
            else {
                // Patch the previous fixup on this page to point to this one
                lastFixup->auth.offsetToNextPointer = ((uint64_t)loc - (uint64_t)lastFixup) / 8;
            }

            dyld_cache_slide_pointer3* fixupLocation = (dyld_cache_slide_pointer3*)loc;

            // Convert this fixup from the chained format in the cache builder, to the version we want in the cache file
            CacheVMAddress vmAddr = Fixup::Cache64::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress,
                                                                                  loc);

            uint8_t high8 = Fixup::Cache64::getHigh8(loc);

            uint16_t    authDiversity  = 0;
            bool        authIsAddr     = false;
            uint8_t     authKey        = 0;
            if ( Fixup::Cache64::hasAuthData(loc, authDiversity, authIsAddr, authKey) ) {
                // Authenticated value
                assert(high8 == 0);
                assert(canContainAuthPointers);

                VMOffset cacheVMOffset = vmAddr - config.layout.cacheBaseAddress;
                fixupLocation->auth.offsetFromSharedCacheBase   = cacheVMOffset.rawValue();
                fixupLocation->auth.diversityData               = authDiversity;
                fixupLocation->auth.hasAddressDiversity         = authIsAddr ? 1 : 0;
                fixupLocation->auth.key                         = authKey;
                fixupLocation->auth.offsetToNextPointer         = 0;
                fixupLocation->auth.unused                      = 0;
                fixupLocation->auth.authenticated               = 1;

                assert(fixupLocation->auth.offsetFromSharedCacheBase == cacheVMOffset.rawValue());
            } else {
                // Unauthenticated value
                uint64_t pointerValue = vmAddr.rawValue() | ((uint64_t)high8 << 43);
                fixupLocation->plain.pointerValue           = pointerValue;
                fixupLocation->plain.offsetToNextPointer    = 0;
                fixupLocation->plain.unused                 = 0;
                assert(fixupLocation->plain.pointerValue == pointerValue);
            }

            lastFixup     = fixupLocation;
            lastPageIndex = pageIndex;
        });
    }

    if ( diag.hasError() )
        return Error("could not build slide info because: %s", diag.errorMessageCStr());

    // V3 doesn't deduplicate content like V1, so the used size is the original size too
    slideChunk->usedFileSize = slideChunk->subCacheFileSize;

    return Error();
}

Error SubCache::computeSlideInfoForRegion(const BuilderConfig&           config,
                                          cache_builder::SlideInfoChunk* slideChunk,
                                          Region&                        region)
{
    switch ( config.slideInfo.slideInfoFormat.value() ) {
        case cache_builder::SlideInfo::SlideInfoFormat::v1:
            return computeSlideInfoV1(config, slideChunk, region);
        case cache_builder::SlideInfo::SlideInfoFormat::v2:
            return computeSlideInfoV2(config, slideChunk, region);
        case cache_builder::SlideInfo::SlideInfoFormat::v3:
            return computeSlideInfoV3(config, slideChunk, region);
    }

    return Error();
}

Error SubCache::convertChainsToVMAddresses(const BuilderConfig& config, Region& region)
{
    Diagnostics diag;

    for ( Chunk* chunk : region.chunks ) {
        SlidChunk* slidChunk = chunk->isSlidChunk();

        slidChunk->tracker.forEachFixup(^(void *loc, bool& stop) {
            if ( config.layout.is64 ) {
                CacheVMAddress vmAddr = Fixup::Cache64::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress, loc);
                uint8_t high8 = Fixup::Cache64::getHigh8(loc);
                *(uint64_t*)loc = vmAddr.rawValue() | ((uint64_t)high8 << 56);
            } else {
                CacheVMAddress vmAddr = Fixup::Cache32::getCacheVMAddressFromLocation(config.layout.cacheBaseAddress, loc);
                *(uint32_t*)loc = (uint32_t)vmAddr.rawValue();
            }
        });
    }

    if ( diag.hasError() )
        return Error("could not build slide info because: %s", diag.errorMessageCStr());

    return Error();
}

Error SubCache::computeSlideInfo(const BuilderConfig& config)
{
    for ( Region& region : this->regions ) {
        Error err;
        switch ( region.kind ) {
            case Region::Kind::text:
                // No slide info for text
                break;
            case Region::Kind::data:
                if ( config.slideInfo.slideInfoFormat.has_value() )
                    err = computeSlideInfoForRegion(config, this->dataSlideInfo.get(), region);
                else
                    err = convertChainsToVMAddresses(config, region);
                break;
            case Region::Kind::dataConst:
                if ( config.slideInfo.slideInfoFormat.has_value() )
                    err = computeSlideInfoForRegion(config, this->dataConstSlideInfo.get(), region);
                else
                    err = convertChainsToVMAddresses(config, region);
                break;
            case Region::Kind::auth:
                if ( config.slideInfo.slideInfoFormat.has_value() )
                    err = computeSlideInfoForRegion(config, this->authSlideInfo.get(), region);
                else
                    err = convertChainsToVMAddresses(config, region);
                break;
            case Region::Kind::authConst:
                if ( config.slideInfo.slideInfoFormat.has_value() )
                    err = computeSlideInfoForRegion(config, this->authConstSlideInfo.get(), region);
                else
                    err = convertChainsToVMAddresses(config, region);
                break;
            case Region::Kind::linkedit:
                // No slide info for text
                break;
            case Region::Kind::unmapped:
            case Region::Kind::dynamicConfig:
            case Region::Kind::codeSignature:
                // No slide info for unmapped/code signature
                break;
            case Region::Kind::numKinds:
                assert(0);
                break;
        }

        if ( err.hasError() )
            return err;
    }

    return Error();
}

bool SubCache::shouldKeepCache(bool keepDevelopmentCaches, bool keepCustomerCaches) const
{
    switch ( this->kind ) {
        case Kind::mainDevelopment:
        case Kind::stubsDevelopment:
            return keepDevelopmentCaches;
        case Kind::mainCustomer:
        case Kind::stubsCustomer:
            return keepCustomerCaches;
        case Kind::subUniversal:
        case Kind::symbols:
            return true;
    }
}
