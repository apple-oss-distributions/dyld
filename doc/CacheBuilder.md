# Cache Builder

## Fixups

The cache consumes binaries with either chained fixups or fixup opcodes.  For simplicity, it doesn't handle threaded rebase or old-style classic relocations.

### Tracking fixups

All fixups are tracked by the ASLRTracker class.  In the source dylib, we'll find fixups using the chained fixup or opcode based fixup information.  For each fixup location, we'll add it to the tracker.  There is one tracker on each SlidChunk in the cache, where each dylib segment is a SlidChunk, and some optimizations such as canonical ObjC protocols are also contained in a SlidChunk.  All pointers in the cache must belong to memory owned by a subclass of SlidChunk.

### AdjustDylibSegments

AdjustDylibSegments will use the split seg information to slide values.  This is going to slide all rebases to their new values.

For opcode based fixups, the new VMAddress fits in the pointer slot, and so AdjustDylibSegments will just store the VMAddress in there.

For chained fixups, the fixup formats in on-disk dylibs don't tend to have enough bits to represent a shared cache VMAddress.  x86_64 caches are too high in the address space, and arm64e caches have the pointer authentication bits in the chained fixups.  Given this, it is typical for the VMAddress to instead be stored in a side table on the ASLRTracker.  The other parts of the pointer, eg, high8 and PAC, will still be stored in the fixup chain.

### Bind

After applying Split Seg Info, we bind the dylib.  This is going to walk the rebases/binds/chained fixups in the dylib.

At the end of binding, every value in the cache dylib should have been converted to a Cache32 or Cache64 value.  These are the internal cache fixup formats and closely resemble chained fixups.   They are abstracted so that we can change them as needed in future.  They currently store everything as cache offsets, ie, VM offsets from the base address of the cache.

### Opcode based rebases

On entry to binding, an opcode based rebase will be a VMAddress.  Specifically, a VMAddress slid by applying split seg info.  We take this address and convert it to a Cache32/64 value, which internally converts it to a cache VM offset.

### Opcode based binds

The bind will have computed a cache VMAddress for the target of the bind.  This can then be converted to a Cache32/64 similarly to rebases.

### Chained fixup rebases

It is highly likely these are stored in a side table on the ASLRTracker.  If they are, that is the VMAddress we use.  Otherwise, we get the VMAddress from the fixup itself.  Other information, eg, high8 and PAC, will both be taken from the fixup location.  They are not stored on the ASLRTracker.

With the above information, we can construct a Cache32/64 value.

### Chained fixup binds

Similarly to other binds, we get the VMAddress for the target of the bind.  We then get the PAC information from the fixup itself, if any.  All of this is then converted to a Cache32/64 value.

### Absolute values

In all of the above cases, absolute values are special.  By definition, they don't get slid.  They are written directly in to the fixup location, and are removed from the ASLRTracker.

### Post Bind

After bind has completed, all values in the cache are of the Cache32/64 kind.  There are no raw VMAddresses in the cache dylibs, or fixup chains.  This simplifies parsers which know that all values they access must be Cache32/64 values.

## GOT Uniquing

Most GOTs in the shared cache are uniqued.  For example, many dylibs have a pointer to `&malloc` which will always resolve to the same location.  These are combined in to special Chunk's which hold uniqued GOTs.  The value to write to each pointer isn't known until binding, when we compute the value which would have occupied the original GOT in the source dylib.

As a side effect of binding, we record the values for each uniqued GOT in the given dylib.  These are then combined later to ensure that each uniqued GOT has been set.

## Patch Table

Another side effect of binding is to compute every patch location.  A patch location corresponds to a bind, when one dylib uses a value from another.

The patch locations are recorded on a per-bind basis.  Every CacheDylib has a list of `bindTargets`.  For each `bindTarget` we have a list of every location in the dylib which used that `bindTarget`.  These are later combined to emit a patch table.