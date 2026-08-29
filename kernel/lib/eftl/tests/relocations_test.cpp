#include <eftl/elf/link.hpp>
#include <eftl/elf/relocation_catalog.hpp>
#include <eftl/elf/relocations.hpp>

#include <array>
#include <limits>

#include <cstring>

#include <ustl/io/direct_memory.hpp>

namespace {
struct NarrowRecordingMemory final {
    using Addr = u32;

    template <typename T>
    auto read_at_n(Addr, usize) noexcept -> ustl::views::Span<T> {
        ++m_read_calls;
        return {};
    }

    [[nodiscard]]
    auto read_call_count() const noexcept -> usize {
        return m_read_calls;
    }

  private:
    usize m_read_calls{0};
};

auto test_rejects_unrepresentable_dynamic_table_address() -> int {
    using Traits = eftl::elf::Traits64;

    std::array<Traits::Dyn, 3> dynamics{};
    dynamics[0].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rel);
    dynamics[0].d_val = u64(1) << 32;
    dynamics[1].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelSz);
    dynamics[1].d_val = sizeof(Traits::Rel);
    dynamics[2].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);

    eftl::elf::Relocations<Traits> relocations{};
    eftl::elf::RelocationObserver<Traits> observer(relocations);
    NarrowRecordingMemory memory{};
    if (eftl::elf::decode_dynamic(
            memory, ustl::views::Span<Traits::Dyn const>(dynamics.data(), dynamics.size()),
            observer)) {
        return 1;
    }
    if (memory.read_call_count() == 0) {
        return 0;
    }
    return 2;
}

auto test_relocation_info_decoding() -> int {
    constexpr u32 ELF32_INFO = (u32(7) << 8) | u32(8);
    if (eftl::elf::Traits32::relocation_type(ELF32_INFO) != 8 ||
        eftl::elf::Traits32::relocation_symbol(ELF32_INFO) != 7) {
        return 1;
    }

    constexpr u64 ELF64_INFO = (u64(7) << 32) | u64(8);
    if (eftl::elf::Traits64::relocation_type(ELF64_INFO) != 8 ||
        eftl::elf::Traits64::relocation_symbol(ELF64_INFO) != 7) {
        return 2;
    }

    return 0;
}

auto test_architecture_relocation_catalog() -> int {
    using I386Catalog = eftl::elf::RelocationCatalog<eftl::elf::Arch::I386>;
    using X8664Catalog = eftl::elf::RelocationCatalog<eftl::elf::Arch::X86_64>;
    auto const i386_relative = I386Catalog::lookup(8);
    auto const i386_got32x = I386Catalog::lookup(43);
    auto const x8664_relative = X8664Catalog::lookup(8);
    auto const x8664_rex = X8664Catalog::lookup(42);
    auto const unknown = X8664Catalog::lookup(0xffffU);
    if (I386Catalog::entries().size() == 42 && X8664Catalog::entries().size() == 47 &&
        i386_relative.is_some() && i386_relative->machine == eftl::elf::Arch::I386 &&
        i386_relative->type == 8 && std::strcmp(i386_relative->name, "X86_RELATIVE") == 0 &&
        i386_got32x.is_some() && i386_got32x->type == 43 && x8664_relative.is_some() &&
        x8664_relative->machine == eftl::elf::Arch::X86_64 && x8664_rex.is_some() &&
        std::strcmp(x8664_rex->name, "X86_64_REX_GOTPCRELX") == 0 && unknown.is_none()) {
        return 0;
    }
    return 1;
}

