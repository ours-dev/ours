/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_MEM_NODE_MASK_HPP
#define OURS_MEM_NODE_MASK_HPP 1

#include <ours/status.hpp>
#include <ours/mem/types.hpp>

#include <ustl/bitset.hpp>
#include <ustl/function/invoke.hpp>
#include <ustl/traits/is_invocable.hpp>

namespace ours::mem {
    class NodeMask: public ustl::BitSet<NUMA_MAX_NODE> {
        using Self = NodeMask;
        using Base = ustl::BitSet<NUMA_MAX_NODE>;
      public:
        using Base::Base;

        template <typename F>
            requires ustl::traits::Invocable<F, NnId>
        auto for_each_if_noerr(F &&functor) const -> Status {
            auto const n = this->size();
            for (NnId nid = 0; nid < n; ++nid) {
                if (this->test(nid)) {
                    auto status = ustl::function::invoke(functor, nid);
                    if (Status::Ok != status) {
                        return status;
                    }
                }
            }

            return Status::Ok;
        }

        template <typename F>
            requires ustl::traits::Invocable<F, NnId>
        auto find(F &&matcher) const -> NnId {
            auto const n = this->size();
            for (NnId nid = 0; nid < n; ++nid) {
                if (this->test(nid)) {
                    continue;
                }

                if (matcher(nid)) {
                    return nid;
                }
            }
        }
    };

} // namespace ours::mem

#endif // #ifndef OURS_MEM_NODE_MASK_HPP