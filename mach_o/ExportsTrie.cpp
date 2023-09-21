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

#include "ChunkBumpAllocator.h"
#include "Symbol.h"
#include "ExportsTrie.h"
#include "Misc.h"
#include "Algorithm.h"

namespace mach_o {

//
// MARK: --- Internal trie builder interface ---
//

#if BUILDING_MACHO_WRITER

// Expensive precondition checks are enabled in unit tests only.
#if BUILDING_UNIT_TESTS
#define trieTraceAssert(x) assert(x)
#else
#define trieTraceAssert(...)
#endif

struct GenericTrieWriterEntry
{
    std::string_view    name;
    std::span<uint8_t>  payload;
};

using WriterEntry = GenericTrieWriterEntry;

struct Edge
{
    std::string_view  partialString;
    GenericTrieNode*  child;

    Edge(const std::string_view& s, GenericTrieNode* n) : partialString(s), child(n) { }
    ~Edge() { }
};

struct GenericTrieNode
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

struct SubtreeRoot 
{
    Node*                           parent=nullptr;
    std::span<const WriterEntry>    entries;
};

struct TrieBuilder
{
    ChunkBumpAllocator allocator;
    std::vector<SubtreeRoot>* roots;

    TrieBuilder(ChunkBumpAllocatorZone& zone, std::vector<SubtreeRoot>* roots): allocator(zone), roots(roots) {}

    Error buildSubtree(Node& parentNode, uint32_t offset, std::span<const WriterEntry> entries);
    void  addTerminalNode(Node& parentNode, const WriterEntry&);
};

struct NodeWriter
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

#endif

//
// MARK: --- GenericTrie methods ---
//

// construct from an already built trie
GenericTrie::GenericTrie(const uint8_t* start, size_t size) : _trieStart(start), _trieEnd(start+size)
#if BUILDING_MACHO_WRITER
    , _allocatorZone(ChunkBumpAllocatorZone::make())
#endif
{
#if BUILDING_MACHO_WRITER
    _trieSize = size;
#endif
}

uint32_t GenericTrie::entryCount() const
{
    if ( _trieStart == _trieEnd )
        return 0;
    __block uint32_t result = 0;
    bool             stop = false;
    char             cummulativeString[0x8000]; // FIXME: make overflow safe
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& innerStop) {
        ++result;
    });
    return result;
}

void GenericTrie::forEachEntry(void (^callback)(const Entry& entry, bool& stop)) const
{
    // ld64 will emit an empty export trie load command as a placeholder to show there are no exports.
    // In that case, don't start recursing as we'll immediately think we ran of the end of the buffer.
    if ( _trieStart == _trieEnd )
        return;
    bool stop = false;
    char cummulativeString[0x8000]; // FIXME: make overflow safe
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& innerStop) {
        Entry entry { name, nodePayload };
        callback(entry, innerStop);
    });
}

Error GenericTrie::recurseTrie(const uint8_t* p, char* cummulativeString, int curStrOffset, bool& stop,
                                  void (^callback)(const char* name, std::span<const uint8_t> nodePayload, bool& stop)) const
{
    if ( p >= _trieEnd ) {
        return Error("malformed trie, node past end");
    }
    bool           malformed;
    const uint64_t terminalSize = read_uleb128(p, _trieEnd, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    const uint8_t* children = p + terminalSize;
    if ( children > _trieEnd )
        return Error("malformed trie, terminalSize extends beyond trie data");

    if ( terminalSize != 0 ) {
        if ( callback )
            callback(cummulativeString, std::span<const uint8_t>(p, p+terminalSize), stop);
        if ( stop )
            return Error::none();
    }
    const uint8_t  childrenCount = *children++;
    const uint8_t* s             = children;
    for ( uint8_t i = 0; (i < childrenCount) && !stop; ++i ) {
        int edgeStrLen = 0;
        while ( *s != '\0' ) {
            // cummulativeString.resize(curStrOffset + edgeStrLen + 1);
            cummulativeString[curStrOffset + edgeStrLen] = *s++;
            ++edgeStrLen;
            if ( s > _trieEnd )
                return Error("malformed trie node, child node name extends beyond trie data");
        }
        // cummulativeString.resize(curStrOffset + edgeStrLen + 1);
        cummulativeString[curStrOffset + edgeStrLen] = *s++;
        uint64_t childNodeOffset                     = read_uleb128(s, _trieEnd, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        if ( childNodeOffset == 0 )
            return Error("malformed trie, childNodeOffset==0");
        if ( Error err = this->recurseTrie(_trieStart + childNodeOffset, cummulativeString, curStrOffset + edgeStrLen, stop, callback) )
            return err;
    }
    return Error::none();
}

#if 1
void GenericTrie::dump() const
{
    fprintf(stderr, "trie terminal nodes:\n");
    if ( _trieStart == _trieEnd )
        return;
    char cummulativeString[0x8000]; // FIXME: make overflow safe
    bool stop = false;
    (void)this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& trieStop) {
        fprintf(stderr, "  0x%04lX: ", (long)(nodePayload.data()-_trieStart - uleb128_size(nodePayload.size())));
        for (size_t i=0; i < nodePayload.size(); ++i)
            fprintf(stderr, "0x%02X ", nodePayload[i]);
        fprintf(stderr, "%s\n", name);
    });
}
#endif

