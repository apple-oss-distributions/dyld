/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
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


#ifndef  LSL_BTree_h
#define  LSL_BTree_h

#include <array>
#include <limits>
#include <cassert>
#include <utility>
#include <algorithm>
#include <functional>

#include "Vector.h"
#include "Defines.h"
#include "Allocator.h"

// This is a mostly complete reimplementation of std::set that can be safely used in dyld
// It uses a custom allocator interface similiar to std::pmr memory resources

// WARNING: One major difference from std::set is that insert invalidates existing iterators
// TODO: Implement merge() overloads
// This is implemented as a tweaked B+Tree

namespace lsl {

template<typename T, class C=std::less<T>, bool Multi=false>
struct TRIVIAL_ABI BTree {
    using key_type          = T;
    using value_type        = T;
    using key_compare       = C;
    using value_compare     = C;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using pointer           = value_type*;
    using size_type         = std::size_t;
private:
    template<uint32_t LC, uint32_t IC>
    struct __attribute__((aligned (256))) TRIVIAL_ABI NodeCore {
        NodeCore(bool leaf) : _metadata(leaf<<7) {
            if (leaf) {
                for (auto& key : _data.leaf.keys) {
                    (void)new ((void*)&key) T();
                }
            } else {
                for (auto& key : _data.internal.keys) {
                    (void)new ((void*)&key) T();
                }
            }
        }
        NodeCore(const NodeCore& other) = default;
        NodeCore& operator=(const NodeCore& other) = default;
        NodeCore(NodeCore&&) = delete;
        NodeCore& operator=(NodeCore&& other) = delete;
        NodeCore(NodeCore* child) : _metadata(0) {
            for (auto& key : _data.internal.keys) {
                (void)new ((void*)&key) T();
            }
            children()[0] = child;
        }

        ~NodeCore() {
            assert(empty());
        }
        uint8_t size() const {
            return (_metadata & 0x7f);
        }
        bool leaf() const {
            return (_metadata >> 7);
        }
        bool empty() const {
            return (size() == 0);
        }
        static
        void deallocate(NodeCore* node, Allocator* allocator) {
            // If this is not a leaf recurse
            if (!node->leaf()) {
                for (auto& child : node->children()) {
                    deallocate(child, allocator);
                }
            }
            // If keys need destructros called call them
            if constexpr(!std::is_trivially_destructible<value_type>::value) {
                for (auto i = 0; i < node->capacity(); ++i) {
                    node->allKeySlots()[i].~T();
                }
            }
            allocator->free((void*)node);
        }
        bool full() const {
            return (size() == capacity());
        }
        uint8_t capacity() const {
            if (leaf()) {
                return LC;
            } else {
                return IC;
            }
        };
        uint8_t pivot() const {
            return (capacity()/2);
        }
        std::span<value_type> keys() const {
            if (leaf()) {
                return std::span<value_type>((value_type*)&_data.leaf.keys[0], size());
            } else {
                return std::span<value_type>((value_type*)&_data.internal.keys[0], size());
            }
        }
        std::span<value_type> allKeySlots() const {
            if (leaf()) {
                return std::span<value_type>((value_type*)&_data.leaf.keys[0], capacity());
            } else {
                return std::span<value_type>((value_type*)&_data.internal.keys[0], capacity());
            }
        }
        std::span<NodeCore*> children() const {
            assert(!leaf() && "Leaf nodes do not contain children");
            return std::span<NodeCore*>((NodeCore**)&_data.internal.children[0], size()+1);
        }
        reference operator[](difference_type idx) {
            return keys()[idx];
        }
        void insert(uint8_t index, value_type&& key) {
            assert(size() != capacity());
            assert(index != capacity());
            std::move_backward(keys().begin()+index, keys().end(), keys().end()+1);
            // Size is the lowest bits of _metadata before we access keys to avoid
            // a potential out of band access
            ++_metadata;
            keys()[index] = std::move(key);
        }

        void erase(uint8_t index) {
            assert(leaf());
            assert(size() > index);
            std::move(keys().begin()+index+1, keys().end(), keys().begin()+index);
            // Size is the lowest bits of _metadata
            --_metadata;
        }

