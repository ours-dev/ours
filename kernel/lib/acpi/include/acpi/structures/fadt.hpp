/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef ACPI_STRUCTURES_FADT_HPP
#define ACPI_STRUCTURES_FADT_HPP 1

#include <acpi/structures/header.hpp>

#include <cstddef>

namespace acpi {
/// Fixed ACPI description table, "FACP", as far as the DSDT's pointers.
///
/// Reference: ACPI v6.5 Section 5.2.9.
///
/// The root table does not list the DSDT: this table points to it, by `dsdt`
/// and, from ACPI 2.0 on, by the 64 bit `x_dsdt`. A firmware's table may be
/// shorter than this structure (a 1.0 table ends before `x_dsdt`), so a reader
/// checks `header.length` against the field it is about to use. The fields
/// after `x_dsdt` (the extended register blocks, the sleep control registers,
/// the hypervisor id) are not declared: nothing reads them.
struct PACKED Fadt {
    TableHeader header;
    u32 firmware_ctrl;
    u32 dsdt;
    u8 reserved0;
    u8 preferred_pm_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8 acpi_enable;
    u8 acpi_disable;
    u8 s4bios_req;
    u8 pstate_cnt;
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
    u32 pm2_cnt_blk;
    u32 pm_tmr_blk;
    u32 gpe0_blk;
    u32 gpe1_blk;
    u8 pm1_evt_len;
    u8 pm1_cnt_len;
    u8 pm2_cnt_len;
    u8 pm_tmr_len;
    u8 gpe0_blk_len;
    u8 gpe1_blk_len;
    u8 gpe1_base;
    u8 cst_cnt;
    u16 p_lvl2_lat;
    u16 p_lvl3_lat;
    u16 flush_size;
    u16 flush_stride;
    u8 duty_offset;
    u8 duty_width;
    u8 day_alrm;
    u8 mon_alrm;
    u8 century;
    u16 iapc_boot_arch;
    u8 reserved1;
    u32 flags;
    GenericAddress reset_reg;
    u8 reset_value;
    u16 arm_boot_arch;
    u8 fadt_minor_version;
    u64 x_firmware_ctrl;
    u64 x_dsdt;

    constexpr static Signature SIGNATURE = Signature::from_chars("FACP");
};
static_assert(offsetof(Fadt, dsdt) == 40);
static_assert(offsetof(Fadt, flags) == 112);
static_assert(offsetof(Fadt, reset_reg) == 116);
static_assert(offsetof(Fadt, x_dsdt) == 140);
static_assert(sizeof(Fadt) == 148);

} // namespace acpi

#endif // #ifndef ACPI_STRUCTURES_FADT_HPP
