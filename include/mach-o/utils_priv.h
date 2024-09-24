/*
 * Copyright (c) 2022 Apple Inc. All rights reserved.
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
#ifndef _MACH_O_UTILS_PRIV_H_
#define _MACH_O_UTILS_PRIV_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <mach-o/loader.h>
#include <Availability.h>

#include <TargetConditionals.h>

#if __cplusplus
extern "C" {
#endif


/*!
 * @function macho_dylib_install_name
 *
 * @abstract
 *      Returns the install_name from the LC_ID_DYLIB of an MH_DYLIB mach_header.
 *
 * @param mh
 *      A pointer to the header of a mach-o dylib.
 *
 * @return
 *		Returns a static c-string which is the -install_name the dylib was built with.
 *		If mh is not a mach_header or not a dylib (MH_DYLIB), NULL will be returned.
 *		The string returned is static and does not need to be deallocated.
 */
extern const char* _Nullable macho_dylib_install_name(const struct mach_header* _Nonnull mh)
__API_AVAILABLE(macos(13.0), ios(16.0), tvos(16.0), watchos(8.0)) ;


/*!
 * @function macho_for_each_dependent_dylib
 *
 * @abstract
 *      Iterates over each dylib this mach-o links against
 *
 * @param mh
 *      Pointer to a mach-o file/slice loaded into memory.
 *
 * @param mappedSize
 *      Total size of the loaded file/slice.
 *      If the mach_header is from an image loaded dyld, use zero for the size.
 *
 * @param callback
 *      A block to call once per dependent dylib.
 *      To stop iterating, set *stop to true.
 *
 * @return
 *      Returns zero on success (meaning it iterated dependent dylibs), otherwise it returns an errno value.
 *      Common returned errors:
 *          EFTYPE -  mh content is not a mach-o
 *          EBADMACHO - mh content is a mach-o file, but it is malformed
 */
extern int macho_for_each_dependent_dylib(const struct mach_header* _Nonnull mh, size_t mappedSize, void (^ _Nonnull callback)(const char* _Nonnull loadPath, const char* _Nonnull attributes, bool* _Nonnull stop))
SPI_AVAILABLE(macos(15.0), ios(18.0), tvos(18.0), watchos(11.0));

/*!
 * @function macho_for_each_imported_symbol
 *
 * @abstract
 *      Iterates over each symbol the mach-o would need resolved at runtime.
 *
 * @param mh
 *      Pointer to a mach-o file loaded into memory.
 *
 * @param mappedSize
 *      Total size of the loaded file/slice.
 *      If the mach_header is from an image loaded dyld, use zero for the size.
 *      Note: dylibs in the dyld cache have lost their imports info, so this function
 *            will report no imports for dylibs in the dyld cache.
 *
 * @param callback
 *      A block to call once per imported symbol.
 *      To stop iterating, set *stop to true.
 *
 * @return
 *      Returns zero on success (meaning it iterated dependent dylibs), otherwise it returns an errno value.
 *      Common returned errors:
 *          EFTYPE -  mh content is not a mach-o
 *          EBADMACHO - mh content is a mach-o file, but it is malformed
 */
extern int macho_for_each_imported_symbol(const struct mach_header* _Nonnull mh, size_t mappedSize, void (^ _Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull libraryPath, bool weakImport, bool* _Nonnull stop))
SPI_AVAILABLE(macos(15.0), ios(18.0), tvos(18.0), watchos(11.0));

/*!
 * @function macho_for_each_exported_symbol
 *
 * @abstract
 *      Iterates over each symbol the mach-o exports.
 *
 * @param mh
 *      Pointer to the start of mach-o file loaded into memory.
 *
 * @param mappedSize
 *      Total size of the loaded file/slice.
 *      If the mach_header is from an image loaded dyld, use zero for the size.
 *
 * @param callback
 *      A block to call once per exported symbol.
 *      Cannot be NULL.
 *      To stop iterating, set *stop to true.
 *
 * @return
 *      Returns zero on success (meaning it iterated dependent dylibs), otherwise it returns an errno value.
 *      Common returned errors:
 *          EFTYPE -  mh content is not a mach-o
 *          EBADMACHO - mh content is a mach-o file, but it is malformed
 */
extern int macho_for_each_exported_symbol(const struct mach_header* _Nonnull mh, size_t mappedSize, void (^ _Nonnull callback)(const char* _Nonnull symbolName, const char* _Nonnull attributes, bool* _Nonnull stop))
SPI_AVAILABLE(macos(15.0), ios(18.0), tvos(18.0), watchos(11.0));


/*!
 * @function macho_for_each_defined_rpath
 *
 * @abstract
 *      Iterates over each LC_RPATH in a binary
 *
 * @param mh
 *      Pointer to a mach-o file/slice loaded into memory.
 *
 * @param mappedSize
 *      Total size of the loaded file/slice.
 *      If the mach_header is from an image loaded dyld, use zero for the size.
 *
 * @param callback
 *      A block to call once per rpath.
 *      To stop iterating, set *stop to true.
 *
 * @return
 *      Returns zero on success (meaning it iterated dependent dylibs), otherwise it returns an errno value.
 *      Common returned errors:
 *          EFTYPE -  mh content is not a mach-o
 *          EBADMACHO - mh content is a mach-o file, but it is malformed
 */
extern int macho_for_each_defined_rpath(const struct mach_header* _Nonnull mh, size_t mappedSize, void (^ _Nonnull callback)(const char* _Nonnull rpath, bool* _Nonnull stop))
SPI_AVAILABLE(macos(15.0), ios(18.0), tvos(18.0), watchos(11.0));


#if __cplusplus
}
#endif


#endif // _MACH_O_UTILS_PRIV_H_

