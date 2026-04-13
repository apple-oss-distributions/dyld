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


#ifndef StringUtils_h
#define StringUtils_h

#include <algorithm>
#include <string>

inline bool startsWith(const std::string& str, const std::string& prefix)
{
    return std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
}

inline bool startsWith(const std::string_view& str, const std::string_view& prefix)
{
    return std::mismatch(prefix.begin(), prefix.end(), str.begin()).first == prefix.end();
}

inline bool startsWith(const char* str, const char* prefix)
{
    return startsWith(std::string_view(str), std::string_view(prefix));
}

inline bool endsWith(const std::string& str, const std::string& suffix)
{
    std::size_t index = str.find(suffix, str.size() - suffix.size());
    return (index != std::string::npos);
}

inline bool endsWith(const std::string_view& str, const std::string_view& suffix)
{
    std::size_t index = str.find(suffix, str.size() - suffix.size());
    return (index != std::string::npos);
}

inline bool contains(const std::string& str, const std::string& search)
{
    std::size_t index = str.find(search);
    return (index != std::string::npos);
}

inline char hexDigit(uint8_t value)
{
    if ( value < 10 )
        return '0' + value;
    else
        return 'a' + value - 10;
}

inline void bytesToHex(const uint8_t* bytes, size_t byteCount, char buffer[])
{
    char* p = buffer;
    for (size_t i=0; i < byteCount; ++i) {
        *p++ = hexDigit(bytes[i] >> 4);
        *p++ = hexDigit(bytes[i] & 0x0F);
    }
    *p++ =  '\0';
}

inline void putHexNibble(uint8_t value, char*& p)
{
    if ( value < 10 )
        *p++ = '0' + value;
    else
        *p++ = 'A' + value - 10;
}

inline void putHexByte(uint8_t value, char*& p)
{
    value &= 0xFF;
    putHexNibble(value >> 4,   p);
    putHexNibble(value & 0x0F, p);
}

inline bool hexCharToUInt(const char hexByte, uint8_t& value) {
    if (hexByte >= '0' && hexByte <= '9') {
        value = hexByte - '0';
        return true;
    } else if (hexByte >= 'A' && hexByte <= 'F') {
        value = hexByte - 'A' + 10;
        return true;
    } else if (hexByte >= 'a' && hexByte <= 'f') {
        value = hexByte - 'a' + 10;
        return true;
    }

    return false;
}

inline bool hexCharToByte(const char hexByte, uint8_t& value)
{
    if ( hexByte >= '0' && hexByte <= '9' ) {
        value = hexByte - '0';
        return true;
    }
    else if ( hexByte >= 'A' && hexByte <= 'F' ) {
        value = hexByte - 'A' + 10;
        return true;
    }
    else if ( hexByte >= 'a' && hexByte <= 'f' ) {
        value = hexByte - 'a' + 10;
        return true;
    }

    return false;
}

inline uint64_t hexToUInt64(const char* startHexByte, const char** endHexByte) {
    const char* scratch;
    if (endHexByte == nullptr) {
        endHexByte = &scratch;
    }
    if (startHexByte == nullptr)
        return 0;
    uint64_t retval = 0;
    if (startHexByte[0] == '0' &&  startHexByte[1] == 'x') {
        startHexByte +=2;
    }
    *endHexByte = startHexByte + 16;

    //FIXME overrun?
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t value;
        if (!hexCharToUInt(startHexByte[i], value)) {
            *endHexByte = &startHexByte[i];
            break;
        }
        retval = (retval << 4) + value;
    }
    return retval;
}

inline bool hexStringToBytes(const char* hexString, uint8_t buffer[], unsigned bufferMaxSize, unsigned& bufferLenUsed)
{
    bufferLenUsed = 0;
    bool high = true;
    for (const char* s=hexString; *s != '\0'; ++s) {
        if ( bufferLenUsed > bufferMaxSize )
            return false;
        uint8_t value;
        if ( !hexCharToUInt(*s, value) )
            return false;
        if ( high )
            buffer[bufferLenUsed] = value << 4;
        else
            buffer[bufferLenUsed++] |= value;
        high = !high;
    }
    return true;
}

