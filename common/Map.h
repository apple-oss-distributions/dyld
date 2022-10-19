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

#ifndef Map_h
#define Map_h

#include <string_view>

#include "Array.h"
#include "BumpAllocator.h"

namespace dyld3 {



template<typename T>
struct Hash {
    static size_t hash(const T&);
};

template<typename T>
struct Equal {
    static bool equal(const T&a, const T& b);
};


template<typename KeyT, typename ValueT, class GetHash = Hash<KeyT>, class IsEqual = Equal<KeyT>>
class Map {
public:

    // Use our own struct for the NodeT, as std::pair doesn't have the copyable/trivially_construcible traits we need
    struct NodeT {
        KeyT    first;
        ValueT  second;
    };

private:
    typedef NodeT*                      iterator;
    typedef const NodeT*                const_iterator;

    enum : size_t {
        SentinelHash = (size_t)-1
    };

public:
    Map() {
        // Keep the hash buffer about 75% full
        nextHashBufferGrowth = 768;
        hashBufferUseCount = 0;
        hashBuffer.reserve(1024);
        for (size_t i = 0; i != 1024; ++i) {
            hashBuffer.push_back(SentinelHash);
        }
        nodeBuffer.reserve(1024);
    }


    iterator find(const KeyT& key) {
        // Find the index to look up in the hash buffer
        size_t hashIndex = GetHash::hash(key) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        size_t probeAmount = 1;
        while (true) {
            size_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end();
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, key)) {
                // Keys match so we found this element
                return &nodeBuffer[nodeBufferIndex];
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }

    const_iterator find(const KeyT& key) const {
        // Find the index to look up in the hash buffer
        size_t hashIndex = GetHash::hash(key) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        size_t probeAmount = 1;
        while (true) {
            size_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end();
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, key)) {
                // Keys match so we found this element
                return &nodeBuffer[nodeBufferIndex];
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }

    iterator begin() {
        return nodeBuffer.begin();
    }

    iterator end() {
        return nodeBuffer.end();
    }

    const_iterator begin() const {
        return nodeBuffer.begin();
    }

    const_iterator end() const {
        return nodeBuffer.end();
    }

    const Array<NodeT>& array() const {
        return nodeBuffer;
    }

    void reserve(size_t size) {
        nodeBuffer.reserve(size);
    }

    bool contains(const KeyT& key) const {
        return find(key) != end();
    }

    bool empty() const {
        return nodeBuffer.empty();
    }

    size_t size() const {
        return nodeBuffer.count();
    }

    std::pair<iterator, bool> insert(NodeT&& v) {
        // First see if we have enough space.  We don't want the hash buffer to get too full.
        if (hashBufferUseCount == nextHashBufferGrowth) {
            // Grow and rehash everything.
            size_t newHashTableSize = hashBuffer.count() * 2;
            nextHashBufferGrowth *= 2;

            dyld3::OverflowSafeArray<size_t> newHashBuffer;
            newHashBuffer.reserve(newHashTableSize);
            for (size_t i = 0; i != newHashTableSize; ++i) {
                newHashBuffer.push_back(SentinelHash);
            }

            // Walk the existing nodes trying to populate the new hash buffer and looking for collisions
            for (size_t i = 0; i != nodeBuffer.count(); ++i) {
                const KeyT& key = nodeBuffer[i].first;
                size_t newHashIndex = GetHash::hash(key) & (newHashBuffer.count() - 1);

                // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
                size_t probeAmount = 1;
                while (true) {
                    size_t newNodeBufferIndex = newHashBuffer[newHashIndex];

                    if (newNodeBufferIndex == SentinelHash) {
                        // This node is unused, so we don't have this element.  Lets add it
                        newHashBuffer[newHashIndex] = i;
                        break;
                    }

                    // Don't bother checking for matching keys here.  We know we are adding elements with different keys
                    // Just probe to find the next sentinel

                    // We didn't find this node, so try with a later one
                    newHashIndex += probeAmount;
                    newHashIndex &= (newHashBuffer.count() - 1);
                    ++probeAmount;
                }
            }

            // Use the new buffer
            hashBuffer = std::move(newHashBuffer);
        }

        // Find the index to look up in the hash buffer
        size_t hashIndex = GetHash::hash(v.first) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        size_t probeAmount = 1;
        while (true) {
            size_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element.  Lets add it
                hashBuffer[hashIndex] = nodeBuffer.count();
                ++hashBufferUseCount;
                nodeBuffer.push_back(std::move(v));
                return { &nodeBuffer.back(), true };
            }

            // If that hash is in use, then check if that node is actually the one we are trying to insert
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, v.first)) {
                // Keys match.  We already have this element
                return { &nodeBuffer[nodeBufferIndex], false };
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }


