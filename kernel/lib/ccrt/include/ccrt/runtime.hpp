/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef CCRT_RUNTIME_HPP
#define CCRT_RUNTIME_HPP 1

namespace ccrt {
class alignas(8) Guard final {
  public:
    constexpr Guard() noexcept : m_state{} {}

    Guard(Guard const &) = delete;
    auto operator=(Guard const &) -> Guard & = delete;

    [[nodiscard]]
    auto acquire() noexcept -> bool;
    auto release() noexcept -> void;
    auto abort() noexcept -> void;

  private:
    // The first byte is the compiler-visible completion flag. The second byte
    // is owned by the runtime as the initialization lock.
    unsigned char m_state[8];
};

using Initializer = auto (*)() -> void;
using Destructor = auto (*)(void *) -> void;
using ExitFunction = auto (*)() -> void;

struct Scope {
    void const *m_identity;
};

class Registry final {
  public:
    constexpr Registry() noexcept = default;

    Registry(Registry const &) = delete;
    auto operator=(Registry const &) -> Registry & = delete;

    static auto instance() noexcept -> Registry &;

    [[nodiscard]]
    auto register_destructor(Destructor destructor, void *argument, Scope scope) noexcept -> bool;
    [[nodiscard]]
    auto register_exit_function(ExitFunction function) noexcept -> bool;
    auto finalize(Scope scope = {}) noexcept -> void;

  private:
    using Byte = unsigned char;

    /// What a record holds. None is a free slot: one never filled, or one
    /// whose callback finalize() has taken. It is zero, so the
    /// zero-initialised table starts free, and it is the record's only
    /// "active" fact: a separate flag would say the same thing twice.
    enum class CallbackKind : Byte {
        None,
        Destructor,
        ExitFunction,
    };

    union Callback {
        Destructor m_destructor;
        ExitFunction m_exit_function;
    };

    struct Record {
        Callback m_callback;
        void *m_argument;
        Scope m_scope;
        CallbackKind m_kind;
    };

    static constexpr auto MAX_RECORDS = 256U;

    auto lock() noexcept -> void;
    auto unlock() noexcept -> void;
    auto compact() noexcept -> void;
    [[nodiscard]]
    auto matches(Record const &record, Scope scope) const noexcept -> bool;
    [[nodiscard]]
    auto register_callback(Callback callback, void *argument, Scope scope,
                           CallbackKind kind) noexcept -> bool;

    Record m_records[MAX_RECORDS]{};
    unsigned int m_record_count{};
    Byte m_lock{};
};

static_assert(sizeof(Guard) == 8);
static_assert(alignof(Guard) == 8);

/// Run the linker-owned dynamic initialization table exactly once per image.
auto init_global_static_objects() -> void;
} // namespace ccrt

#endif // #ifndef CCRT_RUNTIME_HPP
