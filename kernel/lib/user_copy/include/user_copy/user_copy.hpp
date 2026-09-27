/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USER_COPY_USER_COPY_HPP
#define USER_COPY_USER_COPY_HPP 1

#include <limits>
#include <type_traits>

#include <ours/arch/user_copy.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace user_copy {
/// The only ways the kernel reads or writes user memory.
///
/// A system call's pointer argument is an address a user program chose. The
/// kernel may not dereference it: it may point into the kernel half, wrap past
/// the top of the address space, or name a page another of the program's
/// threads unmaps between a check and the access -- and on a CPU with SMAP
/// the dereference itself faults, because ring 0 may touch user pages only
/// inside the stac/clac window this library opens. Before it existed, both
/// handlers read and wrote through the raw pointer after a range check, which
/// on a SMAP CPU hung the machine at the first debug_write.
///
/// Zircon counterpart: //zircon/kernel/lib/user_copy -- user_ptr<T, Policy>
/// over arch_copy_from_user()/arch_copy_to_user().
///
/// Rules for callers, which the types cannot check:
///  - The plain copies resolve a fault in place, so they must not run under a
///    spin lock, nor under any lock the page-fault path takes -- the address
///    space's lock, or a VMO's. Resolving can sleep, and takes those locks. The
///    first version of the SMAP check hung silently for exactly this reason: the
///    user buffer was first touched inside the logger, under its lock, and the
///    fault handler's own log line waited on that lock forever.
///  - A copy that has to run under such a lock uses the *_capture_faults
///    variants. They do not resolve the fault; they hand it back, and the
///    caller drops its lock, resolves it with Thread::Current::soft_fault(),
///    re-takes the lock, looks its state up again, and retries. Do not route
///    the bytes through a kernel bounce buffer instead: it costs a copy per
///    page and exists only to avoid holding the lock, which capture already
///    makes safe.

/// Whether [addr, addr + size) is a range a user program may name.
[[nodiscard]]
auto is_user_range(ours::VirtAddr addr, usize size) -> bool;

/// Copies |size| bytes from user address |src| into kernel memory at |dst|.
///
/// InvArgs if the range is not wholly in the user half, and InvArgs if it is
/// but some page of it cannot be made present -- unmapped, or mapped without
/// the access the copy needs. The copy may have moved some bytes by then; a
/// caller that failed has nothing to rely on in |dst|.
[[nodiscard]]
auto copy_from_user(void *dst, ours::VirtAddr src, usize size) -> ours::Status;

/// Copies |size| bytes from kernel memory at |src| to user address |dst|.
/// Same contract as copy_from_user().
[[nodiscard]]
auto copy_to_user(ours::VirtAddr dst, void const *src, usize size) -> ours::Status;

/// copy_from_user(), except that a page fault on the user side is handed back
/// rather than resolved, so that it is safe under a lock (see the rules above).
///
/// Three outcomes, as in Zircon's UserCopyCaptureFaultsResult:
///  - Ok, no fault: everything was copied.
///  - InvArgs with a fault: the copy stopped on a user page that is not
///    present or not accessible the way the copy needed. soft_fault() the
///    fault once the lock is dropped; if that succeeds, retry the whole
///    request. The bytes before the fault may or may not have moved.
///  - InvArgs, no fault: the range is not wholly in the user half, and nothing
///    was attempted. There is nothing to resolve; it is a bad argument.
[[nodiscard]]
auto copy_from_user_capture_faults(void *dst, ours::VirtAddr src, usize size)
    -> ours::UserCopyCaptureResult;

/// copy_to_user() with fault capture. Same contract as
/// copy_from_user_capture_faults().
[[nodiscard]]
auto copy_to_user_capture_faults(ours::VirtAddr dst, void const *src, usize size)
    -> ours::UserCopyCaptureResult;

/// Which way a user pointer may be copied through.
enum class Direction {
    In,
    Out,
    InOut,
};

/// A system call's pointer argument, as a type the kernel cannot dereference.
///
/// It holds the address as a number, not as a `T *`, so that `*p` and `p->x`
/// do not compile: the only operations are copying through it, which checks
/// the range, and asking for the number, which is for logging. The direction
/// is part of the type so that a handler cannot write through a pointer the
/// ABI documents as input -- that mistake becomes a compile error rather than
/// a user buffer the kernel quietly scribbles on.
///
/// The element type must be trivially copyable, because copying is all that
/// will ever happen to it.
template <typename T, Direction D>
class UserPtr final {
    using Value = std::remove_const_t<T>;
    static_assert(std::is_trivially_copyable_v<Value>,
                  "a user pointer's element is copied as bytes, and nothing else");