    ValueT& operator[](KeyT idx) {
        auto itAndInserted = insert({ idx, ValueT() });
        return itAndInserted.first->second;
    }

private:
    size_t                              nextHashBufferGrowth;
    size_t                              hashBufferUseCount;
    dyld3::OverflowSafeArray<size_t>    hashBuffer;
    dyld3::OverflowSafeArray<NodeT>     nodeBuffer;
};


template<typename T>
struct HashMulti {
    static uint64_t hash(const T&, void* state);
};

template<typename T>
struct EqualMulti {
    static bool equal(const T&a, const T& b, void* state);
};


template<typename KeyT, typename ValueT, class GetHash = HashMulti<KeyT>, class IsEqual = EqualMulti<KeyT>>
class MultiMap {
    
    struct NextNode {
        size_t isDuplicateHead  : 1;
        size_t isDuplicateEntry : 1;
        size_t isDuplicateTail  : 1;
        size_t nextIndex        : 29;
        
        bool hasAnyDuplicates() const {
            return isDuplicateHead || isDuplicateEntry || isDuplicateTail;
        }
        
        bool hasMoreDuplicates() const {
            return isDuplicateHead || isDuplicateEntry;
        }
        
        static NextNode makeNoDuplicates() {
            return { 0, 0, 0, 0 };
        }
        
        static NextNode makeDuplicateTailNode() {
            return { 0, 0, 1, 0 };
        }
    };
    static_assert(sizeof(NextNode) == sizeof(size_t), "Invalid size");

    // Use our own struct for the NodeT/NodeEntryT, as std::pair doesn't have the copyable/trivially_construcible traits we need
    struct NodeT {
        KeyT    first;
        ValueT  second;
    };
    struct NodeEntryT {
        KeyT        key;
        ValueT      value;
        NextNode    next;
    };
    typedef NodeEntryT*                              iterator;
    typedef const NodeEntryT*                        const_iterator;

    enum : size_t {
        SentinelHash        = (size_t)-1
    };

public:
    MultiMap(void* externalState) {
        // Keep the hash buffer about 75% full
        nextHashBufferGrowth = 768;
        hashBufferUseCount = 0;
        hashBuffer.reserve(1024);
        for (size_t i = 0; i != 1024; ++i) {
            hashBuffer.push_back(SentinelHash);
        }
        nodeBuffer.reserve(1024);
        state = externalState;
    }

    MultiMap(void* externalState, const uint64_t* data) {
        uint64_t* p = (uint64_t*)data;
        nextHashBufferGrowth = *p++;
        hashBufferUseCount = *p++;

        uint64_t hashBufferCount = *p++;
        hashBuffer.setInitialStorage(p, (uintptr_t)hashBufferCount);
        hashBuffer.resize((uintptr_t)hashBufferCount);
        p += hashBufferCount;

        uint64_t nodeBufferCount = *p++;
        NodeEntryT* nodes = (NodeEntryT*)p;
        nodeBuffer.setInitialStorage(nodes, (uintptr_t)nodeBufferCount);
        nodeBuffer.resize((uintptr_t)nodeBufferCount);

        state = externalState;
    }

    const Array<NodeEntryT>& array() const {
        return nodeBuffer;
    }

