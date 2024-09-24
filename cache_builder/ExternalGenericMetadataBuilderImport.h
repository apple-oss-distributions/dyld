/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
* Copyright (c) 2024 Apple Inc. All rights reserved.
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

#ifndef ExternalGenericMetadataBuilderImport
#define ExternalGenericMetadataBuilderImport

#include <stdint.h>

#define WEAK_IMPORT_ATTR __attribute__((weak_import))

#ifdef __cplusplus
extern "C" {
#endif

struct SwiftExternalMetadataBuilder;
struct mach_header;

// Create a builder object with the given platform and architecture name.
WEAK_IMPORT_ATTR
struct SwiftExternalMetadataBuilder *
swift_externalMetadataBuilder_create(int platform, const char *arch);

// Destroy a builder object.
WEAK_IMPORT_ATTR
void swift_externalMetadataBuilder_destroy(
    struct SwiftExternalMetadataBuilder *);

// Returns an error string if the dylib could not be added
// The builder owns the string, so the caller does not have to free it
// The mach_header* is the raw dylib from disk/memory, before the shared cache
// builder has created its own copy of it
WEAK_IMPORT_ATTR
const char *swift_externalMetadataBuilder_addDylib(
    struct SwiftExternalMetadataBuilder *, const char *install_name,
    const struct mach_header *, uint64_t size);

WEAK_IMPORT_ATTR
const char *swift_externalMetadataBuilder_readNamesJSON(
    struct SwiftExternalMetadataBuilder *, const char *names_json);

// Returns an error string if the dylib could not be added
// The builder owns the string, so the caller does not have to free it
WEAK_IMPORT_ATTR
const char *swift_externalMetadataBuilder_buildMetadata(
    struct SwiftExternalMetadataBuilder *);

// Get the JSON for the built metadata
WEAK_IMPORT_ATTR
const char *swift_externalMetadataBuilder_getMetadataJSON(
    struct SwiftExternalMetadataBuilder *);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ExternalGenericMetadataBuilderImport
