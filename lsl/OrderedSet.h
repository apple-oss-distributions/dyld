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

#ifndef  LSL_OrderedSet_h
#define  LSL_OrderedSet_h

#include "BTree.h"

namespace lsl {

struct ConstCharStarCompare {
    bool operator() (const char* x, const char *y) const {
        for (size_t i = 0;; ++i) {
            if (x[i] < y[i]) {
                return true;
            } else if (x[i] > y[i]) {
                return false;
            } else if (y[i] == 0) {
                return false;
            } else if (x[i] == 0) {
                return true;
            }
        }
    }
};

template<typename T, class C=std::less<T>>
struct TRIVIAL_ABI OrderedSet {
    using key_type          = T;
    using value_type        = T;
    using key_compare       = C;
    using value_compare     = C;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using pointer           = value_type*;
    using size_type         = std::size_t;
    struct const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
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
            return *_i;
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
        const_iterator(typename BTree<T,C,false>::const_iterator I) : _i(I) {}
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }
    private:
        friend struct OrderedSet;
        void swap(const_iterator& other) {
            using std::swap;
            if (this == &other) { return; }
            swap(_i, other._i);
        }
        typename BTree<T,C,false>::const_iterator _i;
    };
    using iterator = const_iterator;

    const_iterator              cbegin() const                                      { return _btree.begin(); }
    const_iterator              cend() const                                        { return _btree.end();}
    const_iterator              begin() const                                       { return cbegin(); }
    const_iterator              end() const                                         { return cend(); }
    iterator                    begin()                                             { return std::as_const(*this).begin(); }
    iterator                    end()                                               { return std::as_const(*this).end(); }

    iterator                    insert(const_iterator hint, const value_type& key)  { return _btree.insert(hint._i, key).first; }
    iterator                    insert(const_iterator hint, value_type&& key)       { return _btree.insert(hint._i, std::move(key)).first; }
    std::pair<iterator,bool>    insert(const value_type& key)                       { return _btree.insert(key); }
    std::pair<iterator,bool>    insert(value_type&& key)                            { return _btree.insert(std::move(key)); }

    const_iterator              find(const key_type& key) const                     { return _btree.find(key); }
    iterator                    find(const value_type& key)                         { return iterator(std::as_const(*this).find(key)); }
    const_iterator              lower_bound(const value_type& key) const            { return _btree.lower_bound(key); }
    iterator                    lower_bound(const value_type& key)                  { return iterator(std::as_const(*this).lower_bound(key)); }
    iterator                    erase(iterator i)                                   { return _btree.erase(i._i); }
    size_type                   erase(const value_type& key)                        { return _btree.erase(key); }

    size_type                   size() const                                        { return _btree.size(); }
    bool                        empty() const                                       { return _btree.empty(); }
    void                        clear()                                             { return _btree.clear(); }
    size_type                   count(const value_type& key) const                  { return _btree.count(); }

    OrderedSet() = delete;
    //OrderedSet(const OrderedSet& other); //PRIVATE
    explicit OrderedSet(Allocator& allocator) :  _btree(allocator) {}
    explicit OrderedSet(value_compare comp, Allocator& allocator) :  _btree(comp, allocator) {}
    template< class InputIt1,  class InputIt2>
    OrderedSet( InputIt1 first, InputIt2 last, value_compare comp, Allocator& allocator) : _btree(first, last, comp, allocator) {}
    template< class InputIt1,  class InputIt2>
    OrderedSet( InputIt1 first, InputIt2 last, Allocator& allocator) : _btree(first, last, value_compare(C()), allocator) {}
    OrderedSet(const OrderedSet& other, Allocator& allocator) : _btree(other._btree, allocator) {}
    OrderedSet(OrderedSet&& other) {
        swap(other);
    }
    OrderedSet& operator=(const OrderedSet& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    OrderedSet& operator=(OrderedSet&& other) {
        swap(other);
        return *this;
    }
    friend void swap(OrderedSet& x, OrderedSet& y) {
        x.swap(y);
    }
private:
    OrderedSet(const OrderedSet& other) : _btree(other._btree) {}
    void swap(OrderedSet& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_btree, other._btree);
    }
    BTree<T,C, false> _btree;
};