        void splitChild(uint8_t index, Allocator& allocator) {
            assert(!leaf() && "Leaf nodes do not have children to split");
            assert(size() < capacity() && "There must be room in this node for an additional child");
            assert(children()[index]->full() && "The child being split must be full");

            Node*&          child       = children()[index];
            const uint8_t   pivot       = child->pivot();
            const uint8_t   keysToMove  = child->keys().size() - (pivot + 1);

            // Make room for the new node
            std::move_backward(keys().begin()+index, keys().end(), keys().end()+1);
            std::move_backward(children().begin()+index+1, children().end(), children().end()+1);

            // We need to increase the size of the keys in the node before we work on it to avoid
            // exceeding the bounds of the span if the index is the last key
            ++_metadata;

            //  Move pivot key up to root
            keys()[index] =  std::move(child->keys()[pivot]);

            // Create and insert the new child
            auto childStorage = allocator.aligned_alloc(alignof(Node), sizeof(Node));
            auto newChild = new (childStorage) NodeCore(child->leaf());
            children()[index+1] = newChild;

            // Move keys into the new mode
            std::move(child->keys().begin()+pivot+1, child->keys().begin()+pivot+1+keysToMove, newChild->keys().begin());

            // Move children into the new mode
            if (!child->leaf()) {
                std::move(child->children().begin()+pivot+1, child->children().begin()+pivot+2+keysToMove, newChild->children().begin());
            }

            // Adjust metadata;
            child->_metadata    -=  (keysToMove+1);
            newChild->_metadata +=  keysToMove;

            assert(!newChild->full() && !child->full() && "After split the child nodes should be full");
        }

        void rotateFromLeft(uint8_t idx) {
            // Move keys from the predecessor node into this node
            Node* left = children()[idx-1];
            Node* right = children()[idx];
            const uint8_t totalSize = left->size() + right->size();
            const uint8_t targetSize = totalSize/2;
            const uint8_t shift = left->size() - targetSize;

            //Shift the keys in the right node to make room
            std::move_backward(right->keys().begin(), right->keys().end(), right->keys().end()+shift);

            // Copy the keys from the left node to the right
            std::move(left->keys().end()-shift+1, left->keys().end(), right->keys().begin());

            //Shift the key at the node inthe parent index
            right->keys()[shift-1] = std::move(keys()[idx-1]);
            keys()[idx-1] = std::move(left->keys()[left->size()-shift]);

            if (!left->leaf()) {
                std::move_backward(right->children().begin(), right->children().end(), right->children().end()+shift);
                std::move(left->children().end()-shift, left->children().end(), right->children().begin());
            }
            left->_metadata -= shift;
            right->_metadata += shift;
        }

        void rotateFromRight(uint8_t idx) {
            // Move keys from the successor node into this node
            Node* left = children()[idx];
            Node* right = children()[idx+1];
            const uint8_t totalSize = left->size() + right->size();
            const uint8_t targetSize = totalSize/2;
            const uint8_t shift = right->size() - targetSize;

            left->keys()[left->size()] = std::move(keys()[idx]);
            keys()[idx] = std::move(right->keys()[shift-1]);

            std::move(right->keys().begin(), right->keys().begin()+shift, left->keys().end()+1);
            std::move(right->keys().begin()+shift, right->keys().end(), right->keys().begin());

            if (!left->leaf()) {
                std::move(right->children().begin(), right->children().begin()+shift, left->children().end());
                std::move(right->children().begin()+shift, right->children().end(), right->children().begin());
            }
            left->_metadata += shift;
            right->_metadata -= shift;
        }

        void merge(Allocator* allocator, uint8_t index) {
            assert(!leaf() && "A leaf node does not have children to merge");
            assert(index < size() && "A node must have a successor node to merge with");
            // We will merge with the left node unless we can't because it is the left most node (0)
            Node* left = children()[index];
            Node* right = children()[index+1];

            // Move the key from the index down into the merged child and shift eleements
            left->allKeySlots()[left->size()] = std::move(keys()[index]);
            std::move(keys().begin()+index+1, keys().end(), keys().begin()+index);
            std::move(children().begin()+index+2, children().end(), children().begin()+index+1);

            // Merge the contents of the node we are abot to deallocate
            std::move(right->keys().begin(), right->keys().end(), left->keys().end()+1);
            if (!left->leaf()) {
                std::move(right->children().begin(), right->children().end(), left->children().end());
            }

            //Adjust metadata
            left->_metadata += (right->size()+1);
            --_metadata;

            // deallocate empty node
            allocator->free((void*)right);
        }

        uint8_t lower_bound_index(const T& key, value_compare comp) const {
            return std::lower_bound(keys().begin(), keys().end(), key, comp) - keys().begin();
        };

        uint8_t begin_index() const {
            return 0;
        }

        uint8_t end_index() const {
            return size();
        }
    private:
        friend struct BTree;
        struct  LeafStorage {
            std::array<value_type,LC> keys;
        };
        struct  InternalStorage {
            std::array<value_type,IC>   keys;
            std::array<NodeCore* ,IC+1> children;
        };
        union Data {
            constexpr Data() {}
            LeafStorage     leaf;
            InternalStorage internal;
            ~Data() {}
        };
         __attribute__((aligned(std::max(alignof(LeafStorage), alignof(InternalStorage)))))
        Data        _data;
        uint8_t     _metadata   = 0;
    };

