#include <ours/arch/apic.hpp>

#include <ustl/views/span.hpp>

namespace ours {
auto apic_send_ipi_self(arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    arch::IpiRequest request{};
    request.set_vector(u32(vector));
    request.set_dest_mode(arch::ApicDestinationMode::Physical);
    request.set_level(arch::ApicIpiLevel::Assert);
    request.set_delivery_mode(mode);
    XAPIC->send_ipi_self(request);
}

namespace {
    /// Everything about an IPI except who it is for.
    ///
    /// Split out because a multicast differs from a unicast in exactly one
    /// field: physical destination mode names one APIC per send, so the rest
    /// of the request is the same word for every target.
    auto make_ipi_request(arch::IrqVec vector, ApicDeliveryMode mode) -> arch::IpiRequest {
        arch::IpiRequest request{};
        request.set_vector(u32(vector));
        request.set_target(arch::ApicIpiTarget::TheOne);
        request.set_dest_mode(arch::ApicDestinationMode::Physical);
        request.set_level(arch::ApicIpiLevel::Assert);
        request.set_delivery_mode(mode);
        return request;
    }
} // namespace

auto apic_send_ipi_unicast(CpuNum target, arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    auto request = make_ipi_request(vector, mode);
    request.set_dest(cpunum_to_apic_id(target));
    XAPIC->send_ipi(request);
}

auto apic_send_ipi_multicast(CpuMask const &targets, arch::IrqVec vector, ApicDeliveryMode mode)
    -> void {
    // The CPU-to-APIC translation happens here and not inside the send loop,
    // because it is this layer that knows the mapping -- and because doing it
    // up front keeps the interrupt-disabled window down there to nothing but
    // register writes.
    u32 dests[MAX_CPU];
    auto count = usize{0};
    targets.for_each([&dests, &count](CpuNum cpunum) { dests[count++] = cpunum_to_apic_id(cpunum); });

    if (count == 0) {
        return;
    }

    XAPIC->send_ipi_multicast(make_ipi_request(vector, mode),
                              ustl::views::Span<u32 const>(dests, count));
}

auto apic_send_ipi_broadcast(arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    arch::IpiRequest request{};
    request.set_vector(u32(vector));
    request.set_dest_mode(arch::ApicDestinationMode::Physical);
    request.set_level(arch::ApicIpiLevel::Assert);
    request.set_delivery_mode(mode);
    XAPIC->broadcast_ipi(request);
}

} // namespace ours
