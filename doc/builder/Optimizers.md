# Cache Builder Optimizers

The cache builder runs a number of optimization passes.  These are:

* Pack segments
* Convert binds to rebases
* Remove unused LINKEDIT
* Deduplicate LINKEDIT Symbol Strings
* Unmap Local Symbols
* Patch Table
* dyld4 PrebuiltLoader's
* ObjC IMP Caches
* ObjC Hash Tables
* ObjC Canonical Selector References
* ObjC Canonical Protocols
* ObjC Relative Method Lists Conversion
* ObjC Method Lists Sorting
* ObjC Selector References Loading
* Swift Protocol Conformance Hash Table
* Optimize Stubs
* GOT uniquing
* FIPS Sign
* Code Sign

## Optimizer Properties

Optimizer                               | Runs In Parallel
:---------------------------------------|:-----------------:|
Pack segments                           | Yes (On Dylibs)
Convert binds to rebases                | Yes (On Dylibs)
Remove unused LINKEDIT                  | Yes (On Dylibs)
Deduplicate LINKEDIT Symbol Strings     | No
Unmap Local Symbols                     | No
Patch Table                             | No
dyld4 dylib PrebuiltLoader's            | No
dyld4 executablePrebuiltLoader's        | Yes (On Exes)
ObjC IMP Caches                         | No
ObjC Hash Tables                        | No
ObjC Canonical Selector References      | Yes (On Dylibs)
ObjC Canonical Protocols                | No
ObjC Relative Method Lists Conversion   | Yes (On Dylibs)
ObjC Method Lists Sorting               | Yes (On Dylibs)
ObjC Selector References Loading        | Yes (On Dylibs)
Swift Protocol Conformance Hash Table   | No
Optimize Stubs                          | Yes (On Dylibs)
GOT uniquing                            | No
FIPS Sign                               | Yes (On Dylibs)
Code Sign                               | Yes (On Pages)

## Optimizers 

### Pack segments

This optimization puts Chunks with the same permissions into the same Regions.  Each Region corresponds to a range of memory with a specific min/max protection.

Chunks are placed in a Region in `SubCache::addDylib()`.  They are then sorted in `SharedCacheBuilder::sortSubCacheSegments()`.

When dylibs were linked, ld64 set all fixups as if the dylib would be loaded in its on-disk layout.  These fixups are wrong once the cache builder moves segments to pack them with other dylibs.  Split Seg information is used to fix up any references, which is done in `CacheDylib::applySplitSegInfo()`.

Allocating Chunks to Regions, and sorting them are both done serially.  However, adjusting references using Split Seg information is done in parallel on each dylib.

A side effect of applying Split Seg is that some chained fixup values no longer fit in the chained format bits available, eg, a 48-bit address can't fit in a 32-bit address slot.  Values which don't fit are stored in the ASLRTracker and will be retrieved later.

### Convert binds to rebases

Dylibs contain two kinds of fixups:

* rebases, where a pointer value in the dylib points to a location in that dylib
* binds, where a pointer value in the dylib points to a location in another dylib

The shared cache contains only rebases, not binds.  This pass converts all binds to rebases by resolving symbols in dependent dylibs.  The main work here is done in `CacheDylib::calculateBindTargets()` and `CacheDylib::bind()`.

This pass runs in parallel over each CacheDylib.  As a given CacheDylib is looking up symbols in other dylibs, we can't guarantee that we are looking at up-to-date information.  Otherwise we'd need to run serially.  This is solved by having each CacheDylib look at the unmodified on-disk dylib for dependents.  If a symbol is resolved in a dependent, then it can be converted to a Cache VMAddress.

As a result of this pass, all binds have been resolved, and are stored in their location in a Fixup::Cache32/Fixup::Cache64 format, which can be thought of as a chained fixup format internal to the builder.

Rebases were not in the Fixup::Cache32/Fixup::Cache64 format.  They are also converted during this pass.  This differs from the previous cache builder where everything was a VMAddress.  Now to save memory, everything is a packed value with high8, and PointerMetaData if necessary.

### Remove unused LINKEDIT

Some LINKEDIT isn't needed in cache dylib.  This is removed by `Adjustor::rebuildLinkEditAndLoadCommands()`.

Examples are:

* LC\_RPATH
* LC\_CODE\_SIGNATURE
* LC\_DYLIB\_CODE\_SIGN\_DRS
* LC\_DYLD\_CHAINED\_FIXUPS
* LC\_SEGMENT\_SPLIT\_INFO

Note that both chained fixups and split seg are used later, even though the load commands have been removed.  In both cases, we can use the data from the input dylib, as it can be easily adjusted to account for the builder memory layout.

### Deduplicate LINKEDIT Symbol Strings

This pass has to be done on each SubCache which contains LINKEDIT.  Currently that is only required on x86_64(h), as all arm64* caches use a single LINKEDIT.  For each set of Chunks in a given LINKEDIT Region, symbols are found and deduplicated.

The original Chunks are of kind `linkeditSymbolStrings`.  These are removed by this pass as a single, new Chunk of kind `optimizedSymbolStrings` is created.  Additionally, new nlist Chunks are created of kind `optimizedSymbolStrings`.  The original nlist Chunks are removed.

Note this pass can be found in `SharedCacheBuilder::calculateSubCacheSymbolStrings()`.  It must run after sub caches have been computed, but before allocating buffer.  This pass is able to walk all the Chunks in each SubCache, and optimize them.  Whatever new/remaining Chunks we have after this pass can then be allocated buffers.

### Unmap Local Symbols

