# abi changes

## Title: The exception channel's ABI: ExceptionInfo, ExceptionType and PolicyCode made public, ExceptionState, Property, ObjectType::Exception (2026-10-04)

**Files**
- `system/public/ours/exception.hpp` — new: `ExceptionType` and `PolicyCode` (moved from the kernel), `ExceptionInfo`, `ExceptionChannelOptions`, `ExceptionState`.
- `system/public/ours/object_property.hpp` — new: `Property`, with `ExceptionState` the only one.
- `system/public/ours/types.hpp` — `ObjectType::Exception = 29`.
- `system/public/public_headers.gni` — lists the two headers.
- `kernel/include/ours/task/exception.hpp` — uses the public enums instead of defining its own.
- `kernel/object/include/ours/object/thread_dispatcher.hpp` — its opaque declaration of `PolicyCode` follows the enum to namespace `ours`.
- `.agents/tasks/exception-channels/PLAN.md` — step 2 done.

**Symptom**
None: nothing uses the new types yet, and the kernel compiles to the same behaviour. Step 2 of `.agents/tasks/exception-channels/`.

**Cause**
An exception channel hands user space a record of each exception, so the kinds of exception stop being the kernel's private business. `ExceptionType` and `PolicyCode` were defined in `kernel/include/ours/task/exception.hpp`, with Zircon's values already, "so that the day it becomes ABI it needs no renumbering". That day is this one. They move to `system/public` instead of being copied there: two definitions of one ABI drift apart, and nothing would notice.

`ExceptionInfo` is the message's data, and it is copied byte for byte into a channel message that user space reads. A struct the compiler pads leaves the padding bytes unwritten, so whatever the kernel stack held there would reach the reader. Zircon spells the hole out as `uint8_t padding1[4]`; so does this, and `static_assert`s pin its size, alignment and every offset. `user_copy::CopyAllowed` (`kernel/lib/user_copy/include/user_copy/copy_allowed.hpp`) refuses a type with implicit padding through `std::has_unique_object_representations_v`, so the explicit field is what will let the kernel copy it.

**Change**
- `ExceptionInfo { KoId pid; KoId tid; ExceptionType type; u8 padding1[4]; }`, 24 bytes, Zircon's `zx_exception_info_t`.
- `ExceptionChannelOptions { None = 0 }`; Zircon's `DEBUGGER` (1) comes with the debugger channel.
- `ExceptionState { TryNext = 0, Handled = 1 }`; Zircon's `THREAD_EXIT` (2) comes with a consumer for it.
- `Property { ExceptionState = 16 }`, Zircon's `ZX_PROP_EXCEPTION_STATE`, in a header of its own beside `object_info.hpp`, since properties are not exception-specific.
- `ObjectType::Exception = 29`, Zircon's `ZX_OBJ_TYPE_EXCEPTION`.
- In the kernel, `using ours::ExceptionType; using ours::PolicyCode;` in `ours::task`, so `task::ExceptionType::General` and the `task::to_string()` overloads still work; the diagnostics stay in the kernel.

Values a slice-1 kernel does not answer are left out rather than defined and refused, as `PortCreateOptions` leaves Zircon's `BIND_TO_INTERRUPT` out.

**Reference**
Zircon `zircon/system/public/zircon/syscalls/exception.h` (`zx_exception_info_t`, `ZX_EXCP_*`, `ZX_EXCP_POLICY_CODE_*`, `ZX_EXCEPTION_STATE_*`, `ZX_EXCEPTION_CHANNEL_DEBUGGER`), `zircon/syscalls/object.h` (`ZX_PROP_EXCEPTION_STATE`), `zircon/types.h` (`ZX_OBJ_TYPE_EXCEPTION`).

**Verification**
- Isolated copy of HEAD with only this change: `boot_check.py --out out/task-wake`, dbg PASS (22 passed), rel PASS (20 passed), 0 error lines; all 16 fault probes `killed (retcode -1028)` as before. The kernel's own translation units include `exception.hpp`, so its `static_assert`s are checked by the build.
- Both new headers compiled on their own (`-fsyntax-only`, with the kernel's flags): ok. `object_property.hpp` has no includer until step 4.
- clang-tidy: no finding in the headers changed.

**Deliberately not done**
- The default rights of an exception handle (`RIGHTS_DEFAULT_EXCEPTION`): they live beside the dispatcher, as `RIGHTS_DEFAULT_THREAD` does, in step 3.
- The synthetic exception types (THREAD_STARTING and the like) and `ZX_EXCP_USER`: with the code that raises them.

**How it was found**
None: a planned step.

**The user's prediction**
None.

**Three questions you should be able to answer**
- What would a reader of the exception message see in bytes 20 to 23 if `padding1` were not a field?
- Why move `ExceptionType` to `system/public` rather than define a public copy?
- Why does `thread_dispatcher.hpp`'s opaque declaration of `PolicyCode` have to move namespaces along with the enum?
