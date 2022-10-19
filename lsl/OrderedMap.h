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

#ifndef  LSL_OrderedMap_h
#define  LSL_OrderedMap_h

#include "BTree.h"

namespace lsl {

template<typename K, typename T, class C=std::less<K>>
struct TRIVIAL_ABI OrderedMap {
    using key_type          = K;
    using mapped_type       = T;
    using value_type        = std::pair<const key_type,mapped_type>;
    using key_compare       = C;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using pointer           = value_type*;
    using size_type         = std::size_t;
private:
    // So this is gross. The issue is that the value_types can't be copied because of the const key_type.
    // std::map handles that by constructing in place and never moving the pairs, but that does not work
    // for a B+Tree. Instead we use an internal type so we can move things around as necessary, and just
    // cast it to the value_type before hand it to users so they don't mutate the key and invalidate the
    // the tree.
    using internal_value_type = std::pair<key_type,mapped_type>;
public:
    struct value_compare {
        bool operator()(const value_type& lhs, const value_type& rhs) const {
            return _comp(lhs.first, rhs.first);
        }
    private:
        friend struct OrderedMap;
        value_compare( key_compare Comp ) : _comp(Comp) {}
        key_compare _comp;
    };
    struct const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const key_type,mapped_type>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        const_iterator(const const_iterator& other) : _i(other._i) {}
        const_iterator(const_iterator&& other) : _i(other._i) {
            swap(other);
        }
        const_iterator& operator=(const const_iterator& other) {
            auto tmp = other;
            swap(tmp);
            return *this;
        }
        const_iterator& operator=(const_iterator&& other) {
            swap(other);
            return *this;
        }
        reference operator*() {
            return *((pointer)&*_i);
        }
         pointer operator->() {
             // So this is gross. std::map avoid this by never moving
             return (pointer)&*_i;
         }
         const_iterator& operator++() {
             ++_i;
             return *this;
         }
         const_iterator operator++(int) {
             auto tmp = *this;
             ++*this;
             return tmp;
         }
         const_iterator& operator--() {
             --_i;
             return *this;
         }
         const_iterator operator--(int) const {
             auto result = *this;
             --*this;
             return result;
         }
        std::strong_ordering operator<=>(const const_iterator& other) const = default;
        const_iterator(typename BTree<internal_value_type, value_compare,false>::const_iterator I) : _i(I) {}
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }
    private:
        friend struct OrderedMap;
        void swap(const_iterator& other) {
            using std::swap;
            if (this == &other) { return; }
            swap(_i, other._i);
        }
        typename BTree<internal_value_type,value_compare,false>::const_iterator _i;
    };
    using iterator = const_iterator;

    mapped_type& operator[]( const key_type& key ) {
        auto i = find(key);
        if (i == end()) {
            auto j = insert({key, mapped_type()});
            i = j.first;
        }
        return i->second;
    }

    const_iterator              cbegin() const  { return _btree.begin(); }
    const_iterator              cend() const    { return _btree.end();}
    const_iterator              begin() const   { return cbegin(); }
    const_iterator              end() const     { return cend(); }
    iterator                    begin()         { return std::as_const(*this).begin(); }
    iterator                    end()           { return std::as_const(*this).end(); }

    iterator                    insert(const_iterator hint, const value_type& key)  { return _btree.insert(hint._i, key).first; }
    iterator                    insert(const_iterator hint, value_type&& key)       { return _btree.insert(hint._i, std::move(key)).first; }
    std::pair<iterator,bool>    insert(const value_type& key)                       { return _btree.insert(key); }
    std::pair<iterator,bool>    insert(value_type&& key)                            { return _btree.insert(std::move(key)); }

    const_iterator              find(const key_type& key) const                     { return _btree.find({key, mapped_type()}); }
    iterator                    find(const key_type& key)                           { return iterator(std::as_const(*this).find(key)); }
    const_iterator              lower_bound(const key_type& key) const              { return _btree.lower_bound({key, mapped_type()}); }
    iterator                    lower_bound(const key_type& key)                    { return iterator(std::as_const(*this).lower_bound(key)); }
    iterator                    erase(iterator i)                                   { return _btree.erase(i._i); }
    size_type                   erase(const key_type& key)                          { return _btree.erase({key, mapped_type()}); }

    size_type                   size() const                                        { return _btree.size(); }
    bool                        empty() const                                       { return _btree.empty(); }
    void                        clear()                                             { return _btree.clear(); }
    size_type                   count(const key_type& key) const                    { return _btree.count({key, mapped_type()}); }

    OrderedMap() = delete;
//    OrderedMap(const OrderedMap&); PRIVATE
    explicit OrderedMap(key_compare comp, Allocator& allocator) : _btree(value_compare(comp), allocator) {}
    explicit OrderedMap(Allocator& allocator) :  OrderedMap(key_compare(), allocator) {}
    OrderedMap(OrderedMap&& other) {
        swap(other);
    }
    OrderedMap& operator=(const OrderedMap& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    OrderedMap& operator=(OrderedMap&& other) {
        swap(other);
        return *this;
    }
    friend void swap(OrderedMap& x, OrderedMap& y) {
        x.swap(y);
    }
private:
    OrderedMap(const OrderedMap& other) : _btree(other._btree) {}
    void swap(OrderedMap& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_btree, other._btree);
    }
    value_compare value_comp() const { return value_compare(); }
    BTree<internal_value_type,value_compare,false> _btree;
};