    static const uint16_t kTargetSize = 256;
    template<uint8_t N>
    constexpr static uint8_t getInteriorNodeCapacity() {
        if (sizeof(NodeCore<1,N>) <= kTargetSize) { return N; }
        return getInteriorNodeCapacity<N-1>();
    }

    template<>
    constexpr uint8_t getInteriorNodeCapacity<2>() {
        return 2;
    }

    constexpr static uint8_t getInteriorNodeCapacity() {
        if (sizeof(T) == 1) { return 13; }
        return getInteriorNodeCapacity<255>();
    }

    template<uint8_t N>
    constexpr static uint8_t getLeafNodeCapacity(uint16_t targetSize) {
        if (sizeof(NodeCore<N,1>) <= targetSize) { return N; }
        return getLeafNodeCapacity<N-1>(targetSize);
    }

    template<>
    constexpr uint8_t getLeafNodeCapacity<2>(uint16_t targetSize) {
        return 2;
    }

    constexpr static uint8_t getLeafNodeCapacity() {
        if (sizeof(T) == 1) { return 255; }
        uint16_t targetSize = sizeof(NodeCore<getInteriorNodeCapacity(),1>);
        if (targetSize < kTargetSize) { targetSize = kTargetSize; }
        return getLeafNodeCapacity<255>(targetSize);
    }

    constexpr static uint8_t getMaxDepth() {
        uint8_t minLeafCount = getLeafNodeCapacity()/2;
        uint8_t minInteriorCount = (getInteriorNodeCapacity()/2) + 1;
        uint64_t capacity = minLeafCount;
        for (uint8_t i = 1; i < 40; ++i) {
            capacity = (capacity * minInteriorCount);
            if (capacity >= std::numeric_limits<uint32_t>::max()) {
                return i;
            }
        }
        __builtin_unreachable();
    }
public:
    static const uint32_t kLeafNodeCapacity = getLeafNodeCapacity();
    static const uint32_t kInteriorNodeCapacity = getInteriorNodeCapacity();
    static const uint8_t kMaxDepth = getMaxDepth();
    using Node = NodeCore<kLeafNodeCapacity, kInteriorNodeCapacity>;

    // This iterator is a series of nodes and indexes used to sequentially walk an ordered tree. An iterator with 0 depth is the
    // end iterator. As an internal implementation detail incrementing an end() iterator will actually cycle back to begin()
    // This simplifies some of iterator increment and decrement logic, and is safe for the collection to do to in its own iterator
    // even though it is not generally safe in C++
    struct  const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        const_iterator(const const_iterator& other)
            : _btree(other._btree), _nodes(other._nodes), _indexes(other._indexes), _depth(other._depth) {
#if BTREE_VALIDATION
                    _generation = other._generation;
#endif
            }

        const_iterator& operator=(const const_iterator& other) {
            auto tmp = other;
            swap(tmp);
            return *this;
        }

        const_iterator(const BTree* btree) : _btree(const_cast<BTree*>(btree)) {
#if BTREE_VALIDATION
                _generation = _btree->_generation;
#endif
        }

        // This is the "lower_bound" constructor
        const_iterator(const BTree* btree, const value_type& key, value_compare comp) : const_iterator(btree) {
            if (btree->depth() == 0) { return; }
            // Setup the inital node
            auto nextNode = btree->_root;
            for (uint8_t i = 0; i < btree->_depth; ++i) {
                // Figure out the lower_bound_index within the node
                _nodes[i] = nextNode;
                _indexes[i] = _nodes[i]->lower_bound_index(key, comp);
                if (_indexes[i] != _nodes[i]->end_index() && comp(_nodes[i]->keys()[_indexes[i]], key)) {
                    // The key is an exact match, return early
                    _depth = i + 1;
                    return;
                }
                // Prep the next node unless we are in the final iteration
                if (i+1 != btree->_depth) {
                    nextNode = _nodes[i]->children()[_indexes[i]];
                }
            }
            _depth = btree->_depth;
            // We have an iterator that hits a leaf node. The last index in an iterator cannot be the end_index of the node it is
            // part of, so lower the depth until it is not
            while ((_depth != 0) && (_indexes[_depth-1] == _nodes[_depth-1]->end_index())) {
                --_depth;
            }
        }
        reference operator*() {
            checkGeneration();
            return (*_nodes[_depth-1])[_indexes[_depth-1]];
        }
        pointer operator->() const {
            checkGeneration();
            return &(*_nodes[_depth-1])[_indexes[_depth-1]];
        }

