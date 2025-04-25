/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*- vim: ft=cpp et ts=4 sw=4:
 *
 * Copyright (c) 2023 Apple Inc. All rights reserved.
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

#include "PropertyList.h"

#include <bit>
#include <array>
#include <algorithm>

#include "Vector.h"

using lsl::Allocator;
using lsl::Vector;

namespace {
// Figure out how large of integer is needed to store the value
static uint8_t bytesNeededForIntegerValue(uint64_t value) {    // Check to see if value fits by generating an inverse mask of 2^result-1 and see if any bits leak
    for(uint8_t i = 1; i < 8; i<<=1) {
        if ((value & ~((1ULL<<(i*8))-1)) == 0) {
            // No bytes leaked, so we have enough bytes to hold the value
            return i;
        }
    }
    return 8;
}

// Figure out how large of integer is needed to store the value
static uint8_t bytesNeededForIntegerValue(int64_t value) {
    bool negativeValue = value & 0xf000'0000'0000'0000;
    if (!negativeValue) {
        // If the value is positive and the highest bit in the encoding is set it will incorrectly decode as negative
        // to avoid that we bit shift all positive values by 1, that way if the top most bit is set it will over flow
        // into the next byte and increase the encoding size
        value <<= 1;
    }
    return bytesNeededForIntegerValue((uint64_t)value);
}

// A non-allocating quick sort. Hard wired for indirect comparisons
template<class T>
void quickSort(T begin, T end) {
    if (begin == end) {
        return;
    }

    auto pivot = *std::next(begin, (end - begin) / 2);
    auto parition1 = std::partition(begin, end, [pivot](const auto& em) {
        // We are sorting pointers to elements by the value of the element,
        // so we need an extra deref here.
        return *em < *pivot;
    });
    auto parition2 = std::partition(parition1, end, [pivot](const auto& em) {
        // We are sorting pointers to elements by the value of the element,
        // so we need an extra deref here.
        return !(*pivot < *em);
    });

    quickSort(begin, parition1);
    quickSort(parition2, end);
}

template<typename T>
void sortUniqueAndRedirect(Allocator& allocator, Vector<T>& objects, Vector<uint64_t>& offsets, ByteStream& bytes) {
    if (objects.empty()) { return; }

    // Sort the elements
    quickSort(objects.begin(), objects.end());

    uint64_t lastObjectIndex = offsets.size();
    T lastObject = nullptr;

    // We walk through the sorted vector one element at a time. If it matches the lastUniqueObject we redirect it to that, otherwise
    // we set lastUniqueObject to the new object, increment the object index, and emit it to the output stream
    for (auto i = 0; i < objects.size(); ++i) {
        if (lastObject && (*objects[i] == *lastObject)) {
            objects[i]->convertToRedirect(lastObject->index());
        } else {
            objects[i]->setIndex(lastObjectIndex++);
            lastObject = objects[i];
            offsets.push_back(bytes.size());
            lastObject->emit(0, bytes);
        }
    }
}

// bplist00 uses the encoded integer internally in arrays and dictionaries
static void emitPlistEncodedInteger(int64_t _value, ByteStream &bytes) {
    uint8_t size = bytesNeededForIntegerValue(_value);
    bytes.push_back(std::byte{0x10} | std::byte(std::countr_zero(size)));
    bytes.push_back(size, _value);
}

// bplist00 uses the encoded integer internally in arrays and dictionaries
static void emitUnsignedPlistEncodedInteger(uint64_t _value, ByteStream &bytes) {
    uint8_t size = bytesNeededForIntegerValue(_value);
    bytes.push_back(std::byte{0x10} | std::byte(std::countr_zero(size)));
    bytes.push_back(size, _value);
}

};

#pragma mark -
#pragma mark Plist object types

#pragma mark Base Object

void PropertyList::Object::convertToRedirect(uint64_t index) {
    deallocate();
    _isRedirect = true;
    _index = index;
}
void PropertyList::Object::setIndex(uint64_t index) {
    _index = index;
};
uint64_t PropertyList::Object::index() const {
    return _index;
};

PropertyList::Object::Type PropertyList::Object::type() const {
    return Type(_type);
}

bool PropertyList::Object::processed() const {
    return _processed;
}

void PropertyList::Object::setProcessed() {
    _processed = true;
}

#pragma mark Integer

PropertyList::Integer::Integer(int64_t value) : Object(Type::Integer), _value(value) {}
PropertyList::Integer::Integer(Allocator& allocator, int64_t value) : Object(Type::Integer), _value(value) {}

