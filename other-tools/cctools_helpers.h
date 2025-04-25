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
#include <stdint.h>


#if __cplusplus
extern "C" {
#endif /* __cplusplus */


/*
 * prune_trie() is a C vended function that is used by strip(1) to prune out
 * defined exported symbols from the export trie.  It is passed a pointer to
 * the start of bytes of the the trie and the size.  The prune() function
 * callback is called with each symbol name in the trie to determine if it is
 * to be pruned (retuning 1) or not (returning 0).  prune_trie() writes the new trie
 * back into the trie buffer and returns the new size in trie_new_size.
 * This can be done be cause the new trie will always be the same size or smaller.
 * If the pruning succeeds, NULL is returned.  If there was an error processing
 * the trie (e.g. it is malformed), then an error message string is returned
 * and is now owned by the caller.  Use free() to release the error string.
 */
extern const char*
prune_trie(uint8_t* trie_start, uint32_t trie_start_size,
           int (*prune)(const char *name), uint32_t* trie_new_size);



/*
 * make_obj_file_with_linker_options() is used by libtool to create a .o file
 * that contains just auto-linking hints.
 *
 * cpu_type and cpu_subtype specify the arch of the .o file to be created.
 * libHitCount is the length of the libNames array.
 * libNames is an array of lib base names (e.g. "foo" for the hit to use libfoo.a).
 * frameworkHintCount is the length of the frameworkNames array.
 * frameworkNames is an array of framework names (e.g. "Foo" for the hit to use Foo.framework/Foo).
 * outPath is the location (in TMPDIR) of the .o file created.
 */
extern void
make_obj_file_with_linker_options(uint32_t cpu_type, uint32_t cpu_subtype,
                                  uint32_t libHitCount, const char* libNames[],
                                  uint32_t frameworkHintCount, const char* frameworkNames[],
                                  char outPath[PATH_MAX]);



#if __cplusplus
}
#endif /* __cplusplus */
