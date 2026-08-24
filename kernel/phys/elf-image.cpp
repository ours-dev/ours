#include <ours/phys/kernel-image.hpp>

#include <ours/phys/print.hpp>

#include <limits>
#include <type_traits>

#include <cstring>

#include <eftl/elf/mapped_memory.hpp>

namespace ours::phys {
namespace {
    template <typename Value>
    [[nodiscard]]
    constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    template <typename Traits>
    [[nodiscard]]
    auto dynamic_entries(eftl::elf::FileImpl<Traits> const &binary)
        -> ustl::Option<eftl::elf::TableView<typename Traits::Dyn>> {
        auto const image = binary.dynamic_image();
        return image.is_some() ? ustl::some(image->entries()) : ustl::NONE;
    }

    template <typename Traits, typename IsRelative>
    [[nodiscard]]
    auto apply_kernel_relative_relocations(eftl::elf::FileImpl<Traits> const &binary,
                                           eftl::elf::MappedMemory<Traits> &memory,
                                           typename Traits::Addr bias, IsRelative &&is_relative)
        -> bool {
        auto const dynamics = dynamic_entries(binary);
        if (dynamics.is_none()) {
            return false;
        }
        if (dynamics->empty()) {
            return true;
        }

        eftl::elf::Relocations<Traits> relocations;
        if (!eftl::elf::decode_dynamic(memory, dynamics.value(),
                                       eftl::elf::RelocationObserver<Traits>(relocations))) {
            return false;
        }
        return eftl::elf::relocate_relative(memory, relocations, bias,
                                            std::forward<IsRelative>(is_relative));
    }

    /// Relocation types that take a symbol value plus an addend and store
    /// the result as a full-width address.  These are all a phys module can
    /// produce: it is compiled -fno-plt, so an imported function reaches the
    /// GOT the same way an imported variable does (kernel/phys/BUILD.gn,
    /// config("phys-module.compile")), and nothing in a boot image could
    /// service a lazily-bound PLT stub anyway.
    [[nodiscard]]
    auto symbolic_relocation_type(eftl::elf::Arch machine, u32 type) -> bool {
        switch (machine) {
            case eftl::elf::Arch::X86_64:
                // R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_JUMP_SLOT
                return type == 1 || type == 6 || type == 7;
            case eftl::elf::Arch::AARCH64:
                // R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT
                return type == 257 || type == 1025 || type == 1026;
            case eftl::elf::Arch::RISCV:
                // R_RISCV_64, R_RISCV_JUMP_SLOT
                return type == 2 || type == 5;
            default:
                return false;
        }
    }