bool GenericTrie::hasEntry(const char* name, std::span<const uint8_t>& terminalPayload) const
{
    uint32_t  visitedNodeOffsets[256]; // FIXME: overflow
    uint32_t* visitedNodeOffsetsPtr = visitedNodeOffsets;
    *visitedNodeOffsetsPtr++        = 0;
    const uint8_t* p                = _trieStart;
    while ( p < _trieEnd ) {
        bool     malformed;
        uint64_t terminalSize = *p++;
        if ( terminalSize > 127 ) {
            // except for re-export-with-rename, all terminal sizes fit in one byte
            --p;
            terminalSize = read_uleb128(p, _trieEnd, malformed);
            if ( malformed )
                return false;
        }
        if ( (*name == '\0') && (terminalSize != 0) ) {
            terminalPayload = std::span<const uint8_t>(p, (size_t)terminalSize);
            return true;
        }
        const uint8_t* children = p + terminalSize;
        if ( children > _trieEnd ) {
            // diag.error("malformed trie node, terminalSize=0x%llX extends past end of trie\n", terminalSize);
            return false;
        }
        uint8_t childrenRemaining = *children++;
        p                         = children;
        uint64_t nodeOffset       = 0;
        for ( ; childrenRemaining > 0; --childrenRemaining ) {
            const char* ss        = name;
            bool        wrongEdge = false;
            // scan whole edge to get to next edge
            // if edge is longer than target symbol name, don't read past end of symbol name
            char c = *p;
            while ( c != '\0' ) {
                if ( !wrongEdge ) {
                    if ( c != *ss )
                        wrongEdge = true;
                    ++ss;
                }
                ++p;
                c = *p;
            }
            if ( wrongEdge ) {
                // advance to next child
                ++p; // skip over zero terminator
                // skip over uleb128 until last byte is found
                while ( (*p & 0x80) != 0 )
                    ++p;
                ++p; // skip over last byte of uleb128
                if ( p > _trieEnd ) {
                    return false;
                }
            }
            else {
                // the symbol so far matches this edge (child)
                // so advance to the child's node
                ++p;
                nodeOffset = read_uleb128(p, _trieEnd, malformed);
                if ( malformed )
                    return false;
                if ( (nodeOffset == 0) || (&_trieStart[nodeOffset] > _trieEnd) )
                    return false;
                name = ss;
                break;
            }
        }
        if ( nodeOffset != 0 ) {
            if ( nodeOffset > (uint64_t)(_trieEnd - _trieStart) )
                return false;
            // check for cycles
            for ( uint32_t* nodePtr = visitedNodeOffsets; nodePtr < visitedNodeOffsetsPtr; ++nodePtr ) {
                if ( *nodePtr == nodeOffset )
                    return false;
            }
            *visitedNodeOffsetsPtr++ = (uint32_t)nodeOffset;
            p                        = &_trieStart[nodeOffset];
        }
        else {
            p = _trieEnd;
        }
    }
    return false;
}


#if BUILDING_MACHO_WRITER

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

const uint8_t* GenericTrie::bytes(size_t& size)
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

void GenericTrie::buildNodes(std::span<const WriterEntry> entries)
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

void GenericTrie::writeTrieBytes(std::span<uint8_t> bytes)
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


#endif // BUILDING_MACHO_WRITER


//
// MARK: --- ExportsTrie methods ---
//


uint32_t ExportsTrie::symbolCount() const
{
    return this->entryCount();
}

bool ExportsTrie::hasExportedSymbol(const char* symbolName, Symbol& symbol) const
{
    std::span<const uint8_t> terminalPayload;
    if ( this->hasEntry(symbolName, terminalPayload) ) {
        Entry entry = { symbolName, terminalPayload };
        if ( terminalPayloadToSymbol(entry, symbol).noError() )
            return true;
    }
    return false;
}

void ExportsTrie::forEachExportedSymbol(void (^callback)(const Symbol& symbol, bool& stop)) const
{
    this->forEachEntry(^(const Entry& entry, bool& stop) {
        Symbol symbol;
        if ( terminalPayloadToSymbol(entry, symbol).noError() )
             callback(symbol, stop);
    });
}