        const_iterator& operator++() {
            checkGeneration();
            if (_depth == 0) {
                // This is technically an end() iterator, but our internal implementation is such that end-1 == begin
                // We could include this code in the begin() function, but leaving it here provides symmetry between ++ and --,
                // and leave all the complex code related to it in the iterator instead of the collection.
                auto nextNode = _btree->_root;
                for (_depth = 0; _depth < _btree->_depth; ++_depth) {
                    _nodes[_depth]      = nextNode;
                    _indexes[_depth]    = 0;
                    // Prep the next node unless we are in the final iteration
                    if (_depth+1 != _btree->_depth) {
                        nextNode = _nodes[_depth]->children()[0];
                    }
                }
            } else if (_depth == _btree->_depth) {
                // This is a leaf node. Increment the value
                ++_indexes[_depth-1];
                for (uint8_t i = 0; i < _btree->_depth; ++i) {
                    uint8_t currentDepth = _btree->_depth - (i + 1);
                    // If the index exceeds the number of elements in the node ascend until we find a node where it doesn't
                    if (_indexes[currentDepth] != _nodes[currentDepth]->size()) { break; }
                    _depth = currentDepth;
                }
            } else {
                // This is an interior node, increment and then descend down the 0th indexes until we hit a leaf
                for ( ++_indexes[_depth-1]; _depth != _btree->_depth; ++_depth) {
                    _nodes[_depth]      = _nodes[_depth-1]->children()[_indexes[_depth-1]];
                    _indexes[_depth]    = 0;
                }
            }
            return *this;
        }

        const_iterator& operator--() {
            checkGeneration();
            if (_depth == 0) {
                auto nextNode = _btree->_root;
                for (_depth = 0; _depth < _btree->_depth; ++_depth) {
                    _nodes[_depth]      = nextNode;
                    _indexes[_depth]    = _nodes[_depth]->size();
                    // Prep the next node unless we are in the final iteration
                    if (_depth+1 != _btree->_depth) {
                        nextNode = _nodes[_depth]->children()[_nodes[_depth]->size()];
                    }
                }
                // The last node is a leaf, which means it has one less index than the interior nodes, so fix it up
                --_indexes[_depth-1];
            } else if (_depth == _btree->_depth) {
                // This is a leaf node. Decrement the value
                if (_indexes[_depth-1] > 0) {
                    --_indexes[_depth-1];
                } else {
                    while (_indexes[_depth-1] == 0) {
                        --_depth;
                    }
                    --_indexes[_depth-1];
                }
            } else {
                // This is an interior node, decrement and then descend down the 0th indexes until we hit a leaf
                while (_depth != _btree->_depth) {
                    _nodes[_depth]      = _nodes[_depth-1]->children()[_indexes[_depth-1]];
                    _indexes[_depth]    = _nodes[_depth]->size();
                    ++_depth;
                }
                --_indexes[_depth-1];
            }
            return *this;
        }
        const_iterator operator++(int) {
           auto tmp = *this;
           ++*this;
           return tmp;
        }
        const_iterator operator--(int) const {
           auto result = *this;
            --*this;
           return result;
        }
        std::strong_ordering operator<=>(const const_iterator& other) const {
            for (auto i = 0; i < std::min(_depth, other._depth); ++i) {
                auto result = _indexes[i] <=> other._indexes[i];
                if (result != std::strong_ordering::equal) {
                    return result;
                }
            }
            // The indexes were the same up to this point, and one iterator has hit _depth. Whichever is shorter is ordered first
            return (_depth <=> other._depth);
        }
        bool operator==(const const_iterator& other) const {
            return (operator<=>(other) == std::strong_ordering::equal);
        }
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }

