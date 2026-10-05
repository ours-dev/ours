# ustl changes

## Vec::resize, so a vector can be made a given length in one allocation (2026-10-05)

**Files**
- `system/lib/ustl/include/ustl/collections/vec.hpp` — `resize(n)`
- `system/lib/ustl/test/vec_test.cpp`, `system/lib/ustl/BUILD.gn` — new `vec-test`

**Symptom**
Code that needed a buffer of a known length kept it as a raw pointer from `new T[n]` or `kmalloc`, freed by hand, because `Vec` could only grow one `emplace_back()` at a time.

**Cause**
`Vec` had `reserve()` but nothing that sets the length, so a length of n took n appends.

**Change**
`resize(n)` value-initializes the elements it adds and shrinks without freeing, like the rest of `Vec`. Growing past the capacity allocates exactly n once, and the allocator has value-initialized every slot of a new buffer already, so those slots are not written again. Growing within the capacity resets the reused slots, which may hold what a shrink cut off.

**Reference**
`std::vector::resize`, with this `Vec`'s allocator contract: slots come value-initialized and stay constructed.

**Verification**
`vec-test`: growing from nothing, keeping elements while growing, shrinking without freeing, the reused slots reset when growing back, and no buffer for zero elements. Built with the two users below; both images boot.

**Deliberately not done**
None.

**How it was found**
The user asked for `Vec` in place of hand-written raw pointers; the two users below needed a length set in one step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- Why can `resize` skip writing the new slots when it reallocates, but not when it does not?
- What does this `Vec` require of its element type that `std::vector` does not, and why?
- What does `resize(0)` on an empty vector allocate?
