# Cache Builder Design

The new cache builder is designed to scale better, given the continued growth in the OS.  In particular, it aims to keep memory usage as low as possible and use threads where viable.

## Internal Data Structures

### Chunk

The smallest atomic unit in the builder is the Chunk.  It's analygous to Atom from ld64, and represents some piece of the final cache file/VM.

Subclasses of chunk may have additional fields, but the Chunk class contains those which are necessary to describe a range of the file/memory:

    Kind                kind;
    CacheFileOffset     subCacheFileOffset;
    CacheFileSize       subCacheFileSize;
    uint8_t*            subCacheBuffer;
    CacheVMAddress      cacheVMAddress;
    CacheVMSize         cacheVMSize;
    uint64_t            minAlignment;

The most common chunks are DylibSegmentChunk and LinkeditDataChunk.

#### DylibSegmentChunk

There is a 1:1 mapping from segments in the cache dylibs to DylibSegmentChunk.

For DATA* segments, which contain slid values, DylibSegmentChunk contains an ASLRTracker.  This tracks any slid values in the memory range covered by that chunk.

You can find more details on the ASLRTracker and fixups in the Fixups.md document.

#### LinkeditDataChunk

To aid the linkedit optimizations, such as symbol uniquing, the linkedit in the source dylib is decomposed into LinkeditDataChunk's.  These can be sorted to keep similar linkedit adjacent in the cache, or can be combined into large chunks, for example, of a shared symbol table.

## Region

Chunk's are combined into Regions.  A region represents a contiguous area of memory with the same permissions.  For example, the text/data/linkedit regions will all become "mappings" in the final cache.

Some regions don't get mappings.  These represent parts of the file, but are not in the VM address space.  Examples of these are the code signature and the unmapped local symbols in the .symbols file.

Much like chunks, regions contain fields which describe where they should be in file/VM layout:

    Kind                                kind;
    std::vector<Chunk*>                 chunks;
    CacheFileOffset                     subCacheFileOffset;
    CacheFileSize                       subCacheFileSize;
    CacheVMAddress                      subCacheVMAddress;
    CacheVMSize                         subCacheVMSize;
    
Note the `chunks` above.  It does not own the memory for the chunks.  They are owned elsewhere, and just referenced by the region.

## SubCache

Region's are combined into SubCache's.  Each sub-cache owns one or more regions.  A sub-cache must start with a cache header, so at the very least there will be a text region containing the cache header.  Other regions are optional, depending on the contents of the subCache.

The SubCache struct also describes how to lay out its owned regions in file/VM layout:

    Kind                kind;
    std::vector<Region> regions;
    uint8_t*            buffer;
    uint64_t            bufferSize;
    CacheVMAddress      subCacheVMAddress;
    uint8_t             cdHash[20];
    uuid_string_t       uuidString;
    std::string         fileSuffix;

Notice the `buffer`/`bufferSize` fields.  Those are the allocated space which owns the intermediate buffer used to build the SubCache.  This buffer is the only one not free()d by the builder itself.  Instead it is returned to the caller (`runSharedCacheBuilder()` in this case) which will free it once it has been written to disk.

The SubCache also owns any optimizations which may occur more than once.  For example, each SubCache may have its own uniqued GOTs, not just a single set of uniqued GOTs for the whole cache.

### Main SubCache's

Every shared cache must start with a main cache.  It is optionally (although typically) followed by other subCaches.  

In addition to the earlier fields, a main cache contains the list of subCaches:

    std::vector<SubCache*> subCaches;

When building for platforms such as macOS, the cache builder will have a single `main` cache, for the development shared cache.  When building for embedded (the universal configuration), the cache builder will have both development and customer `main` caches.  When building for an embedded target which needs only one of customer/development, the builder will still create a universal cache, and it is up to the caller (`runSharedCacheBuilder()`) to skip cache files it doesn't need for the required configuration.

### Sub SubCache's

This sub-cache contains text/data/linkedit which is not in the main cache.  There are no additional fields compared to the other sub-caches.

When building the universal configuration, a given sub-cache may be referenced by multiple `main` caches.  Given that, care is requried when walking the sub-caches to ensure they are not visited twice unless desired.

