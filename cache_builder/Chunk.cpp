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

#include "Chunk.h"
#include "CodeSigningTypes.h"

#include "BuilderConfig.h"

#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonDigestSPI.h>

using namespace cache_builder;

//
// MARK: --- Alignment ---
//

namespace cache_builder
{
    namespace impl
    {
        // Many chunks are aligned for a variety of reasons.  This tracks what alignments and why
        enum class Alignment
        {
            // The atom doesn't need any special alignment, or is guaranteed to be aligned for other reasons
            // eg, the cache header is always page aligned because its at the start of the buffer
            none        = 1,

            // const char* only needs 1-byte alignment
            string      = 1,

            // uleb's only need 1-byte alignment
            uleb        = 1,

            // This chunk points to data which contains a uint64_t or similar, so needs 64-bit alignment
            struct64    = 8,

            // The objc runtime needs these to be 8-byte aligned for now.  If we ever supported arm64_32,
            // those would need 16-byte alignment
            impCaches   = 8,

            // Keep stubs 16-byte aligned to improve cache performance as then the whole stub will hopefully
            // be on the same cache line
            stubs       = 16,

            // FIXME: Not sure why this is 16.  Seems like 8 would be sufficient.
            nlist       = 16,

            // Inside the cache there is minimal overhead for 16K alignment even on 4K hardware
            page        = 16*1024,
        };
    } // namespace impl
} // namespace cache_builder;

using namespace cache_builder::impl;

//
// MARK: --- Chunk methods ---
//

Chunk::Chunk(Kind kind, uint64_t minAlignment)
    : kind(kind), minAlignment(minAlignment)
{

}

Chunk::Chunk(Kind kind, Alignment minAlignment)
    : Chunk(kind, (uint64_t)minAlignment)
{

}

Chunk::~Chunk()
{

}

void Chunk::dump() const
{
    printf("Chunk\n");
}

bool Chunk::isZeroFill() const
{
    return false;
}

SlidChunk* Chunk::isSlidChunk()
{
    return nullptr;
}

const DylibSegmentChunk* Chunk::isDylibSegmentChunk() const
{
    return nullptr;
}

const LinkeditDataChunk* Chunk::isLinkeditDataChunk() const
{
    return nullptr;
}

StubsChunk* Chunk::isStubsChunk()
{
    return nullptr;
}

UniquedGOTsChunk* Chunk::isUniquedGOTsChunk()
{
    return nullptr;
}

uint32_t Chunk::sortOrder() const
{
    return (uint32_t)kind;
}

uint64_t Chunk::alignment() const
{
    return this->minAlignment;
}

//
// MARK: --- HeaderChunk methods ---
//

CacheHeaderChunk::CacheHeaderChunk()
    : Chunk(Kind::cacheHeader, Alignment::none)
{
}

CacheHeaderChunk::~CacheHeaderChunk()
{

}

void CacheHeaderChunk::dump() const
{
    printf("HeaderChunk\n");
}

const char* CacheHeaderChunk::name() const
{
    return "cache header";
}

//
// MARK: --- SlideInfoChunk methods ---
//

SlideInfoChunk::SlideInfoChunk()
    : Chunk(Kind::slideInfo, Alignment::none)
{
}

SlideInfoChunk::~SlideInfoChunk()
{

}

void SlideInfoChunk::dump() const
{
    printf("SlideInfoChunk\n");
}

const char* SlideInfoChunk::name() const
{
    return "slide info";
}

//
// MARK: --- CodeSignatureChunk methods ---
//

CodeSignatureChunk::CodeSignatureChunk()
    : Chunk(Kind::codeSignature, Alignment::none)
{
}

CodeSignatureChunk::~CodeSignatureChunk()
{

}

void CodeSignatureChunk::dump() const
{
    printf("CodeSignatureChunk\n");
}

const char* CodeSignatureChunk::name() const
{
    return "code signature";
}

//
// MARK: --- SlidChunk methods ---
//

SlidChunk::SlidChunk(Kind kind, uint64_t minAlignment)
    : Chunk(kind, minAlignment)
{
}

SlidChunk::~SlidChunk()
{

}

void SlidChunk::dump() const
{
    printf("SlidChunk\n");
}

SlidChunk* SlidChunk::isSlidChunk()
{
    return this;
}

//
// MARK: --- ObjCOptsHeaderChunk methods ---
//

