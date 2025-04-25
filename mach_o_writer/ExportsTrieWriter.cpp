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

#include <climits>
#include <sys/types.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <mach-o/loader.h>

// mach_o
#include "Symbol.h"
#include "Misc.h"
#include "Algorithm.h"
#include "Header.h"

// mach_o_writer
#include "ChunkBumpAllocator.h"
#include "ExportsTrieWriter.h"

using mach_o::Error;

using mach_o::GenericTrieNode;

namespace mach_o {

//
// MARK: --- Internal trie builder interface ---
//

// Expensive precondition checks are enabled in unit tests only.
// FIXME: Actually implement these again for unit tests
#define trieTraceAssert(...)

struct VIS_HIDDEN GenericTrieWriterEntry
{
    std::string_view    name;
    std::span<uint8_t>  payload;
};

using WriterEntry = GenericTrieWriterEntry;

struct VIS_HIDDEN Edge
{
    std::string_view  partialString;
    GenericTrieNode*  child;

    Edge(const std::string_view& s, GenericTrieNode* n) : partialString(s), child(n) { }
    ~Edge() { }
};

struct VIS_HIDDEN GenericTrieNode
{
    std::string_view    cummulativeString;
    std::vector<Edge>   children;
    std::span<uint8_t>  terminalPayload;
    uint32_t            trieOffset = 0;
    uint32_t            trieSize = 0;

    GenericTrieNode(const std::string_view& s) : cummulativeString(s) {}
    ~GenericTrieNode() = default;

    void  updateOffset(uint32_t& curOffset);
    void  writeToStream(std::span<uint8_t>& bytes) const;
};

using Node = GenericTrieNode;

struct VIS_HIDDEN SubtreeRoot
{
    Node*                           parent=nullptr;
    std::span<const WriterEntry>    entries;
};

struct VIS_HIDDEN TrieBuilder
{
    ChunkBumpAllocator allocator;
    std::vector<SubtreeRoot>* roots;

    TrieBuilder(ChunkBumpAllocatorZone& zone, std::vector<SubtreeRoot>* roots): allocator(zone), roots(roots) {}

    Error buildSubtree(Node& parentNode, uint32_t offset, std::span<const WriterEntry> entries);
    void  addTerminalNode(Node& parentNode, const WriterEntry&);
};

struct VIS_HIDDEN NodeWriter
{
    // root nodes of subtrees that are written concurrently
    std::vector<const Node*> subtreeRoots;
    // list of nodes that are written written separately, without their children
    std::vector<const Node*> standaloneNodes;
    std::span<uint8_t>       bytes;

