# thread-sampler outstanding repairs

## 1. A session's record slots are zeroed twice at create

**Severity**
Low: up to 4 MiB per possible CPU written twice per `sampler_create`, about 16.8 MB at 4 CPUs at the maximum.

**Symptom**
The slots are above KMALLOC_MAX_SIZE, so `new` takes them from vmalloc, whose pages `VmCowPages::alloc_pages` has zeroed already; since de9ea9c6 `Vec`'s value-initialization zeroes them again. The change record of de9ea9c6 does not say so.

**Cause**
Vec's allocator contract, on top of vmalloc's zeroed pages.

**Change**
The same opt-in non-zeroing path as object item 1, or accept it: it is paid once per create. Time `sampler_create` at the maximum buffer size before and after.

**Reference**
None.

**Acceptance**
`sampler_create` at 4 MiB per CPU takes no longer than before de9ea9c6.