template<typename T>
inline void appendHexToString(char *dst, T value, uint64_t size) {
    char buffer[130];
    bytesToHex((const uint8_t*)&value, sizeof(T), buffer);
    strlcat(dst, buffer, (size_t)size);
}

inline std::string_view trimSpaces(std::string_view str)
{
    while ( !str.empty() && std::isspace(str.front()) ) {
        str = str.substr(1);
    }
    while ( !str.empty() && std::isspace(str.back()) ) {
        str = str.substr(0, str.size()-1);
    }
    return str;
}

//
// *response file tokenization logic from ld64*
//
// get_next_response_option() tokenizes a string of command-line options separated by
// whitespace. given a pointer to a string, get_next_response_option() will return a pointer
// to the first word in that string and adjust the pointer to point to the
// remainder of the string. this promotes usage in a simple loop:
//
//   if (string) {
//     char* p = string;
//     for (char* arg = get_next_response_option(&p); arg; arg = get_next_response_option(&p)) {
//       // do something
//     }
//   }
//
// the string, buf, provides all of the storage necessary for tokenization;
// both the contents of buf as well as the value of *buf will be modified by
// get_next_response_option().
//
// get_next_response_option() honors characters escaped by \ or wrapped in single or double
// quotes. using these features callers can force options to contain whitespace,
// other backslashes, or quote characters.
//
// BUG: get_next_response_option() will not return an error if an option contains an
// unterminated quote character. The string "'one more time" will yield a single
// option "'one more time". callers will need to deal with this explicitly, if
// they care.
//
// NB: get_next_response_option() will allow callers to incldude quotes in the middle of
// an option; e.g., "one'    'two" will expand to "one    two" rather than
// "one" and "two". This is consistent with unix shell behavior, but not
// consistent with some implementations of the @file command line option.
//
inline char* get_next_response_option(char** buf)
{
    char* p = NULL; // beginning of option
    char* q = NULL; // end of option

    while ( buf && *buf && *(*buf) ) {
        char c = *(*buf);

        // whitespace
        //   ignore the space. if in an option, end option parsing. the option
        //   string (q) will be terminated later.
        if ( ' ' == c || '\t' == c || '\n' == c || '\r' == c ) {
            (*buf)++;
            if ( p )
                break;
        }

        // backslash
        //   ignore the backslash, but treat the next character as a literal
        //   character. start an option if not yet in an option.
        else if ( '\\' == c ) {
            // ignore the backslash (don't advance q)
            (*buf)++;
            // start a new option if necessary
            if ( !p )
                p = q = *buf;
            // if the string continues, include that next character in the option.
            if ( *(*buf) ) {
                *q++ = *(*buf);
                (*buf)++;
            }
        }

        // single or double quote
        //   ignore the quote character, but treat all characters (except backslash
        //   escaped cahracters) until a closing character as literal characters.
        //
        //   BUG: unterminated quotes are indistinguishable from terminated ones.
        else if ( '\'' == c || '"' == c ) {
            // ignore the quote (don't advance q)
            (*buf)++;
            // start a new option if necessary
            if ( !p )
                p = q = *buf;
            // consume remaining characters
            while ( *(*buf) && c != *(*buf) ) {
                if ( '\\' == *(*buf) ) {
                    // ignore the backslash (don't advance q)
                    (*buf)++;
                    // if the string continues, include that next character in the option.
                    if ( *(*buf) ) {
                        *q++ = *(*buf);
                        (*buf)++;
                    }
                }
                else {
                    // include this character in the option.
                    *q++ = *(*buf);
                    (*buf)++;
                }
            }
            // ignore the closing quote if we found one (don't advance q)
            if ( *(*buf) )
                (*buf)++;
        }

        // default (all other characters)
        //   start an option if necessary, and consume the character
        else {
            if ( !p )
                p = q = *buf;
            *q++ = *(*buf);
            (*buf)++;
        }
    }

    // terminate the option string
    if ( q )
        *q = '\0';

    return p;
}



#endif // StringUtils_h