template<typename T, class C=std::less<T>>
struct TRIVIAL_ABI OrderedMultiSet {
    using key_type          = T;
    using value_type        = T;
    using key_compare       = C;
    using value_compare     = C;
    using difference_type   = std::ptrdiff_t;
    using reference         = value_type&;
    using pointer           = value_type*;
    using size_type         = std::size_t;
    struct const_iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
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
            return *_i;
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
        const_iterator(typename BTree<T,C,true>::const_iterator I) : _i(I) {}
        friend void swap(const_iterator& x, const_iterator& y) {
            x.swap(y);
        }
    private:
        friend struct OrderedMultiSet;
        void swap(const_iterator& other) {
            using std::swap;
            if (this == &other) { return; }
            swap(_i, other._i);
        }
        typename BTree<T,C,true>::const_iterator _i;
    };
    using iterator = const_iterator;

    const_iterator              cbegin() const                                      { return _btree.begin(); }
    const_iterator              cend() const                                        { return _btree.end();}
    const_iterator              begin() const                                       { return cbegin(); }
    const_iterator              end() const                                         { return cend(); }
    iterator                    begin()                                             { return std::as_const(*this).begin(); }
    iterator                    end()                                               { return std::as_const(*this).end(); }

    iterator                    insert(const_iterator hint, const value_type& key)  { return _btree.insert(hint._i, key).first; }
    iterator                    insert(const value_type& key)                       { return _btree.insert(key).first; }
    iterator                    insert(const_iterator hint, value_type&& key)       { return _btree.insert(hint._i, std::move(key)).first; }
    iterator                    insert(value_type&& key)                            { return _btree.insert(std::move(key)).first; }

    const_iterator              find(const key_type& key) const                     { return _btree.find(key); }
    iterator                    find(const value_type& key)                         { return iterator(std::as_const(*this).find(key)); }
    const_iterator              lower_bound(const value_type& key) const            { return _btree.lower_bound(key); }
    iterator                    lower_bound(const value_type& key)                  { return iterator(std::as_const(*this).lower_bound(key)); }
    iterator                    erase(iterator i)                                   { return _btree.erase(i._i); }
    size_type                   erase(const value_type& key)                        { return _btree.erase(key); }

    size_type                   size() const                                        { return _btree.size(); }
    bool                        empty() const                                       { return _btree.empty(); }
    void                        clear()                                             { return _btree.clear(); }
    size_type                   count(const value_type& key) const                  { return _btree.count(key); }

    OrderedMultiSet() = delete;
    //OrderedMultiSet(const OrderedMultiSet&) // PRIVATE
    explicit OrderedMultiSet(Allocator& allocator) :  _btree(allocator) {}
    explicit OrderedMultiSet(value_compare comp, Allocator& allocator) :  _btree(comp, allocator) {}
    template< class InputIt1,  class InputIt2>
    OrderedMultiSet( InputIt1 first, InputIt2 last, value_compare comp, Allocator& allocator) : _btree(first, last, comp, allocator) {}
    template< class InputIt1,  class InputIt2>
    OrderedMultiSet( InputIt1 first, InputIt2 last, Allocator& allocator) : _btree(first, last, value_compare(C()), allocator) {}
    OrderedMultiSet(const OrderedMultiSet& other, Allocator& allocator) : _btree(other, allocator) {}
    OrderedMultiSet(OrderedMultiSet&& other) {
        swap(other);
    }
    OrderedMultiSet& operator=(const OrderedMultiSet& other) {
        auto tmp = other;
        swap(tmp);
        return *this;
    }
    OrderedMultiSet& operator=(OrderedMultiSet&& other) {
        swap(other);
        return *this;
    }
    friend void swap(OrderedMultiSet& x, OrderedMultiSet& y) {
        x.swap(y);
    }
private:
    OrderedMultiSet(const OrderedMultiSet& other) : _btree(other._btree) {}
    void swap(OrderedMultiSet& other) {
        using std::swap;
        if (this == &other) { return; }
        swap(_btree, other._btree);
    }
    BTree<T,C, true> _btree;
};

};

#endif /*  LSL_OrderedSet_h */