ObjCOptsHeaderChunk::ObjCOptsHeaderChunk()
    : Chunk(Kind::objcOptsHeader, Alignment::struct64)
{
}

ObjCOptsHeaderChunk::~ObjCOptsHeaderChunk()
{

}

void ObjCOptsHeaderChunk::dump() const
{
    printf("ObjCOptsHeaderChunk\n");
}

const char* ObjCOptsHeaderChunk::name() const
{
    return "objc opts header";
}

//
// MARK: --- ObjCHeaderInfoReadOnlyChunk methods ---
//

ObjCHeaderInfoReadOnlyChunk::ObjCHeaderInfoReadOnlyChunk()
    : Chunk(Kind::objcHeaderInfoRO, Alignment::struct64)
{
}

ObjCHeaderInfoReadOnlyChunk::~ObjCHeaderInfoReadOnlyChunk()
{

}

void ObjCHeaderInfoReadOnlyChunk::dump() const
{
    printf("ObjCHeaderInfoReadOnlyChunk\n");
}

const char* ObjCHeaderInfoReadOnlyChunk::name() const
{
    return "objc headerinfo RO";
}

//
// MARK: --- ObjCHeaderInfoReadWriteChunk methods ---
//

ObjCHeaderInfoReadWriteChunk::ObjCHeaderInfoReadWriteChunk()
    : SlidChunk(Kind::objcHeaderInfoRW, (uint64_t)Alignment::struct64)
{
}

ObjCHeaderInfoReadWriteChunk::~ObjCHeaderInfoReadWriteChunk()
{

}

void ObjCHeaderInfoReadWriteChunk::dump() const
{
    printf("ObjCHeaderInfoReadWriteChunk\n");
}

const char* ObjCHeaderInfoReadWriteChunk::name() const
{
    return "objc headerinfo RW";
}

//
// MARK: --- ObjCStringsChunk methods ---
//

ObjCStringsChunk::ObjCStringsChunk()
    : Chunk(Kind::objcStrings, Alignment::string)
{
}

ObjCStringsChunk::~ObjCStringsChunk()
{

}

void ObjCStringsChunk::dump() const
{
    printf("ObjCStringsChunk\n");
}

const char* ObjCStringsChunk::name() const
{
    return "objc strings";
}

//
// MARK: --- ObjCSelectorHashTableChunk methods ---
//

ObjCSelectorHashTableChunk::ObjCSelectorHashTableChunk()
    : Chunk(Kind::objcSelectorsHashTable, Alignment::struct64)
{
}

ObjCSelectorHashTableChunk::~ObjCSelectorHashTableChunk()
{

}

void ObjCSelectorHashTableChunk::dump() const
{
    printf("ObjCSelectorHashTableChunk\n");
}

const char* ObjCSelectorHashTableChunk::name() const
{
    return "objc selector hash table";
}

//
// MARK: --- ObjCClassHashTableChunk methods ---
//

ObjCClassHashTableChunk::ObjCClassHashTableChunk()
    : Chunk(Kind::objcClassesHashTable, Alignment::struct64)
{
}

ObjCClassHashTableChunk::~ObjCClassHashTableChunk()
{

}

void ObjCClassHashTableChunk::dump() const
{
    printf("ObjCClassHashTableChunk\n");
}

const char* ObjCClassHashTableChunk::name() const
{
    return "objc class hash table";
}

//
// MARK: --- SwiftOptsHeaderChunk methods ---
//

SwiftOptsHeaderChunk::SwiftOptsHeaderChunk()
    : Chunk(Kind::swiftOptsHeader, Alignment::struct64)
{
}

SwiftOptsHeaderChunk::~SwiftOptsHeaderChunk()
{

}

void SwiftOptsHeaderChunk::dump() const
{
    printf("SwiftOptsHeaderChunk\n");
}

const char* SwiftOptsHeaderChunk::name() const
{
    return "swift opts header";
}

//
// MARK: --- SwiftProtocolConformancesHashTableChunk methods ---
//

SwiftProtocolConformancesHashTableChunk::SwiftProtocolConformancesHashTableChunk()
    : Chunk(Kind::swiftConformanceHashTable, Alignment::struct64)
{
}

SwiftProtocolConformancesHashTableChunk::~SwiftProtocolConformancesHashTableChunk()
{

}

void SwiftProtocolConformancesHashTableChunk::dump() const
{
    printf("SwiftProtocolConformancesHashTableChunk\n");
}

