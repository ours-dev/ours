/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <syscall/handlers.hpp>

#include <cstring>

#include <gktl/command.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace ours::syscall {
namespace {
    /// The registry's bounds, as the linker computes them.
    ///
    /// kernel/kernel.ld puts every GKTL_COMMAND record between these two, so
    /// walking them is the whole of "what commands exist" -- the same source
    /// gktl::run_command() builds its radix tree from
    /// (kernel/lib/gktl/src/command.cpp), rather than a second list that could
    /// disagree with it.
    extern "C" gktl::Command const COMMANDS_START[] LINK_NAME("__commands_start");
    extern "C" gktl::Command const COMMANDS_END[] LINK_NAME("__commands_end");

    /// Text on its way into a user buffer, a piece at a time.
    ///
    /// The registry's strings live in the kernel image, and the destination is
    /// user memory, so every piece crosses the boundary through a copy; there
    /// is no buffer large enough on a kernel stack to assemble the whole list
    /// first. It keeps the old contract: what fits is written, the length that
    /// *would* have been written is still counted so the answer can tell "filled
    /// exactly" (Ok) from "truncated" (NoSpace), and either answer leaves the
    /// text NUL terminated; after a failed copy the answer is that copy's status
    /// and no terminator is written.
    ///
    /// One byte of the caller's buffer is held back for that terminator.
    class UserTextWriter final {
      public:
        UserTextWriter(user_copy::UserOutPtr<char> const buffer, usize const size)
            : m_buffer(buffer), m_capacity(size - 1) {}

        auto append(char const *const text) -> void {
            if (text == nullptr) {
                return;
            }
            auto const length = std::strlen(text);
            if (m_status == Status::Ok && m_length < m_capacity) {
                auto count = m_capacity - m_length;
                if (length < count) {
                    count = length;
                }
                m_status = m_buffer.element_offset(m_length).copy_array_to_user(text, count);
            }
            m_length += length;
        }

        /// Terminates the text and says whether all of it fitted (Ok or
        /// NoSpace), unless a copy already failed: then that copy's status, and
        /// no terminator.
        [[nodiscard]]
        auto finish() -> Status {
            if (m_status != Status::Ok) {
                return m_status;
            }
            auto end = m_length;
            auto result = Status::Ok;
            if (end > m_capacity) {
                end = m_capacity;
                result = Status::NoSpace;
            }
            constexpr char NUL = '\0';
            if (auto const status = m_buffer.element_offset(end).copy_to_user(NUL);
                status != Status::Ok) {
                return status;
            }
            return result;
        }

      private:
        user_copy::UserOutPtr<char> m_buffer;
        usize m_capacity;
        usize m_length = 0;
        Status m_status = Status::Ok;
    };
} // namespace

auto command_list(user_copy::UserOutPtr<char> const buffer, usize const length) -> Status {
    if (buffer.is_null() || length == 0) {
        return Status::InvArgs;
    }

    // The whole buffer up front, not piece by piece as the copies go: a buffer
    // that runs out of the user half is refused before any of it is written,
    // rather than half-filled and then refused.
    if (!user_copy::is_user_range(buffer.address(), length)) {
        return Status::InvArgs;
    }

    UserTextWriter writer(buffer, length);
    for (auto const *command = COMMANDS_START; command != COMMANDS_END; ++command) {
        if (command->cmd == nullptr) {
            continue;
        }
        writer.append(command->cmd);
        if (command->help != nullptr && command->help[0] != '\0') {
            writer.append("  ");
            writer.append(command->help);
        }
        writer.append("\n");
    }
    return writer.finish();
}
} // namespace ours::syscall
