/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * "Portions Copyright (c) 1999 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 1.0 (the 'License').  You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License."
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stddef.h>

#include <vector>

#include "ExportsTrie.h"
#include "Symbol.h"
#include "ExportsTrieWriter.h"

#include "cctools_helpers.h"


using mach_o::ExportsTrie;
using mach_o::ExportsTrieWriter;
using mach_o::Symbol;
using mach_o::Error;

VIS_HIDDEN
const char* prune_trie(uint8_t* trie_start, uint32_t trie_start_size,
                        int (*remove)(const char* symbolName), uint32_t* trie_new_size)
{
    ExportsTrie inputTrie(trie_start, trie_start_size);
    if ( Error err = inputTrie.valid(0x100000000) )
        return strdup(err.message()); // freed by caller

    // built new trie from existing, filtering out some symbols
    __block bool removedSomething = false;
    ExportsTrieWriter newTrie(inputTrie, ^(const Symbol& symbol) {
        if ( remove(symbol.name().c_str()) != 0 ) {
            removedSomething = true;
            return true;
        }
        return false;
    });

    // special case when nothing removed, to leave buffer as-is
    if ( !removedSomething ) {
        *trie_new_size = (uint32_t)trie_start_size;
        return NULL;
    }

    // get info about new trie buffer
    size_t          newTrieSize;
    const uint8_t*  newTrieBuffer = newTrie.bytes(newTrieSize);

    // Need to align trie to 8 or 4 bytes.  We don't know the arch, but if the incoming trie
    // was not 8-byte aligned, then it can't be a 64-bit arch, so use 4-byte alignement.
    uint8_t triePadding = 0;
    if ( (trie_start_size % 8) != 0 ) {
        if ( (newTrieSize % 4) != 0 )
            triePadding = 4 - (newTrieSize % 4);
    }
    else {
        if ( (newTrieSize % 8) != 0 )
            triePadding = 8 - (newTrieSize % 8);
    }

    // sanity check it is same size or smaller than input buffer
    if ( newTrieSize+triePadding > trie_start_size ) {
        char* msg;
        asprintf(&msg, "new trie is larger (%ld) than original (%d)", newTrieSize, trie_start_size);
        return msg;
    }

    // copy new trie into input buffer
    memcpy(trie_start, newTrieBuffer, newTrieSize);
    if ( triePadding != 0 )
        bzero(trie_start+newTrieSize, triePadding);
    *trie_new_size = (uint32_t)(newTrieSize+triePadding);

    return NULL;
}