template<typename K, typename T, class C=std::less<K>>
struct TRIVIAL_ABI OrderedMultiMap {
    using key_type          = K;
    using mapped_type       = T;
    using value_type        = std::pair<const key_type,mapped_type>;
    using key_compare       = C;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using pointer           = value_type*;
    using size_type         = std::size_t;
private:
    // So this is gross. The issue is that the value_types can't be copied because of the const key_type.
    // std::map handles that by constructing in place and never moving the pairs, but that does not work
    // for a B+Tree. Instead we use an internal type so we can move things around as necessary, and just
    // cast it to the value_type before hand it to users so they don't mutate the key and invalidate the
    // the tree.
    using internal_value_type = std::pair<key_type,mapped_type>;
public:
    struct value_compare {
        bool operator()(const value_type& lhs, const value_type& rhs) const {
            return _comp(lhs.first, rhs.first);
        }
    private:
        friend struct OrderedMultiMap;
        value_compare( key_compare Comp ) : _comp(Comp) {}
        key_compare _comp;
    };
    struct const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = std::pair<const key_type,mapped_type>;
        using difference_type   = std::ptrdiff_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        const_iterator(const const_iterator& other) : _i(other._i) {}
        const_iterator(const_iterator&& other) : _i(other._i) {
            swap(other);
        }
        const_iterator& operator=(const const_iterator& other) {
            auto tmp = other;
            swap(tmp);
            return *this;
        }
        const_iterator& operator=(const_iterator&& other) {
            swap(other);
            return *this;
        }
        reference operator*() {
            return *((pointer)&*_i);
        }
         pointer operator->() {
             // So this is gross. std::map avoid this by never moving
             return (pointer)&*_i;
         }
         const_iterator& operator++() {
             ++_i;
             return *this;
         }
         const_iterator operator++(int) {
             auto tmp = *this;
             ++*this;
             return tmp;
         }
         const_iterator& operator--() {
             --_i;
             return *this;
         }
         const_iterator operator--(int) const {
             auto result = *this;
             --*this;
             return result;
         }
        std::strong_ordering operator<=>(const const_iterator& other) const = default;
        const_iterator(typename BTree<internal_value_type, value_compare,true>::const_iterator I) : _i(I) {}
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }
    private:
        friend struct OrderedMultiMap;
        void swap(const_iterator& other) {
            using std::swap;
            if (this == &other) { return; }
            swap(_i, other._i);
        }
        typename BTree<internal_value_type,value_compare,true>::const_iterator _i;
    };
    using iterator = const_iterator;

    const_iterator              cbegin() const  { return _btree.begin(); }
    const_iterator              cend() const    { return _btree.end();}
    const_iterator              begin() const   { return cbegin(); }
    const_iterator              end() const     { return cend(); }
    iterator                    begin()         { return std::as_const(*this).begin(); }
    iterator                    end()           { return std::as_const(*this).end(); }

    iterator                    insert(const_iterator hint, const value_type& key)  { return _btree.insert(hint._i, key).first; }
    iterator                    insert(const_iterator hint, value_type&& key)       { return _btree.insert(hint._i, std::move(key)).first; }
    iterator                    insert(const value_type& key)                       { return _btree.insert(key).first; }
    iterator                    insert(value_type&& key)                            { return _btree.insert(std::move(key)).first; }

    const_iterator              find(const key_type& key) const                     { return _btree.find({key, mapped_type()}); }
    iterator                    find(const key_type& key)                           { return iterator(std::as_const(*this).find(key)); }
    const_iterator              lower_bound(const key_type& key) const              { return _btree.lower_bound({key, mapped_type()}); }
    iterator                    lower_bound(const key_type& key)                    { return iterator(std::as_const(*this).lower_bound(key)); }
    iterator                    erase(iterator i)                                   { return _btree.erase(i._i); }
    size_type                   erase(const key_type& key)                          { return _btree.erase({key, mapped_type()}); }

    size_type                   size() const                                        { return _btree.size(); }
    bool                        empty() const                                       { return _btree.empty(); }
    void                        clear()                                             { return _btree.clear(); }
    size_type                   count(const key_type& key) const                    { return _btree.count({key, mapped_type()}); }

    OrderedMultiMap() = delete;
    //OrderedMultiMap(const OrderedMultiMap&); PRIVATE
    explicit OrderedMultiMap(key_compare comp, Allocator& allocator) : _btree(value_compare(comp), allocator) {}
    explicit OrderedMultiMap(Allocator& allocator) :  OrderedMultiMap(key_compare(), allocator) {}
    OrderedMultiMap(OrderedMultiMap&& other) {
        swap(other);
    }
    OrderedMultiMap& operator=(const OrderedMultiMap& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    OrderedMultiMap& operator=(OrderedMultiMap&& other) {
        swap(other);
        return *this;
    }
    friend void swap(OrderedMultiMap& x, OrderedMultiMap& y) {
        x.swap(y);
    }
private:
    OrderedMultiMap(const OrderedMultiMap& other) : _btree(other._btree) {}
    void swap(OrderedMultiMap& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_btree, other._btree);
    }
    value_compare value_comp() const { return value_compare(); }
    BTree<internal_value_type,value_compare,true> _btree;
};


};
#endif /*  LSL_OrderedMap_h */