### Stub SubCache's

When building for the universal configuration, dylibs don't use their own __stubs sections to jump to external functions.  Instead, they are redirected to stubs sections in distinct sub-caches.  For each regular sub-cache containing `__text`, there will be a subsequent sub-cache containing `__stubs`.  That sub-cache will be part of a development/customer pair.  These pairs will be in distinct sub-caches, but will be assigned to the same VM address.

### Symbols SubCache

On embedded, local symbols are "unmapped".  This means they don't occupy VM space, but are just in a file to be used later.  This sub-cache represents that file.

## SharedCacheBuilder

This is the top level struct which owns everything else.  It owns the inputs (dylibs and executables), the `BuilderOptions`, and `BuilderConfig` and an array of `SubCache`.  Additioally, it owns a number of optimizations which are global, ie, optimizations which take place once in the whole build.  For optimizations which run more than once, eg, per-SubCache, see `SubCache`.

## mach_o

The mach_o namespace contains new classes to help with walking Mach-O files, and in particular their linkedit.  To reduce memory usage, the cache builder no longer uses the MachOLoaded/MachOAnalyzer classes.  Those run only on VM layout, which often requires a VM allocated buffer.  To avoid these buffers, we avoid using VM layout at all.

### mach_o::Segment

In file layout, a segment can be found by adding the segment's file offset to the mach_header*.  In VM layout, the segment can be found by adding the slide to the VM offset.

The Segment class abstracts these 2 layouts.  It holds a buffer pointer which can be used to record where the segment lives.  It can also abstract the cache builder layout, where the intermediate buffers may not be in file/VM layout, and can just be arbitrary buffers.

### mach_o::LinkeditLayout

In load commands, most linkedit is found via file offsets.  These have to be adjusted when looking at a Mach-O in VM layout.  Similarly to Segment, LinkeditLayout holds pointers to the given linkedit.   This can abstract over the location of the linkedit in any given buffer.

### mach_o::Layout

This holds the above segment and linkedit layouts.  It can be accessed via these helpers

    MachOFile::withFileLayout(^(const Layout& layout) { ... });
    MachOAnalyzer:withVMLayout(^(const Layout& layout) { ... });

In the cache builder, mach-o's aren't in file or VM layout, but "builder layout" where segments may be in arbitrary buffers.  The builder can also take a `CacheDylib` and get a `Layout`.  See `LayoutBuilder::LayoutBuilder()` for example.

This layout value can then be passed to any of the following structs to operate on the layout:

* mach_o::ExportTrie
* mach_o::Fixups
* mach_o::SplitSeg
* mach_o::SymbolTable

### mach_o::ExportTrie

This can be constructed with:

    mach_o:: ExportTrie exportTrie(layout);
    
It is currently only used by the cache builder to walk all symbols on the trie.  It can be extended with other methods from MachOAnalyzer as needed.

### mach_o::Fixups

This can be constructed with:

    mach_o::Fixups fixups(layout);
    
It replaces many of the methods currently on MachOAnalyzer, and can be used to walk chained fixups, bind opcodes, and classic relocations.

### mach_o::SplitSeg

This can be constructed with:

    mach_o:: SplitSeg splitSeg(layout);
    
It allows checking if a given binary has split seg, and which format.  It also allows walking all the split seg V2 references.  This will be used to replace many of the duplicate copies of parsing SplitSeg V2 in the builder.

### mach_o::SymbolTable

This can be constructed with:

    mach_o:: Symbols symbols(layout);
    
It also replaces many methods on MachOAnalyzer.  It can be used to look up whether a dylib exports a given symbol, walk locals, etc.

### MachoFileRef

Many algorithms take pointers to mach_header's and add offsets to them.  For example:

    uint8_t* fixupLoc = (uint8_t*)ma + (n_value - loadAddress);

This isn't safe in the cache builder, where the MachOFile may not be mapped in VM layout, or where segments may not even be allocated in the same buffer.

To avoid the above, MachoFileRef wraps a `MachOFile*` and prevents any kind of arithmetic.  The struct does provide an `operator->` overload so that `mf->...` still works.

