# Dyld Allocator Design

The `dyld` allocator is a modified bump pointer allocator designed to support `dyld`'s performance and security needs. Allocators
may be instantiated on the stack, or statically pre-allocated in the `dyld` binary. Each allocator consists of at least on `Pool`,
which is the memory used to service requests. The `Pool` is used until it is exhausted, at which point a new pool is allocated.

The allocator supports the following features:

* Inline metadata before and after each allocation and free block. A single metadata my serve as both the end of one block and the
  start of the next block.
* Coalescing of adjacent free blocks on deallocation
* High water mark reduction if the last allocated block is freed
* In place realloc() if free memory is available after the current allocation
* The allocator can be marked read only when dyld is not updating internal data structures.
* Two allocation strategies, the default bump pointer mode, and s simple best fit mode

These features together enable efficient memory usage under our normal pattern of a growing vector at the top of the heap, and allow
us to reclaim freed memory in most cases.

<!--
-->

Each piece of inline metadata consists of a pointer to the proceeding piece of metadata, and a pointer to the succeeding piece of
metadata. For the first metadata in the pool the previous pointer points to the pool itself (this is indicated by masking 0x01)
into the pointer. The low bit of the next metadata pointer isused to indicate if the space between the two metadata's is free (0x00)
or allocated (0x01), and 0x02 is used to indicate if it is the last metadata in the pool. Size is implicitly tracked by the the
distance between the current metdata and the next metdata.

Based on the above it is possible to find the pool the metadata is associated with by walking back through all the metadatas. This
is necessary for some operations, and can be fairly slow as it needs to touch almost all the allocated memory. As an optimization
pool pointers are stored in the first word of each free block large enough to hold them (exlcuding the final free metadata). Below shows the basic layout of the allocations and metadata:

                        ┌───────────┐                              
          ┌────────────▶│   Pool    │                              
          │          ┌─▶│           ├─────────┐                    
          │          │  └───────────┘         │                    
          │          │                        │                    
          │ Previous │                        │                    
          │          │  ┌───────────┐         │                    
          │          └──┤ Metadata  │         │                    
          │          ┌─▶│           ├──┐      │  _lastFreeMetadata 
Pool Hint │          │  ├───────────┤  │      │                    
          │          │  │           │  │      │                    
          │          │  │Allocation │  │      │                    
          │ Previous │  │           │  │ Next │                    
          │          │  │           │  │      │                    
          │          │  ├───────────┤  │      │                    
          │          └──┤ Metadata  │◀─┘      │                    
          │          ┌─▶│           ├──┐      │                    
          │          │  ├┬─────────┬┤  │      │                    
          └──────────┼──┼┤Pool Hint││  │      │                    
                     │  │└─────────┘│  │      │                    
            Previous │  │   Free    │  │ Next │                    
                     │  │           │  │      │                    
                     │  │           │  │      │                    
                     │  ├───────────┤  │      │                    
                     └──┤ Metadata  │◀─┘      │                    
                     ┌─▶│           ├──┐      │                    
                     │  ├───────────┤  │      │                    
                     │  │           │  │      │                    
            Previous │  │Allocation │  │      │                    
                     │  │           │  │ Next │                    
                     │  │           │  │      │                    
                     │  ├───────────┤  │      │                    
                     └──┤ Metadata  │◀─┴──────┘                    
                        │           │──┐                           
                        ├───────────┤  │                           
                        │           │  │                           
                        │   Free    │  │   Last                    
                        │  (Last)   │  │  Address                  
                        │           │  │                           
                        │           │  │                           
                        └───────────┘◀─┘                           

Except for the final block in the pool each block of free or allocated and ends with a metadata. In order to avoid dirtying the
last page of the pool the metadata for the last block floats with the end of the allocated space, ands its next pointer (which is
masked with 0x02) points to the end of the available space.

In order to guarantee it is possible to setup another pool in the event the currentPool overflows, space for the next pool is
always pre-allocated in the current pool when it is constructed.

## Best fit mode

By default the allocator simply allocates the first available bytes from the last free metadata. For most uses that is adequete since ephemeral allocators are disposed of. For the persistent pool that can leave holes as dlopen() calls are made and various arrays are updates and unless they are all freed they block reclamation from the high water mark. In order to cope with this a simple best fit mode is supported. In this mode the format of the pool is identical, there are no changes to the metadata. A simple best fit algorithm operates as follows:

1. The current pool is selected
2. If the alignment is anything other than 16 bytes it falls back to the normal algorithm
3. All the metadatas are walked looking for any free metadata large enough to hold the requested allocation
4. If an exact size is found it immediately selects that block otherwise the one with the least extra space is chosen.
4. If no blocks are found it falls back to the normal algorithm
5. The best fit block is marked as allocated
6. At that point the logic from `realloc` is invoked to shrink the block to the correct size