        /* This validation routine can be run to check if the iterator is valid. That means:
         *
         * 1. The depth is either:
         *   A. 0 (EMPTY)
         *   B, Less than the depth of the btree if it is an interior node
         *   C. Equal to the depth of the btree if it is a leaf node
         * 2. That at depth of iteraotr X: key < _node[X][_indexes[X]]
         * 3. That at depth of iteraotr X: _node[X-1][_indexes[X-1]] < key
         * 4. That for each entry in the path next node is what the index points to: _nodes[X+1] == _nodes[X][_indexes[X]]
         */
        void validate() const {
            if constexpr(!BTREE_VALIDATION) { return; }
            assert((_depth == 0) || currentNode()->leaf() == (_btree->_depth == _depth));
            if (_depth == 0) { return; }
            for (auto i = 0; i < _depth-1; ++i) {
                assert(_nodes[i+1] == _nodes[i]->children()[_indexes[i]]);
            }
            auto& key = (*_nodes[_depth-1])[_indexes[_depth-1]];
            for (auto i = 0; i < _depth; ++i) {
                if (_indexes[i] != 0) {
                    auto& previousKey = _nodes[i]->keys()[_indexes[i]-1];
                    if constexpr(Multi) {
                        assert(_btree->_comp(previousKey, key) || (!_btree->_comp(previousKey, key) && !_btree->_comp(key, previousKey)));
                    } else {
                        assert(_btree->_comp(previousKey, key));
                    }
                }
            }
            for (auto i = 0; i < _depth-1; ++i) {
                if (_indexes[i] < _nodes[i]->size()) {
                    auto& nextKey = _nodes[i]->keys()[_indexes[i]];
                    if constexpr(Multi) {
                        assert(_btree->_comp(key, nextKey) || (!_btree->_comp(key, nextKey) && !_btree->_comp(nextKey, key)));
                    } else {
                        assert(_btree->_comp(key, nextKey));
                    }
                }
            }
        }
private:
        uint8_t depth() const {
            return _depth;
        }
        uint8_t& currentIndex() {
            return _indexes[_depth-1];
        }
        Node* currentNode() const {
            return _nodes[_depth-1];
        }
        std::array<Node*,kMaxDepth>& nodes() {
            return _nodes;
        }
        std::array<uint8_t,kMaxDepth>& indexes() {
            return _indexes;
        }