auto test_rejects_symbolic_relative_relocation() -> int {
    using Traits = eftl::elf::Traits64;
    using Addr = Traits::Addr;

    Traits::Rela rela{};
    rela.r_offset = 0;
    rela.r_info = (u64(7) << 32) | u64(8);
    rela.r_addend = 0;

    eftl::elf::Relocations<Traits> relocations{};
    relocations.set_rela(ustl::views::make_span(&rela, 1), 1);

    bool visited = false;
    if (relocations.visit_relative([](u32 type) { return type == 8; },
                                   [&visited](auto) {
                                       visited = true;
                                       return true;
                                   })) {
        return 1;
    }
    if (visited) {
        return 2;
    }

    alignas(Addr) std::array<u8, sizeof(Addr)> image{};
    ustl::io::DirectMemory memory(image.data(), image.size());
    if (eftl::elf::relocate_relative(memory, relocations, Addr(0x100),
                                     [](u32 type) { return type == 8; })) {
        return 3;
    }

    return 0;
}

auto test_rejects_elf32_symbolic_relative_relocation() -> int {
    using Traits = eftl::elf::Traits32;
    using Addr = Traits::Addr;

    Traits::Rel rel{};
    rel.r_offset = 0;
    rel.r_info = (u32(7) << 8) | u32(8);

    eftl::elf::Relocations<Traits> relocations{};
    relocations.set_rel(ustl::views::make_span(&rel, 1), 1);

    bool visited = false;
    if (relocations.visit_relative([](u32 type) { return type == 8; },
                                   [&visited](auto) {
                                       visited = true;
                                       return true;
                                   })) {
        return 1;
    }
    if (visited) {
        return 2;
    }

    alignas(Addr) std::array<u8, sizeof(Addr)> image{};
    ustl::io::DirectMemory memory(image.data(), image.size());
    if (eftl::elf::relocate_relative(memory, relocations, Addr(0x100),
                                     [](u32 type) { return type == 8; })) {
        return 3;
    }

    return 0;
}

auto test_rejects_relr_address_overflow() -> int {
    using Traits = eftl::elf::Traits64;
    using Addr = Traits::Addr;

    constexpr Addr WRAP_ANCHOR =
        std::numeric_limits<Addr>::max() - static_cast<Addr>(sizeof(Addr) - 1);
    std::array<Addr, 2> const entries{
        WRAP_ANCHOR,
        3,
    };

    eftl::elf::Relocations<Traits> relocations{};
    relocations.set_relr(ustl::views::make_span(entries.data(), entries.size()));

    std::array<Addr, 2> visited{};
    usize count = 0;
    auto const complete = relocations.visit_relative([&](auto relocation) {
        if (count == visited.size()) {
            return false;
        }
        visited[count++] = relocation.offset;
        return true;
    });
    if (count != 0) {
        return 1;
    }
    if (complete) {
        return 2;
    }
    return 0;
}
} // namespace

