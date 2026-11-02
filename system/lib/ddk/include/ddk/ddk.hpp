/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef DDK_DDK_HPP
#define DDK_DDK_HPP 1

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include <cstring>

#include <ort/startup.hpp>
#include <ours/interrupt.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>
#include <ours/vdso.hpp>
#include <ours_resource/client.hpp>
#include <ustl/result.hpp>

/// The runtime helpers every userspace driver restates by hand, as objects
/// whose destructors own the teardown orders.
///
/// The alignment rule, the one that keeps this library valid when the driver
/// manager's `Claim` lands: helpers consume handles their caller holds; only
/// `Resources` acquires, and only through a getter a host test can replace.
///
/// The objects:
///   - `Resources`, the resource ladder: `take(kind)` records what it holds,
///     `drop(handle)` releases one root early -- a root is a connection from
///     a finite pool, and both drivers today close them as soon as their line
///     or mapping is taken -- and the destructor closes everything still
///     recorded, newest first. The quadratic unwind ladder every driver
///     hand-rolls becomes stack unwinding.
///   - `startup_handle(type, name)`, the take plus the "started without X"
///     line every driver types twice.
///   - `MappedMmio`, a physical window mapped uncached and unmapped on
///     destruction.
///   - `Bti` and `DmaRegion`, the DMA chain and its teardown order -- unmap,
///     unpin, close -- that a mistake turns into a kernel quarantine.
///   - `irq::take`, the interrupt idiom: map on the root, create on the
///     root, close the root, because the interrupt object holds the line for
///     as long as it lives.
///
/// One thread, as the drivers run: nothing here locks. Every object here
/// must outlive the work that borrows its handles, the way the drivers'
/// `LazyInit` storages outlive their `main()`.
namespace ddk {

/// Which resource protocol `Resources::take` asks for.
enum class Kind : u8 {
    Mmio,
    Ioport,
    Iommu,
    Irq,
};

/// How `Resources` reaches the root's built-ins: the real client by default,
/// a fake in the host tests.
using Getter = auto (*)(ours_resource::Protocol const &)
    -> ustl::Result<ours::Uandle, ours::Status>;

/// The resource ladder. Copying is refused: the handles are this object's to
/// close.
class Resources final {
  public:
    /// A ladder over the real resource service, or |getter| in its place.
    explicit Resources(Getter const getter = &default_getter) : m_get(getter) {}

    Resources(Resources const &) = delete;
    auto operator=(Resources const &) -> Resources & = delete;

    ~Resources() {
        // Newest first, which is the order the hand-rolled ladders closed
        // in; a handle already dropped is no longer recorded, so nothing is
        // closed twice.
        for (auto index = m_held; index > 0; --index) {
            auto _ = ours::handle_close(m_handles[index - 1]);
        }
    }

    /// Acquires the root of |kind| and records it. The caller drops it with
    /// drop() once nothing more will be asked of that root.
    [[nodiscard]]
    auto take(Kind const kind) -> ustl::Result<ours::Uandle, ours::Status> {
        if (m_held == m_handles.size()) {
            constexpr auto LINE = std::string_view{"ddk: too many resources held\n"};
            auto _ = ours::debug_write(LINE.data(), static_cast<u32>(LINE.size()));
            return ustl::err(ours::Status::NoResource);
        }
        auto const acquired = m_get(protocol_of(kind));
        if (acquired.is_err()) {
            return acquired;
        }
        m_handles[m_held] = acquired.unwrap();
        m_held += 1;
        return acquired;
    }

    /// Closes |handle|, which must be one this object took and has not
    /// dropped: the driver is done with that root, and the connection goes
    /// back to the pool.
    auto drop(ours::Uandle const handle) -> void {
        for (auto index = m_held; index > 0; --index) {
            if (m_handles[index - 1] == handle) {
                // The ones after it shift down one, keeping the acquisition
                // order the destructor unwinds.
                for (auto rest = index - 1; rest + 1 < m_held; ++rest) {
                    m_handles[rest] = m_handles[rest + 1];
                }
                m_held -= 1;
                auto _ = ours::handle_close(handle);
                return;
            }
        }
    }

  private:
    static auto default_getter(ours_resource::Protocol const &protocol)
        -> ustl::Result<ours::Uandle, ours::Status> {
        return ours_resource::get(protocol);
    }

    static auto protocol_of(Kind const kind) -> ours_resource::Protocol const & {
        switch (kind) {
            case Kind::Mmio:
                return ours_resource::MMIO_PROTOCOL;
            case Kind::Ioport:
                return ours_resource::IOPORT_PROTOCOL;
            case Kind::Iommu:
                return ours_resource::IOMMU_PROTOCOL;
            case Kind::Irq:
                return ours_resource::IRQ_PROTOCOL;
        }
        return ours_resource::MMIO_PROTOCOL;
    }

