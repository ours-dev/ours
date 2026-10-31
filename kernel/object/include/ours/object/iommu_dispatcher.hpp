/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_OBJECT_IOMMU_DISPATCHER_HPP
#define OURS_OBJECT_IOMMU_DISPATCHER_HPP 1

#include <ours/bti.hpp>
#include <ours/object/dispatcher.hpp>
#include <ours/object/handle.hpp>
#include <ours/object/types.hpp>

namespace ours::object {
/// Rights a freshly created IOMMU handle carries: it is handed to whoever may
/// make bus transaction initiators (BtiDispatcher), which is all it is for, so
/// it is not waited on.
constexpr auto RIGHTS_DEFAULT_IOMMU = Rights::Basic & ~Rights::Wait;

using IommuDispatcherBase = SoloDispatcher<IommuDispatcher, RIGHTS_DEFAULT_IOMMU>;

/// The IOMMU a BTI is made on: the object that says what stands between a
/// device and memory.
///
/// Only IommuType::Stub exists, the case of no IOMMU the kernel drives: a
/// device reaches memory by its physical address and nothing limits which, so
/// a BTI over it is bookkeeping (what has been pinned for DMA, and whether it
/// may be given back) and not isolation. The object is here so that a driver
/// for a real IOMMU adds a type and not a new way to ask for DMA: a program
/// reaches memory for a device through a BTI, and which kind of IOMMU is
/// behind it is this object's.
///
/// Holding one is the right to make BTIs, so it is minted from the System
/// resource slot IOMMU (iommu_create), and not by anything with just a handle
/// to a device.
class IommuDispatcher final: public IommuDispatcherBase {
    using Self = IommuDispatcher;
    using Base = IommuDispatcherBase;

  public:
    static constexpr auto TYPE_TAG = type_tag(ObjectType::Iommu);

    /// An IOMMU of |type|. NotSupported for a type this kernel has no
    /// implementation of, which is every value but IommuType::Stub.
    static auto create(IommuType type, KernelHandle<Self> &handle_out, Rights &rights_out)
        -> Status;

    /// Which kind of IOMMU this is; type() is the kind of object, already
    /// taken by Dispatcher.
    [[nodiscard]]
    auto iommu_type() const -> IommuType {
        return m_type;
    }

  private:
    explicit IommuDispatcher(IommuType type);
    ~IommuDispatcher() override = default;

    IommuType const m_type;
};
} // namespace ours::object

#endif // OURS_OBJECT_IOMMU_DISPATCHER_HPP
