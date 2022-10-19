#!/usr/bin/python3

def findCacheSegmentVMAddr(kernel_cache, segment_name):
    for segment in kernel_cache.dictionary()["cache-segments"]:
        if segment["name"] == segment_name:
            return segment["vmAddr"]
    return None

def findCacheSegmentVMSize(kernel_cache, segment_name):
    for segment in kernel_cache.dictionary()["cache-segments"]:
        if segment["name"] == segment_name:
            return segment["vmSize"]
    return None

# Get the base address of the kernel collection
def findCacheBaseVMAddr(kernel_cache):
    for segment in kernel_cache.dictionary()["cache-segments"]:
        if segment["name"] == "__HIB":
            return segment["vmAddr"]
    return findCacheSegmentVMAddr(kernel_cache, "__TEXT")

def findSegmentVMAddr(kernel_cache, dylib_index, segment_name):
    for segment in kernel_cache.dictionary()["dylibs"][dylib_index]["segments"]:
        if segment["name"] == segment_name:
            return segment["vmAddr"]
    return None

def findSectionVMAddr(kernel_cache, dylib_index, segment_name, section_name):
    for segment in kernel_cache.dictionary()["dylibs"][dylib_index]["segments"]:
        if segment["name"] == segment_name:
            for section in segment["sections"]:
                if section["name"] == section_name:
                    return section["vmAddr"]
    return None

# Get the base address of a binary inside the kernel collection
def findBaseVMAddr(kernel_cache, dylib_index):
    return findSegmentVMAddr(kernel_cache, dylib_index, "__TEXT")

def findGlobalSymbolVMAddr(kernel_cache, dylib_index, symbol_name):
    for symbol_and_addr in kernel_cache.dictionary()["dylibs"][dylib_index]["global-symbols"]:
        if symbol_and_addr["name"] == symbol_name:
            return symbol_and_addr["vmAddr"]
    return None

def findLocalSymbolVMAddr(kernel_cache, dylib_index, symbol_name):
    for symbol_and_addr in kernel_cache.dictionary()["dylibs"][dylib_index]["local-symbols"]:
        if symbol_and_addr["name"] == symbol_name:
            return symbol_and_addr["vmAddr"]
    return None

def findFixupVMAddr(kernel_cache, fixup_name):
    for fixup_vmaddr, fixup_target in kernel_cache.dictionary()["fixups"].items():
        if fixup_target == fixup_name:
            return fixup_vmaddr
    return None

def findPagableFixupVMAddr(kernel_cache, dylib_index, fixup_name):
    for fixup_vmaddr, fixup_target in kernel_cache.dictionary()["dylibs"][dylib_index]["fixups"].items():
        if fixup_target == fixup_name:
            return fixup_vmaddr
    return None

def findAuxFixupVMAddr(kernel_cache, dylib_index, fixup_name):
    for fixup_vmaddr, fixup_target in kernel_cache.dictionary()["dylibs"][dylib_index]["fixups"].items():
        if fixup_target == fixup_name:
            return fixup_vmaddr
    return None

def offsetVMAddr(vmAddr, offset):
    het_int = int(vmAddr, 16)
    het_int = het_int + offset
    return ''.join([ '0x', hex(het_int).upper()[2:] ])

def fixupOffset(vmAddr, cacheBaseVMAddr):
    het_int = int(vmAddr, 16)
    het_int = het_int - int(cacheBaseVMAddr, 16)
    return ''.join([ '0x', hex(het_int).upper()[2:] ])