## Impact on dyld4::Loader

The dyld4 Loader, JustInTimeLoader, and PrebuiltLoader all make extensive use of MachOAnalyzer.  These all requried changes to accomodate the cache builder no longer placing buffers in their final VM allocated layout.

The MachOAnalyzer is typically accessed via `Loader::analyzer()`. That method, and many others, are no longer available in the cache builder.  A new `Loader::mf()` method is available to get a MachOFile.  This is sufficient for many uses which only need to access the mach_header or load commands.

For layout specific data, or linkedit, the cache builder passes a `mach_o::Layout` for each `dyld4::Loader`.

Finally, many methods on MachOAnaluzer aren't needed, as their code cannot run in the cache builder.  For example, the cache builder will never run `applyFixups()`, a method which calls `MachOAnalyzer::getSlide()`.  These methods in Loader are #ifdef'ed out to prevent needing MachOAnalyzer in the builder.

## Impact on dyld4::RuntimeState

Similarly to `dyld4::Loader`, `RuntimeState` often deals with `MachOAnalyzer`'s.  This is typically via `Loader::loadAddress`.  Any of these uses won't work in the builder, as `Loader::loadAddress` itself isn't available.

The majority of uses in `RuntimeState` which needed `MachOAnalyzer` are notifiers and other dyld-only method.  These have also been #ifdef'ed out to prevent needing them in the builder.

### DyldSharedCache and dyld4

When building PrebuildLoader's for executables, the Loader code may need to use data structures such as the ObjC hash tables in the cache builder, or the cache patch table, when building iOSMac PrebuildLoader's.  The DyldSharedCache is also unavailable in the cache builder, as all of its methods assume a cache in VM layout.

Any such data structures have been given fields in the `DyldProcessConfig::DyldCache` structure.  For example, instead of accessing the selector hash table with `this->config.dyldCache->objcSelectorHashTable()`, its field, and others were added to the `DyldProcessConfig::DyldCache ` structure:

        const objc::HeaderInfoRO*       objcHeaderInfoRO;
        const objc::HeaderInfoRW*       objcHeaderInfoRW;
        const objc::SelectorHashTable*  objcSelectorHashTable;
        const objc::ClassHashTable*     objcClassHashTable;
        const objc::ProtocolHashTable*  objcProtocolHashTable;
        const SwiftOptimizationHeader*  swiftCacheInfo;
        uint64_t                        objcHeaderInfoROUnslidVMAddr;
        uint64_t                        objcProtocolClassCacheOffset;

The cache builder can set these values to the location of the buffers in the cache builder, instead of relying on them being at their correct VM address.

## Types

You may have noticed some of the previous lists of fields reference types such as `CacheVMAddress`.  This, and others, are wrappers around uint64_t's, and provide type safety throughout the builder.

The list of types are:

* VMAddress
* VMOffset
* CacheVMAddress
* CacheVMSize
* CacheFileSize
* CacheFileOffset
* InputDylibVMAddress
* InputDylibVMSize
* InputDylibFileOffset
* InputDylibFileSize

Hopefully most of the above names are self-descriptive.

### Operations on types

Convenience operators are available to make it easier to work with types in a safe manner.  For example:

* VMAddress + VMSize -> VMAddress
* VMAddress + VMOffset -> VMAddress
* VMAddress - VMAddress -> VMOffset
* VMSize + VMSize -> VMSize
* FileSize + FileSize -> FileSize
* FileOffset + FileSize -> FileOffset

Where necessary, you can also access the internal uint64_t value in the types.  This is typically required when serializing, eg, converting from a VMAddress to a uint64\_t to write into the dyld\_cache\_header.  Use of `rawValue()` to access the internal value is unfortunately more pervasive than desired, but its use should stil be kept to a minimum.  These type-safe wrappers weren't added to any of the MachO classes or methods, so it is around them that `rawValue()` is often needed.  We may migrate some/all MachO classes to these types if its worthwhile.

## BuilderOptions

