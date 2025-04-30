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

#ifndef __JSON_WRITER_H__
#define __JSON_WRITER_H__

#include <iostream>

#include "JSON.h"

namespace json {

static inline std::string hex(uint64_t value) {
    char buff[64];
    snprintf(buff, sizeof(buff), "0x%llX", value);
    return buff;
}

static inline std::string hex4(uint64_t value) {
    char buff[64];
    snprintf(buff, sizeof(buff), "0x%04llX", value);
    return buff;
}

static inline std::string hex8(uint64_t value) {
    char buff[64];
    snprintf(buff, sizeof(buff), "0x%08llX", value);
    return buff;
}

static inline std::string unpaddedDecimal(uint64_t value) {
    char buff[64];
    snprintf(buff, sizeof(buff), "%llu", value);
    return buff;
}

static inline std::string decimal(uint64_t value) {
    char buff[64];
    snprintf(buff, sizeof(buff), "%02llu", value);
    return buff;
}

static inline void indentBy(uint32_t spaces, std::ostream& out) {
    for (uint32_t i=0; i < spaces; ++i) {
        out << " ";
    }
}

static inline void printJSON(const Node& node, uint32_t indent = 0, std::ostream& out = std::cout)
{
    if ( !node.map.empty() ) {
        out << "{";
        bool needComma = false;
        for (const auto& entry : node.map) {
            if ( needComma )
                out << ",";
            out << "\n";
            indentBy(indent+2, out);
            out << "\"" << entry.first << "\": ";
            printJSON(entry.second, indent+2, out);
            needComma = true;
        }
        out << "\n";
        indentBy(indent, out);
        out << "}";
    }
    else if ( !node.array.empty() ) {
        out << "[";
        bool needComma = false;
        for (const auto& entry : node.array) {
            if ( needComma )
                out << ",";
            out << "\n";
            indentBy(indent+2, out);
            printJSON(entry, indent+2, out);
            needComma = true;
        }
        out << "\n";
        indentBy(indent, out);
        out << "]";
    }
    else {
        auto &value = node.value;
        switch (node.type) {
        case NodeValueType::Default:
        case NodeValueType::String:
            if (value.find('"') == std::string::npos) {
                out << "\"" << value << "\"";
            } else {
                std::string escapedString;
                escapedString.reserve(value.size());
                for (char c : value) {
                    if (c == '"')
                        escapedString += '\\';
                    escapedString += c;
                }
                out << "\"" << escapedString << "\"";
            }
            break;
        case NodeValueType::RawValue:
            out << value;
            break;
        case NodeValueType::Array:
        case NodeValueType::Map:
            // handled earlier
            break;
        }
    }
    if ( indent == 0 )
        out << "\n";
}

static inline void streamArrayBegin(bool &needsComma, std::ostream& out = std::cout)
{
    out << "[";
    needsComma = false;
}

static inline void streamArrayNode(bool &needsComma, Node &node, std::ostream& out = std::cout)
{
    if (needsComma)
        out << ",";
    out << "\n";
    indentBy(2, out);
    printJSON(node, 2, out);
    needsComma = true;
}

static inline void streamArrayEnd(bool &needsComma, std::ostream& out = std::cout)
{
    if (needsComma)
        out << "\n";
    out << "]\n";
}

} // namespace json


#endif // __JSON_WRITER_H__
