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

template<typename T, class C=std::less<T>, bool M=false>
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
        //TODO: This can be probably be replaced with std::span when we move to C++20
        template<typename U>
        struct IteratorProxy {
                        IteratorProxy(const uint8_t* begin, size_t size) : _begin((const U*)begin), _end(&_begin[size]) {}
            const U*    begin() const                   { return _begin; }
            const U*    end() const                     { return _end; }
            const U&    operator[](uint8_t idx) const   { return begin()[idx]; }
            U&          operator[](uint8_t idx)         { return const_cast<U&>(std::as_const(*this).operator[](idx)); }
            uint8_t     size() const                    { return _end - _begin; }
        private:
            const U* _begin;
            const U* _end;
        };
        using KeyIterator = value_type;
        using ChildIterator = NodeCore*;

        uint8_t size() const {
            return (_metadata & 0x00ff);
        }
        bool leaf() const {
            return ((_metadata & 0x0100)>>8);
        }

        NodeCore(uint8_t flags, Allocator* allocator) : _metadata(((uint64_t)allocator<<9) | (flags << 8)) {
            contract((uintptr_t)this%alignof(Node)==0);
            for (auto i = 0; i < (leaf() ? LC : IC); ++i) {
                (void)new ((void*)&keys()[i]) value_type();
            }
            for (auto i = 0; i < (leaf() ? 0 : IC+1); ++i) {
                children()[i] = nullptr;
            }
        }
        Allocator* allocator() const {
            return (Allocator*)(_metadata>>9);
        }
        NodeCore(const NodeCore& other, Allocator* allocator) : _metadata(((uint64_t)allocator<<9) | (other.leaf() << 8)){
            _metadata += other.size();
            for (auto i = 0; i < (leaf() ? LC : IC); ++i) {
                if (i < other.size()) {
                    keys()[i] = std::move(other.keys()[i]);
                } else {
                    (void)new ((void*)&keys()[i]) value_type();
                }
            }
            for (auto i = 0; i < (leaf() ? 0 : IC+1); ++i) {
                if (i < (other.size()+1)) {
                    children()[i] = other.children()[i];
                } else {
                    children()[i] = nullptr;
                }
            }
        }
        NodeCore(const NodeCore&) = delete;
        NodeCore(NodeCore&&) = delete;
        ~NodeCore() {
            contract(empty());
        }
        NodeCore& operator=(NodeCore&& other) {
            swap(other);
            return *this;
        }
        void deallocateChildren() {
            if (!leaf()) {
                for (auto& child : children()) {
                    child->deallocateChildren();
                    child->allocator()->deallocate_buffer((void*)child, sizeof(NodeCore), alignof(NodeCore));
                }
            }
            if constexpr(!std::is_trivially_destructible<value_type>::value) {
                for (auto i = 0; i < (leaf() ? LC : IC); ++i) {
                    keys()[i].~T();
                }
            }
            _metadata &= ~0x00ff;
        }
        bool empty() const {
            return (size() == 0);
        }
        uint8_t capacity() const {
            if (leaf()) { return LC; }
            return IC;
        };
        bool full() const {
            return (size() == capacity());
        }
        uint8_t pivot() const {
            return (capacity()/2);
        }
        IteratorProxy<KeyIterator> keys() const {
            return IteratorProxy<KeyIterator>(&_data[0], size());
        }
        IteratorProxy<ChildIterator> children() const {
            assert(!leaf());
            return IteratorProxy<ChildIterator>(&_data[__offsetof(InternalStorage, children)], leaf() ? 0 : size()+1);
        }
        reference operator[](difference_type idx) {
            return keys()[idx];
        }

        void insert(uint8_t idx, value_type&& key) {
            assert(size() != capacity());
            assert(idx != capacity());
            for (auto i = size(); i > idx; --i) {
                keys()[i] = std::move(keys()[i-1]);
            }
            keys()[idx] = std::move(key);
            // Size is the lowest bits of _metadata
            ++_metadata;
        }

        void erase(uint8_t idx) {
            assert(leaf());
            assert(size() > idx);
            for (auto i = idx; i < size()-1; ++i) {
                keys()[i] = std::move(keys()[i+1]);
            }
            // Size is the lowest bits of _metadata
            --_metadata;
        }

        void splitChild(uint8_t index, Allocator& allocator) {
            assert(!leaf());
            assert(size() < capacity());
            Node*& child = children()[index];
            const uint8_t end = child->capacity();
            const uint8_t pivot = child->pivot();
            assert(child->full());

            // Create and populate the new child
            Allocator* deallocator = nullptr;
            auto newNodeBuffer = allocator.allocate_buffer(sizeof(NodeCore), alignof(NodeCore), &deallocator);
            auto newChild = new (newNodeBuffer.address) NodeCore(child->leaf(), deallocator);
            for(auto i = pivot+1; i < end; ++i) {
                newChild->keys()[i-(pivot+1)] = std::move(child->keys()[i]);
                ++newChild->_metadata;
                --child->_metadata;
            }
            if (!newChild->leaf()) {
                for(auto i = pivot+1; i < end+1; ++i) {
                    newChild->children()[i-(pivot+1)] = child->children()[i];
                }
            }

            // Make space for the new child
            for (auto i = size(); i > index; --i) {
                keys()[i] = std::move(keys()[i-1]);
                children()[i+1] = children()[i];
            }

            // Insert the new child
            keys()[index] =  std::move(child->keys()[pivot]);
            children()[index+1] = newChild;
            // Size is the lowest bits of _metadata
            ++_metadata;
            --child->_metadata;

            assert(!newChild->full());
            assert(!child->full());
        }

        void rotateFromLeft(uint8_t idx) {
            // Move keys from the predecessor node into this node
            Node* left = children()[idx-1];
            Node* right = children()[idx];
            const uint8_t totalSize = left->size() + right->size();
            const uint8_t targetSize = totalSize/2;
            const uint8_t shift = left->size() - targetSize;

            for (auto i = 0; i < right->size(); ++i) {
                right->keys()[right->size()+shift-i-1] = std::move(right->keys()[right->size()-i-1]);
            }
            right->keys()[shift-1] = std::move(keys()[idx-1]);
            for (auto i = 1; i < shift; ++i) {
                right->keys()[i-1] = std::move(left->keys()[left->size()-shift+i]);
            }
            keys()[idx-1] = std::move(left->keys()[left->size()-shift]);
            if (!left->leaf()) {
                for (auto i = 0; i < right->size()+1; ++i) {
                    right->children()[right->size()+shift-i] = right->children()[right->size()-i];
                }
                for (auto i = 0; i < shift; ++i) {
                    right->children()[i] = left->children()[left->size()+1-shift+i];
                }
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
            for (auto i = 1; i < shift; ++i) {
                left->keys()[left->size()+i] = std::move(right->keys()[i-1]);
            }
            keys()[idx] = std::move(right->keys()[shift-1]);
            for (auto i = 0; i < right->size()-shift; ++i) {
                right->keys()[i] = std::move(right->keys()[shift+i]);
            }
            if (!left->leaf()) {
                for (auto i = 0; i < shift; ++i) {
                    left->children()[left->size()+1+i] = right->children()[i];
                }
                for (auto i = 0; i < right->size()-shift+1; ++i) {
                    right->children()[i] = right->children()[i+shift];
                }
            }
            left->_metadata += shift;
            right->_metadata -= shift;
        }

        // This is only safe to be called after rebalance has failed in both directions.
        // That rebalnce fails means that both the left and the right node can be merged with.
        void merge(uint8_t idx) {
            contract(!leaf());
            contract(idx < size());
            // We will merge with the left node unless we can't becasue it is the left most node (0)
            Node* left = children()[idx];;
            Node* right = children()[idx+1];;
            left->keys()[left->size()] = std::move(keys()[idx]);
            for (auto i = idx; i < size()-1; ++i) {
                keys()[i] = std::move(keys()[i+1]);
                children()[i+1] = children()[i+2];
            }
            for(auto i = 0; i < right->size(); ++i) {
                left->keys()[left->size()+1+i] = std::move(right->keys()[i]);
            }
            if (!left->leaf()) {
                for(auto i = 0; i < right->size()+1; ++i) {
                    left->children()[left->size()+1+i] = right->children()[i];
                }
            }
            left->_metadata += (right->size()+1);
            // Size is the lowest bits of _metadata
            --_metadata;
            allocator()->deallocate_buffer((void *)right, sizeof(NodeCore), alignof(NodeCore));
        }
        friend void swap(NodeCore& x, NodeCore& y) {
            x.swap(y);
        }
    private:
        friend struct BTree;
        struct  LeafStorage {
            value_type elements[LC];
        };
        struct  InternalStorage {
            value_type  elements[IC];
            NodeCore*   children[IC+1];
        };
        void swap(NodeCore& other) {
            using std::swap;
            if (this == &other) { return; }
            std::array<value_type, LC>  tempKeys;
            std::array<NodeCore*, IC+1> tempChildren;

            // other -> Local temp
            std::move(&other.keys()[0], &other.keys()[other.size()], &tempKeys[0]);
            if (!other.leaf()) {
                std::move(other.children().begin(), other.children().end(), &tempChildren[0]);
            }

            // this -> other
            std::move(&keys()[0], &keys()[size()], &other.keys()[0]);
            if (!leaf()) {
                std::move(children().begin(), children().end(), &other.children()[0]);
            }

            // local temp -> this
            std::move(&tempKeys[0], &tempKeys[other.size()], &keys()[0]);
            if (!leaf()) {
                std::move(&tempChildren[0], &tempChildren[other.size()+1], &children()[0]);
            }
            std::swap(_metadata,    other._metadata);
        }
         __attribute__((aligned(std::max(alignof(LeafStorage), alignof(InternalStorage)))))
        std::array<uint8_t, (std::max(sizeof(LeafStorage), sizeof(InternalStorage)))>   _data;
        uint64_t                                                                        _metadata   = 0;
    };

    static const uint16_t kTargetSize = 256;
    template<uint8_t N>
    constexpr static uint8_t getInteriorNodeCapacity() {
        if (sizeof(NodeCore<1,N>) <= kTargetSize) { return N; }
        return getInteriorNodeCapacity<N-1>();
    }

    template<>
    constexpr static uint8_t getInteriorNodeCapacity<2>() {
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
    constexpr static uint8_t getLeafNodeCapacity<2>(uint16_t targetSize) {
        return 2;
    }

    constexpr static uint8_t getLeafNodeCapacity() {
        if (sizeof(T) == 1) { return 124; }
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

    struct  const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        const_iterator(const const_iterator& other)
            : _rootNode(other._rootNode), _currentNode(other._currentNode), _currentDepth(other._currentDepth), _indexes(other._indexes) {}

        const_iterator& operator=(const const_iterator& other) {
            auto tmp = other;
            swap(tmp);
            return *this;
        }

        const_iterator(const Node* node, uint8_t depth, std::array<uint8_t, kMaxDepth>& indexes)
                : _rootNode(node), _currentNode(const_cast<Node*>(node)), _currentDepth(depth),  _indexes(indexes) {
            for (auto i = 0; i < depth; ++i) {
                _indexes[i] = indexes[i];
                if (!_currentNode->leaf()) {
                    _currentNode = _currentNode->children()[_indexes[i]];
                } else {
                    // Only allow leafs on the last index
                    assert(i == depth-1);
                }
            }
        }
        std::array<Node*,kMaxDepth> materializeNodes(uint16_t start = 0) const {
            std::array<Node*,kMaxDepth> result;
            if (start == 0) {
                result[0] = const_cast<Node*>(_rootNode);
            }
            for (auto i = std::max<uint16_t>(1, start); i <= _currentDepth; ++i) {
                result[i] = result[i-1]->children()[_indexes[i-1]];
            }
            return result;
        }
        reference operator*() {
            return *&(*_currentNode)[_indexes[_currentDepth]];
        }
        pointer operator->() const {
            return &(*_currentNode)[_indexes[_currentDepth]];
        }

        const_iterator& operator++() {
            ++_indexes[_currentDepth];
            if (_currentNode->leaf()) {
                if (_indexes[_currentDepth] == _currentNode->size()) {
                    auto nodes = materializeNodes();
                    while ((_currentDepth != 0) && _currentNode->size() == _indexes[_currentDepth]) {
                        --_currentDepth;
                        _currentNode = nodes[_currentDepth];
                    }
                }
            } else {
                _currentNode = _currentNode->children()[_indexes[_currentDepth]];
                _currentDepth++;
                _indexes[_currentDepth] = 0;
                while (!_currentNode->leaf()) {
                    _currentNode = *_currentNode->children().begin();
                    _currentDepth++;
                    _indexes[_currentDepth] = 0;
                }
            }
            return *this;
        }
        const_iterator operator++(int) {
           auto tmp = *this;
           ++*this;
           return tmp;
        }

        const_iterator& operator--() {
            if (_currentNode->leaf()) {
                if (_indexes[_currentDepth] != 0) {
                    --_indexes[_currentDepth];
                } else {
                    auto nodes = materializeNodes();
                    while ((_currentDepth != 0) && _indexes[_currentDepth] == 0) {
                        --_indexes[_currentDepth];
                        --_currentDepth;
                        _currentNode = nodes[_currentDepth];
                    }
                    --_indexes[_currentDepth];
                }
            } else {
                while (!_currentNode->leaf()) {
                    _currentNode = _currentNode->children()[_indexes[_currentDepth]];
                    _currentDepth++;
                    _indexes[_currentDepth] = _currentNode->size();
                }
                --_indexes[_currentDepth];
            }
            return *this;
        }
        const_iterator operator--(int) const {
           auto result = *this;
            --*this;
           return result;
        }
        std::strong_ordering operator<=>(const const_iterator& other) const {
            for (auto i = 0; i <= std::min(_currentDepth, other._currentDepth); ++i) {
                if (_indexes[i] < other._indexes[i]) {
                    return std::strong_ordering::less;
                } else if (_indexes[i] > other._indexes[i]) {
                    return std::strong_ordering::greater;
                }
            }
            // The indexes were the same up to this point, see if one set is larger
            if (_currentDepth > other._currentDepth) {
                return std::strong_ordering::less;
            } else if (_currentDepth < other._currentDepth) {
                return std::strong_ordering::greater;
            }
            return std::strong_ordering::equal;
        }
        bool operator==(const const_iterator& other) const {
            return (operator<=>(other) == std::strong_ordering::equal);
        }
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }
private:
        void swap(const_iterator& other) {
            using std::swap;
            swap(_rootNode,     other._rootNode);
            swap(_currentNode,  other._currentNode);
            swap(_currentDepth, other._currentDepth);
            swap(_indexes,      other._indexes);
        }
        // Returns: true if the tree depth decreased, false otherwise
        bool rebalanceFromErasure() {
            contract(_currentNode->leaf());
            auto nodes = materializeNodes();
            for (int8_t i = _currentDepth-1; i >= 0; --i) {
                // If the node has at least pivot() elements then we are done
                if (nodes[i+1]->size() >= nodes[i+1]->pivot()) { break; }
                auto node = nodes[i];               // The node we containing the potentially illegal child node
                auto& index = _indexes[i];          // The index into the node pointing to the potentially illegal child node
                auto& childIndex = _indexes[i+1];   // The index within the potentially illegal child node
                int8_t rightScore = 0;
                int8_t leftScore = 0;
                if (index != node->size()) {
                    rightScore = node->children()[index+1]->size()-node->children()[index+1]->pivot();
                }
                if (index != 0) {
                    leftScore = node->children()[index-1]->size()-node->children()[index-1]->pivot();
                }
                if ((rightScore > 0) && (rightScore >= leftScore)) {
                    // The right node has enough children, rotate them in
                    node->rotateFromRight(index);
                } else if ((leftScore > 0) && (leftScore > rightScore)) {
                    // The left node has enough children, rotate them in
                    uint8_t oldSize = node->children()[index]->size();
                    node->rotateFromLeft(index);
                    // Update the child index since a number of new elements appeared at the beginning of the node
                    childIndex += (node->children()[index]->size() - oldSize);
                } else if (index != node->size()) {
                    // Rotate did not work merge the right node in
                    node->merge(index);
                } else {
                    // Merge the left node into this node
                    // We need to update both index and childIndex
                    childIndex += (node->children()[--index]->size()+1);
                    node->merge(index);
                }
                nodes[i+1] = nodes[i]->children()[_indexes[i]];
            }
            // Okay, we've merged the nodes, now deal with fact that our index path my exceed the bounds
            // of a Node. We handle that by deleting levels off depth from the path until we get to a valid
            // entry which will be successor
            for (int8_t i = _currentDepth; i > 0; --i) {
                if (nodes[i]->size() != _indexes[i]) { break; }
                --_currentDepth;
            }
            _currentNode = nodes[_currentDepth];
            // Finally we need to handle the case where the root node only has a single entry, by making
            // that entry the new root node
            if (!_rootNode->leaf() && _rootNode->size() == 0) {
                auto oldNode = _rootNode->children()[0];
                _rootNode = new ((void*)_rootNode) Node(*oldNode, _rootNode->allocator());
                oldNode->allocator()->deallocate_buffer((void *)oldNode, sizeof(Node), alignof(Node));
                std::move(&_indexes[1], &_indexes[_currentDepth+1],&_indexes[0]);
                if (_currentDepth == 0) {
                    // if _currentDepth == 0 then there was only a single entry then this was end(),
                    // so just set the index to make the iterator == end()
                    _indexes[0] = _rootNode->size();
                } else {
                    --_currentDepth;
                    if (_currentDepth == 0) {
                        // Normally we don't have adjust the node, but since we moved the root node we need to reset it
                        // if we pointed to the drynamically allocated node we copied into the root node
                        _currentNode = nodes[0];
                    }
                }
                return true;
            }
            // Finally, fixup the _currentNode pointer
            return false;
        }

        // The basic algorithm here only works when the index path points to a value in the a leaf node.
        // This can be an issue because sometimes it is possible to have to split an iterator that does not
        // point to such a node (see lower_bound case 3). We can exploit the following properties though:
        //
        // 1. Any index path that points to a non-leaf node will point to a node whose children are leafs
        // 2. Any index path pointing into a non-leaf node can be safely decremented
        // 3. Decrementing such a node will result in the last element of a leaf node
        // 4. Any non-leaf node that needs to be split will need to split the leaf node proceeding it
        //
        // Using that we can simply decrement the iterator if it is a non-leaf node, perform the split algorithm, then increment the pointer
        //
        // Returns: true if the tree depth increased, false otherwise
        bool prepareForInsertion(Allocator& allocator) {
            bool result = false;
            bool isLeaf = _currentNode->leaf();
            if (!isLeaf) { --*this; }
            contract(_currentNode->leaf());
            if (!_currentNode->full()) {
                if (!isLeaf) {++*this; }
                return result;
            }
            auto nodes = materializeNodes();
            int8_t i;
            for (i = _currentDepth; i >= 0; --i) {
                if (!nodes[i]->full()) { break; }
            }
            if (i == -1 && nodes[0]->full()) {
                // The root is full
                Allocator* deallocator = nullptr;
                auto  oldRootBuffer = allocator.allocate_buffer(sizeof(Node), alignof(Node), &deallocator);
                contract(oldRootBuffer.address != nullptr);
                auto oldRootPtr = new (oldRootBuffer.address) Node(*_rootNode, deallocator);
                auto newRootNode = new ((void*)_rootNode) Node(0x00, _rootNode->allocator());
                newRootNode->children()[0] = oldRootPtr;
                ++_currentDepth;
                ++i;
                std::move_backward(&_indexes[0], &_indexes[_currentDepth], &_indexes[_currentDepth+1]);
                std::move_backward(&nodes[1], &nodes[_currentDepth], &nodes[_currentDepth+1]);
                _indexes[0] = 0;
                nodes[1] = oldRootPtr;
                result = true;
            }
            _currentNode = nodes[i];
            contract(!_currentNode->leaf());
            for (; i < _currentDepth; ++i) {
                _currentNode->splitChild(_indexes[i], allocator);
                auto newNode = _currentNode->children()[_indexes[i]];
                if (_indexes[i+1] > newNode->size()) {
                    ++_indexes[i];
                    _indexes[i+1] = _indexes[i+1] - (newNode->size()+1);
                    newNode = _currentNode->children()[_indexes[i]];
                }
                _currentNode = newNode;
            }
            if (!isLeaf) {++*this; }
            return result;
        }
    public:
        friend struct BTree;
        const Node*                     _rootNode;
        Node*                           _currentNode;
        uint8_t                         _currentDepth;
        std::array<uint8_t, kMaxDepth> _indexes = {0};
    };
    using iterator = const_iterator;

    const_iterator cbegin() const {
        std::array<uint8_t, kMaxDepth> indexes = {0};
        if (!_root) {
            return const_iterator(_root, 0, indexes);
        }
        auto node = _root;
        for(auto i = 0; i < kMaxDepth; ++i) {
            indexes[i] = 0;
            if (node->leaf()) { return const_iterator(_root, i, indexes); }
            node = *node->children().begin();
        }
        __builtin_unreachable();
    }

    const_iterator cend() const {
        std::array<uint8_t, kMaxDepth> indexes = {0};
        if (_root) {
            indexes[0] = _root->size();
        } else {
            indexes[0] = 0;
        }
        return const_iterator(_root, 0, indexes);
    }

    const_iterator  begin() const   { return cbegin(); }
    const_iterator  end() const     { return cend(); }
    iterator        begin()         { return std::as_const(*this).begin(); }
    iterator        end()           { return std::as_const(*this).end(); }

    // The lower_bound index path will point to one of the 4 things.
    // 1. An index path directly to an element equal to key
    // 2. An index path to a leaf node that contains the first element greater than the key
    // 3. An index path to an interior node one level above the leaf nodes. This will only happen if the key
    //    is greater than the last elment of the leaf and less then interior key that points to it
    // 4. It be an index path to the last valid node in the tree, with the leaf index incremeneted by one.
    //    this is the index path representation of the end iterator
    //
    // These cases may need to specially handled prepareForInsertion() and rebalanceFromErasure()
    const_iterator lower_bound(const value_type& key) const {
        if (!_root) {
            return end();
        }
        const Node* node = _root;
        std::array<uint8_t, kMaxDepth> indexes = {0};
        for(auto i = 0; i < kMaxDepth; ++i) {
            auto j = std::lower_bound(node->keys().begin(), node->keys().end(), key, _comp);
            if (j == node->keys().end()) {
                if (node->leaf()) {
                    // We hit a leaf but the lower_bound is not inside of it, create
                    // an iterator to the last element in the leaf and increment it;
                    indexes[i] = node->size()-1;
                    return std::next(iterator(_root, i, indexes));
                } else {
                    // There are is one more child then there are keys, so if it larger than the largest key the index is size()
                    indexes[i] = node->size();
                }
            } else {
                indexes[i] = j-(node->keys().begin());
                if (node->leaf() || !_comp(key, *j)) {
                    auto result = iterator(_root, i, indexes);
                    if constexpr(M) {
                        while (result != begin()) {
                            auto previous = std::prev(result);
                            if (_comp(*previous, *result)) { break; }
                            result = previous;
                        }
                    }
                    return result;
                }
            }
            node = node->children()[indexes[i]];
        }
        __builtin_unreachable();
    }

    const_iterator find(const value_type& key) const {
        auto i = lower_bound(key);
        if ((i != end()) && !_comp(key, *i)) { return i; }
        return end();
   }

    iterator lower_bound(const value_type& key) {
        return iterator(std::as_const(*this).lower_bound(key));
    }

    iterator find(const value_type& key) {
        return iterator(std::as_const(*this).find(key));
    }

    std::pair<iterator, bool> insert_internal(iterator&& i, value_type&& key) {
        if (!_root) {
            Allocator* deallocator = nullptr;
            auto newNodeBuffer = _allocator->allocate_buffer(sizeof(Node), alignof(Node), &deallocator);
            _root = new (newNodeBuffer.address) Node(0x01, deallocator);
            i = std::move(lower_bound(key));
        }
        bool rotated = false;

        if constexpr(!M) {
            if ((i != end()) && !_comp(key, *i)) { return { i, false }; }
        }

        if (_size != 0 && (i == end() || !i._currentNode->leaf())) {
            --i;
            rotated = true;
        }
        if (i.prepareForInsertion(*_allocator)) {
            ++_depth;
        }
        uint8_t leafIndex = i._indexes[i._currentDepth];
        if (rotated) {
            ++leafIndex;
        }
        i._currentNode->insert(leafIndex, std::move(key));
        if (rotated) {
            ++i;
        }
        ++_size;
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
        bool swapped = false;
        if (!i._currentNode->leaf()) {
//            fprintf(stderr, "ADJUST\n");
            auto& oldElement = *i;
            ++i;
            std::swap(oldElement, *i);
            swapped = true;
        }
        i._currentNode->erase(i._indexes[i._currentDepth]);
        if (i.rebalanceFromErasure()) {
            --_depth;
        }
        if ((--_size == 0) && _shouldFreeRoot) {
            _root->deallocateChildren();
            _root->allocator()->deallocate_buffer((void*)_root, sizeof(Node), alignof(Node));
            _root = nullptr;
        }
        if (swapped) {
            --i;
        }
        return i;
    }
    size_type erase(const value_type& key) {
        auto i = find(key);
        if constexpr(M) {
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
            _root->deallocateChildren();
            if (_shouldFreeRoot) {
                _root->allocator()->deallocate_buffer((void*)_root, sizeof(Node), alignof(Node));
                _root = nullptr;
            }
        }
        _size = 0;
        _depth = 1;
    }
    size_type count(const value_type& key) const {
        if constexpr(M) {
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
//        dumpVizNodes(&_root);
//        dumpVizEdges(&_root);
//        printf("}\n\n");
//    }
//#endif
    template< class InputIt1,  class InputIt2>
    void bulkConstruct(InputIt1 first, InputIt2 last, Allocator& allocator) {
        contract(size() == 0);
        //TODO: Make this fast someday, by:
        // 1. Sequentialy building full nodes bottom up
        // 2. Taking the right most nodes (which may not be full) and balancing them
        for (auto i = first; i != last; ++i) {
            insert(*i);
        }
    }
    BTree() = default;
    explicit BTree(Allocator& allocator) :  _allocator(&allocator) {}
    explicit BTree(Allocator& allocator, Node* root) :  _root(root), _allocator(&allocator), _shouldFreeRoot(false) {}
    explicit BTree(value_compare comp, Allocator& allocator) : _allocator(&allocator), _comp(comp) {}
    template< class InputIt1,  class InputIt2>
    BTree( InputIt1 first, InputIt2 last, value_compare comp, Allocator& allocator) : BTree(comp, allocator) {
        //TOOO: Replace this with an optimized creation algorithm
        bulkConstruct(first, last, allocator);
    }
    ~BTree() {
        clear();
    }
    BTree(const BTree& other, Allocator& allocator) : BTree(other.begin(), other.end(), value_compare(), allocator) {}
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
private:
    void swap(BTree& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_root,         other._root);
        swap(_allocator,    other._allocator);
        swap(_comp,         other._comp);
        swap(_size,         other._size);
        swap(_depth,        other._depth);
    }
    Node*           _root           = nullptr;
    Allocator*      _allocator      = nullptr;
    value_compare   _comp           = value_compare();
    size_type       _size           = 0;
    uint8_t         _depth          = 1;
    bool            _shouldFreeRoot = true;
};

};

#endif /*  LSL_BTree_h */
