# syscall outstanding repairs

## 1. channel_write and channel_read zero and destroy a 64-slot handle array on every call

**Severity**
Low: about 128 quadword stores and 64 branches per call, even with no handles; below about 512 bytes it outweighs the payload zeroing.

**Symptom**
`kernel/lib/syscall/src/channel.cpp` declares `ustl::Box<object::Handle> taken[64]`: 64 Box constructors zero it and 64 destructors run on every exit.

**Cause**
A fixed staging array sized for the maximum.

**Change**
A `StaticVec<Box<Handle>, 64>` filled with exactly the handles given, or the upstream order: build the packet first and move the handles from the table straight into its slots. It touches the code that keeps each handle in one place; it needs the handle-transfer checks and a review.

**Reference**
Zircon `lib/syscalls/channel.cc` `msg_put_handles` writes straight into the packet.

**Acceptance**
Handle-transfer checks pass, and the measurement's 0-handle cells get faster.
