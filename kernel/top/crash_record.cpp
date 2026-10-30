/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// The kernel's half of the crash record: where the region is, what a panic
// writes into it, and what the next boot does with what it finds. The format
// is //kernel/lib/crashlog's; .agents/tasks/panic-crashlog/DESIGN.md has the
// design.
#include "crash_record.hpp"

#include <ours/boot/handoff.hpp>
#include <ours/mm/physmap.hpp>

#include <span>

#include <cstddef>

#include <arch/cache.hpp>
#include <arch/tick.hpp>
#include <crashlog/crashlog.hpp>
#include <gktl/init_hook.hpp>
#include <logz4/log.hpp>
#include <logz4/ring.hpp>
#ifndef KERNEL_NO_USERABI
#include <userabi/userboot.hpp>
#endif // #ifndef KERNEL_NO_USERABI

namespace ours {
namespace {
    /// The region, through the physmap, or empty when the image declared
    /// none. Written once, by find_region() on the boot CPU before any other
    /// CPU runs; read by the panic path and by recover_record(). Plain is
    /// enough: every reader is ordered after the write by the bring-up of the
    /// CPU or thread it runs on.
    std::span<std::byte> CRASH_REGION;

    /// A log sink that appends to the record rather than printing.
    class RecordSink final: public logz4::Logger {
      public:
        explicit RecordSink(crashlog::Writer &writer) : m_writer(writer) {}

        auto write(char const *s, usize n) -> void override {
            m_writer.append(std::string_view(s, n));
        }

      private:
        crashlog::Writer &m_writer;
    };

    /// Finds the range phys reserved as MemoryType::Crashlog in the memory
    /// map the handoff carries. The range is RAM inside the physmap, which
    /// phys maps whole, so nothing has to be mapped for it -- least of all at
    /// panic time.
    ///
    /// As early as the hooks run, so that a panic during the rest of the boot
    /// still leaves a record. A panic before this point leaves none.
    auto find_region() -> void {
        for (auto const &entry: boot::handoff().mem.ranges) {
            if (entry.type != bootmem::MemoryType::Crashlog) {
                continue;
            }
            auto *const bytes = mm::PhysMap::phys_to_virt<std::byte>(entry.range.base);
            if (bytes == nullptr ||
                !mm::PhysMap::is_valid_phys_addr(entry.range.base + entry.range.size - 1)) {
                log::warn("crashlog: [{:#x}, +{:#x}) is outside the physmap", entry.range.base,
                          entry.range.size);
                return;
            }
            CRASH_REGION = std::span(bytes, entry.range.size);
            return;
        }
    }

    /// Reports the record the previous boot left, if it left one, and clears
    /// it so that the next boot does not report it again.
    ///
    /// Each line of the record is logged at Warn under "crashlog: | ". Warn,
    /// not Error: the panic happened on the previous boot, and this boot is
    /// fine.
    auto recover_record() -> void {
        if (CRASH_REGION.empty()) {
            return;
        }
        auto const record = crashlog::read(CRASH_REGION);
        if (record.is_none()) {
            log::info("crashlog: no record from the previous boot");
            return;
        }

        auto text = record.value().text;
        log::warn("crashlog: the previous boot panicked; its record ({} bytes) follows",
                  text.size());
        while (!text.empty()) {
            auto const end = text.find('\n');
            auto const line = text.substr(0, end);
            log::warn("crashlog: | {}", line);
            if (end == std::string_view::npos) {
                break;
            }
            text.remove_prefix(end + 1);
        }
        log::warn("crashlog: end of the previous boot's record");
        // The copy is for userboot. Without user space (kernel_no_userabi)
        // the report above is the record's only reader.
#ifndef KERNEL_NO_USERABI
        userabi::stash_crashlog(record.value().text);
#endif // #ifndef KERNEL_NO_USERABI

        crashlog::clear(CRASH_REGION);
        arch::clean_cache_range(CRASH_REGION.data(), sizeof(crashlog::Header));
    }
} // namespace

auto record_panic(std::string_view message, usize tail_lines) -> void {
    if (CRASH_REGION.empty()) {
        return;
    }
    auto writer = crashlog::Writer(CRASH_REGION);
    writer.append(message);
    writer.append("\n");
    auto sink = RecordSink(writer);
    logz4::ring::dump_tail_to(sink, tail_lines);
    writer.seal(arch::Tick::get().count);
    arch::clean_cache_range(CRASH_REGION.data(), CRASH_REGION.size());
}
} // namespace ours

GKTL_INIT_HOOK(CrashRecordRegion, ours::find_region, gktl::InitLevel::PlatformEarly);

// Late, on the laborer thread: the report is for whoever reads the boot log,
// and the copy for userboot needs the VMM, which is up by then. Before
// userboot_init(), which hands that copy on, on the same thread.
GKTL_INIT_HOOK(CrashRecordRecover, ours::recover_record, gktl::InitLevel::Platform);