const char* SwiftProtocolConformancesHashTableChunk::name() const
{
    return "swift conformance hash table";
}

//
// MARK: --- ObjCProtocolHashTableChunk methods ---
//

ObjCProtocolHashTableChunk::ObjCProtocolHashTableChunk()
    : Chunk(Kind::objcProtocolsHashTable, Alignment::struct64)
{
}

ObjCProtocolHashTableChunk::~ObjCProtocolHashTableChunk()
{

}

void ObjCProtocolHashTableChunk::dump() const
{
    printf("ObjCProtocolHashTableChunk\n");
}

const char* ObjCProtocolHashTableChunk::name() const
{
    return "objc protocol hash table";
}

//
// MARK: --- ObjCCanonicalProtocolsChunk methods ---
//

ObjCCanonicalProtocolsChunk::ObjCCanonicalProtocolsChunk()
    : SlidChunk(Kind::objcCanonicalProtocols, (uint64_t)Alignment::struct64)
{
}

ObjCCanonicalProtocolsChunk::~ObjCCanonicalProtocolsChunk()
{

}

void ObjCCanonicalProtocolsChunk::dump() const
{
    printf("ObjCCanonicalProtocolsChunk\n");
}

const char* ObjCCanonicalProtocolsChunk::name() const
{
    return "objc canonical protocols";
}

//
// MARK: --- ObjCIMPCachesChunk methods ---
//

ObjCIMPCachesChunk::ObjCIMPCachesChunk()
    : Chunk(Kind::objcIMPCaches, Alignment::impCaches)
{
}

ObjCIMPCachesChunk::~ObjCIMPCachesChunk()
{

}

void ObjCIMPCachesChunk::dump() const
{
    printf("ObjCIMPCachesChunk\n");
}

const char* ObjCIMPCachesChunk::name() const
{
    return "objc IMP caches";
}

//
// MARK: --- CacheTrieChunk methods ---
//

CacheTrieChunk::CacheTrieChunk(Kind kind)
    : Chunk(kind, Alignment::uleb)
{
}

CacheTrieChunk::~CacheTrieChunk()
{

}

void CacheTrieChunk::dump() const
{
    printf("CacheTrieChunk\n");
}

const char* CacheTrieChunk::name() const
{
    return "cache dylibs trie";
}

//
// MARK: --- PatchTableChunk methods ---
//

PatchTableChunk::PatchTableChunk()
    : Chunk(Kind::cachePatchTable, Alignment::struct64)
{
}

PatchTableChunk::~PatchTableChunk()
{

}

void PatchTableChunk::dump() const
{
    printf("PatchTableChunk\n");
}

const char* PatchTableChunk::name() const
{
    return "cache patch table";
}

//
// MARK: --- PrebuiltLoaderChunk methods ---
//

PrebuiltLoaderChunk::PrebuiltLoaderChunk(Kind kind)
    : Chunk(kind, Alignment::struct64)
{
}

PrebuiltLoaderChunk::~PrebuiltLoaderChunk()
{

}

void PrebuiltLoaderChunk::dump() const
{
    printf("PrebuiltLoaderChunk\n");
}

const char* PrebuiltLoaderChunk::name() const
{
    return "cache dylib Loaders";
}

//
// MARK: --- UnmappedSymbolsChunk methods ---
//

UnmappedSymbolsChunk::UnmappedSymbolsChunk()
    : Chunk(Kind::unmappedSymbols, Alignment::struct64)
{
}

UnmappedSymbolsChunk::~UnmappedSymbolsChunk()
{

}

void UnmappedSymbolsChunk::dump() const
{
    printf("UnmappedSymbolsChunk\n");
}

const char* UnmappedSymbolsChunk::name() const
{
    return "unmapped symbols";
}

//
// MARK: --- SegmentInfo methods ---
//

DylibSegmentChunk::DylibSegmentChunk(Kind kind, uint64_t minAlignment)
    : SlidChunk(kind, minAlignment)
{
}

DylibSegmentChunk::~DylibSegmentChunk()
{

}

void DylibSegmentChunk::dump() const
{
    printf("SegmentInfo\n");
}

const char* DylibSegmentChunk::name() const
{
    return this->segmentName.data();
}

const DylibSegmentChunk* DylibSegmentChunk::isDylibSegmentChunk() const
{
    return this;
}

