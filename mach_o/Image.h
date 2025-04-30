/*
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


#ifndef mach_o_Image_h
#define mach_o_Image_h

#include "MachODefines.h"
#include "Error.h"
#include "Header.h"
#include "ExportsTrie.h"
#include "NListSymbolTable.h"
#include "RebaseOpcodes.h"
#include "BindOpcodes.h"
#include "ChainedFixups.h"
#include "Fixups.h"
#include "FunctionStarts.h"
#include "CompactUnwind.h"
#include "SplitSeg.h"
#include "FunctionVariants.h"


namespace mach_o {


/*!
 * @class Image
 *
 * @abstract
 *      Class analyzing mach-o files.
 */
class VIS_HIDDEN Image
{
public:
    enum class MappingKind : uint8_t { wholeSliceMapped, dyldLoadedPreFixups, dyldLoadedPostFixups, unknown };

                    Image() = delete;
                    Image(const void* buffer, size_t bufferSize, MappingKind kind);
                    Image(const mach_header* mh);  // for dyld loaded images only
                    Image(const Image&& other);
                    Image(const Image& other) = default;

    Error                 validate() const;

    const Header*         header() const         { return _buffer; }
    uint32_t              pageSize() const;

    void                  withSegments(void (^callback)(std::span<const MappedSegment> segments)) const;
    void                  forEachBindTarget(void (^callback)(const Fixup::BindTarget& targetInfo, bool& stop)) const;
    void                  forEachFixup(void (^callback)(const Fixup& fixup, bool& stop)) const;
    void                  forEachFixup(std::span<const MappedSegment> segments, void (^callback)(const Fixup& fixup, bool& stop)) const;
    void                  forEachFilesetImage(void (^callback)(const Image& entryImage, std::string_view name, bool& stop)) const;

    bool                    hasExportsTrie() const     { return (_exportsTrie != nullptr); }
    bool                    hasSymbolTable() const     { return (_symbolTable != nullptr); }
    bool                    hasRebaseOpcodes() const   { return (_rebaseOpcodes != nullptr); }
    bool                    hasBindOpcodes() const     { return (_bindOpcodes != nullptr); }
    bool                    hasLazyBindOpcodes() const { return (_lazyBindOpcodes != nullptr); }
    bool                    hasWeakBindOpcodes() const { return (_weakBindOpcodes != nullptr); }
    bool                    hasChainedFixups() const   { return (_chainedFixups != nullptr); }
    bool                    hasFunctionStarts() const  { return (_functionStarts != nullptr); }
    bool                    hasCompactUnwind() const   { return (_compactUnwind != nullptr); }
    bool                    hasSplitSegInfo() const    { return (_splitSegInfo != nullptr); }
    bool                    hasFunctionVariants() const      { return (_functionVariants != nullptr); }
    bool                    hasFunctionVariantFixups() const { return (_functionVariantFixups != nullptr); }
    bool                    hasFirmwareFixups() const        { return _buffer->hasFirmwareChainStarts(); }
    const ExportsTrie&           exportsTrie() const            { return *_exportsTrie; }
    const NListSymbolTable&      symbolTable() const            { return *_symbolTable; }
    const RebaseOpcodes&         rebaseOpcodes() const          { return *_rebaseOpcodes; }
    const BindOpcodes&           bindOpcodes() const            { return *_bindOpcodes; }
    const LazyBindOpcodes&       lazyBindOpcodes() const        { return *_lazyBindOpcodes; }
    const BindOpcodes&           weakBindOpcodes() const        { return *_weakBindOpcodes; }
    const ChainedFixups&         chainedFixups() const          { return *_chainedFixups; }
    const FunctionStarts&        functionStarts() const         { return *_functionStarts; }
    const CompactUnwind&         compactUnwind() const          { return *_compactUnwind; }
    const SplitSegInfo&          splitSegInfo() const           { return *_splitSegInfo; }
    const FunctionVariants&      functionVariants() const       { return *_functionVariants; }
    const FunctionVariantFixups& functionVariantFixups() const  { return *_functionVariantFixups; }

    std::span<const uint32_t> indirectSymbolTable() const;
    std::span<uint8_t>        atomInfo() const;

    uint32_t                segmentCount() const;
    MappedSegment           segment(uint32_t segIndex) const;
    void                    forEachInitializer(bool contentRebased, void (^callback)(uint32_t offset)) const;
    void                    forEachClassicTerminator(bool contentRebased, void (^callback)(uint32_t offset)) const;

private:
    bool                inferIfZerofillExpanded() const;
    Error               validLinkedit(const Policy& policy) const;
    Error               validStructureLinkedit(const Policy& policy) const;
    Error               validInitializers(const Policy& policy) const;

    void                makeExportsTrie();
    void                makeSymbolTable();
    void                makeRebaseOpcodes();
    void                makeBindOpcodes();
    void                makeLazyBindOpcodes();
    void                makeWeakBindOpcodes();
    void                makeChainedFixups();
    void                makeFunctionStarts();
    void                makeCompactUnwind();
    void                makeSplitSegInfo();
    void                makeFunctionVariants();
    void                makeFunctionVariantFixups();


    const Header*       _buffer;
    size_t              _bufferSize;
    const uint8_t*      _linkeditBias    = nullptr; // add LC file-offset to this to get linkedit content
    MappingKind         _mappingKind;
    bool                _hasZerofillExpansion;

    const ExportsTrie*           _exportsTrie           = nullptr;
    const NListSymbolTable*      _symbolTable           = nullptr;
    const RebaseOpcodes*         _rebaseOpcodes         = nullptr;
    const BindOpcodes*           _bindOpcodes           = nullptr;
    const LazyBindOpcodes*       _lazyBindOpcodes       = nullptr;
    const BindOpcodes*           _weakBindOpcodes       = nullptr;
    const ChainedFixups*         _chainedFixups         = nullptr;
    const FunctionStarts*        _functionStarts        = nullptr;
    const CompactUnwind*         _compactUnwind         = nullptr;
    const SplitSegInfo*          _splitSegInfo          = nullptr;
    const FunctionVariants*      _functionVariants      = nullptr;
    const FunctionVariantFixups* _functionVariantFixups = nullptr;

    uint8_t                 _exportsTrieSpace[sizeof(ExportsTrie)];
    uint8_t                 _symbolTableSpace[sizeof(NListSymbolTable)];
    uint8_t                 _rebaseOpcodesSpace[sizeof(RebaseOpcodes)];
    uint8_t                 _bindOpcodesSpace[sizeof(BindOpcodes)];
    uint8_t                 _lazyBindOpcodesSpace[sizeof(LazyBindOpcodes)];
    uint8_t                 _weakBindOpcodesSpace[sizeof(BindOpcodes)];
    uint8_t                 _chainedFixupsSpace[sizeof(ChainedFixups)];
    uint8_t                 _functionStartsSpace[sizeof(FunctionStarts)];
    uint8_t                 _compactUnwindSpace[sizeof(CompactUnwind)];
    uint8_t                 _splitSegSpace[sizeof(SplitSegInfo)];
    uint8_t                 _functionVariantsSpace[sizeof(FunctionVariants)];
    uint8_t                 _functionVariantFixupsSpace[sizeof(FunctionVariantFixups)];
};



} // namespace mach_o

#endif /* mach_o_Image_h */
