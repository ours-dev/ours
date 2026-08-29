/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef GKTL_DEFER_HPP
#define GKTL_DEFER_HPP

namespace gktl {
template <typename Invocable_>
class DeferredAction final {
    using Self = DeferredAction;

  public:
    using Invocable = Invocable_;

    DeferredAction(Invocable invocable) : invoked_(false), invocable_(invocable) {}

    ~DeferredAction() {
        invoke();
    }

    auto invoke() -> void {
        if (!invoked_) {
            invocable_();
        }
    }

    auto invoke() const -> void {
        if (!invoked_) {
            invocable_();
        }
    }

    auto cancel() -> void {
        invoked_ = true;
    }

  private:
    bool invoked_;
    Invocable invocable_;
};

template <typename Invocable>
DeferredAction(Invocable invocable) -> DeferredAction<Invocable>;

template <typename Invocable>
auto defer(Invocable invocable) -> DeferredAction<Invocable> {
    return DeferredAction<Invocable>(invocable);
}
} // namespace gktl

#endif // #ifndef GKTL_DEFER_HPP