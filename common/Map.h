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
    static uint64_t hash(const T&, void* state);
};

template<typename T>
struct Equal {
    static bool equal(const T&a, const T& b, void* state);
};

// A base class of Map which provides helper methods to read from the map
// A concrete subclass needs to exist to call this
template<typename KeyT, typename ValueT, class GetHash = Hash<KeyT>, class IsEqual = Equal<KeyT>>
class MapBase {
public:

    // Use our own struct for the NodeT, as std::pair doesn't have the copyable/trivially_construcible traits we need
    template<bool isVoid>
    struct NodeImplT {
        KeyT    first;
        ValueT  second;
    };

    // HACK: If you map to void, then you get a set, with no "second" value
    template<>
    struct NodeImplT<true> {
        KeyT    first;
    };

    typedef NodeImplT<std::is_void_v<ValueT>> NodeT;

    typedef NodeT*                      iterator;
    typedef const NodeT*                const_iterator;

    enum : uint32_t {
        SentinelHash = (uint32_t)-1
    };

    typedef KeyT KeyType;
    typedef ValueT ValueType;

protected:
    void forEachEntry(const dyld3::Array<uint32_t>& hashBuffer,
                      const dyld3::Array<NodeT>& nodeBuffer,
                      void (^handler)(const NodeT& node)) const {
        for ( const NodeT& node : nodeBuffer ) {
            handler(node);
        }
    }

    iterator begin(dyld3::Array<NodeT>& nodeBuffer) {
        return nodeBuffer.begin();
    }

    iterator end(dyld3::Array<NodeT>& nodeBuffer) {
        return nodeBuffer.end();
    }

    const_iterator begin(const dyld3::Array<NodeT>& nodeBuffer) const {
        return nodeBuffer.begin();
    }

    const_iterator end(const dyld3::Array<NodeT>& nodeBuffer) const {
        return nodeBuffer.end();
    }