The `SharedCacheBuilder::calculateSubCacheSymbolStrings()` function deduplicates symbol strings, but may also unmap local symbols.  This is done on embedded platforms only.  An `unmappedSymbols` Chunk is created, and embedded in a new SubCache just for the unmapped local symbols.  Locals in the cache are replaced by the symbol "<redacted>".

### Patch Table

If roots are installed at run time, dyld needs to patch the cache.  There is a much more detailed document on the PatchTable format, but we'll cover the builder piece here.

The first step is to estimate the size of patch table we need.  The patch table consists of lists of dylibs, clients of dylibs, patch locations, export symbol strings, etc.  Each dylib is walked to look for binds, uses of binds, their symbol names, etc, and all of this is pulled into the estimate for the patch table.  This is done in `SharedCacheBuilder::estimatePatchTableSize()`.

When fixing up each CacheDylib, in `CacheDylib::bind()`, we have a list of locations where binds were applied, and we know the target dylib where we resolved the bind to.  Each CacheDylib maintains a list of `bindTargets` for resolving binds.  Binding also populates lists of which locations used each `bindTarget`.  These locations are computed in parallel for each dylib.

Later, these lists of bind targets, and uses of each bind target, are processed in `SharedCacheBuilder::emitPatchTable()` into a patch table, which is then emitted into the cache.  As the patch table must be able to patch any value, it must run late in the builder, after any pass which may create new locations to be patched.

### dyld4 PrebuiltLoader's

dyld4 builds PrebuiltLoaders for both cache dylibs and executables.  The first step for both of these is to estimate the size we'll need, and is done in `SharedCacheBuilder::estimateCacheLoadersSize()`.

For cache dylibs, the estimate is quite simple as the loader is mostly fixed size structs, or arrays based on things like how many dependents the dylib has.

For executables, the estimate is much harder, as executables may link dylibs from disk, or may contain objc hash tables, and Swift hash tables.  Based on the current PreBuiltLoaders, an estimate of 10KB per executable is used.

Next, we emit dylib loaders in `SharedCacheBuilder::emitCacheDylibsPrebuiltLoaders()`.  For these, we need to have all dylibs in their final cache addresses, and the PatchTable needs to be available for macOS/iOSMac unzippered twin patching.  Additionally, the `dyld4::Loader` now takes a `mach_o::Layout` for each CacheDylib to build a Loader for.  This is used to walk any linkedit or other information on the dylib, as needed.

Executable loaders are emitted next, in `SharedCacheBuilder::emitExecutablePrebuiltLoaders()`, using the cache loaders, ObjC hash tables, and patch table as inputs.  Executable loaders are totally independent so can be calculated in parallel.  Similarly to cache dylibs, executables and any on-disk dylibs they link must have `mach_o::Layout` available.  As these are all buffers in file layout, they can use `MachOFile::withFileLayout()` to get a layout. 

### ObjC IMP Caches

IMP Caches are built for arm64(e) iOS devices.  These are prebuilt method caches for eligible classes.

It is hard to estimate IMP caches as they rely on laying out selector strings in a buffer at specific offsets.  This is to achieve a perfect hash on each method cache.  Given this, we instead compute the method caches early in the builder, and keep the list of caches to be emitted later.  This is done in `SharedCacheBuilder::estimateIMPCaches()`.

The first step to build IMP caches is to find all classes, categories, and selectors.  This is done by creating an `objc_visitor::Visitor` for each CacheDylib, and recording all required information.  This is then passed "over the wire" to the imp caches builder to get the caches and a buffer of selectors to emit in that layout.  This buffer of selectors only contains the IMP caches selectors, but forms the start of the buffer to add all other selectors to later.

IMP caches are emitted in parallel for each dylib in `CacheDylib::emitObjCIMPCaches()`.  This takes the caches computed earlier, converts from addresses in input dylibs to cache addresses, and attaches the cache to each classes, as needed.

### ObjC Hash Tables

The cache builder computes ObjC hash tables for selectors, classes, and protocols.  In each case, the key to the map is a string value, where keys are passed through a perfect hash function.  Selectors are a map, as each key points to a single canonical selector string definition.  Classes and Protocols are multimaps, as we need to point to every class/protocol for the given name.

Hash tables can be estimated in `SharedCacheBuilder::estimateObjCHashTableSizes()`.  The estimate is based on how much space the perfect hash needs for the given number of strings in the map.  We have this information available as a side effect of `SharedCacheBuilder::findCanonicalObjCSelectors()`, `SharedCacheBuilder::findObjCClasses()` and `SharedCacheBuilder::findObjCProtocols()`.

Hash tables are emitted later, in `SharedCacheBuilder::emitObjCHashTables()`, once we know the addresses of each class/protocol.  This is currently done in serial, although it would be possible to build the hash tables in parallel with each other.

### ObjC Canonical Selector References

ObjC selector strings must be canonicalized.  This involves choosing a single definition for each string, and enables other optimizations such as uniquing method lists and building hash tables.

A list of all selectors is built by walking all the CacheDylibs.  This list first takes the IMP Caches selectors, if any, and appends any other selectors from dylibs.  This is added to a Chunk of kind `objcStrings`, whose size is estimated in `SharedCacheBuilder::findCanonicalObjCSelectors()`.

The selectors are then emitted in `SharedCacheBuilder::emitObjCSelectorStrings()`.

### ObjC Canonical Protocols



### ObjC Relative Method Lists Conversion
### ObjC Method Lists Sorting
### ObjC Selector References Loading
### Swift Protocol Conformance Hash Table
### Optimize Stubs
### GOT uniquing
### FIPS Sign
### Code Sign