        void prepareForInsertion() {
            assert(_depth == _btree->_depth && "prepareForInsertion only works on iterators leaf nodes");
            uint8_t splitStart  = 0;
            // If it is not a leaf
            // Empty slots in the leaf node, nothing to do here
            if (!_nodes[_depth-1]->full()) {
                return;
            }
            for (uint8_t i = 0; i < _depth; ++i) {
                if (!_nodes[i]->full()) {
                    // If this node is not earliest the split can start is after, move the start down
                    splitStart = i;
                }
            }
            // If the root is full. Create a new root with the old root as its only child and no keys, and then split the old root
            if (splitStart == 0 && _nodes[0]->full()) {
                void* rootStorage = _btree->_allocator->aligned_alloc(alignof(Node), sizeof(Node));
                _btree->_root = new (rootStorage) Node(_btree->_root);
                std::move_backward(_indexes.begin(), _indexes.begin() + _depth,  _indexes.begin() + _depth + 1);
                std::move_backward(_nodes.begin(), _nodes.begin() + _depth ,  _nodes.begin() + _depth + 1);
                _indexes[0] = 0;
                _nodes[0] = _btree->_root;
                ++_btree->_depth;
                ++_depth;
            }
            // We know where the split starts, walk down the entire tree and split it
            for (uint8_t i = splitStart; i+1 < _depth; ++i) {
                _nodes[i]->splitChild(_indexes[i], *_btree->_allocator);
                auto newNode = _nodes[i]->children()[_indexes[i]];
                if (_indexes[i+1] > newNode->size()) {
                    ++_indexes[i];
                    _indexes[i+1] = _indexes[i+1] - (newNode->size()+1);
                    _nodes[i+1] = _nodes[i]->children()[_indexes[i]];
                }
            }
        }
        // Returns: true if the tree depth decreased, false otherwise
        void rebalanceFromErasure() {
            assert(_depth == _btree->_depth && "rebalanceFromErasure only works on iterators to leaf nodes");
            for (uint8_t i = 0; i < _btree->_depth-1; ++i) {
                uint8_t currentDepth = _btree->_depth - (i + 2);
                // If the node has at least pivot() elements then we are done
                if (_nodes[currentDepth+1]->size() >= _nodes[currentDepth+1]->pivot()) { break; }

                auto& node              = _nodes[currentDepth];
                auto& nodeIndex         = _indexes[currentDepth];
                auto& childNodeIndex    = _indexes[currentDepth+1];
                int8_t rightScore = 0;
                int8_t leftScore = 0;
                if (nodeIndex != node->size()) {
                    rightScore = node->children()[nodeIndex+1]->size()-node->children()[nodeIndex+1]->pivot();
                }
                if (nodeIndex != 0) {
                    leftScore = node->children()[nodeIndex-1]->size()-node->children()[nodeIndex-1]->pivot();
                }

                if ((rightScore > 0) && (rightScore >= leftScore)) {
                    // The right node has enough children, rotate them in
                    node->rotateFromRight(nodeIndex);
                } else if ((leftScore > 0) && (leftScore > rightScore)) {
                    // The left node has enough children, rotate them in
                    uint8_t oldSize = node->children()[nodeIndex]->size();
                    node->rotateFromLeft(nodeIndex);
                    // Update the child index since a number of new elements appeared at the beginning of the node
                    childNodeIndex += (node->children()[nodeIndex]->size() - oldSize);
                } else if (nodeIndex != node->size()) {
                    // Rotate did not work merge the right node in
                    node->merge(_btree->_allocator, nodeIndex);
                } else {
                    // Merge the left node into this node
                    // We need to update both index, childIndex, and the childNode entry in _nodes
                    childNodeIndex += (node->children()[--nodeIndex]->size()+1);
                    node->merge(_btree->_allocator, nodeIndex);
                    _nodes[currentDepth+1] = _nodes[currentDepth]->children()[_indexes[currentDepth]];
                }
            }

            // FHandle the case where the root node only has a single entry, by making that entry the new root node
            if (_nodes[0]->size() == 0) {
                assert(_indexes[0] == 0);
                std::move(_indexes.begin() + 1, _indexes.begin() + _depth, _indexes.begin());
                std::move(_nodes.begin() + 1, _nodes.begin() + _depth, _nodes.begin());
                --_depth;
                _btree->_allocator->free((void*)_btree->_root);
                --_btree->_depth;
                if (_depth) {
                    _btree->_root = _nodes[0];
                } else {
                    _btree->_root = nullptr;
                }
            }

            // Okay, we've merged the nodes, now deal with fact that our index path my exceed the bounds
            // of a Node. We handle that by deleting levels off depth from the path until we get to a valid
            // entry which will be successor
            for (uint8_t i = 0; i < _btree->_depth; ++i) {
                uint8_t currentDepth = _btree->_depth - (i + 1);
                if (_nodes[currentDepth]->size() != _indexes[currentDepth]) { break; }
                --_depth;
            }
        }
        void setGeneration(uint64_t generation) {
#if BTREE_VALIDATION
            _generation = generation;
#endif
        }
        void checkGeneration() const {
#if BTREE_VALIDATION
            assert(_btree->_generation == _generation);
#endif
        }
        void swap(const_iterator& other) {
            using std::swap;
            swap(_btree,    other._btree);
            swap(_nodes,    other._nodes);
            swap(_indexes,  other._indexes);
            swap(_depth,    other._depth);
#if BTREE_VALIDATION
            swap(_generation,    other._generation);
#endif
        }
    public:
        friend struct BTree;
        BTree*                          _btree      = nullptr;
#if BTREE_VALIDATION
        uint64_t                        _generation = 0;
#endif
        std::array<Node*,   kMaxDepth>  _nodes      = {nullptr};
        std::array<uint8_t, kMaxDepth>  _indexes    = {0};
        uint8_t                         _depth      = 0;

    };
    using iterator = const_iterator;

    const_iterator cbegin() const {
        return ++const_iterator(this);
    }

    const_iterator cend() const {
        return const_iterator(this);
    }

    const_iterator  begin() const   { return cbegin(); }
    const_iterator  end() const     { return cend(); }
    iterator        begin()         { return std::as_const(*this).begin(); }
    iterator        end()           { return std::as_const(*this).end(); }

    // The lower_bound index path will point to one of the 4 things.
    // 1. An index path directly to an element equal to key
    // 2. An index path to a leaf node that contains the first element greater than the key
    // 3. An index path to an interior node. This will only happen if the key is greater than the last elment of the leaf node and less then interior key that points to it
    // 4. It be an index path to the last valid node in the tree, with the leaf index incremeneted by one.
    //    this is the index path representation of the end iterator

    const_iterator lower_bound(const value_type& key) const {
        return iterator(this, key, _comp);
    }

    const_iterator find(const value_type& key) const {
        auto i = lower_bound(key);
        if ((i != end()) && !_comp(key, *i)) { return i; }
        return end();
   }

    iterator lower_bound(const value_type& key) {
        auto i = iterator(std::as_const(*this).lower_bound(key));
        i.validate();
        return i;
    }

    iterator find(const value_type& key) {
        return iterator(std::as_const(*this).find(key));
    }

    std::pair<iterator, bool> insert_internal(iterator&& i, value_type&& key) {
        i.checkGeneration();
        if (!_root) {
            void* rootStorage = _allocator->aligned_alloc(alignof(Node), sizeof(Node));
            _root = new (rootStorage) Node(true);
            _depth = 1;
            i.nodes()[0] = _root;
            i._depth = 1;
            i.currentNode()->insert(0, std::move(key));
            ++_size;
            validate();
            i.validate();
            return { i, true };
        }
        bool rotated = false;

        if constexpr(!Multi) {
            if ((i != end()) && !_comp(key, *i)) { return { i, false }; }
        }

        if ((i == end() || (i.depth() != _depth))){
            --i;
            rotated = true;
        }
        i.prepareForInsertion();
        uint8_t& leafIndex = i.currentIndex();
        if (rotated) {
            ++leafIndex;
        }
        i.currentNode()->insert(leafIndex, std::move(key));
        ++_size;
#if BTREE_VALIDATION
            i.setGeneration(++_generation);
            validate();
            i.validate();
#endif
        return { i, true };
    }

    //Amortized constant if the insertion happens in the position just before the hint, logarithmic in the size of the container otherwise.
    std::pair<iterator,bool> insert(const_iterator hint, value_type&& key) {
        if (_size == 0) {
            return insert_internal(std::move(end()), std::move(key));
        }
        if (hint == end()) {
            if (_comp(*std::prev(hint), key)) {
                return insert_internal(std::move(hint), std::move(key));
            }
        } else  if (_comp(key, *hint)) {
            if (hint == begin() || _comp(*std::prev(hint), key)) {
                return insert_internal(std::move(hint), std::move(key));
            }
        }
        return insert_internal(std::move(lower_bound(key)), std::move(key));
    }

    std::pair<iterator,bool> insert(value_type&& key) {
        return insert_internal(std::move(lower_bound(key)), std::move(key));
    }

    std::pair<iterator,bool> insert(const_iterator hint, const value_type& key) {
        return insert(hint, value_type(key));
    }

    std::pair<iterator,bool> insert(const value_type& key) {
        return insert(value_type(key));
    }

    // The basic erase operation only works on leaf nodes. Since ++ of any index path that
    // is not a leaf node will return the first element of a leaf node we exploit that
    // to find the next value, and swap it. While that temporarily results in an ordering
    // violation, the issue will be resolved as soon as we delete the value
    iterator erase(iterator i) {
        i.checkGeneration();
        bool rotated = false;
        if (i.depth() != _depth) {
            auto& oldElement = *i;
            ++i;
            std::swap(oldElement, *i);
            rotated = true;
        }
        assert(i.currentNode()->leaf());
        i.currentNode()->erase(i.indexes()[i.depth()-1]);
        // We have an iterator that hits a leaf node. The last index in an iterator cannot be the end_index of the node it is
        // part of, so lower the depth until it is not
        i.rebalanceFromErasure();
        if (rotated) {
            --i;
        }
        --_size;
#if BTREE_VALIDATION
        i.setGeneration(++_generation);
        validate();
        i.validate();
#endif
        return i;
    }
    size_type erase(const value_type& key) {
        auto i = find(key);
        if constexpr(Multi) {
            size_type result = 0;
            while (i != end() && !_comp(*i, key) && !_comp(key, *i)) {
                i = erase(i);
                ++result;
            }
            return result;
        } else {
            if (i == end()) {
                return 0;
            }
            (void)erase(i);
            return 1;
        }
    }
    size_type   size() const    { return _size; }
    bool        empty() const   { return (size() == 0); }
    void        clear() {
        if (_root) {
            Node::deallocate(_root, _allocator);
            _root = nullptr;
        }
#if BTREE_VALIDATION
        ++_generation;
#endif
        _size = 0;
        _depth = 0;
    }
    size_type count(const value_type& key) const {
        if constexpr(Multi) {
            size_type result = 0;
            for (auto i = find(key); i != end() && !_comp(*i, key) && !_comp(key, *i); ++i) {
                ++result;
            }
            return result;
        }
        auto i = find(key);
        return (i == end()) ? 0 : 1;
    }