    /// The roots this object holds, in acquisition order.
    static constexpr auto MAX_HELD = usize{5};
    std::array<ours::Uandle, MAX_HELD> m_handles = {};
    usize m_held = 0;
    Getter m_get;
};

/// The startup-handle guard pair, as one call: the handle, or the
/// "started without X" line and an error the caller returns.
[[nodiscard]]
inline auto startup_handle(ours::HandleType const type, std::string_view const name)
    -> ustl::Result<ours::Uandle, ours::Status> {
    auto const handle = ort::take_startup_handle(type, 0);
    if (handle == ours::Uandle::Invalid) {
        // The one way a user-space library says anything: the debug console,
        // the line composed in a fixed buffer, one write.
        constexpr auto PREFIX = std::string_view{"ddk: started without "};
        auto line = std::array<char, PREFIX.size() + 32>{};
        auto *cursor = line.data();
        std::memcpy(cursor, PREFIX.data(), PREFIX.size());
        cursor += PREFIX.size();
        auto const named = std::min(name.size(), line.size() - PREFIX.size() - 1);
        std::memcpy(cursor, name.data(), named);
        cursor += named;
        *cursor = '\n';
        auto _ = ours::debug_write(line.data(), static_cast<u32>(cursor - line.data() + 1));
        return ustl::err(ours::Status::NotFound);
    }
    return ustl::ok(handle);
}

/// A physical window mapped uncached, unmapped on destruction.
class MappedMmio final {
  public:
    MappedMmio() = default;

    MappedMmio(MappedMmio const &) = delete;
    auto operator=(MappedMmio const &) -> MappedMmio & = delete;

    /// Maps [paddr, paddr + size) through the MmIo root |resource|, into the
    /// address space |vma| names -- the startup VMA handle, which every
    /// driver takes first. The mapping is uncached whichever way the caller
    /// would have decided: a physical VMO's pages are device memory, and the
    /// kernel maps them so.
    [[nodiscard]]
    static auto map(ours::Uandle const vma, ours::Uandle const resource, ours::PhysAddr const paddr,
                    usize const size) -> ustl::Result<MappedMmio, ours::Status> {
        auto window = ours::Uandle::Invalid;
        if (auto const status = ours::vmo_create_physical(resource, paddr, size, &window);
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto mapped = MappedMmio{};
        mapped.m_size = size;
        auto const at = ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite,
                                      0, window, 0, size, &mapped.m_addr);
        // The mapping holds no reference to the VMO once made, so the window
        // handle goes either way.
        auto _ = ours::handle_close(window);
        if (at != ours::Status::Ok) {
            return ustl::err(at);
        }
        return ustl::ok(std::move(mapped));
    }

    MappedMmio(MappedMmio &&other) noexcept
        : m_vma(std::exchange(other.m_vma, ours::Uandle::Invalid))
        , m_addr(std::exchange(other.m_addr, ours::VirtAddr{0}))
        , m_size(std::exchange(other.m_size, usize{0})) {}
    auto operator=(MappedMmio &&) noexcept -> MappedMmio & = default;

    ~MappedMmio() {
        release();
    }

    [[nodiscard]]
    auto addr() const -> ours::VirtAddr {
        return m_addr;
    }

  private:
    auto release() -> void {
        if (m_vma != ours::Uandle::Invalid) {
            auto _ = ours::vma_unmap(m_vma, m_addr, m_size);
            m_vma = ours::Uandle::Invalid;
        }
    }

    ours::Uandle m_vma = ours::Uandle::Invalid;
    ours::VirtAddr m_addr = ours::VirtAddr{0};
    usize m_size = 0;
};

/// The device's BTI, made from the Iommu root: a stub IOMMU, one BTI for the
/// device. Held for the driver's life; released on destruction.
class Bti final {
  public:
    Bti() = default;

    Bti(Bti const &) = delete;
    auto operator=(Bti const &) -> Bti & = delete;

    [[nodiscard]]
    static auto create(ours::Uandle const iommu_resource) -> ustl::Result<Bti, ours::Status> {
        auto iommu = ours::Uandle::Invalid;
        if (auto const status =
                ours::iommu_create(iommu_resource, ours::IommuType::Stub, nullptr, 0, &iommu);
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto bti = Bti{};
        bti.m_iommu = iommu;
        if (auto const status = ours::bti_create(iommu, 0, 0, &bti.m_bti);
            status != ours::Status::Ok) {
            auto _ = ours::handle_close(iommu);
            return ustl::err(status);
        }
        return ustl::ok(std::move(bti));
    }

    Bti(Bti &&other) noexcept
        : m_iommu(std::exchange(other.m_iommu, ours::Uandle::Invalid))
        , m_bti(std::exchange(other.m_bti, ours::Uandle::Invalid)) {}
    auto operator=(Bti &&) noexcept -> Bti & = default;

    ~Bti() {
        release();
    }

    [[nodiscard]]
    auto handle() const -> ours::Uandle {
        return m_bti;
    }

  private:
    auto release() -> void {
        if (m_bti != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(m_bti);
            m_bti = ours::Uandle::Invalid;
        }
        if (m_iommu != ours::Uandle::Invalid) {
            auto _ = ours::handle_close(m_iommu);
            m_iommu = ours::Uandle::Invalid;
        }
    }

    ours::Uandle m_iommu = ours::Uandle::Invalid;
    ours::Uandle m_bti = ours::Uandle::Invalid;
};

/// A contiguous DMA region: allocated, pinned, and mapped in one call, torn
/// down in the order that answers the kernel's pin discipline -- unmap,
/// unpin, close -- on destruction.
class DmaRegion final {
  public:
    DmaRegion() = default;

