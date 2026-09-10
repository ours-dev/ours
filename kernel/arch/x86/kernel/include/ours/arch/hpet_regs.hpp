/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_ARCH_HPET_REGS_HPP
#define OURS_ARCH_HPET_REGS_HPP 1

#include <ours/assert.hpp>
#include <ours/compiler.hpp>

#include <ustl/bitfields.hpp>

/// The HPET memory mapped register block.
///
/// Reference: IA-PC HPET Specification 1.0a, Section 2.3.
///
/// This used to live in //kernel/lib/acpi (acpi/structures/hpet.hpp), next to
/// the ACPI HPET table.  The two describe different things: the table is
/// firmware-provided data that tells you where the register block is, and the
/// register block is device MMIO.  Keeping them together also pulled
/// ustl/bitfields.hpp and ours/assert.hpp into what is otherwise a plain layout
/// header.  //kernel/arch/x86/kernel/time/hpet.cpp is the only consumer, and
/// its include of the old header carried the comment "This is a very
/// inconvenient way".
namespace ours::arch {
struct HpetTimerRegs {
    template <usize Id, usize Bits, ustl::util::FixedString Name>
    using Field = ustl::bitfields::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Bits<Bits>,
                                         ustl::bitfields::Name<Name>>;

    struct ConfigAndCapRegs {
        using Self = ConfigAndCapRegs;

        enum FieldId {
            RESERVED0,
            INTR_TYPE_CONFIG,
            INTR_ENABLE_CONFIG,
            TIMER_TYPE_CONFIG,
            PERIODIC_CAP,
            SIZE_CAP,
            VAL_SET_CONFIG,
            RESERVED1,
            k32ModeConfig,
            INTR_ROUTE_CONFIG,
            TN_FSB_EN_CONFIG,
            TN_FSB_INTR_DEL_CAP,
            RESERVED2,
            TN_INTR_ROUTE_CAP,
        };

        enum class TriggerMode {
            Edge,
            Level,
        };

        FORCE_INLINE
        auto mask(bool mask = true) volatile -> Self volatile & {
            inner.set<INTR_ENABLE_CONFIG>(!mask);
            return *this;
        }

        FORCE_INLINE
        auto irq() const volatile -> usize {
            return inner.get<INTR_ROUTE_CONFIG>();
        }

        FORCE_INLINE
        auto configure_irq(usize irqnum) volatile -> Self volatile & {
            inner.set<INTR_ROUTE_CONFIG>(irqnum);
            return *this;
        }

        FORCE_INLINE
        auto support_periodic() const volatile -> bool {
            return inner.get<PERIODIC_CAP>();
        }

        FORCE_INLINE
        auto set_mode_periodic() volatile -> Self volatile & {
            DEBUG_ASSERT(support_periodic());
            inner.set<TIMER_TYPE_CONFIG>(1);
            inner.set<VAL_SET_CONFIG>(1);
            return *this;
        }

        /// One-shot is mandatory for every HPET timer, so unlike
        /// set_mode_periodic() this asserts nothing.  It used to assert
        /// support_periodic(), copied from the function above.
        FORCE_INLINE
        auto set_mode_oneshot() volatile -> Self volatile & {
            inner.set<TIMER_TYPE_CONFIG>(0);
            inner.set<VAL_SET_CONFIG>(1);
            return *this;
        }

        FORCE_INLINE
        auto trigger_mode() volatile -> TriggerMode {
            return TriggerMode(inner.get<INTR_TYPE_CONFIG>());
        }

