/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef RUNTIME_PROCESS_ARGS_HPP
#define RUNTIME_PROCESS_ARGS_HPP 1

#include <span>
#include <string_view>

#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

namespace runtime {
/// A processargs message (<ours/process_args.hpp>), checked, and read in
/// place: every string handed out points into the message, so the message
/// must outlive this view.
///
/// The one decoder of the one format. It is strict where Zircon's two are
/// not quite: the old musl processargs_read() never looks at the names
/// section, and the newer ld::ProcessargsBuffer accepts a message whose
/// strings run off its end and quietly reports fewer of them. With one
/// encoder in the whole system (ours::encode_process_args()), a message that
/// fails any check here is that encoder's bug or someone else's forgery, and
/// either way the program should stop rather than start with an argument
/// missing.
class ProcessArgs {
  public:
    /// A run of NUL-terminated strings that parse() has checked: each one
    /// ends inside the message. Iterates as std::string_views whose data()
    /// is NUL-terminated, so each can also be handed out as a C string.
    class Strings {
      public:
        class Iterator {
          public:
            [[nodiscard]]
            auto operator*() const -> std::string_view {
                return m_at;
            }

            auto operator++() -> Iterator & {
                m_at += std::string_view(m_at).size() + 1;
                m_left -= 1;
                return *this;
            }

            [[nodiscard]]
            auto operator==(Iterator const &other) const -> bool {
                return m_left == other.m_left;
            }

          private:
            friend class Strings;

            Iterator(char const *const at, u32 const left) : m_at(at), m_left(left) {}

            char const *m_at;
            u32 m_left;
        };

        Strings() = default;

        [[nodiscard]]
        auto size() const -> u32 {
            return m_count;
        }

        [[nodiscard]]
        auto begin() const -> Iterator {
            return {m_first, m_count};
        }

        [[nodiscard]]
        auto end() const -> Iterator {
            return {nullptr, 0};
        }

      private:
        friend class ProcessArgs;

        Strings(char const *const first, u32 const count) : m_first(first), m_count(count) {}

        char const *m_first = nullptr;
        u32 m_count = 0;
    };

    ProcessArgs() = default;

    /// Checks |message|, which arrived with |num_handles| handles, and on Ok
    /// makes |out| a view of it. Every failure is InvArgs, as Zircon's
    /// processargs_read() answers ZX_ERR_INVALID_ARGS for a malformed
    /// message; the checks, in order:
    ///
    ///   - the message holds at least the header;
    ///   - protocol and version are exactly PROCARGS_PROTOCOL and
    ///     PROCARGS_VERSION;
    ///   - the handle_info array starts after the header, on a 4-byte
    ///     boundary, and holds |num_handles| entries before the end;
    ///   - no handle_info sets the reserved bits 8-15;
    ///   - each string section that has any strings starts after the header
    ///     and before the end, and has room for that many (each at least its
    ///     NUL);
    ///   - every one of those strings ends in a NUL inside the message.
    ///
    /// An unknown handle type is not an error: it is the reader's to ignore.
    /// Nothing checks that an NsDir's arg names an existing name; whoever
    /// uses it looks it up in names() and checks there.
    [[nodiscard]]
    static auto parse(std::span<u8 const> message, u32 num_handles, ProcessArgs *out)
        -> ours::Status;

    /// How many handles the message carried, and so how many handle_info
    /// entries it has.
    [[nodiscard]]
    auto num_handles() const -> u32 {
        return m_num_handles;
    }

    /// The handle_info of the message's |index|-th handle; |index| must be
    /// below num_handles().
    [[nodiscard]]
    auto handle_info(u32 index) const -> u32;

    [[nodiscard]]
    auto args() const -> Strings {
        return m_args;
    }

    [[nodiscard]]
    auto environ() const -> Strings {
        return m_environ;
    }

    [[nodiscard]]
    auto names() const -> Strings {
        return m_names;
    }

  private:
    u8 const *m_message = nullptr;
    u32 m_num_handles = 0;
    u32 m_handle_info_off = 0;
    Strings m_args;
    Strings m_environ;
    Strings m_names;
};
} // namespace runtime

#endif // #ifndef RUNTIME_PROCESS_ARGS_HPP