    DmaRegion(DmaRegion const &) = delete;
    auto operator=(DmaRegion const &) -> DmaRegion & = delete;

    [[nodiscard]]
    static auto allocate(ours::Uandle const vma, ours::Uandle const bti, usize const bytes)
        -> ustl::Result<DmaRegion, ours::Status> {
        auto vmo = ours::Uandle::Invalid;
        if (auto const status = ours::vmo_create_contiguous(bti, bytes, 0, &vmo);
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto region = DmaRegion{};
        region.m_bytes = bytes;
        region.m_vma = vma;
        auto const pinned =
            ours::bti_pin(bti,
                          ours::BtiPinOptions::PermRead | ours::BtiPinOptions::PermWrite |
                              ours::BtiPinOptions::Contiguous,
                          vmo, 0, bytes, &region.m_phys, 1, &region.m_pmt);
        if (pinned != ours::Status::Ok) {
            auto _ = ours::handle_close(vmo);
            return ustl::err(pinned);
        }
        auto const mapped =
            ours::vma_map(vma, ours::VmOptions::PermRead | ours::VmOptions::PermWrite, 0, vmo, 0,
                          bytes, &region.m_virt);
        // The mapping holds the pages; the VMO handle's job is done either
        // way.
        auto _ = ours::handle_close(vmo);
        if (mapped != ours::Status::Ok) {
            auto _ = ours::pmt_unpin(region.m_pmt);
            region.m_pmt = ours::Uandle::Invalid;
            return ustl::err(mapped);
        }
        return ustl::ok(std::move(region));
    }

    DmaRegion(DmaRegion &&other) noexcept
        : m_virt(std::exchange(other.m_virt, ours::VirtAddr{0}))
        , m_phys(std::exchange(other.m_phys, ours::PhysAddr{0}))
        , m_bytes(std::exchange(other.m_bytes, usize{0}))
        , m_vma(std::exchange(other.m_vma, ours::Uandle::Invalid))
        , m_pmt(std::exchange(other.m_pmt, ours::Uandle::Invalid)) {}
    auto operator=(DmaRegion &&) noexcept -> DmaRegion & = default;

    ~DmaRegion() {
        release();
    }

    [[nodiscard]]
    auto virt() const -> ours::VirtAddr {
        return m_virt;
    }

    [[nodiscard]]
    auto phys() const -> ours::PhysAddr {
        return m_phys;
    }

    [[nodiscard]]
    auto bytes() const -> usize {
        return m_bytes;
    }

  private:
    auto release() -> void {
        if (m_vma != ours::Uandle::Invalid) {
            auto _ = ours::vma_unmap(m_vma, m_virt, m_bytes);
            m_vma = ours::Uandle::Invalid;
        }
        if (m_pmt != ours::Uandle::Invalid) {
            auto _ = ours::pmt_unpin(m_pmt);
            m_pmt = ours::Uandle::Invalid;
        }
    }

    ours::VirtAddr m_virt = ours::VirtAddr{0};
    ours::PhysAddr m_phys = ours::PhysAddr{0};
    usize m_bytes = 0;
    ours::Uandle m_vma = ours::Uandle::Invalid;
    ours::Uandle m_pmt = ours::Uandle::Invalid;
};

/// The interrupt idiom: map on the root, create on the root, close the root,
/// because the interrupt object holds the line for as long as it lives.
namespace irq {
    /// Takes the line |source| of |domain| names, through |irq_root|, and hands
    /// back the interrupt object. On success the root is closed -- the caller's
    /// ladder holds it otherwise, and its destructor owns it from there.
    [[nodiscard]]
    inline auto take(ours::Uandle const irq_root, u32 const domain, u32 const source,
                     ours::InterruptOptions const options)
        -> ustl::Result<ours::Uandle, ours::Status> {
        auto number = u32{0};
        if (auto const status = ours::interrupt_map(irq_root, domain, source, &number);
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto interrupt = ours::Uandle::Invalid;
        if (auto const status = ours::interrupt_create(irq_root, number, options, &interrupt);
            status != ours::Status::Ok) {
            return ustl::err(status);
        }
        auto _ = ours::handle_close(irq_root);
        return ustl::ok(interrupt);
    }
} // namespace irq
} // namespace ddk

#endif // #ifndef DDK_DDK_HPP
