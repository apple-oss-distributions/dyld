#  Linker Standard Library

This is `lsl`, the Linker Standard Library. It is not intended as a complete replacement for `std`, but it is designed to provide collections,
allocators, and basic utility functions in environments where there is not a libc. It is intended to be used by common code shared between
`ld64`, `dyld`, and the `dyld_shared_cache_builder`. That means it needs to work in both single threaded memory constrained environments for
`dyld`,  and high memory aggressively multi-threaded environments for the other tools. It currently provides the following pieces of
functionality:

* Allocators
** `EphemeralAllocator`: A fast bump pointer allocator, intended for use with scoped operations
** `PersistentAllocator`: A best fit allocator intended for long lived allocations

* SmartPointers
** `UniquePtr`: A pointer for object ownership and automatic memory reclamation
** `SharedPtr`: A reference counted pointer. All allocation from the provided allocators support implicit `shared_from_this` semantics

* Collections
** `OrderedSet`
** `OrderedMultiSet`
** `OrderedMap`
** `OrderedMultiMap`
** `Vector`

* Other Data Types:
** `UUID`

* TODO:
** `WeakPtr`
** `UnorderedSet`
** `UnorderedMap`
** `String`
** Better documentation