    void forEachEntry(void (^handler)(const KeyT& key, const ValueT** values, uint64_t valuesCount)) const {
        // Walk the top level nodes, skipping dupes
        for (const NodeEntryT& headNode : nodeBuffer) {
            NextNode nextNode = headNode.next;
            if (!nextNode.hasAnyDuplicates()) {
                const ValueT* value[1] = { &headNode.value };
                handler(headNode.key, value, 1);
                continue;
            }
            
            if (!nextNode.isDuplicateHead)
                continue;
            
            // This is the head of a list.  Work out how long the list is
            uint64_t valuesCount = 1;
            while (nodeBuffer[nextNode.nextIndex].next.hasMoreDuplicates()) {
                nextNode = nodeBuffer[nextNode.nextIndex].next;
                ++valuesCount;
            }
            
            // Add one more for the last node
            ++valuesCount;
            
            // Now make an array with that many value for the callback
            const ValueT* values[valuesCount];
            // Copy in the head
            values[0] = &(headNode.value);
            
            // And copy the remainder
            nextNode = headNode.next;
            valuesCount = 1;
            while (nodeBuffer[nextNode.nextIndex].next.hasMoreDuplicates()) {
                values[(size_t)valuesCount] = &(nodeBuffer[nextNode.nextIndex].value);
                nextNode = nodeBuffer[nextNode.nextIndex].next;
                ++valuesCount;
            }
            
            // Add in the last node
            values[(size_t)valuesCount] = &(nodeBuffer[nextNode.nextIndex].value);
            ++valuesCount;
            
            // Finally call the handler with a whole array of values.
            handler(headNode.key, values, valuesCount);
        }
    }
    void forEachEntry(const KeyT& key, void (^handler)(const ValueT* values[], uint32_t valuesCount)) const {
            // Find the index to look up in the hash buffer
            uint64_t hashIndex = GetHash::hash(key, state) & (hashBuffer.count() - 1);

            // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
            size_t probeAmount = 1;
            while (true) {
                uint64_t nodeBufferIndex = hashBuffer[(size_t)hashIndex];

                if (nodeBufferIndex == SentinelHash) {
                    // This node is unused, so we don't have this element
                    return;
                }

                // If that hash is in use, then check if that node is actually the one we are trying to find
                if (IsEqual::equal(nodeBuffer[(size_t)nodeBufferIndex].key, key, state)) {
                    // Keys match so we found this element
                    const NodeEntryT& headNode = nodeBuffer[(size_t)nodeBufferIndex];

                    NextNode nextNode = headNode.next;
                    if (!nextNode.hasAnyDuplicates()) {
                        const ValueT* value[1] = { &headNode.value };
                        handler(value, 1);
                        return;
                    }

                    // This is the head of a list.  Work out how long the list is
                    uint32_t valuesCount = 1;
                    while (nodeBuffer[nextNode.nextIndex].next.hasMoreDuplicates()) {
                        nextNode = nodeBuffer[nextNode.nextIndex].next;
                        ++valuesCount;
                    }

                    // Add one more for the last node
                    ++valuesCount;

                    // Now make an array with that many value for the callback
                    const ValueT* values[valuesCount];
                    // Copy in the head
                    values[0] = &headNode.value;

                    // And copy the remainder
                    nextNode = headNode.next;
                    valuesCount = 1;
                    while (nodeBuffer[nextNode.nextIndex].next.hasMoreDuplicates()) {
                        values[valuesCount] = &nodeBuffer[nextNode.nextIndex].value;
                        nextNode = nodeBuffer[nextNode.nextIndex].next;
                        ++valuesCount;
                    }

                    // Add in the last node
                    values[valuesCount] = &nodeBuffer[nextNode.nextIndex].value;
                    ++valuesCount;

                    // Finally call the handler with a whole array of values.
                    handler(values, valuesCount);
                    return;
                }

                // We didn't find this node, so try with a later one
                hashIndex += probeAmount;
                hashIndex &= (hashBuffer.count() - 1);
                ++probeAmount;
            }

            assert(0 && "unreachable");
        }


    iterator end() {
        return nodeBuffer.end();
    }

    iterator nextDuplicate(iterator node) {
        NextNode nextNode = node->next;

        if ( !nextNode.hasMoreDuplicates() )
            return end();

        return &nodeBuffer[nextNode.nextIndex];
    }