Error ExportsTrie::valid(uint64_t maxVmOffset) const
{
#if BUILDING_MACHO_WRITER
    if ( _buildError.hasError() )
        return Error("%s", _buildError.message());
#endif
    if ( _trieStart == _trieEnd )
        return Error::none();
    char cummulativeString[0x8000]; // FIXME: make overflow safe
    bool stop = false;
    __block Error contentErr;
    Error recurseErr = this->recurseTrie(_trieStart, cummulativeString, 0, stop, ^(const char* name, std::span<const uint8_t> nodePayload, bool& trieStop) {
        Entry   entry { name, nodePayload };
        Symbol  symbol;
        if ( Error err = terminalPayloadToSymbol(entry, symbol) ) {
            contentErr = std::move(err);
            trieStop = true;
            return;
        }
        uint64_t    absAddress;
        int         libOrdinal;
        const char* importName;
        if ( !symbol.isAbsolute(absAddress) && !symbol.isReExport(libOrdinal, importName) ) {
            uint64_t vmOffset = symbol.implOffset();
            if ( vmOffset > maxVmOffset ) {
                contentErr = Error("vmOffset too large for %s", symbol.name().c_str());
                trieStop = true;
            }
        }
    });
    if ( recurseErr )
        return recurseErr;
    return std::move(contentErr);
}

Error ExportsTrie::terminalPayloadToSymbol(const Entry& entry, Symbol& symInfo) const
{
    bool           malformed;
    const uint8_t* p          = entry.terminalPayload.data();
    const uint8_t* end        = &p[entry.terminalPayload.size()];
    uint64_t       flags      = read_uleb128(p, end, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    if ( (flags >> 6) != 0 )
        return Error("unknown exports flag bits");

    uint8_t  kind  = (flags & EXPORT_SYMBOL_FLAGS_KIND_MASK);
    uint64_t value = read_uleb128(p, end, malformed);
    if ( malformed )
        return Error("malformed uleb128");
    if ( kind == EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE )
        symInfo = Symbol::makeAbsoluteExport(entry.name.data(), value, false);
    else if ( kind == EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL )
        symInfo = Symbol::makeThreadLocalExport(entry.name.data(), value, 0, false, false, flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION);
    else if ( flags & EXPORT_SYMBOL_FLAGS_REEXPORT ) {
        const char* importName = entry.name.data();
        if ( *p != '\0' ) {
            importName = (char*)p;
            while (*p != '\0')
                ++p;
        }
        ++p;
        symInfo                = Symbol::makeReExport(entry.name.data(), (int)value, importName);
        if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
            symInfo.setWeakDef();
    }
    else if ( flags & EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER ) {
        uint64_t funcOffset = read_uleb128(p, end, malformed);
        if ( malformed )
            return Error("malformed uleb128");
        symInfo = Symbol::makeDynamicResolver(entry.name.data(), 1, value, funcOffset);
    }
    else if ( flags & EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION )
        symInfo = Symbol::makeWeakDefExport(entry.name.data(), value, 0, false, false);
    else
        symInfo = Symbol::makeRegularExport(entry.name.data(), value, 0, false, false);

    return Error::none();
}

#if BUILDING_MACHO_WRITER

// generic trie builder
struct Export { std::string_view name; uint64_t offset=0; uint64_t flags=0; uint64_t other=0; std::string_view importName; };

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


ExportsTrie::ExportsTrie(std::span<const Symbol> exports, bool writeBytes, bool needsSort)
 : GenericTrie(nullptr, 0)
{
    std::vector<WriterEntry> entries = buildWriterEntries(_allocatorZone, exports.size(), needsSort, [exports](size_t index, ChunkBumpAllocator& allocator) {
        const Symbol& sym = exports[index];
        Export        exp;
        const char*   importName   = nullptr;
        int           libOrdinal   = 0;
        bool          weakImport   = false;
        uint64_t      resolverStub = 0;
        uint64_t      absAddr      = 0;
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

#endif // BUILDING_MACHO_WRITER



//
// MARK: --- DylibsPathTrie methods ---
//


#if BUILDING_MACHO_WRITER
DylibsPathTrie::DylibsPathTrie(std::span<const DylibAndIndex> dylibs, bool needsSort)
: GenericTrie(nullptr, 0)
{
    std::vector<WriterEntry> entries = buildWriterEntries(_allocatorZone, dylibs.size(), /* needsSort */ needsSort, [dylibs](size_t index, ChunkBumpAllocator& allocator) {
        const DylibAndIndex& info = dylibs[index];
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
#endif

bool DylibsPathTrie::entryToIndex(std::span<const uint8_t> payload, uint32_t& dylibIndex) const
{
    const uint8_t* p = payload.data();
    const uint8_t* end = &p[payload.size()];
    bool malformed;
    uint64_t value = read_uleb128(p, end, malformed);
    if ( !malformed ) {
        dylibIndex = (uint32_t)value;
        return true;
    }
    return false;
}

bool DylibsPathTrie::hasPath(const char* path, uint32_t& dylibIndex) const
{
    std::span<const uint8_t> terminalPayload;
    if ( this->hasEntry(path, terminalPayload) ) {
        if ( entryToIndex(terminalPayload, dylibIndex) )
            return true;
    }
    return false;
}

void DylibsPathTrie::forEachDylibPath(void (^callback)(const DylibAndIndex& info, bool& stop)) const
{
    this->forEachEntry(^(const Entry& entry, bool& stop) {
        DylibAndIndex info;
        info.path  = entry.name;
        if ( entryToIndex(entry.terminalPayload, info.index) )
            callback(info, stop);
    });
}


} // namespace mach_o
