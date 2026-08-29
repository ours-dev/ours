#include <ours/arch/apic.hpp>

namespace ours {
auto apic_send_ipi_self(arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    arch::IpiRequest request{};
    request.set_vector(u32(vector));
    request.set_dest_mode(arch::ApicDestinationMode::Physical);
    request.set_level(arch::ApicIpiLevel::Assert);
    request.set_delivery_mode(mode);
    g_xapic->send_ipi_self(request);
}

auto apic_send_ipi_unicast(CpuNum target, arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    arch::IpiRequest request{};
    request.set_vector(u32(vector));
    request.set_target(arch::ApicIpiTarget::TheOne);
    request.set_dest(cpunum_to_apic_id(target));
    request.set_dest_mode(arch::ApicDestinationMode::Physical);
    request.set_level(arch::ApicIpiLevel::Assert);
    request.set_delivery_mode(mode);
    g_xapic->send_ipi(target, request);
}

auto apic_send_ipi_mulitcast(CpuMask const &targets, arch::IrqVec vector, ApicDeliveryMode mode)
    -> void {
    targets.for_each(
        [vector, mode](CpuNum cpunum) { apic_send_ipi_unicast(cpunum, vector, mode); });
}

auto apic_send_ipi_broadcast(arch::IrqVec vector, ApicDeliveryMode mode) -> void {
    arch::IpiRequest request{};
    request.set_vector(u32(vector));
    request.set_dest_mode(arch::ApicDestinationMode::Physical);
    request.set_level(arch::ApicIpiLevel::Assert);
    request.set_delivery_mode(mode);
    g_xapic->broadcast_ipi(request);
}

} // namespace ours
