/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_IRQ_DOMAIN_HPP
#define OURS_IRQ_DOMAIN_HPP 1

#include <ours/irq/controller.hpp>
#include <ours/irq/types.hpp>
#include <ours/status.hpp>
#include <ours/task/irq_rcu.hpp>

#include <ustl/option.hpp>
#include <ustl/util/noncopyable.hpp>
#include <ustl/views/span.hpp>

namespace ours::irq {
class Domain;
class DomainRegistry;

/// Controller-owned dispatch access to its registered source set. The endpoint
/// hides lookup and reader lifetime; no domain or line pointer escapes it.
/// It must outlive registration and every controller entry that uses it.
class DomainEndpoint final {
  public:
    DomainEndpoint() = default;
    USTL_NO_MOVEABLE_AND_COPYABLE(DomainEndpoint);
    ~DomainEndpoint() = default;

    /// Some means the source existed and its receiving-CPU interrupt was
    /// ended, including an unclaimed source. None leaves completion to the
    /// root controller. The endpoint opens its own reader scope.
    [[nodiscard]]
    auto dispatch(HIrqNum source) const -> ustl::Option<IrqReturn>;

  private:
    friend class DomainRegistry;
    task::IrqRcuPointer<Domain> m_domain;
};

/// Register a complete source set. Controller operations and context outlive
/// the domain. The root uses DomainId::Root; other ids are registrant-chosen.
/// Registration failure keeps neither storage nor an endpoint publication.
[[nodiscard]]
auto register_domain(DomainId id, ustl::views::Span<HIrqNum const> sources,
                     ControllerOps const &controller, DomainEndpoint &endpoint,
                     void *context = nullptr) -> Status;

/// Thread context with interrupts and preemption on. The caller first stops
/// delivery into the endpoint; on success revocation and the reader grace
/// period have completed, so controller context may be released. Root is
/// permanent and answers NotSupported.
[[nodiscard]]
auto unregister_domain(DomainId id) -> Status;
} // namespace ours::irq

#endif // OURS_IRQ_DOMAIN_HPP
