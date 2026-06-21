# XArray Review

## Resolved In This Slice

- `Entry` is now private implementation detail; users no longer construct tagged entries.
- Public API is pointer-oriented: `store(index, object)`, `load(index)`, `erase(index)`, `find(cursor)`, and typed wrappers such as `load<T>()`.
- `store()` rejects null and low-bit-tagged object pointers, keeping the internal tag invariant contained inside XArray.

## Remaining Design Notes

1. **Synchronization is still external/unspecified.**  
   The implementation is a non-concurrent radix tree. Before using it in shared kernel paths, decide whether `XArray` should own a lock or require callers to provide synchronization.

2. **Multi-index entries are not implemented.**  
   `reserve()` and `order > 0` remain unsupported until the API and node accounting for multi-index ranges are settled.

3. **Allocation policy is still plain C++ allocation.**  
   Nodes currently use `new (std::nothrow)` / `delete`. Kernel integration may want an explicit allocator or object cache hook.

## Verification

- `c++ -std=c++23 -Ikernel/lib/xarray/include -Isystem/public -Isystem/lib/ustl/include -Isystem/lib/bits/include -fsyntax-only kernel/lib/xarray/xarray.cpp` passes.
- `c++ -std=c++23 -Ikernel/lib/xarray/include -Isystem/public -Isystem/lib/ustl/include -Isystem/lib/bits/include -Iexternal/googletest+/googletest/include -fsyntax-only kernel/lib/xarray/tests/xarray-test.cpp` passes.
- `/tmp/xarray-smoke` compiled and ran successfully against the pointer-oriented API.
- `bazel test //kernel/lib/xarray:xarray-test` still fails before test execution because the configured `llvm-ar-22` archive action receives no archive argument.