#pragma mark -
#pragma mark Debugging (Graphviz)
// Commented out for code coverage
//#if 0
//    void dumpVizNodes(const Node* node) const {
//        if (node->leaf()) {
//            // "node-1042ce240"[label = "{Count|{<0>|339|<1>|845|<2>}}"];
//
//            printf("\"node-%lx\"[label = \"{Count = %u|{", (uintptr_t)node, node->size());
//            bool first = true;
//            for (auto& element : node->keys()) {
//                if (first) {
//                    first = false;
//                } else {
//                    printf("|");
//                }
//                debugDump(element);
//            }
//            printf("}}\"];\n");
//        } else {
//            printf("\"node-%lx\"[label = \"{Count = %u|{", (uintptr_t)node, node->size());
//            uint8_t index = 0;
//            for (auto& element : node->keys()) {
//                printf("<%u>|", index);
//                debugDump(element);
//                printf("|");
//                ++index;
//            }
//            printf("<%u>}}\"];\n", index);
//            for (auto& child : node->children()) {
//                dumpVizNodes(child);
//            }
//        }
//    }
//
//    void dumpVizEdges(const Node* node) const {
//        if (!node->leaf()) {
//            uint8_t index = 0;
//            for (auto& child : node->children()) {
//                printf("\"node-%lx\":%u -> \"node-%lx\"\n", (uintptr_t)node, index, (uintptr_t)child);
//                dumpVizEdges(child);
//                ++index;
//            }
//        }
//    }
//
//    void dumpViz() const {
//        printf("digraph g {\n");
//        printf("node [shape = record,height=.1];\n");
//        dumpVizNodes(_root);
//        dumpVizEdges(_root);
//        printf("}\n\n");
//    }
//#endif
    BTree() = default;
    explicit BTree(Allocator& allocator) :  _allocator(&allocator) {}
    explicit BTree(Allocator& allocator, Node* root) :  _root(root), _allocator(&allocator) {}
    explicit BTree(value_compare comp, Allocator& allocator) : _allocator(&allocator), _comp(comp) {}
    template< class InputIt1,  class InputIt2>
    BTree( InputIt1 first, InputIt2 last, value_compare comp, Allocator& allocator) : BTree(comp, allocator) {
        for (auto i = first; i != last; ++i) {
            insert(*i);
        }
    }
    ~BTree() {
        clear();
    }
    BTree(const BTree& other, Allocator& allocator) :  _allocator(&allocator) {
        //TODO: Make this fast and compact someday, by:
        // 1. Sequentialy building full nodes bottom up
        // 2. Taking the right most nodes (which may not be full) and balancing them
        for (auto& i : other) {
            insert(end(), i);
        }
    }
    BTree(const BTree& other) : BTree(other, _allocator) {}
    BTree(BTree&& other) {
        swap(other);
    }
    BTree& operator=(const BTree& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    BTree& operator=(BTree&& other) {
        swap(other);
        return *this;
    }
    uint8_t depth() const {
        return _depth;
    }
    friend void swap(BTree& x, BTree& y) {
        x.swap(y);
    }
    void validate() const {
        if constexpr(!BTREE_VALIDATION) { return; }
        uint64_t size = validate(_depth, _root);
        assert(size == _size);
    }

    uint64_t validate(uint8_t depth, Node* node) const {
        static uint64_t count = 0;
        ++count;
        if (_depth == 0) {
            assert(node == nullptr);
            return 0;
        }
        assert(node != nullptr);
        uint64_t result = node->size();
        key_type* lastKey = nullptr;

        if (depth == 1) {
            assert(node->leaf());
        } else {
            assert(!node->leaf());
            auto child = node->children()[0];
            lastKey = &child->keys()[child->size()-1];
        }
        uint32_t i;
        for (i = 0; i < node->size(); ++i) {
            key_type *key = &node->keys()[i];
            if (lastKey) {
                if constexpr(Multi) {
                    assert(_comp(*lastKey, *key) || (!_comp(*lastKey, *key) && !_comp(*key, *lastKey)));
                } else {
                    assert(_comp(*lastKey, *key));
                }
            }
            if (!node->leaf()) {
                result += validate(depth-1, node->children()[i]);
            }
            lastKey = key;
        }
#if 0
        // This only works correctly for well behaved types, it fails for things like strings. Disabled by default, but useful for
        // debugging
        if (std::is_default_constructible<key_type>::value && !std::is_trivially_destructible<value_type>::value) {
            key_type default_value;
            for ( ; i < node->capacity(); ++i) {
                key_type *key = &node->keys()[i];
                assert(!_comp(default_value, *key));
                assert(!_comp(*key, default_value));
            }
        }
#endif
        if (!node->leaf()) {
            result += validate(depth-1, node->children()[node->size()]);
        }
        if (!node->leaf()) {
            auto child = node->children()[node->size()];
            key_type *key = &child->keys()[child->size()-1];
            if constexpr(Multi) {
                assert(_comp(*lastKey, *key) || (!_comp(*lastKey, *key) && !_comp(*key, *lastKey)));
            } else {
                assert(_comp(*lastKey, *key));
            }
        }

        return result;
    }
private:
    void swap(BTree& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_root,         other._root);
        swap(_allocator,    other._allocator);
        swap(_comp,         other._comp);
        swap(_size,         other._size);
        swap(_depth,        other._depth);
#if BTREE_VALIDATION
        swap(_generation,    other._generation);
#endif
    }
    Node*           _root           = nullptr;
    Allocator*      _allocator      = nullptr;
    value_compare   _comp           = value_compare();
#if BTREE_VALIDATION
    uint64_t        _generation     = 0;
#endif
    size_type       _size           = 0;
    uint8_t         _depth          = 0;
};

};

#endif /*  LSL_BTree_h */