    void collectRecursive(const Node* node, size_t depth);
    void write(const Node& node);
    void writeRecursive(const Node& node);
};

//
// MARK: --- GenericTrieWriter methods ---
//

// construct from an already built trie
GenericTrieWriter::GenericTrieWriter()
    : GenericTrie(nullptr, 0), _trieSize(0), _allocatorZone(ChunkBumpAllocatorZone::make())
{
}

static void write_uleb128(uint64_t value, std::span<uint8_t>& out)
{
    uint8_t* outByte = out.data();
    uint8_t byte;
    do {
        byte = value & 0x7F;
        value &= ~0x7F;
        if ( value != 0 )
            byte |= 0x80;
        *outByte = byte;
        ++outByte;
        value = value >> 7;
    } while ( byte >= 0x80 );
    out = out.subspan(outByte - out.data());
}

static void write_string(const std::string_view& str, std::span<uint8_t>& out)
{
    std::copy(str.begin(), str.end(), out.begin());
    *(out.data() + str.size()) = '\0';
    out = out.subspan(str.size() + 1);
}

// find the number of leading elements in \a entries that have the same character \a ch at \a offset.
// this assumes that the entries are sorted
static uint32_t binSearchNumEntriesWithChar(std::span<const WriterEntry> entries, uint32_t offset, char ch)
{
    if ( entries.empty() )
        assert(false && "cant divide empty list");
    if ( entries.size() == 1 )
        return 1;

    auto cmpOp = [&](const WriterEntry& entry) {
        return offset < entry.name.size() && entry.name[offset] == ch;
    };

    // optimize case where last entry has the same character
    if ( cmpOp(entries.back()) ) {
        return (uint32_t)entries.size();
    }
    auto low = entries.begin();
    // optimize case where second entry has a different character
    if ( !cmpOp(*(low + 1)) )
        return (uint32_t)1;
    auto high = entries.end() - 1;
    while ( true ) {
        auto middle = low + ((high - low) / 2);

        if ( cmpOp(*middle) ) {
            low = middle;
        } else {
            high = middle;
        }
        if ( (high - low) == 1 )
            return (uint32_t)std::distance(entries.begin(), low) + 1;
    }
}

// find first offset that has a different character in any of the entries, starting at an input offset
static Error findFirstDifferentChar(uint32_t& inOutOffset, std::span<const WriterEntry> entries)
{
    if ( entries.size() < 2 )
        return Error::none();

    // entries are sorted, so we only need to compare first and last
    uint32_t diffOffset = inOutOffset;
    while ( entries.front().name.size() > diffOffset && entries.back().name.size() > diffOffset &&
            entries.front().name[diffOffset] == entries.back().name[diffOffset] )
        ++diffOffset;
    inOutOffset = diffOffset;
    // no duplicates, but there's more than one entry so there are duplicates
    if ( diffOffset == entries.back().name.size() ) {
        const WriterEntry& newEntry = entries.back();
        char cstr[entries.back().name.size()+2];
        memcpy(cstr, newEntry.name.data(), newEntry.name.size());
        cstr[newEntry.name.size()] = '\0';
        return Error("duplicate symbol '%s'", (const char*)cstr); // cast is to work around va_list aliasing issue
    }
    return Error::none();
}

// Find the number of leading entries from \a entries span that all have the same character at \a offset.
// If all entries in \a entries have the same character at \a offset then \a edgeBreak
// will be set to the number of all entries and offset will point to the last common character.
static Error nextEdgeBreak(uint32_t& offset, uint32_t& edgeBreak, std::span<const WriterEntry> entries)
{
    assert(entries.size() >= 1);
    auto& entry = entries.front();
    assert(offset < entry.name.size());

    uint32_t diffOffset = offset;
    if ( Error err = findFirstDifferentChar(diffOffset, entries) )
        return err;
    if ( diffOffset != offset ) {
        // common characters found in all entries, update offset to point to the last common character
        offset = diffOffset - 1;
        edgeBreak = (uint32_t)entries.size();
        return Error::none();
    }

    edgeBreak = binSearchNumEntriesWithChar(entries, offset, entry.name[offset]);
    return Error::none();
}

void TrieBuilder::addTerminalNode(Node& parentNode, const WriterEntry& entry)
{
    std::string_view name = entry.name;
    trieTraceAssert(name.starts_with(parentNode.cummulativeString));
    assert(name.size() >= parentNode.cummulativeString.size());
    std::string_view tail = name.substr(parentNode.cummulativeString.size());

    if ( tail.empty() ) {
        assert(parentNode.terminalPayload.empty() && "duplicate node should have been handled before calling addTerminalNode");
        parentNode.terminalPayload = entry.payload;
    } else {
        Node* newNode  = allocator.allocate<Node>();
        new (newNode) Node(name);
        newNode->terminalPayload = entry.payload;
        Edge newEdge(tail, newNode);
        parentNode.children.push_back(newEdge);
    }
}

// Trie building algorithm is based on the requirement that the input entries are sorted.
// Sorted input allows to use binary search to quickly find the number of nodes in a subtree
// and compare first/last entries from the subrange to determine the longest common partial string for an edge.
// The problem of creating the trie is defined recursively - given a parent node, a list of
// entries and a name offset create a subtree with optimal partial strings in edges. Thanks to the sorted input
// we know that all characters are the same up to, but not including, the current offset.
// For example, given symbols `foo`, `fop`, `read`, a root node and offset 0 - use binary search to find
// the first symbol whose character at offset 0 is different from the character of the first symbol.
// Character of the first entry at offset 0 is `f` and next different entry is `read`, so `foo` and `fop` entries
// will form a subtree. Edge from a parent node to the subtree should cover the longest common prefix of all entries
// in the subtree. Again, thanks to the input being sorted it's sufficient to compare character of the first and last
// subtree entries, because if they're the same then so are all entries in between.
// In this approach we know at all times how many more terminal nodes will be created in subtree, which makes it
// easy to parallelize. Once a threshold of remaining entries in a subtree is reached the root of the subtree
// is placed in a list of root nodes and after initial iteration all subtrees will be built concurrently.
//
//
// Below is a visualization of the algorithm with a bit more complex example:
// ```
//  ┌───────────────────┐            ┌──────────────────┐           ┌──────────────────┐         ┌──────────────┐
//  │     offset: 0     │            │    offset: 6     │           │    offset: 7     │         │  offset: 8   │
//  └───────────────────┘            └──────────────────┘           └──────────────────┘         └──────────────┘
//                                                                                                               
//  ┌───────────────────┐                                                                                        
//  │  ┌──────────────┐ │                                                                                        
//  │  │   aaaaaaaa   │ │                                                                                        
//  │  └──────────────┘ │                                                                                        
//  │  ┌──────────────┐ │                                                                                        
//  │  │   aaaaaaab   │ │             ┌──────────────────┐          ┌──────────────────┐         ┌──────────────┐
//  │  └──────────────┘ │             │ ┌──────────────┐ │          │ ┌──────────────┐ │    ┌───▶│   aaaaaaaa   │
//  │  ┌──────────────┐ │             │ │   aaaaaaaa   │ │          │ │   aaaaaaaa   │ │    a    └──────────────┘
//  │  │   aaaaaaba   │ │             │ └──────────────┘ │   a      │ └──────────────┘ │    │                    
//  │  └──────────────┘ │             │                  │    ┌────▶│ ┌──────────────┐ │────┤                    
//  │  ┌─────┐          │             │ ┌──────────────┐ │    │     │ │   aaaaaaab   │ │    │    ┌──────────────┐
//  │  │ foo │          │────────────▶│ │   aaaaaaab   │ │────┤     │ └──────────────┘ │    b───▶│   aaaaaaab   │
//  │  └─────┘          │  aaaaaa     │ └──────────────┘ │    │     └──────────────────┘         └──────────────┘
//  │  ┌─────┐          │             │                  │    │                                                  
//  │  │ fop │          │             │ ┌──────────────┐ │    │     ┌──────────────────┐                         
//  │  └─────┘          │             │ │   aaaaaaba   │ │    └────▶│     aaaaaaba     │                         
//  │  ┌─────┐          │             │ └──────────────┘ │   b      └──────────────────┘                         
//  │  │read │          │             └──────────────────┘                                                       
//  │  └─────┘          │                                                                                        
//  │  ┌─────┐          │                                                                                        
//  │  │write│          │                                                                                        
//  │  └─────┘          │                                                                                        
//  └───────────────────┘                                                                                        
//                                                                                                               
//                                                                                                               
//  ┌───────────────────┐             ┌───────────┐                ┌─────────┐                                   
//  │     offset: 0     │             │ offset: 2 │                │offset: 3│                                   
//  └───────────────────┘             └───────────┘                └─────────┘                                   
//                                                                                                               
//  ┌───────────────────┐                                                                                        
//  │  ┌─────┐          │                                                                                        
//  │  │ foo │          │                                                                                        
//  │  └─────┘          │             ┌───────────┐                                                              
//  │  ┌─────┐          │             │  ┌─────┐  │            o    ┌─────┐                                      
//  │  │ fop │          │             │  │ foo │  │        ┌───────▶│ foo │                                      
//  │  └─────┘          │     fo      │  └─────┘  │        │        └─────┘                                      
//  │  ┌─────┐          │────────────▶│  ┌─────┐  │────────┤                                                     
//  │  │read │          │             │  │ fop │  │        │        ┌─────┐                                      
//  │  └─────┘          │             │  └─────┘  │        └───────▶│ fop │                                      
//  │  ┌─────┐          │             └───────────┘            p    └─────┘                                      
//  │  │write│          │                                                                                        
//  │  └─────┘          │                                                                                        
//  └───────────────────┘                                                                                        
//                                                                                                               
//  ┌───────────────────┐            ┌─────────┐                                                                 
//  │     offset: 0     │            │offset: 4│                                                                 
//  └───────────────────┘            └─────────┘                                                                 
//  ┌───────────────────┐                                                                                        
//  │ ┌─────┐           │                                                                                        
//  │ │read │           │                                                                                        
//  │ └─────┘           │      read    ┌─────┐                                                                   
//  │ ┌─────┐           │─────────────▶│read │                                                                   
//  │ │write│           │              └─────┘                                                                   
//  │ └─────┘           │                                                                                        
//  └───────────────────┘                                                                                        
//                                                                                                               
//  ┌───────────────────┐                     
//  │     offset: 0     │                     
//  └───────────────────┘                     
//                                            
//  ┌───────────────────┐                     
//  │                   │                     
//  │ ┌─────┐           │                     
//  │ │write│           │      write   ┌─────┐
//  │ └─────┘           │─────────────▶│write│
//  │                   │              └─────┘
//  │                   │                     
//  └───────────────────┘                           
// ```
Error TrieBuilder::buildSubtree(Node& parentNode, uint32_t offset, std::span<const WriterEntry> entries)
{
    while ( !entries.empty() ) {
        // one entry left, add the terminal
        if ( entries.size() == 1 ) {
            addTerminalNode(parentNode, entries.front());
            return Error::none();
        }

        // offset equal to the current offset, there's no free characters to make
        // an edge, so this must the current root terminal payload
        if ( entries.front().name.size() == offset ) {
            addTerminalNode(parentNode, entries.front());
            entries = entries.subspan(1);

            // another entry also with length equal to offset, it's a duplicate then
            // otherwise it wouldn't be in the same edge
            const WriterEntry& newEntry = entries.front();
            if ( newEntry.name.size() == offset ) {
                char cstr[newEntry.name.size()+2];
                memcpy(cstr, newEntry.name.data(), newEntry.name.size());
                cstr[newEntry.name.size()] = '\0';
                return Error("duplicate symbol '%s'", (const char*)cstr); // cast is to work around va_list aliasing issue
            }
        }

        uint32_t edgeBreak;
        if ( Error err = nextEdgeBreak(offset, edgeBreak, entries) )
            return err;
        auto edgeNodes = std::span(entries).subspan(0, edgeBreak);
        entries = entries.subspan(edgeNodes.size());

        if ( edgeNodes.size() == 1 ) {
            addTerminalNode(parentNode, edgeNodes.front());
            continue;
        }

        // multiple entries with the same character at the current offset,
        // so make an edge with the common characters
        // first, find all common characters among the nodes in this edge this is to make the edge
        // partial string as long as possible
        uint32_t commonLen = offset;
        if ( Error err = findFirstDifferentChar(commonLen, edgeNodes) )
            return err;
        if ( commonLen == offset )
            assert(false && "edgeNodes come from the size determined by nextEdgeBreak, there must be at least one common character");

        std::string_view firstEntryName = edgeNodes.front().name;
        // first entry will have size longer than the common length or equal, if it's
        // equal then it will become a terminal entry in the newly created node
        assert(firstEntryName.size() >= commonLen);
        std::string_view cummulativeStr = firstEntryName.substr(0, commonLen);
        trieTraceAssert(cummulativeStr.starts_with(parentNode.cummulativeString));
        std::string_view edgePartialStr= cummulativeStr.substr(parentNode.cummulativeString.size());
        assert(edgePartialStr.size() >= 1);

        Node* child = allocator.allocate<Node>();
        new (child) Node(cummulativeStr);
        Edge edge(edgePartialStr, child);
        parentNode.children.push_back(edge);

        // place this node and its items in the roots vector if requested and match a threshold
        // they'll be processed concurrenctly later
        if ( roots && edgeNodes.size() < 0x4000 ) {
            roots->push_back({child, edgeNodes});
        } else {
            if ( Error err = buildSubtree(*child, commonLen, edgeNodes) )
                return err;
        }
    }

    return Error::none();
}

#define DUMP_NODES 0

#if DUMP_NODES
static void dumpNodes(const Node* node, size_t depth=0)
{
    for ( size_t i = 0; i < depth; ++i ) {
        fprintf(stderr, " ");
    }
    fprintf(stderr, "%.*s (%d)\n", (int)node->cummulativeString.size(), node->cummulativeString.data(), !node->terminalEntry.payload.empty());
    for ( auto& edge : node->children ) {
        dumpNodes(edge.child, depth + 1);
    }
}
#endif

const uint8_t* GenericTrieWriter::bytes(size_t& size)
{
    size = _trieEnd - _trieStart;
    return _trieStart;
}

static void updateOffsetPostorder(Node* node, uint32_t& curOffset)
{
    for ( Edge& e : node->children ) {
        updateOffsetPostorder(e.child, curOffset);
    }
    node->updateOffset(curOffset);
}

void GenericTrieWriter::buildNodes(std::span<const WriterEntry> entries)
{
    // build nodes
    _rootNode = _allocatorZone.makeAllocator().allocate<Node>();
    new (_rootNode) Node("");

    std::vector<SubtreeRoot> roots;
    roots.reserve(entries.size() / 0x4000);
    // build initial set of nodes, collecting some subtree roots along the way
    // subtries will be then build concurrently
    if ( !entries.empty() ) {
        TrieBuilder builder(_allocatorZone, &roots);
        if ( Error err = builder.buildSubtree(*_rootNode, 0, entries) ) {
            _buildError = std::move(err);
            return;
        }
    }

    // build subtrees in parallel
    mapReduce(std::span(roots), 1, ^(size_t, Error& chunkErr, std::span<SubtreeRoot> current) {
        // create a builder per a subtree root, without using a roots vector, so all the nodes will be built
        TrieBuilder builder(_allocatorZone, nullptr);

        for ( const SubtreeRoot& root : current ) {
            if ( Error err = builder.buildSubtree(*root.parent, (uint32_t)root.parent->cummulativeString.size(), root.entries) )
                chunkErr = std::move(err);
        }
    }, ^(std::span<Error> errors) {
        for ( Error& err : errors ) {
            if ( err ) {
                _buildError = std::move(err);
                return;
            }
        }
    });

#if DUMP_NODES
    dumpNodes(&rootNode);
#endif

    uint32_t curOffset = 0;
    {
        // set a dummy large trie offset for all chidren of the root node to ensure
        // enough space is reserved for their actual offset, so that the root
        // node size is stable
        for ( Edge& e : _rootNode->children ) {
            e.child->trieOffset = UINT_MAX;
        }
        _rootNode->updateOffset(curOffset);
    }

    // now that the size of the root node is known, offsets can
    // be computed recursively in a single iteration through a postorder traversal
    for ( Edge& e : _rootNode->children )
        updateOffsetPostorder(e.child, curOffset);
    _trieSize = curOffset;

    if ( uint32_t pad = _trieSize % 8; pad != 0 )
        _trieSize += 8 - pad;
}

void NodeWriter::collectRecursive(const Node* node, size_t depth)
{
    standaloneNodes.push_back(node);

    if ( (depth + 1) > 4 ) {
        subtreeRoots.reserve(subtreeRoots.size() + node->children.size());
        std::transform(node->children.begin(), node->children.end(), std::back_inserter(subtreeRoots), [](const Edge& e) { return e.child; });
    } else {
        for ( const Edge& e : node->children ) {
            collectRecursive(e.child, depth + 1);
        }
    }
}

void NodeWriter::write(const Node& node)
{
    std::span<uint8_t> nodeChunk = bytes.subspan(node.trieOffset);
    assert(nodeChunk.size() >= node.trieSize);
    nodeChunk = nodeChunk.subspan(0, node.trieSize);
    node.writeToStream(nodeChunk);
}

void NodeWriter::writeRecursive(const Node& node)
{
    write(node);

    for ( const Edge& e : node.children ) {
        writeRecursive(*e.child);
    }
}

void GenericTrieWriter::writeTrieBytes(std::span<uint8_t> bytes)
{
    // set up trie buffer
    _trieStart = bytes.data();
    _trieEnd   = bytes.end().base();

    assert(_rootNode != nullptr);

    NodeWriter writer;
    // reserve some initial space for nodes
    writer.subtreeRoots.reserve(0x1000);
    writer.standaloneNodes.reserve(0x1000);
    writer.bytes = bytes;
    writer.collectRecursive(_rootNode, 0);

    // write subtrees
    dispatchForEach(std::span(writer.subtreeRoots), 1, [&writer](size_t, const Node* node) {
        writer.writeRecursive(*node);
    });
    // write standalone nodes
    dispatchForEach(std::span(writer.standaloneNodes), 64, [&writer](size_t, const Node* node) {
        writer.write(*node);
    });
}

// byte for terminal node size in bytes, or 0x00 if not terminal node
// teminal node (uleb128 flags, uleb128 addr [uleb128 other])
// byte for child node count
//  each child: zero terminated substring, uleb128 node offset
void Node::updateOffset(uint32_t& curOffset)
{
    trieSize = 1; // length of node payload info when there is no payload (non-terminal)
    if ( !terminalPayload.empty() ) {
        // in terminal nodes, size is uleb128 encoded, so we include that in calculation
        trieSize = (uint32_t)terminalPayload.size();
        trieSize += uleb128_size(trieSize);
    }
    // add children
    ++trieSize; // byte for count of chidren
    for ( Edge& edge : this->children ) {
        trieSize += edge.partialString.size() + 1 + uleb128_size(edge.child->trieOffset);
    }
    trieOffset = curOffset;
    curOffset += trieSize;
}

void Node::writeToStream(std::span<uint8_t>& bytes) const
{
    if ( !terminalPayload.empty() ) {
        std::span<const uint8_t> payload = terminalPayload;
        write_uleb128(payload.size(), bytes);
        std::copy(payload.begin(), payload.end(), bytes.begin());
        bytes = bytes.subspan(payload.size());
    }
    else {
        // no terminal uleb128 of zero is one byte of zero
        *bytes.data() = 0;
        bytes = bytes.subspan(1);
    }
    // write number of children
    *bytes.data() = children.size();
    bytes = bytes.subspan(1);
    // write each child
    for ( const Edge& e : children ) {
        write_string(e.partialString, bytes);
        write_uleb128(e.child->trieOffset, bytes);
    }
}

template<typename WriterEntryGetter>
static inline std::vector<WriterEntry> buildWriterEntries(ChunkBumpAllocatorZone& allocatorZone, size_t entriesCount, bool needsSort, WriterEntryGetter get)
{
    std::vector<WriterEntry> allEntries;
    allEntries.resize(entriesCount);

    // create generic trie's writer entries from higher-level entries used by one of the trie's subclasses
    // this requires that the getter implementation is thread-safe
    const size_t elementsPerChunk = 0x4000;
    mapReduce(std::span(allEntries), elementsPerChunk, ^(size_t chunkIndex, int&, std::span<WriterEntry> entries) {
        ChunkBumpAllocator allocator = allocatorZone.makeAllocator();
        size_t startIndex = chunkIndex * elementsPerChunk;
        for ( size_t i = 0; i < entries.size(); ++i )
            entries[i] = get(startIndex + i, allocator);
    });

    if ( needsSort ) {
        std::sort(allEntries.begin(), allEntries.end(), [](const WriterEntry& lhs, const WriterEntry& rhs) {
            return lhs.name < rhs.name;
        });
    } else {
        trieTraceAssert(std::is_sorted(allEntries.begin(), allEntries.end(), [](const WriterEntry& lhs, const WriterEntry& rhs) {
            return lhs.name < rhs.name;
        }));
    }
    return allEntries;
}


//
// MARK: --- ExportsTrieWriter methods ---
//

ExportsTrieWriter::operator ExportsTrie() const
{
    return { _trieStart, _trieSize };
}

bool ExportsTrieWriter::hasExportedSymbol(const char* symbolName, Symbol& symbol) const
{
    ExportsTrie trie = *this;
    return trie.hasExportedSymbol(symbolName, symbol);
}

void ExportsTrieWriter::forEachExportedSymbol(void (^callback)(const Symbol& symbol, bool& stop)) const
{
    ExportsTrie trie = *this;
    return trie.forEachExportedSymbol(callback);
}

Error ExportsTrieWriter::valid(uint64_t maxVmOffset) const
{
    if ( _buildError.hasError() )
        return Error("%s", _buildError.message());

    ExportsTrie trie = *this;
    return trie.valid(maxVmOffset);
}

// generic trie builder
struct VIS_HIDDEN Export { std::string_view name; uint64_t offset=0; uint64_t flags=0; uint64_t other=0; std::string_view importName; };

static WriterEntry exportToEntry(const Export& exportInfo, ChunkBumpAllocator& allocator)
{
    // encode exportInfo as uleb128s
    std::span<uint8_t> payload;
    assert(!exportInfo.name.empty() && "empty export info");
    if ( exportInfo.flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
        std::string_view importName = exportInfo.importName;
        // optimize case where re-export does not change name to just have a trailing empty string
        if ( importName == exportInfo.name )
            importName = "";
        // nodes with re-export info: size, flags, ordinal, string
        size_t size = uleb128_size(exportInfo.flags) + uleb128_size(exportInfo.other) + importName.size() + 1;
        payload = allocator.allocate(size);
        std::span<uint8_t> temp = payload;
        write_uleb128(exportInfo.flags, temp);
        write_uleb128(exportInfo.other, temp);
        write_string(importName, temp);
        assert(temp.size() == 0);
    }
    else if ( exportInfo.flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
        // nodes with stub and resolver: size, flags, address, other
        size_t size = uleb128_size(exportInfo.flags) + uleb128_size(exportInfo.offset) + uleb128_size(exportInfo.other);
        payload = allocator.allocate(size);
        std::span<uint8_t> temp = payload;
        write_uleb128(exportInfo.flags, temp);
        write_uleb128(exportInfo.offset, temp);
        write_uleb128(exportInfo.other, temp);
        assert(temp.size() == 0);
    }
    else if ( exportInfo.flags & EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT ) {
        // nodes with default and tableIndex: size, flags, address, other
        size_t size = uleb128_size(exportInfo.flags) + uleb128_size(exportInfo.offset) + uleb128_size(exportInfo.other);
        payload = allocator.allocate(size);
        std::span<uint8_t> temp = payload;
        write_uleb128(exportInfo.flags, temp);
        write_uleb128(exportInfo.offset, temp);
        write_uleb128(exportInfo.other, temp);
        assert(temp.size() == 0);
    }
    else {
        // nodes with export info: size, flags, address
        size_t size = uleb128_size(exportInfo.flags) + uleb128_size(exportInfo.offset);
        payload = allocator.allocate(size);
        std::span<uint8_t> temp = payload;
        write_uleb128(exportInfo.flags, temp);
        write_uleb128(exportInfo.offset, temp);
        assert(temp.size() == 0);
    }
    WriterEntry entry;
    entry.name      = exportInfo.name;
    entry.payload   = payload;
    return entry;
}


ExportsTrieWriter::ExportsTrieWriter(std::span<const Symbol> exports, bool writeBytes, bool needsSort)
    : GenericTrieWriter()
{
    std::vector<WriterEntry> entries = buildWriterEntries(_allocatorZone, exports.size(), needsSort, [exports](size_t index, ChunkBumpAllocator& allocator) {
        const Symbol& sym = exports[index];
        Export        exp;
        const char*   importName   = nullptr;
        int           libOrdinal   = 0;
        bool          weakImport   = false;
        uint64_t      resolverStub = 0;
        uint64_t      absAddr      = 0;
        uint32_t      fvtIndex     = 0;
        assert((sym.scope() == Symbol::Scope::global) || (sym.scope() == Symbol::Scope::globalNeverStrip));
        assert(!sym.isUndefined(libOrdinal, weakImport));
        exp.name = sym.name();
        if ( sym.isThreadLocal() ) {
            exp.offset = sym.implOffset();
            exp.flags  = EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL;
            if ( sym.isWeakDef() )
                exp.flags |= EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;
        }
        else if ( sym.isAbsolute(absAddr) ) {
            exp.offset = absAddr;
            exp.flags  = EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE;
        }
        else if ( sym.isReExport(libOrdinal, importName) ) {
            exp.flags      = EXPORT_SYMBOL_FLAGS_REEXPORT;
            exp.other      = libOrdinal;
            exp.importName = importName;
            if ( sym.isWeakDef() )
                exp.flags |= EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;
        }
        else if ( sym.isDynamicResolver(resolverStub) ) {
            exp.offset = resolverStub;
            exp.flags  = EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER;
            exp.other  = sym.implOffset();
        }
        else if ( sym.isFunctionVariant(fvtIndex) ) {
            exp.offset = sym.implOffset();
            exp.flags  = EXPORT_SYMBOL_FLAGS_FUNCTION_VARIANT;
            exp.other  = fvtIndex;
        }
        else if ( sym.isWeakDef() ) {
            exp.offset = sym.implOffset();
            exp.flags  = EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION;
        }
        else {
            exp.offset = sym.implOffset();
            exp.flags  = 0;
        }
        return exportToEntry(exp, allocator);
    });
    buildNodes(entries);
    if ( _buildError.hasError() || !writeBytes )
        return;
    _trieBytes.resize(_trieSize);
    writeTrieBytes(_trieBytes);
}

// build a trie from an existing trie, but filter out some entries
ExportsTrieWriter::ExportsTrieWriter(const ExportsTrie& inputExportsTrie, bool (^remove)(const Symbol& sym))
   : GenericTrieWriter()
{
    ChunkBumpAllocatorZone      zone = ChunkBumpAllocatorZone::make();
    __block ChunkBumpAllocator  bumpAllocator(zone);
    __block std::vector<Symbol> keptSymbols;

    inputExportsTrie.forEachExportedSymbol(^(const Symbol& symbol, bool& stop) {
        if ( remove(symbol) )
            return;
        // The CString in symbol is ephemeral.
        // Make a copy with a long term string
        Symbol newSymbol = symbol;
        const char* ephemString = symbol.name().c_str();
        std::span<uint8_t> buffer = bumpAllocator.allocate(symbol.name().size()+1);
        strcpy((char*)buffer.data(), ephemString);
        newSymbol.setName((char*)buffer.data());
        keptSymbols.push_back(newSymbol);
    });

    // call constructor that takes span of symbols
    *this = ExportsTrieWriter(keptSymbols);
}


//
// MARK: --- DylibsPathTrie methods ---
//


DylibsPathTrieWriter::DylibsPathTrieWriter(std::span<const mach_o::DylibsPathTrie::DylibAndIndex> dylibs, bool needsSort)
    : GenericTrieWriter()
{
    std::vector<WriterEntry> entries = buildWriterEntries(_allocatorZone, dylibs.size(), /* needsSort */ needsSort, [dylibs](size_t index, ChunkBumpAllocator& allocator) {
        const mach_o::DylibsPathTrie::DylibAndIndex& info = dylibs[index];
        // payload for DylibsPathTrie is just uleb128 encoded dylib index
        size_t size = uleb128_size(info.index);
        std::span<uint8_t> payload = allocator.allocate(size);
        std::span<uint8_t> temp = payload;
        write_uleb128(info.index, temp);
        assert(temp.size() == 0);
        WriterEntry entry;
        entry.name      = info.path;
        entry.payload   = payload;
        return entry;
    });
    buildNodes(entries);
    if ( _buildError.hasError() )
        return;
    _trieBytes.resize(_trieSize);
    writeTrieBytes(_trieBytes);
}

DylibsPathTrieWriter::operator DylibsPathTrie() const
{
    return { _trieStart, _trieSize };
}

bool DylibsPathTrieWriter::hasPath(const char* path, uint32_t& dylibIndex) const
{
    DylibsPathTrie trie = *this;
    return trie.hasPath(path, dylibIndex);
}

void DylibsPathTrieWriter::forEachDylibPath(void (^callback)(const mach_o::DylibsPathTrie::DylibAndIndex& info, bool& stop)) const
{
    DylibsPathTrie trie = *this;
    return trie.forEachDylibPath(callback);
}

} // namespace mach_o