This struct holds any options to configure the specific build.  For example, architecture, platform, verbose flags.  The values here are read-only throughout the lifetime of the builder.

## BuilderConfig

This read-only struct is created from the BuilderOptions.  It takes the given architecture, platform, etc, and sets up a configuration based on it.  For example, BuilderConfig::Layout::pageSize is 4k vs 16k depending on architecture.

## CacheDylib

For each dylib eligible for the cache, a CacheDylib is created.  This then owns the dylib, its segment chunks, linkedit chunks, dependents, etc.

## Phases

The cache builder runs in several distinct phases:

1. Calculate inputs
2. Estimate Global Optimizations
3. Create Sub Caches
4. Pre-dylib Emit Chunks
5. Dylib passes
6. Post-dylib Emit Chunks
7. Finalize

### Calculate inputs

At the start of this phase, we have just a list of input files and the builder options/config.  This phase works out which dylibs and executables are eligible to be in the cache, or optimized by the cache.

By the end of this phase, the builder will have the list of CacheDylib's.

### Estimate Global Optimizations

In order to allocate enough space for the sub-cache's, we must know how large the various optimizations will be in the sub-cache files.  This phase takes the CacheDylib's and estimates how much space will be needed for each optimization.

For any eligible optimizations, this phase will also record information to be used later.  For example, record the list of objc selectors, classes, protocols, etc.  This is faster than re-creating that information later.

### Create Sub Caches

The previous phases computed either the actual chunks, or at least the size of every chunk we need.  This phase takes those chunks and forms them into one or more SubCache's.

There are several different possible cache layouts, depending on the BuilderConfig::Layout:

* Regular Contiguous
* Regular Discontiguous
* Large Contiguous
* Large Discontiguous

Regular layouts are those were we emit a single cache file.  These are only used for simulators.  Large layouts support multiple cache files.

Contiguous and discontiguous describe the VM properties of the cache, not how we represent it on disk.  Discontiguous corresponds to te x86_64 layout where each mapping must start on its own 1GB boundary.  Contiguous is all arm64 caches, were padding may be requried between mappings, but otherwise there are no alignment requirements.

In addition to computing what Chunks/Regions will be in each SubCache, this phase also allocates the SubCache buffers.  Ideally we could actually create temporary files on disk and mmap them, as this may reduce the peak memory of the cache builder, but that is difficult to achieve with anonymous files.

As this phase also lays out the SubCache's, it will compute all file offsets and VM addresses.  These will be set on all SubCache's, Regions, and Chunks.

### Pre-dylib Emit Chunks

Before we run the Dylib passes, this phase will run.  It is the first phase where we can emit content into the newly allocated SubCache buffers.

Chunks emitted here are typically those which are required for the Dylib passes.  For example, the Dylib passes will walk ObjC metadata, and it is necessary to emit the uniqued ObjC strings here, so that the later passes can strcmp() against them.

### Dylib Passes

To improve scalability, dylibs are emitted in parallel.  This phase contains any passes which have been able to run safely in parallel at this stage of the builder.

During this phase, dylib segments will be copied into their SubCache buffer locations.  After being copied, they will be adjusted to their new locations in the VM address space.  This is done in the AdjustDylibSegments code.

After being adjusted, dylibs are bound.  This is where pointers to other dylibs are resolved.  The bind phase in particular is tricky to do in parallel, as other dylibs may not be slid yet.  The solution is to use the Export Trie's from the immutable input dylibs.  When a symbol is found at some offset into the input dylib, it can then be easily translated to an offset/address in the corresponding cache dylib.

After being bound, some ObjC passes are run on the dylibs.  These are the ObjC passes which are per-dylib and not global.  For example setting the selectors in the dylib to point to the canonical selectors found earlier in the global optimizations phase.

### Post-dylib Emit Chunks

This emits the remaining global optimizations, and specifically, those which depend on the dylibs.  For example, the dyld4 Loader's cannot be computed until after all dylibs have their adjusted mach_header's and load commands, and the cache patch table is built using values recoded as a side effect of binding dylibs.

### Finalize

This is the last phase of the builder.  It currently only code signs the sub-caches, which is the last operation required.