#ifndef SAN_FUNC
#error SAN_FUNC must be defined
#endif

// SAN_FUNC(<function name>, <return type>, <args>...)
// Declares functions in the ASan runtime that we want
// to be callable.
SAN_FUNC(__sanitizer_get_ownership, int, const volatile void *)
SAN_FUNC(__sanitizer_get_heap_size, size_t, void)
SAN_FUNC(__asan_region_is_poisoned, void*, void *, size_t)