/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_DPC_HPP
#define OURS_DPC_HPP 1

#include <ours/compiler.hpp>
#include <ours/status.hpp>

#include <ours/sync/event.hpp>
#include <ours/sync/spin-lock.hpp>
#include <ours/task/types.hpp>
#include <ours/mem/cpu-local.hpp>

#include <utility>
#include <functional>

#include <ustl/collections/intrusive/list.hpp>

namespace ours {
    class Dpc: public ustl::collections::intrusive::ListBaseHook<> {
        using Self = Dpc;
      public:
        enum class Level {
            Count,
        };

        using Callback = ustl::function::Fn<auto () -> void>;

        template <typename F, typename... Args>
        Dpc(F &&f, Args&&... args)
            : m_callback(std::bind_front(std::forward<F>(f), std::forward<Args>(args)...))
        {}
      private:
        friend class DpcRunner;

        FORCE_INLINE
        auto invoke() -> void {
            if (m_callback) {
                m_callback();
            }
        }

        Callback m_callback;
    };

    class DpcRunner {
        using Self = DpcRunner;
      public:
        using Current = mem::CpuLocal::Current<DpcRunner>;

        static auto init() -> void;

        static auto enqueue(Dpc &dpc, Dpc::Level) -> Status;
      private:
        class Queue {
          public:
            auto do_work() -> i32;

            auto enqueue(Dpc &dpc) -> void;
          private:
            sync::Event m_event;
            sync::SpinLock m_lock;
            task::Thread *m_thread;
            ustl::collections::intrusive::List<Dpc> m_list;
        };

        /// Multi-level priority queues.
        Queue m_dpc_queues[static_cast<usize>(Dpc::Level::Count)];
    };

} // namespace ours

#endif // #ifndef OURS_DPC_HPP