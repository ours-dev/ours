/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef USTL_LAZY_INIT_HPP
#define USTL_LAZY_INIT_HPP 1

#include <cstddef>
#include <memory>
#include <utility>

namespace ustl {
    /// Storage for an object whose lifetime begins only when init() is called.
    /// The owner is responsible for ensuring init() is called at most once.
    template <typename T>
    class LazyInit final {
      public:
        constexpr LazyInit() noexcept = default;

        LazyInit(LazyInit const &) = delete;
        auto operator=(LazyInit const &) -> LazyInit & = delete;

        template <typename... Args>
        auto init(Args &&...args) -> T * {
            return std::construct_at(data(), std::forward<Args>(args)...);
        }

        [[nodiscard]] auto data() noexcept -> T * {
            return reinterpret_cast<T *>(m_storage);
        }

        [[nodiscard]] auto data() const noexcept -> T const * {
            return reinterpret_cast<T const *>(m_storage);
        }

        [[nodiscard]] auto operator->() noexcept -> T * {
            return data();
        }

        [[nodiscard]] auto operator->() const noexcept -> T const * {
            return data();
        }

        [[nodiscard]] auto operator*() noexcept -> T & {
            return *data();
        }

        [[nodiscard]] auto operator*() const noexcept -> T const & {
            return *data();
        }

      private:
        alignas(T) std::byte m_storage[sizeof(T)] {};
    };
} // namespace ustl

#endif // USTL_LAZY_INIT_HPP