//
// MARK: --- LinkeditDataChunk methods ---
//

LinkeditDataChunk::LinkeditDataChunk(Kind kind, uint64_t minAlignment)
    : Chunk(kind, minAlignment)
{
}

LinkeditDataChunk::~LinkeditDataChunk()
{

}

void LinkeditDataChunk::dump() const
{
    printf("LinkeditDataChunk\n");
}

const char* LinkeditDataChunk::name() const
{
    const char* chunkName = nullptr;
    switch ( this->kind ) {
        case Chunk::Kind::linkeditSymbolNList:
            chunkName = "linkedit nlist";
            break;
        case Chunk::Kind::linkeditSymbolStrings:
            chunkName = "linkedit symbol strings";
            break;
        case Chunk::Kind::linkeditIndirectSymbols:
            chunkName = "linkedit indirect symbols";
            break;
        case Chunk::Kind::linkeditFunctionStarts:
            chunkName = "linkedit function starts";
            break;
        case Chunk::Kind::linkeditDataInCode:
            chunkName = "linkedit Mr Data (in code)";
            break;
        case Chunk::Kind::linkeditExportTrie:
            chunkName = "linkedit export trie";
            break;
        default:
            assert("unknown linkedit chunk");
            break;
    }
    return chunkName;
}

const LinkeditDataChunk* LinkeditDataChunk::isLinkeditDataChunk() const
{
    return this;
}

bool LinkeditDataChunk::isIndirectSymbols() const
{
    return this->kind == Chunk::Kind::linkeditIndirectSymbols;
}

bool LinkeditDataChunk::isNList() const
{
    return this->kind == Chunk::Kind::linkeditSymbolNList;
}

bool LinkeditDataChunk::isNSymbolStrings() const
{
    return this->kind == Chunk::Kind::linkeditSymbolStrings;
}

//
// MARK: --- NListChunk methods ---
//

NListChunk::NListChunk()
    : Chunk(Kind::optimizedSymbolNList, Alignment::nlist)
{
}

NListChunk::~NListChunk()
{

}

void NListChunk::dump() const
{
    printf("NListChunk\n");
}

const char* NListChunk::name() const
{
    return "optimized nlist";
}

//
// MARK: --- SymbolStringsChunk methods ---
//

SymbolStringsChunk::SymbolStringsChunk()
    : Chunk(Kind::optimizedSymbolStrings, Alignment::uleb)
{
}

SymbolStringsChunk::~SymbolStringsChunk()
{

}

void SymbolStringsChunk::dump() const
{
    printf("SymbolStringsChunk\n");
}

const char* SymbolStringsChunk::name() const
{
    return "optimize symbol strings";
}

//
// MARK: --- UniquedGOTsChunk methods ---
//

UniquedGOTsChunk::UniquedGOTsChunk()
    : SlidChunk(Kind::uniquedGOTs, (uint64_t)Alignment::struct64)
{
}

UniquedGOTsChunk::~UniquedGOTsChunk()
{

}

void UniquedGOTsChunk::dump() const
{
    printf("UniquedGOTsChunk\n");
}

const char* UniquedGOTsChunk::name() const
{
    return "uniqued GOTs";
}

UniquedGOTsChunk* UniquedGOTsChunk::isUniquedGOTsChunk()
{
    return this;
}

//
// MARK: --- StubsChunk methods ---
//

StubsChunk::StubsChunk()
    : Chunk(Kind::stubs, Alignment::stubs)
{
}

StubsChunk::~StubsChunk()
{

}

void StubsChunk::dump() const
{
    printf("StubsChunk\n");
}

const char* StubsChunk::name() const
{
    return "stubs";
}

StubsChunk* StubsChunk::isStubsChunk()
{
    return this;
}

//
// MARK: --- StubsChunk methods ---
//

DynamicConfigChunk::DynamicConfigChunk()
    : Chunk(Kind::dynamicConfig, Alignment::page)
{
}

DynamicConfigChunk::~DynamicConfigChunk()
{

}

void DynamicConfigChunk::dump() const
{
    printf("DynamicConfigChunk\n");
}

const char* DynamicConfigChunk::name() const
{
    return "dynamic configuration content";
}

// DynamicConfigChunk takes up no space in the file, but does take up VM space
// It will be checked to ensure that its always at the end of its Region
bool DynamicConfigChunk::isZeroFill() const
{
    return true;
}