    [[nodiscard]]
    auto relative_relocation_type(eftl::elf::Arch machine, u32 type) -> bool {
        switch (machine) {
            case eftl::elf::Arch::X86_64:
                return type == 8; // R_X86_64_RELATIVE
            case eftl::elf::Arch::AARCH64:
                return type == 1027; // R_AARCH64_RELATIVE
            case eftl::elf::Arch::RISCV:
                return type == 3; // R_RISCV_RELATIVE
            default:
                return false;
        }
    }

} // namespace

auto ElfImage::init(ustl::views::Span<u8 const> file, usize memory_size) -> Status {
    m_binary = ustl::NONE;
    m_load_plan = LoadPlan{};
    auto parsed = eftl::elf::Parser::parse_as<Traits>(file.data(), file.size());
    if (parsed.is_err()) {
        return Status::InvalidArgument;
    }

    auto const native_image = parsed->native_image();
    if (native_image.is_none()) {
        return Status::InvalidArgument;
    }
    auto planned = LoadPolicy::make_plan(native_image.value());
    if (planned.is_err() || memory_size < planned->image_size()) {
        return Status::InvalidArgument;
    }

    m_binary = ustl::some(std::move(parsed).unwrap());
    m_load_plan = std::move(planned).unwrap();
    return Status::Ok;
}

auto ElfImage::load_at(PhysAddr phys_addr) -> ustl::Result<PhysAddr, Status> {
    if (phys_addr > std::numeric_limits<PhysAddr>::max() - size()) {
        return ustl::err(Status::OutOfRange);
    }

    if (size() != 0) {
        std::memset(reinterpret_cast<u8 *>(phys_addr), 0, size());
    }
    for (auto const &segment: m_load_plan.segments()) {
        auto const &region = segment.region;
        if (region.image_offset > size() || segment.memory_size > size() - region.image_offset ||
            segment.file_size > segment.memory_size ||
            segment.content.size() != segment.file_size) {
            return ustl::err(Status::InvalidArgument);
        }
        if (phys_addr > std::numeric_limits<PhysAddr>::max() - region.image_offset) {
            return ustl::err(Status::OutOfRange);
        }

        auto const alignment = to_usize(segment.alignment);
        if (alignment.is_none()) {
            return ustl::err(Status::InvalidArgument);
        }
        auto const destination_address = phys_addr + region.image_offset;
        if (alignment.value() > 1 && (destination_address % alignment.value()) !=
                                         (region.virtual_start % alignment.value())) {
            return ustl::err(Status::InvalidArgument);
        }

        auto *const destination = reinterpret_cast<u8 *>(destination_address);
        if (!segment.content.empty()) {
            std::memcpy(destination, segment.content.data(), segment.content.size());
        }
        auto const zero_size = static_cast<usize>(segment.memory_size - segment.file_size);
        if (zero_size != 0) {
            std::memset(destination + segment.content.size(), 0, zero_size);
        }
    }

    auto const entry_offset = m_load_plan.entry_offset();
    if (entry_offset.is_none() ||
        phys_addr > std::numeric_limits<PhysAddr>::max() - entry_offset.value()) {
        return ustl::err(Status::OutOfRange);
    }
    m_pa_load = phys_addr;
    return ustl::ok(m_pa_load + entry_offset.value());
}

auto ElfImage::relocate(usize load_address) -> Status {
    if (m_binary.is_none() || load_address < m_load_plan.image_base()) {
        return Status::InvalidArgument;
    }
    if constexpr (sizeof(usize) > sizeof(Traits::Addr)) {
        if (load_address > std::numeric_limits<typename Traits::Addr>::max()) {
            return Status::OutOfRange;
        }
    }

    eftl::elf::MappedMemory<Traits> image(reinterpret_cast<u8 *>(m_pa_load), size(),
                                          m_load_plan.regions());
    auto const load_bias =
        static_cast<typename Traits::Addr>(load_address - m_load_plan.image_base());
    auto const machine = static_cast<eftl::elf::Arch>(m_binary.value().elf_header().e_machine);
    if (!apply_kernel_relative_relocations(m_binary.value(), image, load_bias, [machine](u32 type) {
            return relative_relocation_type(machine, type);
        })) {
        return Status::InvalidArgument;
    }

    m_va_load = load_address;
    return Status::Ok;
}

namespace {
    /// Bind one .rela table against the loader's definitions.
    template <typename Traits, typename Relas, typename Symbols>
    [[nodiscard]]
    auto bind_rela_table(eftl::elf::MappedMemory<Traits> &memory, Relas const &relocations,
                         Symbols const &symbols, typename Traits::Addr bias,
                         eftl::elf::Arch machine, SymbolBinder const &binder) -> Status {
        for (usize index = 0; index < relocations.size(); ++index) {
            auto const entry = relocations.relocation_at(index);
            if (entry.is_none()) {
                return Status::InvalidArgument;
            }
            auto const &rela = entry.value();

            auto const symbol_index = Relas::symbol_index(rela);
            if (symbol_index == 0) {
                // No symbol: a relative relocation, already applied.
                continue;
            }

            auto const type = Relas::type(rela);
            if (!symbolic_relocation_type(machine, type)) {
                dprintln("Module carries unsupported relocation type {}", type);
                return Status::Unsupported;
            }

            auto const symbol = symbols.symbol_at(static_cast<usize>(symbol_index));
            if (symbol.is_none()) {
                return Status::InvalidArgument;
            }

            // SHN_UNDEF (0): unresolved, look up in physload.  SHN_ABS
            // (0xfff1): an absolute symbol -- st_value already is the final
            // address, not an offset into some section, so it must not take
            // `bias`.  Anything else: defined in this image at a
            // section-relative value, which does take the bias.  Binding it
            // to the module's own definition (rather than shadowing it with
            // physload's) matters because a same-named physload symbol may
            // be a different function entirely.
            constexpr auto kShnAbs = decltype(symbol->st_shndx){0xfff1};

            usize address = 0;
            if (symbol->st_shndx == 0) {
                auto const name = symbols.symbol_name(static_cast<usize>(symbol_index));
                if (name.is_none()) {
                    return Status::InvalidArgument;
                }
                auto const spelling = std::string_view(
                    reinterpret_cast<char const *>(name->data()), name->size());

                auto const *definition = binder.resolve(binder.context, spelling);
                if (definition == nullptr) {
                    // Loud on purpose: the alternative is binding to zero and
                    // faulting somewhere unrelated once the module runs.
                    println("Module needs symbol '{}', which physload does not export",
                            spelling);
                    return Status::NotFound;
                }
                address = reinterpret_cast<usize>(definition);
            } else if (symbol->st_shndx == kShnAbs) {
                address = static_cast<usize>(symbol->st_value);
            } else {
                address = static_cast<usize>(symbol->st_value) + static_cast<usize>(bias);
            }

            auto const value = static_cast<typename Traits::Addr>(
                address + static_cast<usize>(static_cast<isize>(rela.r_addend)));
            if (!memory.template write<typename Traits::Addr>(
                    static_cast<typename Traits::Addr>(rela.r_offset) + bias, value)) {
                return Status::OutOfRange;
            }
        }
        return Status::Ok;
    }
} // namespace

auto ElfImage::relocate_and_bind(usize const load_address, SymbolBinder const &binder) -> Status {
    if (auto const status = relocate(load_address); status != Status::Ok) {
        return status;
    }

    auto const image = m_binary.value().dynamic_image();
    if (image.is_none()) {
        // Nothing dynamic to bind: a module with no imports is legal.
        return Status::Ok;
    }

    auto const symbols = image->dynamic_symbols();
    auto const relas = image->dynamic_rela_table();
    auto const plt_relas = image->dynamic_plt_rela_table();
    if (relas.is_none() && plt_relas.is_none()) {
        return Status::Ok;
    }
    if (symbols.is_none()) {
        // There are symbolic relocations but no readable .dynsym to name
        // them; binding cannot proceed.
        return Status::InvalidArgument;
    }

    eftl::elf::MappedMemory<Traits> memory(reinterpret_cast<u8 *>(m_pa_load), size(),
                                           m_load_plan.regions());
    auto const bias = static_cast<typename Traits::Addr>(load_address - m_load_plan.image_base());
    auto const machine = static_cast<eftl::elf::Arch>(m_binary.value().elf_header().e_machine);

    if (relas.is_some()) {
        auto const status =
            bind_rela_table<Traits>(memory, relas.value(), symbols.value(), bias, machine, binder);
        if (status != Status::Ok) {
            return status;
        }
    }
    if (plt_relas.is_some()) {
        auto const status = bind_rela_table<Traits>(memory, plt_relas.value(), symbols.value(),
                                                    bias, machine, binder);
        if (status != Status::Ok) {
            return status;
        }
    }
    return Status::Ok;
}

auto ElfImage::section(std::string_view const name) const -> std::span<u8 const> {
    if (m_binary.is_none() || m_pa_load == 0) {
        return {};
    }

    auto const &binary = m_binary.value();
    auto const sections = binary.sections();
    for (usize index = 0; index < sections.size(); ++index) {
        auto const found = binary.section_name(index);
        if (found.is_none()) {
            continue;
        }
        auto const spelling =
            std::string_view(reinterpret_cast<char const *>(found->data()), found->size());
        if (spelling != name) {
            continue;
        }

        auto const header = sections.at(index);
        if (header.is_none()) {
            return {};
        }
        // SHF_ALLOC only: a section with no runtime address was never
        // loaded, and reporting one would hand back a wild pointer.
        constexpr auto kAlloc = u64{0x2};
        if ((static_cast<u64>(header->sh_flags) & kAlloc) == 0) {
            return {};
        }

        auto const offset = static_cast<usize>(header->sh_addr) - m_load_plan.image_base();
        auto const size = static_cast<usize>(header->sh_size);
        if (offset > this->size() || size > this->size() - offset) {
            return {};
        }
        return {reinterpret_cast<u8 const *>(m_pa_load + offset), size};
    }
    return {};
}

} // namespace ours::phys