bool PropertyList::Integer::operator==(const PropertyList::Integer& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

void PropertyList::Integer::emit(uint8_t objectIndexSize, ByteStream& bytes) {
    // int    0001 0nnn    ...        // # of bytes is 2^nnn, big-endian bytes
    emitPlistEncodedInteger(_value, bytes);

}
void PropertyList::Integer::deallocate() {}

std::strong_ordering PropertyList::Integer::operator<=>(const Integer& other) const {
    return (_value <=> other._value);
}

#pragma mark Data

void PropertyList::Data::emit(uint8_t objectIndexSize, ByteStream& bytes) {
    // data    0100 nnnn    [int]    ...    // nnnn is number of bytes unless 1111 then int count follows, followed by bytes
    uint64_t size = _value.size();
    if (size < 15) {
        bytes.push_back(std::byte{0x40} | (std::byte)size);
    } else {
        bytes.push_back(std::byte{0x4f});
        emitUnsignedPlistEncodedInteger(size, bytes);
    }
    std::copy(_value.begin(), _value.end(), std::back_inserter(bytes));
}

void PropertyList::Data::deallocate() {
    _value.resize(0);
}

std::strong_ordering PropertyList::Data::operator<=>(const PropertyList::Data& other) const {
    std::strong_ordering order = _value.size() <=> other._value.size();
    if (order == std::strong_ordering::equal) {
        order = (memcmp((void*)_value.data(), (void*)other._value.data(),  (size_t)_value.size()) <=> 0);
    }
    return order;
}

bool PropertyList::Data::operator==(const Data& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

PropertyList::Data::Data(Allocator& allocator,  uint64_t size) : Object(Type::Data), _value(allocator) {
    _value.insert(_value.begin(), size, std::byte{0});
}

PropertyList::Data::Data(Allocator& allocator, std::span<std::byte> value) : Object(Type::Data), _value(allocator) {
    _value.insert(_value.begin(), value.begin(), value.end());
}

std::span<std::byte> PropertyList::Data::bytes() {
    return _value;
}


#pragma mark String

bool PropertyList::String::emitUnicode(uint8_t objectIndexSize, uint64_t stringSize, ByteStream& bytes) const {
    // string    0110 nnnn    [int]    ...    // Unicode string, nnnn is # of chars, else 1111 then int count, then big-endian 2-byte uint16_t
    bool foundUnicode = false;
    for(auto i = 0; i < stringSize; ++i) {
        if (((uint8_t*)_value)[i] & 0x80) {
            foundUnicode = true;
            break;
        }
    }
    if (!foundUnicode) {
        return false;
    }

    // We have a UTF-8 string, from which we cannot tell how many UTF-8 chaarcters will result. So we decode the whole thing into a
    // vector then emit it. If the UTF-8 is malformed we bail out here and pass it through as a malformed ASCII string. The decoder
    // has to deal with malformed content anyway, and that way we have the data intact data in the atlas so we can inspect
    // manually if necessary.
    #define CHECK_UTF8_INTERMEDIATY_BYTE(x) \
        if (((x) & 0xc0) != 0x80) { return false;}
    Vector<uint16_t> utf16chars(bytes.allocator());
    for(auto i = 0; i < stringSize; ++i) {
        uint16_t value = ((uint8_t*)_value)[i];
        if ((value & 0x80) == 0x00) {
            // 1 byte
            utf16chars.push_back(value);
        } else if ((value & 0xe0) == 0xc0) {
            // 2 bytes
            if (i+1 >= stringSize) { return false; }
            uint16_t value2 = ((uint8_t*)_value)[i+1];
            CHECK_UTF8_INTERMEDIATY_BYTE(value2);
            utf16chars.push_back(((value & 0x1f) << 6)
                                 | (value2 & 0x3f));
            i += 1;
        } else if ((value & 0xf0) == 0xe0) {
            // 3 bytes
            if (i+2 >= stringSize) { return false; }
            uint16_t value2 = ((uint8_t*)_value)[i+1];
            uint16_t value3 = ((uint8_t*)_value)[i+2];
            CHECK_UTF8_INTERMEDIATY_BYTE(value2);
            CHECK_UTF8_INTERMEDIATY_BYTE(value3);
            utf16chars.push_back(((value & 0x1f) << 12)
                                 | ((value2 & 0x3f) << 6)
                                 | ((value3 & 0x3f)));
                                 i += 2;
        } else if ((value & 0xf8) == 0xf0) {
            // 4 bytes
            if (i+3 >= stringSize) { return false; }
            uint32_t value1 = value;
            uint32_t value2 = ((uint8_t*)_value)[i+1];
            uint32_t value3 = ((uint8_t*)_value)[i+2];
            uint32_t value4 = ((uint8_t*)_value)[i+3];
            CHECK_UTF8_INTERMEDIATY_BYTE(value2);
            CHECK_UTF8_INTERMEDIATY_BYTE(value3);
            CHECK_UTF8_INTERMEDIATY_BYTE(value4);

            value1 = (value1 & 0x07) << 18;
            value1 |= (value2 & 0x3f) << 12;
            value1 |= (value3 & 0x3f) << 6;
            value1 |= (value4 & 0x3f);
            value1 -= 0x10000;

            uint16_t highSurrogate = ((uint16_t)((value1>>10) & 0x03ff) + 0xd800);
            uint16_t lowSurrogate = (uint16_t)((value1 & 0x03ff) + 0xdc00);
            utf16chars.push_back(highSurrogate);
            utf16chars.push_back(lowSurrogate);
            i += 3;
        }
    }

    if (utf16chars.size() < 15) {
        bytes.push_back(std::byte{0x60} | (std::byte)utf16chars.size());
    } else {
        bytes.push_back(std::byte{0x6f});
        PropertyList::Integer encodedSize(utf16chars.size());
        encodedSize.emit(objectIndexSize, bytes);
    }
    for (auto& utf16char : utf16chars) {
        bytes.push_back((uint16_t)utf16char);
    }
    return true;
}

void PropertyList::String::emit(uint8_t objectIndexSize, ByteStream& bytes) {
    uint64_t size = strlen(_value);

    if (emitUnicode(objectIndexSize, size, bytes)) {
        // The string was unicode and has been emitted, return
        return;
    }
    // string    0101 nnnn    [int]    ...    // ASCII string, nnnn is # of chars, else 1111 then int count, then bytes
    if (size < 15) {
        bytes.push_back(std::byte{0x50} | (std::byte)size);
    } else {
        bytes.push_back(std::byte{0x5f});
        PropertyList::Integer encodedSize(size);
        encodedSize.emit(objectIndexSize, bytes);
    }
    std::copy((std::byte*)_value, (std::byte*)_value+size, std::back_inserter(bytes));
}

void PropertyList::String::deallocate() {
    if (_isRedirect) { return; }
    Allocator::freeObject((void*)_value);
    _value = nullptr;
}

std::strong_ordering PropertyList::String::operator<=>(const PropertyList::String& other) const {
    return (strcmp(_value, other._value) <=> 0);
}
bool PropertyList::String::operator==(const String& other) const {
    return (*this <=> other) == std::strong_ordering::equal;
}

PropertyList::String::String(Allocator& allocator, std::string_view value) : Object(Type::String) {
    char* p = (char*)allocator.malloc(value.size()+1);
    memcpy(p, value.data(), value.size());
    p[value.size()] = '\0';
    _value = p;
}

PropertyList::String::~String() {
    if (_isRedirect) { return; }
    if (_value) {
        Allocator::freeObject((void*)_value);
    }
}

#pragma mark  Array

PropertyList::Array::Array(Allocator& allocator) : Object(Type::Array), _values(allocator) {}

std::span<PropertyList::Object*> PropertyList::Array::values() {
    return _values;
}

void  PropertyList::Array::emit(uint8_t objectIndexSize, ByteStream& bytes) {
    // 1010 nnnn    [int]    objref*    // nnnn is count, unless '1111', then int count follows
    uint64_t size = _values.size();
    if (size < 15) {
        bytes.push_back(std::byte{0xa0} | (std::byte)size);
    } else {
        bytes.push_back(std::byte{0xaf});
        emitUnsignedPlistEncodedInteger(size, bytes);
    }
    for (auto i = 0; i < size; ++i) {
        bytes.push_back(objectIndexSize, _values[i]->index());
    }
}

void PropertyList::Array::deallocate() {
    if (_isRedirect) { return; }
    for (auto element : _values) {
        element->deallocate();
    }
}

#pragma mark Dictionary

PropertyList::Dictionary::Dictionary(Allocator& allocator) : Object(Type::Dictionary), _keys(allocator), _values(allocator) {}

std::span<PropertyList::Object*> PropertyList::Dictionary::keys() {
    return _keys;
}
std::span<PropertyList::Object*> PropertyList::Dictionary::values() {
    return _values;
}

void PropertyList::Dictionary::emit(uint8_t objectIndexSize, ByteStream& bytes) {
    //dict    1101 nnnn    [int]    keyref* objref*    // nnnn is count, unless '1111', then int count follows
    uint64_t size = _keys.size();
    if (size < 15) {
        bytes.push_back(std::byte{0xd0} | (std::byte)size);
    } else {
        bytes.push_back(std::byte{0xdf});
        PropertyList::Integer encodedSize(size);
        encodedSize.emit(objectIndexSize, bytes);
    }
    for (auto i = 0; i < size; ++i) {
        bytes.push_back(objectIndexSize, _keys[i]->index());
    }
    for (auto i = 0; i < size; ++i) {
        bytes.push_back(objectIndexSize, _values[i]->index());
    }
}

void PropertyList::Dictionary::deallocate() {
    if (_isRedirect) { return; }
    for (auto element : _keys) {
        element->deallocate();
    }
    for (auto element : _values) {
        element->deallocate();
    }
}

#pragma mark UUID

PropertyList::UUID::UUID(Allocator& allocator, uuid_t uuid) : Data(allocator, std::span((std::byte*)&uuid[0], 16)) {}

#pragma mark Botmap

// Need to round up to the next multiple 8 since we used 8 bit bytes
PropertyList::Bitmap::Bitmap(Allocator& allocator, uint64_t size) : PropertyList::Data(allocator, ((size + 7) & (-8))/8) {}
void PropertyList::Bitmap::setBit(uint64_t bit) {
    assert(bit < bytes().size()*8);
    std::byte* bitmap = bytes().data();
    std::byte* byte = &bitmap[bit/8];
    (*byte) |= (std::byte)(1<<(bit%8));
}

#pragma mark -

// This a WRITEONLY plist implementation. There is no way to query objects in it. It also has a number of other
// limitations, such as only usng strings for keys, etc. Its goal is to work in the dyld runtime environment
// with enough functionality to emit the process info and nothing more.

PropertyList::PropertyList(Allocator& allocator) :  _allocator(allocator), _rootDictionary(allocator) {}

void PropertyList::encode(ByteStream& bytes) {
    Vector<uint64_t>        offsets(bytes.allocator());
    uint64_t                offsetTableOffset   = 0;
    uint64_t                numObjects          = 0;
    uint64_t                topObject           = 0;
    uint8_t                 offsetSize          = 0;
    uint8_t                 objectIndexSize     = 0;

    Vector<String*>     strings(_allocator);
    Vector<Integer*>    integers(_allocator);
    Vector<Data*>       datas(_allocator);
    Vector<Object*>     collections(_allocator);
    Vector<Object*>     objectsToProcess(_allocator);

    // First we sort out all the integers, strings, data for uniquing, while pull out the collections to flatten
    objectsToProcess.push_back(&_rootDictionary);
    while(!objectsToProcess.empty()) {
        Vector<Object*> newObjects(_allocator);
        for (auto i : objectsToProcess) {
            if (i->processed()) { continue; }
            i->setProcessed();
            switch(i->type()) {
                case Object::Type::String:
                    strings.push_back(reinterpret_cast<String*>(i));
                    break;
                case Object::Type::Integer:
                    integers.push_back(reinterpret_cast<Integer*>(i));
                    break;
                case Object::Type::Data:
                    datas.push_back(reinterpret_cast<Data*>(i));
                    break;
                case Object::Type::Array: {
                    Array* array = reinterpret_cast<Array*>(i);
                    collections.push_back(i);
                    std::copy(array->values().begin(), array->values().end(), std::back_inserter(newObjects));
                } break;
                case Object::Type::Dictionary: {
                    Dictionary* dict = reinterpret_cast<Dictionary*>(i);
                    collections.push_back(i);
                    // Since this is a collection its children need to be processed, add them to newObjects so they will be handled,
                    // next time we loop around.
                    std::copy(dict->keys().begin(), dict->keys().end(), std::back_inserter(newObjects));
                    std::copy(dict->values().begin(), dict->values().end(), std::back_inserter(newObjects));
                } break;
            }
        }
        objectsToProcess = newObjects;
    }

    //Write header
    bytes.setEndian(ByteStream::Endian::Big);
    bytes.push_back("bplist00");

    // Sort, unique, and write out each type
    sortUniqueAndRedirect(_allocator, strings, offsets, bytes);
    sortUniqueAndRedirect(_allocator, integers, offsets, bytes);
    sortUniqueAndRedirect(_allocator, datas, offsets, bytes);

    topObject = offsets.size();
    numObjects = offsets.size() + collections.size();
    objectIndexSize = bytesNeededForIntegerValue(numObjects);

    //emit collections
    uint64_t currentIndex = offsets.size();
    for (auto i : collections) {
        i->setIndex(currentIndex++);
    }

    //emit collections
    for (auto i : collections) {
        offsets.push_back(bytes.size());
        i->emit(objectIndexSize, bytes);
    }

    offsetTableOffset = bytes.size();
    offsetSize = bytesNeededForIntegerValue(offsetTableOffset);

    for (auto i = 0; i < offsets.size(); ++i) {
        bytes.push_back(offsetSize, offsets[i]);
    }

    // Write trailer
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)0x0);
    bytes.push_back((uint8_t)offsetSize);
    bytes.push_back((uint8_t)objectIndexSize);

    bytes.push_back((uint64_t)numObjects);
    bytes.push_back((uint64_t)topObject); // Root dictionary is always the first collection
    bytes.push_back((uint64_t)offsetTableOffset);
}

PropertyList::Dictionary& PropertyList::rootDictionary() {
    return _rootDictionary;
}

