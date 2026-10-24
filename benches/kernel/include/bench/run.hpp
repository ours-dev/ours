/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// How a benchmark that runs inside the kernel says which of its metrics mean
// something on the machine it runs on.
//
// A metric needs one of two things. A count or a size -- faults taken, tables
// built, pages committed -- comes out the same on any machine, emulated or
// not. A timing that rests on the processor's caches or its TLB comes out
// right only where the processor itself runs the code: on bare metal or under
// a hypervisor. An emulator models neither, so the number it gives for such a
// metric is not a smaller copy of the real effect but a different quantity,
// and printed beside the others it would be read as one.
//
// So a benchmark makes one bench::Run, which says once what machine it is on,
// and asks it before each metric. A metric that needs hardware is skipped on
// an emulator, with a line saying why, unless the command was given `all`:
// then it runs, and its line says the number is not a hardware one. That is
// how its code gets exercised on a machine that cannot give its number.
#ifndef BENCH_RUN_HPP
#define BENCH_RUN_HPP 1

#include <string_view>

#include <arch/machine.hpp>
#include <logz4/log.hpp>

namespace bench {

/// What a metric needs from the machine for its number to mean anything.
enum class Needs {
    /// Counts and sizes, and timings of work an emulator does too.
    AnyMachine,
    /// The processor's own caches or TLB: bare metal or a hypervisor.
    Hardware,
};

class Run final {
  public:
    /// |name| starts every line the run prints. |argc| and |argv| are the
    /// command's: no argument, or `all` to run the hardware metrics on an
    /// emulator too.
    Run(char const *const name, int const argc, char **const argv)
        : m_name(name)
        , m_machine(arch::machine_kind())
        , m_all(argc == 2 && std::string_view(argv[1]) == "all")
        , m_arguments_ok(argc == 1 || m_all) {
        log::info("{}: machine: {}", m_name, arch::to_string(m_machine));
    }

    /// Whether the command's arguments were ones this convention knows.
    [[nodiscard]]
    auto arguments_ok() const -> bool {
        return m_arguments_ok;
    }

    /// Whether |metric| is measured on this run. When it is not, says why;
    /// when it is measured anyway, says its number is not a hardware one.
    [[nodiscard]]
    auto measures(char const *const metric, Needs const needs) const -> bool {
        if (needs == Needs::AnyMachine || m_machine != arch::MachineKind::Emulated) {
            return true;
        }
        if (m_all) {
            log::info("{}: {}: measured on an emulator because of `all`; not a hardware number",
                      m_name, metric);
            return true;
        }
        log::info("{}: skip  {}: needs the processor's caches or TLB, and this machine is "
                  "emulated (`all` runs it anyway)",
                  m_name, metric);
        return false;
    }

    [[nodiscard]]
    auto name() const -> char const * {
        return m_name;
    }

    /// The line that ends a run that got to the end, and the command's exit
    /// code for it.
    [[nodiscard]]
    auto finish() const -> int {
        log::info("{}: ok", m_name);
        return 0;
    }

  private:
    char const *m_name;
    arch::MachineKind m_machine;
    bool m_all;
    bool m_arguments_ok;
};

} // namespace bench

#endif // BENCH_RUN_HPP
