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

#ifndef mach_o_writer_ExportsTrie_h
#define mach_o_writer_ExportsTrie_h

#include <stdint.h>

#include <span>
#include <string_view>

#include <vector>
#include <list>

// mach_o
#include "Error.h"
#include "ExportsTrie.h"

// common
#include "ChunkBumpAllocator.h"

namespace mach_o
{
class Symbol;
}

namespace mach_o {

using namespace mach_o;

struct GenericTrieWriterEntry;
struct GenericTrieNode;

/*!
 * @class GenericTrieWriter
 *
 * @abstract
 *      Abstract base class for building tries
 */
class VIS_HIDDEN GenericTrieWriter : public GenericTrie
{
public:
                    // construct from an already built trie
                    GenericTrieWriter();

    const uint8_t*  bytes(size_t& size);
    size_t          size() { return _trieSize; }
    Error&          buildError() { return _buildError; }
    void            writeTrieBytes(std::span<uint8_t> bytes);

protected:
    void            buildNodes(std::span<const GenericTrieWriterEntry> entries);

    Error                _buildError;
    std::vector<uint8_t> _trieBytes;
    GenericTrieNode*     _rootNode=nullptr;
    size_t               _trieSize;

    ChunkBumpAllocatorZone _allocatorZone;
};



/*!
 * @class ExportsTrieWriter
 *
 * @abstract
 *      Class to encapsulate building export symbol tries
 */
class VIS_HIDDEN ExportsTrieWriter : public GenericTrieWriter
{
public:
                    // encapsulates exports trie in a final linked image
                    ExportsTrieWriter(std::span<const Symbol> exports, bool writeBytes=true, bool needsSort=true);

                    // build a trie from an existing trie, but filter out some entries
                    ExportsTrieWriter(const ExportsTrie& inputExportsTrie, bool (^remove)(const Symbol& sym));

    // From ExportsTrie
    bool            hasExportedSymbol(const char* symbolName, Symbol& symbol) const;
    void            forEachExportedSymbol(void (^callback)(const Symbol& symbol, bool& stop)) const;

    operator ExportsTrie() const;

    Error           valid(uint64_t maxVmOffset) const;
};


/*!
 * @class DylibsPathTrieWriter
 *
 * @abstract
 *      Class to encapsulate building tries as in the dyld cache
 *      to map paths to dylib index.
 */
class VIS_HIDDEN DylibsPathTrieWriter : public GenericTrieWriter
{
public:
                    // encapsulates dylib path trie in the dyld cache
                    DylibsPathTrieWriter(std::span<const mach_o::DylibsPathTrie::DylibAndIndex> dylibs, bool needsSort=true);

    // From DylibsPathTrie
    bool            hasPath(const char* path, uint32_t& dylibIndex) const;
    void            forEachDylibPath(void (^callback)(const mach_o::DylibsPathTrie::DylibAndIndex& info, bool& stop)) const;

    operator DylibsPathTrie() const;
};


} // namespace mach_o

#endif // mach_o_writer_ExportsTrie_h