    iterator find(const KeyT& key) {
        // Find the index to look up in the hash buffer
        uint64_t hashIndex = GetHash::hash(key, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        size_t probeAmount = 1;
        while (true) {
            uint64_t nodeBufferIndex = hashBuffer[(size_t)hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end();
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[(size_t)nodeBufferIndex].key, key, state)) {
                // Keys match so we found this element
                return &nodeBuffer[(size_t)nodeBufferIndex];
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }

    void insert(NodeT&& v) {
        // First see if we have enough space.  We don't want the hash buffer to get too full.
        if (hashBufferUseCount == nextHashBufferGrowth) {
            // Grow and rehash everything.
            size_t newHashTableSize = hashBuffer.count() * 2;
            nextHashBufferGrowth *= 2;

            dyld3::OverflowSafeArray<uint64_t> newHashBuffer;
            newHashBuffer.reserve(newHashTableSize);
            for (size_t i = 0; i != newHashTableSize; ++i) {
                newHashBuffer.push_back(SentinelHash);
            }

            // Walk the existing nodes trying to populate the new hash buffer and looking for collisions
            for (size_t i = 0; i != nodeBuffer.count(); ++i) {
                // Skip nodes which are not the head of the list
                // They aren't moving the buffer anyway
                NextNode nextNode = nodeBuffer[i].next;
                if (nextNode.isDuplicateEntry || nextNode.isDuplicateTail)
                    continue;
                const KeyT& key = nodeBuffer[i].key;
                uint64_t newHashIndex = GetHash::hash(key, state) & (newHashBuffer.count() - 1);

                // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
                size_t probeAmount = 1;
                while (true) {
                    uint64_t newNodeBufferIndex = newHashBuffer[(size_t)newHashIndex];

                    if (newNodeBufferIndex == SentinelHash) {
                        // This node is unused, so we don't have this element.  Lets add it
                        newHashBuffer[(size_t)newHashIndex] = i;
                        break;
                    }

                    // Don't bother checking for matching keys here.  We know we are adding elements with different keys
                    // Just probe to find the next sentinel

                    // We didn't find this node, so try with a later one
                    newHashIndex += probeAmount;
                    newHashIndex &= (newHashBuffer.count() - 1);
                    ++probeAmount;
                }
            }

            // Use the new buffer
            hashBuffer = std::move(newHashBuffer);
        }

        // Find the index to look up in the hash buffer
        uint64_t hashIndex = GetHash::hash(v.first, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        size_t probeAmount = 1;
        while (true) {
            uint64_t nodeBufferIndex = hashBuffer[(size_t)hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element.  Lets add it
                hashBuffer[(size_t)hashIndex] = nodeBuffer.count();
                ++hashBufferUseCount;
                nodeBuffer.push_back({ v.first, v.second, NextNode::makeNoDuplicates() } );
                return;
            }

            // If that hash is in use, then check if that node is actually the one we are trying to insert
            if (IsEqual::equal(nodeBuffer[(size_t)nodeBufferIndex].key, v.first, state)) {
                // Keys match.  We already have this element
                // But this is a multimap so add the new element too
                // Walk from this node to find the end of the chain
                while (nodeBuffer[(size_t)nodeBufferIndex].next.hasMoreDuplicates()) {
                    nodeBufferIndex = nodeBuffer[(size_t)nodeBufferIndex].next.nextIndex;
                }
                NextNode& tailNode = nodeBuffer[(size_t)nodeBufferIndex].next;
                if (!tailNode.hasAnyDuplicates()) {
                    // If the previous node has no duplicates then its now the new head of a list
                    tailNode.isDuplicateHead = 1;
                    tailNode.nextIndex = nodeBuffer.count();
                } else {
                    // This must be a tail node.  Update it to be an entry node
                    assert(tailNode.isDuplicateTail);
                    tailNode.isDuplicateTail = 0;
                    tailNode.isDuplicateEntry = 1;
                    tailNode.nextIndex = nodeBuffer.count();
                }
                //.nextIndex = nodeBuffer.count();
                nodeBuffer.push_back({ v.first, v.second, NextNode::makeDuplicateTailNode() } );
                return;
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }

    void serialize(dyld4::BumpAllocator& allocator) {
        allocator.append(&nextHashBufferGrowth, sizeof(nextHashBufferGrowth));
        allocator.append(&hashBufferUseCount, sizeof(hashBufferUseCount));

        uint64_t count = hashBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(hashBuffer.begin(), (size_t)count * sizeof(uint64_t));

        count = nodeBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(nodeBuffer.begin(), (size_t)count * sizeof(NodeEntryT));
    }

private:
    uint64_t                                nextHashBufferGrowth;
    uint64_t                                hashBufferUseCount;
    dyld3::OverflowSafeArray<uint64_t>      hashBuffer;
    dyld3::OverflowSafeArray<NodeEntryT>    nodeBuffer;
    void*                                   state;
};


struct HashCString {
    static size_t hash(const char* v) {
        return std::hash<std::string_view>()(v);
    }
};

struct HashCStringMulti {
    static uint64_t hash(const char* v, const void* state) {
        return std::hash<std::string_view>()(v);
    }
};

struct EqualCString {
    static bool equal(const char* s1, const char* s2) {
        return strcmp(s1, s2) == 0;
    }
};

struct EqualCStringMulti {
    static bool equal(const char* s1, const char* s2, const void* state) {
        return strcmp(s1, s2) == 0;
    }
};

// CStringMapTo<T> is a Map from a c-string to a T
template <typename ValueT>
using CStringMapTo = Map<const char*, ValueT, HashCString, EqualCString>;

// CStringMultiMapTo<T> is a MultiMap from a c-string to a set of T
template <typename ValueT>
using CStringMultiMapTo = MultiMap<const char*, ValueT, HashCStringMulti, EqualCStringMulti>;

} // namespace dyld3

#endif /* Map_h */
