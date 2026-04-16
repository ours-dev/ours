/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef OURS_CPU_MASK_HPP
#define OURS_CPU_MASK_HPP 1

#include <ours/types.hpp>

namespace ours {
     class CpuMask: public ustl::BitSet<MAX_CPU> {
        using Self = CpuMask;
        using Base = ustl::BitSet<MAX_CPU>;
      public:
        using Base::Base;
        using Base::for_each;
        using Base::operator=;
        using Base::operator|=;
        using Base::operator&=;
        using Base::operator^=;
        using Base::operator<<=;
        using Base::operator>>=;

        CpuMask() = default;

        CpuMask(Base const &base)
            : Base(base)
        {}

        ~CpuMask() = default;

        static auto of(CpuNum cpunum) -> Self {
            return Self().set(cpunum);
        }

        template <typename F>
            requires ustl::traits::IsSameV<ustl::traits::InvokeResultT<F, CpuNum>, Status>
        auto for_each(F &&functor) const -> void {
            for (auto i = 0; i < this->size(); ++i) {
                if (!this->test(i)) {
                    continue;
                }
                auto const status = ustl::function::invoke(functor, CpuNum(i));
                if (Status::Ok == status) {
                    return;
                }
            }
        }
    };   
} // namespace ours

#endif // #ifndef OURS_CPU_MASK_HPP