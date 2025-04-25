/*
 * Copyright (c) 2017-2021 Apple Inc. All rights reserved.
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


#ifndef mach_o_FunctionVariantsWriter_h
#define mach_o_FunctionVariantsWriter_h

// posix
#include <stdarg.h>

// stl
#include <span>

// mach_o
#include "Defines.h"
#include "Error.h"
#include "FunctionVariants.h"


namespace mach_o {


/*!
 * @struct FunctionVariantsRuntimeTableWriter
 *
 * @abstract
 *      Table for all variants of one function
 */
struct VIS_HIDDEN FunctionVariantsRuntimeTableWriter : public FunctionVariantsRuntimeTable
{
    static FunctionVariantsRuntimeTableWriter*  make(FunctionVariantsRuntimeTable::Kind kind, size_t variantsCount);
    Error                                       setEntry(size_t index, uint32_t implOffset, bool implIsTable, std::span<const uint8_t> flagIndexes);
};


/*!
 * @struct FunctionVariantsWriter
 *
 * @abstract
 *      Wrapper for building a FunctionVariantsRuntimeTable in an image.
 *      Located in LINKEDIT.
 *      Pointed to by `LC_FUNCTION_VARIANTS`
 *
 */
struct VIS_HIDDEN FunctionVariantsWriter : public FunctionVariants
{
public:
                                FunctionVariantsWriter(std::span<const FunctionVariantsRuntimeTable*> entries);
    std::span<const uint8_t>    bytes() const { return _builtBytes; }

private:
    std::vector<uint8_t> _builtBytes;
    Error                _buildError;
};



/*!
 * @struct FunctionVariantFixupsWriter
 *
 * @abstract
 *      Wrapper for building uses of non-exported function variants.
 *      Located in LINKEDIT.
 *      Pointed to by `LC_FUNCTION_VARIANT_FIXUPS`
 *
 */
struct VIS_HIDDEN FunctionVariantFixupsWriter : public FunctionVariantFixups
{
public:
                                // used to build linkedit content
                                FunctionVariantFixupsWriter(std::span<const InternalFixup> entries);
    std::span<const uint8_t>    bytes() const { return _builtBytes; }

private:
    std::vector<uint8_t> _builtBytes;
};



} // namespace mach_o

#endif /* mach_o_FunctionVariantsWriter_h */