  public:
    constexpr UserPtr() = default;

    /// From the register the argument arrived in.
    explicit constexpr UserPtr(ours::VirtAddr const address) : m_address(address) {}

    /// The address, as a number -- for logging and for the range check, never
    /// for dereferencing.
    [[nodiscard]]
    constexpr auto address() const -> ours::VirtAddr {
        return m_address;
    }

    [[nodiscard]]
    constexpr auto is_null() const -> bool {
        return m_address == 0;
    }

    /// The pointer |count| elements further on.
    ///
    /// The arithmetic may wrap, and that is deliberate rather than checked
    /// here: a wrapped address is refused by the copy's range check, which is
    /// the one place every access passes through.
    [[nodiscard]]
    constexpr auto element_offset(usize const count) const -> UserPtr {
        return UserPtr(m_address + count * sizeof(Value));
    }

    [[nodiscard]]
    auto copy_from_user(Value *const dst) const -> ours::Status
        requires(D != Direction::Out)
    {
        return user_copy::copy_from_user(dst, m_address, sizeof(Value));
    }

    [[nodiscard]]
    auto copy_array_from_user(Value *const dst, usize const count) const -> ours::Status
        requires(D != Direction::Out)
    {
        if (count > std::numeric_limits<usize>::max() / sizeof(Value)) {
            return ours::Status::InvArgs;
        }
        return user_copy::copy_from_user(dst, m_address, count * sizeof(Value));
    }

    [[nodiscard]]
    auto copy_to_user(Value const &src) const -> ours::Status
        requires(D != Direction::In)
    {
        return user_copy::copy_to_user(m_address, &src, sizeof(Value));
    }

    [[nodiscard]]
    auto copy_array_to_user(Value const *const src, usize const count) const -> ours::Status
        requires(D != Direction::In)
    {
        if (count > std::numeric_limits<usize>::max() / sizeof(Value)) {
            return ours::Status::InvArgs;
        }
        return user_copy::copy_to_user(m_address, src, count * sizeof(Value));
    }

    /// The four copies above with fault capture. A count too large to express
    /// in bytes is refused without a fault, like an address outside the user
    /// half: there is nothing to resolve.
    [[nodiscard]]
    auto copy_from_user_capture_faults(Value *const dst) const -> ours::UserCopyCaptureResult
        requires(D != Direction::Out)
    {
        return user_copy::copy_from_user_capture_faults(dst, m_address, sizeof(Value));
    }

    [[nodiscard]]
    auto copy_array_from_user_capture_faults(Value *const dst, usize const count) const
        -> ours::UserCopyCaptureResult
        requires(D != Direction::Out)
    {
        if (count > std::numeric_limits<usize>::max() / sizeof(Value)) {
            return ours::UserCopyCaptureResult{.status = ours::Status::InvArgs, .fault = {}};
        }
        return user_copy::copy_from_user_capture_faults(dst, m_address, count * sizeof(Value));
    }

    [[nodiscard]]
    auto copy_to_user_capture_faults(Value const &src) const -> ours::UserCopyCaptureResult
        requires(D != Direction::In)
    {
        return user_copy::copy_to_user_capture_faults(m_address, &src, sizeof(Value));
    }

    [[nodiscard]]
    auto copy_array_to_user_capture_faults(Value const *const src, usize const count) const
        -> ours::UserCopyCaptureResult
        requires(D != Direction::In)
    {
        if (count > std::numeric_limits<usize>::max() / sizeof(Value)) {
            return ours::UserCopyCaptureResult{.status = ours::Status::InvArgs, .fault = {}};
        }
        return user_copy::copy_to_user_capture_faults(m_address, src, count * sizeof(Value));
    }

  private:
    ours::VirtAddr m_address = 0;
};

template <typename T>
using UserInPtr = UserPtr<T, Direction::In>;

template <typename T>
using UserOutPtr = UserPtr<T, Direction::Out>;

template <typename T>
using UserInOutPtr = UserPtr<T, Direction::InOut>;

} // namespace user_copy

#endif // #ifndef USER_COPY_USER_COPY_HPP
