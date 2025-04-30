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

#import <Foundation/Foundation.h>

#include "JSONReader.h"
#include "Diagnostics.h"

namespace json {

static Node gSentinelNode;


const Node& getRequiredValue(Diagnostics& diags, const Node& node, const char* key) {
    if (diags.hasError())
        return gSentinelNode;

    if (!node.array.empty()) {
        diags.error("Cannot get key '%s' from array node\n", key);
        return gSentinelNode;
    }
    if (!node.value.empty()) {
        diags.error("Cannot get key '%s' from value node\n", key);
        return gSentinelNode;
    }

    auto it = node.map.find(key);
    if (it == node.map.end()) {
        diags.error("Map node doesn't have element for key '%s'\n", key);
        return gSentinelNode;
    }
    return it->second;
}


const Node* getOptionalValue(Diagnostics& diags, const Node& node, const char* key) {
    if (diags.hasError())
        return nullptr;

    if (!node.array.empty()) {
        diags.error("Cannot get key '%s' from array node\n", key);
        return nullptr;
    }
    if (!node.value.empty()) {
        diags.error("Cannot get key '%s' from value node\n", key);
        return nullptr;
    }

    auto it = node.map.find(key);
    if (it == node.map.end()) {
        return nullptr;
    }
    return &it->second;
}

uint64_t parseRequiredInt(Diagnostics& diags, const Node& node) {
    if (diags.hasError())
        return 0;

    if (!node.array.empty()) {
        diags.error("Cannot get integer value from array node\n");
        return 0;
    }
    if (!node.map.empty()) {
        diags.error("Cannot get integer value from value node\n");
        return 0;
    }
    if (node.value.empty()) {
        diags.error("Cannot get integer value from empty node\n");
        return 0;
    }

    return atoi(node.value.c_str());
}

bool parseRequiredBool(Diagnostics& diags, const Node& node) {
    if (diags.hasError())
        return false;

    if (!node.array.empty()) {
        diags.error("Cannot get integer value from array node\n");
        return false;
    }
    if (!node.map.empty()) {
        diags.error("Cannot get integer value from value node\n");
        return false;
    }
    if (node.value.empty()) {
        diags.error("Cannot get integer value from empty node\n");
        return false;
    }

    if ( (node.value == "true") || (node.value == "1") )
        return true;

    if ( (node.value == "false") || (node.value == "0") )
        return false;

    diags.error("Boolean value should be true/false/0/1\n");
    return false;
}

const std::string& parseRequiredString(Diagnostics& diags, const Node& node) {
    static std::string sentinelString = "";

    if (diags.hasError())
        return sentinelString;

    if (!node.array.empty()) {
        diags.error("Cannot get string value from array node\n");
        return sentinelString;
    }
    if (!node.map.empty()) {
        diags.error("Cannot get string value from value node\n");
        return sentinelString;
    }
    if (node.value.empty()) {
        diags.error("Cannot get string value from empty node\n");
        return sentinelString;
    }
    return node.value;
}


static Node parseNode(Diagnostics& diags, id jsonObject) {
    __block Node node;

    // NSDictionary -> map
    if ([jsonObject isKindOfClass:[NSDictionary class]]) {
        NSDictionary* dict = (NSDictionary*)jsonObject;
        node.type = NodeValueType::Map;

        [dict enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL* stop) {
            if (![key isKindOfClass:[NSString class]]) {
                diags.error("JSON map key is not of string type\n");
                *stop = true;
                return;
            }
            Node childNode = parseNode(diags, value);
            if (diags.hasError()) {
                *stop = true;
                return;
            }
            node.map[[key UTF8String]] = childNode;
        }];

        if (diags.hasError())
            return Node();

        return node;
    }

    // NSArray -> array
    if ([jsonObject isKindOfClass:[NSArray class]]) {
        NSArray* array = (NSArray*)jsonObject;
        node.type = NodeValueType::Array;

        [array enumerateObjectsUsingBlock:^(id obj, NSUInteger idx, BOOL * stop) {
            Node childNode = parseNode(diags, obj);
            if (diags.hasError()) {
                *stop = true;
                return;
            }
            node.array.push_back(childNode);
        }];

        if (diags.hasError())
            return Node();

        return node;
    }

    // NSString -> value
    if ([jsonObject isKindOfClass:[NSString class]]) {
        node.type = NodeValueType::String;
        node.value = [(NSString*)jsonObject UTF8String];
        return node;
    }

    // NSBoolean -> string
    if ([jsonObject isKindOfClass:[NSNumber class]]) {
        node.type = NodeValueType::String;
        node.value = [[(NSNumber*)jsonObject stringValue] UTF8String];
        return node;
    }

    diags.error("Unknown json deserialized type\n");
    return Node();
}

Node readJSON(Diagnostics& diags, const char* filePath, bool useJSON5) {
    Node resultNode;

    @autoreleasepool {
        NSInputStream* inputStream = [NSInputStream inputStreamWithFileAtPath:[NSString stringWithUTF8String:filePath]];
        if (!inputStream) {
            diags.error("Could not option json file: '%s'\n", filePath);
            return Node();
        }
        [inputStream open];

        NSError* error = nil;
        NSJSONReadingOptions opts = NSJSONReadingMutableContainers;
        if (useJSON5) {
            if (@available(macOS 12.0, *)) {
                opts |= NSJSONReadingJSON5Allowed;
            } else {
                diags.error("JSON5 is unavailable");
                return Node();
            }
        }
        id jsonObject = [NSJSONSerialization JSONObjectWithStream:inputStream options:opts error:&error];
        if (!jsonObject) {
            diags.error("Could not deserialize json file: '%s' because '%s'\n", filePath, [[error debugDescription] UTF8String]);
            [inputStream close];
            return Node();
        }

        resultNode = parseNode(diags, jsonObject);
        [inputStream close];
    }

    return resultNode;
}

Node readJSON(Diagnostics& diags, const void * contents, size_t length, bool useJSON5) {
    Node resultNode;

    @autoreleasepool {
        NSData* data = [NSData dataWithBytes:contents length:length];
        NSError* error = nil;
        NSJSONReadingOptions opts = NSJSONReadingMutableContainers;
        if (useJSON5) {
            if (@available(macOS 12.0, *)) {
                opts |= NSJSONReadingJSON5Allowed;
            } else {
                diags.error("JSON5 is unavailable");
                return Node();
            }
        }
        id jsonObject = [NSJSONSerialization JSONObjectWithData:data options:opts error:&error];
        if (!jsonObject) {
            diags.error("Could not deserialize json because '%s'\n", [[error debugDescription] UTF8String]);
            return Node();
        }
        resultNode = parseNode(diags, jsonObject);
    }

    return resultNode;
}

} //namespace json