int main() {
    using Traits = eftl::elf::Traits64;
    using Addr = Traits::Addr;

    std::array<Addr, 3> const entries{
        0x1000,
        (Addr(0b101) << 1) | 1,
        0x2000,
    };

    eftl::elf::Relocations<Traits> relocations{};
    relocations.set_relr(ustl::views::make_span(entries.data(), entries.size()));

    std::array<Addr, 4> seen{};
    usize count = 0;
    if (!relocations.visit_relative([&](auto relocation) {
            if (!relocation.addend_in_place || count == seen.size()) {
                return false;
            }
            seen[count++] = relocation.offset;
            return true;
        })) {
        return 1;
    }

    if (count != 4 || seen[0] != 0x1000 || seen[1] != 0x1008 || seen[2] != 0x1018 ||
        seen[3] != 0x2000) {
        return 2;
    }

    Traits::Rela rela{};
    rela.r_offset = 0;
    rela.r_info = 8;
    rela.r_addend = -0x10;
    eftl::elf::Relocations<Traits> rela_relocations{};
    rela_relocations.set_rela(ustl::views::make_span(&rela, 1), 1);

    alignas(Addr) std::array<u8, 0x100> image{};
    ustl::io::DirectMemory memory(image.data(), image.size());
    if (!eftl::elf::relocate_relative(memory, rela_relocations, Addr(0x100),
                                      [](u32 type) { return type == 8; })) {
        return 3;
    }
    auto const *const relocated_rela = memory.read_at<Addr>(0);
    if (relocated_rela == nullptr || *relocated_rela != 0xf0) {
        return 4;
    }

    if (eftl::elf::relocate_relative(memory, rela_relocations, Addr(0x100),
                                     [](u32) { return false; })) {
        return 5;
    }

    Traits::Rel rel{};
    rel.r_offset = 0;
    rel.r_info = 8;
    auto *const rel_target = memory.read_at<Addr>(0);
    if (rel_target == nullptr) {
        return 6;
    }
    *rel_target = 0x20;
    eftl::elf::Relocations<Traits> rel_relocations{};
    rel_relocations.set_rel(ustl::views::make_span(&rel, 1), 1);
    if (!eftl::elf::relocate_relative(memory, rel_relocations, Addr(0x100),
                                      [](u32 type) { return type == 8; })) {
        return 7;
    }
    auto const *const relocated_rel = memory.read_at<Addr>(0);
    if (relocated_rel == nullptr || *relocated_rel != 0x120) {
        return 8;
    }

    Traits::Rel dynamic_rel{};
    dynamic_rel.r_offset = 0;
    dynamic_rel.r_info = 8;
    if (!memory.write(0x40, dynamic_rel)) {
        return 9;
    }
    std::array<Traits::Dyn, 4> dynamics{};
    dynamics[0].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::Rel);
    dynamics[0].d_val = 0x40;
    dynamics[1].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelSz);
    dynamics[1].d_val = sizeof(Traits::Rel);
    dynamics[2].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::RelEnt);
    dynamics[2].d_val = sizeof(Traits::Rel);
    dynamics[3].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);
    eftl::elf::Relocations<Traits> decoded_relocations{};
    eftl::elf::RelocationObserver<Traits> relocation_observer(decoded_relocations);
    if (!eftl::elf::decode_dynamic(
            memory, ustl::views::Span<Traits::Dyn const>(dynamics.data(), dynamics.size()),
            relocation_observer)) {
        return 10;
    }
    auto *const dynamic_target = memory.read_at<Addr>(0);
    if (dynamic_target == nullptr) {
        return 11;
    }
    *dynamic_target = 0x20;
    if (!eftl::elf::relocate_relative(memory, decoded_relocations, Addr(0x100),
                                      [](u32 type) { return type == 8; })) {
        return 12;
    }
    auto const *const relocated_dynamic = memory.read_at<Addr>(0);
    if (relocated_dynamic == nullptr || *relocated_dynamic != 0x120) {
        return 13;
    }

    std::array<Traits::Dyn, 4> unsupported_dynamics{};
    unsupported_dynamics[0].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::JmpRel);
    unsupported_dynamics[1].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::PltRelSz);
    unsupported_dynamics[2].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::PltRel);
    unsupported_dynamics[2].d_val = static_cast<Traits::Addr>(eftl::elf::DynTag::Rela);
    unsupported_dynamics[3].d_tag = static_cast<Traits::Sword>(eftl::elf::DynTag::DtNull);
    eftl::elf::Relocations<Traits> unsupported_relocations{};
    eftl::elf::RelocationObserver<Traits> unsupported_observer(unsupported_relocations);
    if (eftl::elf::decode_dynamic(memory,
                                  ustl::views::Span<Traits::Dyn const>(unsupported_dynamics.data(),
                                                                       unsupported_dynamics.size()),
                                  unsupported_observer)) {
        return 14;
    }

    if (auto const result = test_rejects_unrepresentable_dynamic_table_address(); result != 0) {
        return 20 + result;
    }

    if (auto const result = test_architecture_relocation_catalog(); result != 0) {
        return 30 + result;
    }

    if (auto const result = test_relocation_info_decoding(); result != 0) {
        return 40 + result;
    }

    if (auto const result = test_rejects_symbolic_relative_relocation(); result != 0) {
        return 50 + result;
    }

    if (auto const result = test_rejects_elf32_symbolic_relative_relocation(); result != 0) {
        return 60 + result;
    }

    if (auto const result = test_rejects_relr_address_overflow(); result != 0) {
        return 70 + result;
    }
    return 0;
}