    template<typename LookupKeyT>
    iterator find(const dyld3::Array<uint32_t>& hashBuffer,
                  dyld3::Array<NodeT>& nodeBuffer,
                  void* state,
                  const KeyT& key) {

        if ( nodeBuffer.empty() )
            return end(nodeBuffer);

        // Find the index to look up in the hash buffer
        uint64_t hashIndex = GetHash::hash(key, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        uint64_t probeAmount = 1;
        while (true) {
            uint32_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end(nodeBuffer);
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, key, state)) {
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

    template<typename LookupKeyT>
    const_iterator const_find(const dyld3::Array<uint32_t>& hashBuffer,
                              const dyld3::Array<NodeT>& nodeBuffer,
                              void* state,
                              const LookupKeyT& key) const {

        if ( nodeBuffer.empty() )
            return end(nodeBuffer);

        // Find the index to look up in the hash buffer
        uint64_t hashIndex = GetHash::hash(key, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        uint64_t probeAmount = 1;
        while (true) {
            uint32_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end(nodeBuffer);
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, key, state)) {
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
};

template<typename KeyT, typename ValueT, class GetHash = Hash<KeyT>, class IsEqual = Equal<KeyT>>
class Map : public MapBase<KeyT, ValueT, GetHash, IsEqual>
{
    typedef MapBase<KeyT, ValueT, GetHash, IsEqual> BaseMapTy;

    using BaseMapTy::SentinelHash;

public:
    typedef typename BaseMapTy::NodeT NodeT;
    typedef typename BaseMapTy::iterator iterator;
    typedef typename BaseMapTy::const_iterator const_iterator;

    Map() {
        // Keep the hash buffer about 75% full
        nextHashBufferGrowth = 24;
        hashBufferUseCount = 0;
        hashBuffer.reserve(32);
        for (uint64_t i = 0; i != 32; ++i) {
            hashBuffer.push_back(SentinelHash);
        }
        nodeBuffer.reserve(32);
    }

    template<typename LookupKeyT>
    iterator find(const LookupKeyT& key) {
        return BaseMapTy::template find<LookupKeyT>(this->hashBuffer, this->nodeBuffer, nullptr, key);
    }

    template<typename LookupKeyT>
    const_iterator find(const LookupKeyT& key) const {
        return BaseMapTy::template const_find<LookupKeyT>(this->hashBuffer, this->nodeBuffer, nullptr, key);
    }

    iterator begin() {
        return BaseMapTy::begin(this->nodeBuffer);
    }

    iterator end() {
        return BaseMapTy::end(this->nodeBuffer);
    }

    const_iterator begin() const {
        return BaseMapTy::begin(this->nodeBuffer);
    }

    const_iterator end() const {
        return BaseMapTy::end(this->nodeBuffer);
    }

    const Array<NodeT>& array() const {
        return nodeBuffer;
    }

    void reserve(uint64_t size) {
        nodeBuffer.reserve(size);
    }

    bool contains(const KeyT& key) {
        return find(key) != end();
    }

    bool empty() const {
        return nodeBuffer.empty();
    }

    uint64_t size() const {
        return nodeBuffer.count();
    }

    std::pair<iterator, bool> insert(NodeT&& v) {
        // State is only used for constant maps where we look up elements
        // We don't need it here for inserting in to the map
        void* state = nullptr;

        // First see if we have enough space.  We don't want the hash buffer to get too full.
        if (hashBufferUseCount == nextHashBufferGrowth) {
            // Grow and rehash everything.
            uint64_t newHashTableSize = hashBuffer.count() * 2;
            nextHashBufferGrowth *= 2;

            dyld3::OverflowSafeArray<uint32_t> newHashBuffer;
            newHashBuffer.reserve(newHashTableSize);
            for (uint64_t i = 0; i != newHashTableSize; ++i) {
                newHashBuffer.push_back(SentinelHash);
            }

            // Walk the existing nodes trying to populate the new hash buffer and looking for collisions
            for (uint64_t i = 0; i != nodeBuffer.count(); ++i) {
                const KeyT& key = nodeBuffer[i].first;
                uint64_t newHashIndex = GetHash::hash(key, state) & (newHashBuffer.count() - 1);

                // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
                uint64_t probeAmount = 1;
                while (true) {
                    uint32_t newNodeBufferIndex = newHashBuffer[newHashIndex];

                    if (newNodeBufferIndex == SentinelHash) {
                        // This node is unused, so we don't have this element.  Lets add it
                        newHashBuffer[newHashIndex] = (uint32_t)i;
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
        uint64_t probeAmount = 1;
        while (true) {
            uint32_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element.  Lets add it
                hashBuffer[hashIndex] = (uint32_t)nodeBuffer.count();
                ++hashBufferUseCount;
                nodeBuffer.push_back(std::move(v));
                return { &nodeBuffer.back(), true };
            }

            // If that hash is in use, then check if that node is actually the one we are trying to insert
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].first, v.first, state)) {
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

    // Serializes this in to a read only map which can be viewed with a MapView
    template<typename TargetNodeKeyT, typename TargetNodeValueT>
    void serialize(dyld4::BumpAllocator& allocator,
                   TargetNodeKeyT (^keyFunc)(const KeyT& key, const ValueT& value),
                   TargetNodeValueT (^valueFunc)(const KeyT& key, const ValueT& value)) const {

        uint64_t count = hashBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(hashBuffer.begin(), (size_t)count * sizeof(uint32_t));

        count = nodeBuffer.count();
        allocator.append(&count, sizeof(count));

        for ( const NodeT& currentNode : nodeBuffer ) {
            typedef MapBase<TargetNodeKeyT, TargetNodeValueT> TargetMapTy;
            typedef typename TargetMapTy::NodeT NewNodeT;

            // HACK: For now the only client of this is the selector hash table, which is really a set not a map
            static_assert(std::is_void_v<TargetNodeValueT>);
            NewNodeT newNode = {
                .first  = keyFunc(currentNode.first, currentNode.second),
                //.second = valueFunc(currentNode.first, currentNode.second),
            };
            allocator.append(&newNode, sizeof(NewNodeT));
        }
    }

private:
    uint64_t                            nextHashBufferGrowth;
    uint64_t                            hashBufferUseCount;
    dyld3::OverflowSafeArray<uint32_t>  hashBuffer;
    dyld3::OverflowSafeArray<NodeT>     nodeBuffer;
};

template<typename KeyT, typename ValueT, class GetHash = Hash<KeyT>, class IsEqual = Equal<KeyT>>
class MapView : public MapBase<KeyT, ValueT, GetHash, IsEqual>
{
    typedef MapBase<KeyT, ValueT, GetHash, IsEqual> BaseMapTy;

    using BaseMapTy::SentinelHash;

public:
    typedef typename BaseMapTy::NodeT NodeT;
    typedef typename BaseMapTy::iterator iterator;
    typedef typename BaseMapTy::const_iterator const_iterator;

    MapView() = default;
    MapView(const void* serializedMap)
    {
        uint64_t hashArrayCount = this->hashBufferCount(serializedMap);
        this->hashBufferArray = Array<uint32_t>(this->hashBuffer(serializedMap), hashArrayCount, hashArrayCount);

        uint64_t nodeArrayCount = this->nodeBufferCount(serializedMap);
        this->nodeBufferArray = Array<NodeT>(this->nodeBuffer(serializedMap), nodeArrayCount, nodeArrayCount);
    }

    ~MapView() = default;

    MapView(const MapView&) = delete;
    MapView& operator=(const MapView&) = delete;

    MapView(MapView&&) = default;
    MapView& operator=(MapView&&) = default;

    void forEachEntry(void (^handler)(const NodeT& node)) const {
        BaseMapTy::forEachEntry(this->hashBufferArray, this->nodeBufferArray, handler);
    }

    template<typename LookupKeyT>
    iterator find(void* state, const LookupKeyT& key) {
        return BaseMapTy::template find<LookupKeyT>(this->hashBufferArray, this->nodeBufferArray, state, key);
    }

    template<typename LookupKeyT>
    const_iterator find(void* state, const LookupKeyT& key) const {
        return BaseMapTy::template const_find<LookupKeyT>(this->hashBufferArray, this->nodeBufferArray, state, key);
    }

    iterator begin() {
        return BaseMapTy::begin(this->nodeBufferArray);
    }

    iterator end() {
        return BaseMapTy::end(this->nodeBufferArray);
    }

    const_iterator begin() const {
        return BaseMapTy::begin(this->nodeBufferArray);
    }

    const_iterator end() const {
        return BaseMapTy::end(this->nodeBufferArray);
    }

private:
    // These arrays point in to the serializedMap we got in the constructor
    Array<uint32_t> hashBufferArray;
    Array<NodeT>    nodeBufferArray;

    // The serialized map itself looks like:
    // uint64_t     hashBufferCount;
    // uint32_t     hashBuffer[hashBufferCount];
    // uint64_t     nodeBufferCount;
    // NodeT        nodeBuffer[nodeBufferCount];

    // -------------
    // Hash Buffer methods
    // -------------
    static uint64_t hashBufferCountOffset(const void* map) {
        return 0;
    }

    static uint64_t hashBufferOffset(const void* serializedMap) {
        return hashBufferCountOffset(serializedMap) + sizeof(uint64_t);
    }

    static uint64_t hashBufferCount(const void* serializedMap) {
        return *(uint64_t*)((uint8_t*)serializedMap + hashBufferCountOffset(serializedMap));
    }

    static uint32_t* hashBuffer(const void* serializedMap) {
        return (uint32_t*)((uint8_t*)serializedMap + hashBufferOffset(serializedMap));
    }

    // -------------
    // Node Buffer methods
    // -------------

    static uint64_t nodeBufferCountOffset(const void* serializedMap) {
        return hashBufferOffset(serializedMap) + (hashBufferCount(serializedMap) * sizeof(uint32_t));
    }

    static uint64_t nodeBufferOffset(const void* serializedMap) {
        return nodeBufferCountOffset(serializedMap) + sizeof(uint64_t);
    }

    static uint64_t nodeBufferCount(const void* serializedMap) {
        return *(uint64_t*)((uint8_t*)serializedMap + nodeBufferCountOffset(serializedMap));
    }

    static NodeT* nodeBuffer(const void* serializedMap) {
        return (NodeT*)((uint8_t*)serializedMap + nodeBufferOffset(serializedMap));
    }
};


template<typename T>
struct HashMulti {
    static uint64_t hash(const T&, void* state);
};

template<typename T>
struct EqualMulti {
    static bool equal(const T&a, const T& b, void* state);
};

namespace multimap_impl
{

struct NextNode {
    uint64_t isDuplicateHead  : 1;
    uint64_t isDuplicateEntry : 1;
    uint64_t isDuplicateTail  : 1;
    uint64_t nextIndex        : 61;

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
static_assert(sizeof(NextNode) == sizeof(uint64_t), "Invalid size");

} // multimap_impl


// A base class of MultiMap which provides helper methods to read from the map
// A concrete subclass needs to exist to call this
template<typename KeyT, typename ValueT, class GetHash = HashMulti<KeyT>, class IsEqual = EqualMulti<KeyT>>
class MultiMapBase {
protected:
    typedef multimap_impl::NextNode NextNode;

public:
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

    enum : uint64_t {
        SentinelHash        = (uint64_t)-1
    };

    typedef KeyT KeyType;
    typedef ValueT ValueType;

protected:
    void forEachEntry(const dyld3::Array<uint64_t>& hashBuffer,
                      const dyld3::Array<NodeEntryT>& nodeBuffer,
                      void (^handler)(const KeyT& key, const Array<const ValueT*>& values)) const
    {
        if ( nodeBuffer.empty() )
            return;

        // Walk the top level nodes, skipping dupes
        for (const NodeEntryT& headNode : nodeBuffer) {
            NextNode nextNode = headNode.next;
            if (!nextNode.hasAnyDuplicates()) {
                const ValueT* value[1] = { &headNode.value };
                handler(headNode.key, Array<const ValueT*>(value, 1, 1));
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
            handler(headNode.key, Array<const ValueT*>(values, valuesCount, valuesCount));
        }
    }

    template<typename LookupKeyT>
    void forEachEntry(const dyld3::Array<uint64_t>& hashBuffer,
                      const dyld3::Array<NodeEntryT>& nodeBuffer,
                      void* state,
                      const LookupKeyT& key,
                      void (^handler)(const Array<const ValueT*>& values)) const
    {
        if ( nodeBuffer.empty() )
            return;

        // Find the index to look up in the hash buffer
        uint64_t hashIndex = GetHash::hash(key, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        uint64_t probeAmount = 1;
        while (true) {
            uint64_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return;
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].key, key, state)) {
                // Keys match so we found this element
                const NodeEntryT& headNode = nodeBuffer[nodeBufferIndex];

                NextNode nextNode = headNode.next;
                if (!nextNode.hasAnyDuplicates()) {
                    const ValueT* value[1] = { &headNode.value };
                    handler(Array<const ValueT*>(value, 1, 1));
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
                handler(Array<const ValueT*>(values, valuesCount, valuesCount));
                return;
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }
};


template<typename KeyT, typename ValueT, class GetHash = HashMulti<KeyT>, class IsEqual = EqualMulti<KeyT>>
class MultiMap : public MultiMapBase<KeyT, ValueT, GetHash, IsEqual> {
    typedef MultiMapBase<KeyT, ValueT, GetHash, IsEqual> BaseMapTy;

    using BaseMapTy::SentinelHash;

public:
    typedef typename BaseMapTy::NextNode NextNode;
    typedef typename BaseMapTy::NodeT NodeT;
    typedef typename BaseMapTy::NodeEntryT NodeEntryT;
    typedef typename BaseMapTy::iterator iterator;
    typedef typename BaseMapTy::const_iterator const_iterator;

    MultiMap(void* externalState) {
        // Keep the hash buffer about 75% full
        nextHashBufferGrowth = 24;
        hashBufferUseCount = 0;
        hashBuffer.reserve(32);
        for (uint32_t i = 0; i != 32; ++i) {
            hashBuffer.push_back(SentinelHash);
        }
        nodeBuffer.reserve(32);
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

    void forEachKey(void (^handler)(KeyT& key)) {
        // Walk the top level nodes, skipping dupes
        for (NodeEntryT& headNode : nodeBuffer) {
            handler(headNode.key);
        }
    }

    void forEachEntry(void (^handler)(const KeyT& key, const Array<const ValueT*>& values)) const {
        BaseMapTy::forEachEntry(this->hashBuffer, this->nodeBuffer, handler);
    }

    void forEachEntry(const KeyT& key, void (^handler)(const Array<const ValueT*>& values)) const {
        BaseMapTy::forEachEntry(this->hashBuffer, this->nodeBuffer, this->state, key, handler);
    }

    iterator end() {
        return nodeBuffer.end();
    }

    bool empty() const {
        return nodeBuffer.empty();
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
        uint64_t probeAmount = 1;
        while (true) {
            uint64_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element
                return end();
            }

            // If that hash is in use, then check if that node is actually the one we are trying to find
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].key, key, state)) {
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

    // Returns true if the node was not in the map before, ie, we added it
    iterator insert(NodeT&& v, bool& alreadyHaveNodeWithKey) {
        // First see if we have enough space.  We don't want the hash buffer to get too full.
        if (hashBufferUseCount == nextHashBufferGrowth) {
            // Grow and rehash everything.
            uint64_t newHashTableSize = hashBuffer.count() * 2;
            nextHashBufferGrowth *= 2;

            dyld3::OverflowSafeArray<uint64_t> newHashBuffer;
            newHashBuffer.reserve(newHashTableSize);
            for (uint64_t i = 0; i != newHashTableSize; ++i) {
                newHashBuffer.push_back(SentinelHash);
            }

            // Walk the existing nodes trying to populate the new hash buffer and looking for collisions
            for (uint64_t i = 0; i != nodeBuffer.count(); ++i) {
                // Skip nodes which are not the head of the list
                // They aren't moving the buffer anyway
                NextNode nextNode = nodeBuffer[i].next;
                if (nextNode.isDuplicateEntry || nextNode.isDuplicateTail)
                    continue;
                const KeyT& key = nodeBuffer[i].key;
                uint64_t newHashIndex = GetHash::hash(key, state) & (newHashBuffer.count() - 1);

                // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
                uint64_t probeAmount = 1;
                while (true) {
                    uint64_t newNodeBufferIndex = newHashBuffer[newHashIndex];

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
        uint64_t hashIndex = GetHash::hash(v.first, state) & (hashBuffer.count() - 1);

        // Note we'll use a quadratic probe to look past identical hashes until we find our node or a sentinel
        uint64_t probeAmount = 1;
        while (true) {
            uint64_t nodeBufferIndex = hashBuffer[hashIndex];

            if (nodeBufferIndex == SentinelHash) {
                // This node is unused, so we don't have this element.  Lets add it
                hashBuffer[hashIndex] = nodeBuffer.count();
                ++hashBufferUseCount;
                nodeBuffer.push_back({ v.first, v.second, NextNode::makeNoDuplicates() } );
                alreadyHaveNodeWithKey = false;
                return &nodeBuffer.back();
            }

            // If that hash is in use, then check if that node is actually the one we are trying to insert
            if (IsEqual::equal(nodeBuffer[nodeBufferIndex].key, v.first, state)) {
                // Keys match.  We already have this element
                // But this is a multimap so add the new element too
                // Walk from this node to find the end of the chain
                while (nodeBuffer[nodeBufferIndex].next.hasMoreDuplicates()) {
                    nodeBufferIndex = nodeBuffer[nodeBufferIndex].next.nextIndex;
                }
                NextNode& tailNode = nodeBuffer[nodeBufferIndex].next;
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
                alreadyHaveNodeWithKey = true;
                return &nodeBuffer.back();
            }

            // We didn't find this node, so try with a later one
            hashIndex += probeAmount;
            hashIndex &= (hashBuffer.count() - 1);
            ++probeAmount;
        }

        assert(0 && "unreachable");
    }

    // Serializes this in to a map which can later be deserialized by the MultiMap constuctor
    void serialize(dyld4::BumpAllocator& allocator) const {
        allocator.append(&nextHashBufferGrowth, sizeof(nextHashBufferGrowth));
        allocator.append(&hashBufferUseCount, sizeof(hashBufferUseCount));

        uint64_t count = hashBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(hashBuffer.data(), count * sizeof(uint64_t));

        count = nodeBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(nodeBuffer.begin(), count * sizeof(NodeEntryT));
    }

    // Serializes this in to a read only map which can be viewed with a MultiMapView
    template<typename TargetNodeKeyT, typename TargetNodeValueT>
    void serialize(dyld4::BumpAllocator& allocator,
                   TargetNodeKeyT (^keyFunc)(const KeyT& key, const ValueT& value),
                   TargetNodeValueT (^valueFunc)(const KeyT& key, const ValueT& value)) const {

        uint64_t count = hashBuffer.count();
        allocator.append(&count, sizeof(count));
        allocator.append(hashBuffer.begin(), (size_t)count * sizeof(uint64_t));

        count = nodeBuffer.count();
        allocator.append(&count, sizeof(count));

        for ( const NodeEntryT& currentNode : nodeBuffer ) {
            typedef MultiMap<TargetNodeKeyT, TargetNodeValueT> TargetMultiMapTy;
            typedef typename TargetMultiMapTy::NodeEntryT NewNodeEntryT;

            NewNodeEntryT newNode = {
                .key    = keyFunc(currentNode.key, currentNode.value),
                .value  = valueFunc(currentNode.key, currentNode.value),
                .next   = currentNode.next
            };
            allocator.append(&newNode, sizeof(NewNodeEntryT));
        }
    }

private:
    uint64_t                                nextHashBufferGrowth;
    uint64_t                                hashBufferUseCount;
    dyld3::OverflowSafeArray<uint64_t>      hashBuffer;
    dyld3::OverflowSafeArray<NodeEntryT>    nodeBuffer;
    void*                                   state;
};

template<typename KeyT, typename ValueT, class GetHash = HashMulti<KeyT>, class IsEqual = EqualMulti<KeyT>>
class MultiMapView : public MultiMapBase<KeyT, ValueT, GetHash, IsEqual> {
    typedef MultiMapBase<KeyT, ValueT, GetHash, IsEqual> BaseMapTy;

    using BaseMapTy::SentinelHash;

public:
    typedef typename BaseMapTy::NextNode NextNode;
    typedef typename BaseMapTy::NodeT NodeT;
    typedef typename BaseMapTy::NodeEntryT NodeEntryT;
    typedef typename BaseMapTy::iterator iterator;
    typedef typename BaseMapTy::const_iterator const_iterator;

    MultiMapView() = default;
    MultiMapView(const void* serializedMap)
    {
        uint64_t hashArrayCount = this->hashBufferCount(serializedMap);
        this->hashBufferArray = Array<uint64_t>(this->hashBuffer(serializedMap), hashArrayCount, hashArrayCount);

        uint64_t nodeArrayCount = this->nodeBufferCount(serializedMap);
        this->nodeBufferArray = Array<NodeEntryT>(this->nodeBuffer(serializedMap), nodeArrayCount, nodeArrayCount);
    }

    ~MultiMapView() = default;

    MultiMapView(const MultiMapView&) = delete;
    MultiMapView& operator=(const MultiMapView&) = delete;

    MultiMapView(MultiMapView&&) = default;
    MultiMapView& operator=(MultiMapView&&) = default;

    void forEachEntry(void (^handler)(const KeyT& key, const Array<const ValueT*>& values)) const {
        BaseMapTy::forEachEntry(this->hashBufferArray, this->nodeBufferArray, handler);
    }

    template<typename LookupKeyT>
    void forEachEntry(void* state, const LookupKeyT& key, void (^handler)(const Array<const ValueT*>& values)) const {
        BaseMapTy::forEachEntry(this->hashBufferArray, this->nodeBufferArray, state, key, handler);
    }

private:
    // These arrays point in to the serializedMap we got in the constructor
    Array<uint64_t>     hashBufferArray;
    Array<NodeEntryT>   nodeBufferArray;

    // The serialized map itself looks like:
    // uint64_t     hashBufferCount;
    // uint64_t     hashBuffer[hashBufferCount];
    // uint64_t     nodeBufferCount;
    // NodeEntryT   nodeBuffer[nodeBufferCount];

    // -------------
    // Hash Buffer methods
    // -------------

    static uint64_t hashBufferCountOffset(const void* serializedMap) {
        return 0;
    }

    static uint64_t hashBufferOffset(const void* serializedMap) {
        return hashBufferCountOffset(serializedMap) + sizeof(uint64_t);
    }

    static uint64_t hashBufferCount(const void* serializedMap) {
        return *(uint64_t*)((uint8_t*)serializedMap + hashBufferCountOffset(serializedMap));
    }

    static uint64_t* hashBuffer(const void* serializedMap) {
        return (uint64_t*)((uint8_t*)serializedMap + hashBufferOffset(serializedMap));
    }

    // -------------
    // Node Buffer methods
    // -------------

    static uint64_t nodeBufferCountOffset(const void* serializedMap) {
        return hashBufferOffset(serializedMap) + (hashBufferCount(serializedMap) * sizeof(uint64_t));
    }

    static uint64_t nodeBufferOffset(const void* serializedMap) {
        return nodeBufferCountOffset(serializedMap) + sizeof(uint64_t);
    }

    static uint64_t nodeBufferCount(const void* serializedMap) {
        return *(uint64_t*)((uint8_t*)serializedMap + nodeBufferCountOffset(serializedMap));
    }

    static NodeEntryT* nodeBuffer(const void* serializedMap) {
        return (NodeEntryT*)((uint8_t*)serializedMap + nodeBufferOffset(serializedMap));
    }
};


struct HashCString {
    static uint64_t hash(const char* v, void* state) {
        return std::hash<std::string_view>()(v);
    }
};

struct HashCStringMulti {
    static uint64_t hash(const char* v, const void* state) {
        return std::hash<std::string_view>()(v);
    }
};

struct EqualCString {
    static bool equal(const char* s1, const char* s2, void* state) {
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
