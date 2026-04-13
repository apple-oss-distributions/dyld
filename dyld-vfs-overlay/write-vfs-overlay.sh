#!/bin/sh

# Create a VFS overlay to virtually install the build products into the SDK.
# The LLVM virtual filesystem is documented here.
# http://llvm.org/doxygen/classllvm_1_1vfs_1_1RedirectingFileSystem.html#details
if [ "$RC_XBS" = "YES" -a "$RC_BUILDIT" != "YES" ]
then
    # The overlay is not needed in an XBS environment where installhdrs merges the
    # build products into the SDK before the other targets run installapi/install.
    roots=" []"
else
    roots="
-
  type: directory
  name: ${SDKROOT}/usr/include
  contents:
  -
    type: file
    name: dlfcn.h
    external-contents: ${PROJECT_DIR}/include/dlfcn.h
-
  type: directory
  name: ${SDKROOT}/usr/include/mach-o
  contents:
  -
    type: file
    name: dyld.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld.h
  -
    type: file
    name: dyld_images.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld_images.h
  -
    type: file
    name: fixup-chains.h
    external-contents: ${PROJECT_DIR}/include/mach-o/fixup-chains.h
  -
    type: file
    name: utils.h
    external-contents: ${PROJECT_DIR}/include/mach-o/utils.h
-
  type: directory
  name: ${SDKROOT}/usr/local/include
  contents:
  -
    type: file
    name: dlfcn_private.h
    external-contents: ${PROJECT_DIR}/include/dlfcn_private.h
  -
    type: file
    name: objc-shared-cache.h
    external-contents: ${PROJECT_DIR}/include/objc-shared-cache.h
-
  type: directory
  name: ${SDKROOT}/usr/local/include/mach-o
  contents:
  -
    type: file
    name: dyld_priv.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld_priv.h
  -
    type: file
    name: dyld_cache_format.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld_cache_format.h
  -
    type: file
    name: dyld_introspection.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld_introspection.h
  -
    type: file
    name: dyld_process_info.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld_process_info.h
  -
    type: file
    name: dyld-interposing.h
    external-contents: ${PROJECT_DIR}/include/mach-o/dyld-interposing.h
  -
    type: file
    name: function-variant-macros.h
    external-contents: ${PROJECT_DIR}/include/mach-o/function-variant-macros.h
  -
    type: file
    name: utils_priv.h
    external-contents: ${PROJECT_DIR}/include/mach-o/utils_priv.h"
fi

printf "\
version: 0
roots:%s" \
"$roots" > "$SCRIPT_OUTPUT_FILE_0"