        volatile ustl::BitFields<
            Field<RESERVED0, 1, "Reserved">,
            // 0-edge-triggered interrupts.
            // 1-level-triggered interrupts.
            Field<INTR_TYPE_CONFIG, 1, "IntrTypeConfig">,
            // 0-this timer will still set Tn_INT_STS.
            // 1-enables triggering of interrupts.
            Field<INTR_ENABLE_CONFIG, 1, "IntrEnableConfig">,
            // 1-enables periodic type.
            // 0-enables oneshot type.
            Field<TIMER_TYPE_CONFIG, 1, "TimerTypeConfig">,
            // Read-only.
            // 1-this timer supports periodic mode.
            Field<PERIODIC_CAP, 1, "PeriodicCap">,
            // 1-supports 64-bit.
            // 0-supports 32-bit.
            Field<SIZE_CAP, 1, "SizeCap">,
            // Be used to allow software to directly set periodic timer's accumulator.
            Field<VAL_SET_CONFIG, 1, "ValSetConfig">, Field<RESERVED1, 1, "Reserved">,
            // Force to enable 32-bit timer.
            Field<k32ModeConfig, 1, "32ModeConfig">,
            // This field indicates I/O APIC routing. Allowed values can be determined
            // using Tn_INT_ROUTE_CAP. If an illegal value is written, then value read
            // back from this field will not match the written value.
            Field<INTR_ROUTE_CONFIG, 5, "IntrRouteConfig">,
            // 1-this timer will use FSB interrupt mapping.
            Field<TN_FSB_EN_CONFIG, 1, "TnFSBEnConfig">,
            // 1-this timer supports FSB interrupt mapping.
            Field<TN_FSB_INTR_DEL_CAP, 1, "TnFSBIntrDelCap">, Field<RESERVED2, 16, "Reserved">,
            // Timer n Interrupt Routing Capability. If bit X is set in this field,
            // it means that this timer can be mapped to IRQX line of I/O APIC.
            Field<TN_INTR_ROUTE_CAP, 32, "TnIntrRouteCap">>
            inner;
    };

    volatile ConfigAndCapRegs conf_caps;
    u64 comparator_value;
    volatile u64 fsb_int_route;
    u8 _reserved[8];
};
static_assert(sizeof(HpetTimerRegs) == 32);

struct HpetRegs {
    template <usize Id, usize Bits, ustl::util::FixedString Name>
    using Field = ustl::bitfields::Field<ustl::bitfields::Id<Id>, ustl::bitfields::Bits<Bits>,
                                         ustl::bitfields::Name<Name>>;

    struct HpetGeneralCapAndIdRegs {
        using Self = HpetGeneralCapAndIdRegs;

        auto revid() const volatile -> u16 {
            return static_cast<u16>(inner.get<0>());
        }

        auto num_timers() const volatile -> u16 {
            return static_cast<u16>(inner.get<1>() + 1);
        }

        auto support_64bit() const volatile -> bool {
            return inner.get<2>();
        }

        auto support_legacy() const volatile -> bool {
            return inner.get<4>();
        }

        auto vendor_id() const volatile -> u16 {
            return static_cast<u16>(inner.get<5>());
        }

        auto period() const volatile -> u32 {
            return static_cast<u32>(inner.get<6>());
        }

        auto set_period(u32 ns) volatile -> Self volatile & {
            inner.set<6>(ns);
            return *this;
        }

        volatile ustl::BitFields<
            Field<0, 8, "RevId">, Field<1, 5, "NumTimers-1">,
            Field<2, 1, "CounterSize">, // If one bit is set, the counter is 64 bits
            Field<3, 1, "Reserved">, Field<4, 1, "LegacyRoute">, Field<5, 16, "VendorId">,
            Field<6, 32, "Period">>
            inner;
    };
    static_assert(sizeof(HpetGeneralCapAndIdRegs) == 8);

    struct HpetGeneralConfigRegs {
        using Self = HpetGeneralConfigRegs;

        auto disable_cnf() volatile -> void {
            inner.set<0>(0);
        }

        auto enable_cnf() volatile -> void {
            inner.set<0>(1);
        }

        auto is_legacy_route() const volatile -> bool {
            return inner.get<1>();
        }

        auto enable_legacy_route(bool mask) volatile -> Self volatile & {
            inner.set<1>(mask);
            return *this;
        }

        volatile ustl::BitFields<Field<0, 1, "EnableCNF">, Field<1, 1, "LegacyRoute">,
                                 Field<2, 60, "Reserved">>
            inner;
    };
    static_assert(sizeof(HpetGeneralConfigRegs) == 8);

    volatile HpetGeneralCapAndIdRegs general_caps;
    u8 _reserved0[8];
    volatile HpetGeneralConfigRegs general_config;
    u8 _reserved1[8];
    volatile u64 general_intr_status;
    u8 _reserved2[0xf0 - 0x28];
    volatile u64 main_counter_value;
    u8 _reserved3[8];
    HpetTimerRegs timers[];
};
static_assert(offsetof(HpetRegs, timers) == 0x100);

} // namespace ours::arch

#endif // #ifndef OURS_ARCH_HPET_REGS_HPP
