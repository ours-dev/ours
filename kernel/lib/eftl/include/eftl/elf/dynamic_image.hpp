/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef EFTL_ELF_DYNAMIC_IMAGE_HPP
#define EFTL_ELF_DYNAMIC_IMAGE_HPP 1

#include <eftl/binary_stream.hpp>
#include <eftl/elf/details/dyn.hpp>
#include <eftl/elf/dynamic_array.hpp>
#include <eftl/elf/dynamic_linkage.hpp>
#include <eftl/elf/encoding.hpp>
#include <eftl/elf/gnu_hash.hpp>
#include <eftl/elf/relocation_section.hpp>
#include <eftl/elf/segment.hpp>
#include <eftl/elf/sysv_hash.hpp>
#include <eftl/elf/table.hpp>
#include <eftl/elf/traits.hpp>
#include <eftl/elf/version_binding.hpp>
#include <eftl/elf/version_definition.hpp>
#include <eftl/elf/version_requirement.hpp>
#include <eftl/elf/version_symbol.hpp>

#include <limits>
#include <type_traits>

#include <ustl/option.hpp>

namespace eftl::elf {
template <typename Traits>
class DynamicImageView final {
  public:
    using Dynamic = typename Traits::Dyn;
    using Dynamics = TableView<Dynamic>;
    using Addr = typename Traits::Addr;
    using Rel = typename Traits::Rel;
    using Rela = typename Traits::Rela;
    using Relr = TableView<Addr>;
    using Relocations = RelocationTableView<Traits, Rel>;
    using RelaRelocations = RelocationTableView<Traits, Rela>;
    using Segment = typename Traits::Segment;
    using Segments = TableView<Segment>;
    using Symbol = typename Traits::Symbol;
    using Symbols = SymbolTableView<Traits>;
    using GnuHash = GnuHashTableView<Traits>;
    using SysvHash = SysvHashTableView<Traits>;
    using Version = typename Traits::Half;
    using VersionDefinitions = VersionDefinitionTableView<Traits>;
    using VersionRequirements = VersionRequirementTableView<Traits>;
    using VersionSymbols = VersionSymbolTableView<Traits>;
    using FunctionArray = DynamicFunctionArrayView<Traits>;

  private:
    struct SysvHashLayout final {
        ByteRange m_entries{};
        usize m_symbol_count{0};
    };

    struct GnuHashLayout final {
        ByteRange m_entries{};
        usize m_symbol_count{0};
    };

  public:
    [[nodiscard]]
    static auto from(BinaryStream stream, Segments segments, ByteOrder byte_order) noexcept
        -> ustl::Option<DynamicImageView> {
        ustl::Option<Segment> dynamic_segment;
        for (auto const segment: segments) {
            if (segment.type() != SegType::Dynamic) {
                continue;
            }
            if (dynamic_segment.is_some()) {
                return ustl::NONE;
            }
            dynamic_segment = segment;
        }
        if (dynamic_segment.is_none()) {
            return ustl::NONE;
        }

        auto const dynamic_size = to_usize(dynamic_segment->p_filesz);
        auto const dynamic_offset = to_usize(dynamic_segment->p_offset);
        if (dynamic_size.is_none() || dynamic_offset.is_none() || dynamic_size.value() == 0 ||
            dynamic_size.value() % sizeof(Dynamic) != 0 ||
            static_cast<u64>(dynamic_segment->p_filesz) >
                static_cast<u64>(dynamic_segment->p_memsz)) {
            return ustl::NONE;
        }

        auto const dynamic_bytes = stream.bytes(dynamic_offset.value(), dynamic_size.value());
        if (dynamic_bytes.is_err()) {
            return ustl::NONE;
        }
        auto const decoder = decoder_for<RecordCodec<Traits, Dynamic>>(byte_order);
        auto const dynamics = Dynamics::from_checked_bytes(
            dynamic_bytes.unwrap(), dynamic_size.value() / sizeof(Dynamic), decoder);
        if (dynamics.is_none()) {
            return ustl::NONE;
        }
        auto const terminator = terminator_index(dynamics.value());
        if (terminator.is_none()) {
            return ustl::NONE;
        }

        DynamicImageView image(stream, segments, dynamics.value(), terminator.value(), byte_order);
        auto const mapped = image.bytes_at_virtual_address(
            static_cast<u64>(dynamic_segment->p_vaddr), dynamic_size.value());
        if (mapped.is_none() || mapped->data() != dynamic_bytes->data()) {
            return ustl::NONE;
        }
        return ustl::some(DynamicImageView{image});
    }

    [[nodiscard]]
    constexpr auto size() const noexcept -> usize {
        return m_count;
    }

    /// Includes the terminating DT_NULL record required by the dynamic-image contract.
    [[nodiscard]]
    auto entries() const noexcept -> Dynamics {
        return m_dynamics.first(m_count + 1).value();
    }

    [[nodiscard]]
    auto entry_at(usize index) const noexcept -> ustl::Option<Dynamic> {
        if (index < m_count) {
            return m_dynamics.at(index);
        }
        return ustl::NONE;
    }

    /// Returns DT_STRTAB bytes after proving that its address and size tags are unique.
    [[nodiscard]]
    auto dynamic_strings() const noexcept -> ustl::Option<ByteRange> {
        auto const address = unique_tag_value(DynTag::StrTab);
        auto const size = unique_tag_value(DynTag::StrSz);
        if (address.is_none() || size.is_none()) {
            return ustl::NONE;
        }
        auto const byte_count = to_usize(size.value());
        if (byte_count.is_none() || byte_count.value() == 0) {
            return ustl::NONE;
        }
        return bytes_at_virtual_address(address.value(), byte_count.value());
    }

    [[nodiscard]]
    auto linkage() const noexcept -> ustl::Option<DynamicLinkageView<Traits>> {
        auto const strings = dynamic_strings();
        auto const empty_strings = StringTableView::from(ByteRange(nullptr, 0));
        auto const string_table = [&]() -> ustl::Option<StringTableView> {
            if (strings.is_some()) {
                return StringTableView::from(strings.value());
            }
            return empty_strings;
        }();
        auto const entries = m_dynamics.first(m_count);
        if (string_table.is_some() && entries.is_some()) {
            return DynamicLinkageView<Traits>::from(entries.value(), string_table.value());
        }
        return ustl::NONE;
    }

    /// Returns the file-backed DT_INIT_ARRAY function pointers.
    [[nodiscard]]
    auto dynamic_init_array() const noexcept -> ustl::Option<FunctionArray> {
        return function_array_from_tags(DynTag::InitArray, DynTag::InitArraySz);
    }

    /// Returns the file-backed DT_FINI_ARRAY function pointers.
    [[nodiscard]]
    auto dynamic_fini_array() const noexcept -> ustl::Option<FunctionArray> {
        return function_array_from_tags(DynTag::FiniArray, DynTag::FiniArraySz);
    }

    /// Returns the file-backed DT_PREINIT_ARRAY function pointers.
    [[nodiscard]]
    auto dynamic_preinit_array() const noexcept -> ustl::Option<FunctionArray> {
        return function_array_from_tags(DynTag::PreinitArray, DynTag::PreinitArraySz);
    }

    /// Returns the legacy DT_INIT entry point value without executing it.
    [[nodiscard]]
    auto dynamic_init_function() const noexcept -> ustl::Option<u64> {
        return unique_tag_value(DynTag::Init);
    }

    /// Returns the legacy DT_FINI entry point value without executing it.
    [[nodiscard]]
    auto dynamic_fini_function() const noexcept -> ustl::Option<u64> {
        return unique_tag_value(DynTag::Fini);
    }

    /// Returns DT_SYMTAB entries when a dynamic hash table supplies their exact count.
    [[nodiscard]]
    auto dynamic_symbols() const noexcept -> ustl::Option<Symbols> {
        if (auto const sysv = sysv_hash_layout(); sysv.is_some()) {
            return symbols_with_count(sysv->m_symbol_count);
        }
        if (contains_tag(DynTag::Hash)) {
            return ustl::NONE;
        }
        auto const gnu = gnu_hash_layout();
        if (gnu.is_none()) {
            return ustl::NONE;
        }
        return symbols_with_count(gnu->m_symbol_count);
    }

    /// Returns the checked DT_HASH table and its dynamic-symbol linkage.
    [[nodiscard]]
    auto sysv_hash_table() const noexcept -> ustl::Option<SysvHash> {
        auto const layout = sysv_hash_layout();
        if (layout.is_none()) {
            return ustl::NONE;
        }
        auto const symbols = symbols_with_count(layout->m_symbol_count);
        if (symbols.is_none()) {
            return ustl::NONE;
        }
        return SysvHash::from(layout->m_entries, symbols.value(), m_byte_order);
    }

    /// Returns the checked DT_GNU_HASH table and its dynamic-symbol linkage.
    [[nodiscard]]
    auto gnu_hash_table() const noexcept -> ustl::Option<GnuHash> {
        auto const layout = gnu_hash_layout();
        if (layout.is_none()) {
            return ustl::NONE;
        }
        auto const symbols = symbols_with_count(layout->m_symbol_count);
        if (symbols.is_none()) {
            return ustl::NONE;
        }
        return GnuHash::from(layout->m_entries, symbols.value(), m_byte_order);
    }

    /// Returns the DT_VERSYM entries associated one-to-one with dynamic symbols.
    [[nodiscard]]
    auto dynamic_version_symbols() const noexcept -> ustl::Option<VersionSymbols> {
        auto const address = unique_tag_value(DynTag::VerSym);
        auto const symbols = dynamic_symbols();
        if (address.is_none() || symbols.is_none() ||
            symbols->size() > std::numeric_limits<usize>::max() / sizeof(Version)) {
            return ustl::NONE;
        }
        auto const entries =
            bytes_at_virtual_address(address.value(), symbols->size() * sizeof(Version));
        if (entries.is_none()) {
            return ustl::NONE;
        }
        auto const versions = VersionSymbols::from(entries.value(), m_byte_order);
        if (versions.is_some() && versions->size() == symbols->size()) {
            return versions;
        }
        return ustl::NONE;
    }

    /// Returns DT_VERDEF definitions after checking their declared DT_VERDEFNUM count.
    [[nodiscard]]
    auto dynamic_version_definitions() const noexcept -> ustl::Option<VersionDefinitions> {
        auto const address = unique_tag_value(DynTag::VerDef);
        auto const count = unique_tag_value(DynTag::VerDefNum);
        auto const strings = dynamic_strings();
        if (address.is_none() || count.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        auto const expected_count = to_usize(count.value());
        if (expected_count.is_none()) {
            return ustl::NONE;
        }
        auto const entries = bytes_to_file_backed_segment_end(address.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        auto const definitions =
            VersionDefinitions::from(entries.value(), strings.value(), m_byte_order);
        if (definitions.is_some() && definitions->size() == expected_count.value()) {
            return definitions;
        }
        return ustl::NONE;
    }

    /// Returns DT_VERNEED requirements after checking their declared DT_VERNEEDNUM count.
    [[nodiscard]]
    auto dynamic_version_requirements() const noexcept -> ustl::Option<VersionRequirements> {
        auto const address = unique_tag_value(DynTag::VerNeed);
        auto const count = unique_tag_value(DynTag::VerNeedNum);
        auto const strings = dynamic_strings();
        if (address.is_none() || count.is_none() || strings.is_none()) {
            return ustl::NONE;
        }
        auto const expected_count = to_usize(count.value());
        if (expected_count.is_none()) {
            return ustl::NONE;
        }
        auto const entries = bytes_to_file_backed_segment_end(address.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        auto const requirements =
            VersionRequirements::from(entries.value(), strings.value(), m_byte_order);
        if (requirements.is_some() && requirements->size() == expected_count.value()) {
            return requirements;
        }
        return ustl::NONE;
    }

    /// Resolves a dynamic symbol's local, global, definition, or requirement version binding.
    [[nodiscard]]
    auto dynamic_version_binding(usize symbol_index) const noexcept
        -> ustl::Option<VersionBinding<Traits>> {
        auto const versions = dynamic_version_symbols();
        if (versions.is_none()) {
            return ustl::NONE;
        }
        auto const base = VersionResolver<Traits>::resolve_base(versions.value(), symbol_index);
        if (base.is_some()) {
            return base;
        }

        ustl::Option<VersionBinding<Traits>> result;
        if (auto const definitions = dynamic_version_definitions(); definitions.is_some()) {
            result = VersionResolver<Traits>::resolve_definition(versions.value(),
                                                                 definitions.value(), symbol_index);
        }
        if (auto const requirements = dynamic_version_requirements(); requirements.is_some()) {
            auto const requirement = VersionResolver<Traits>::resolve_requirement(
                versions.value(), requirements.value(), symbol_index);
            if (requirement.is_some() && result.is_some()) {
                return ustl::NONE;
            }
            if (requirement.is_some()) {
                result = requirement;
            }
        }
        return result;
    }

    /// Returns the ordinary DT_REL relocation records without applying them.
    [[nodiscard]]
    auto dynamic_rel_table() const noexcept -> ustl::Option<Relocations> {
        return relocation_table_from_tags<Rel>(DynTag::Rel, DynTag::RelSz, DynTag::RelEnt);
    }

    /// Returns the ordinary DT_RELA relocation records without applying them.
    [[nodiscard]]
    auto dynamic_rela_table() const noexcept -> ustl::Option<RelaRelocations> {
        return relocation_table_from_tags<Rela>(DynTag::Rela, DynTag::RelaSz, DynTag::RelaEnt);
    }

    /// Returns the raw DT_RELR words. RELR bitmap expansion remains loader-specific.
    [[nodiscard]]
    auto dynamic_relr_table() const noexcept -> ustl::Option<Relr> {
        return scalar_table_from_tags<Addr>(DynTag::Relr, DynTag::RelrSz, DynTag::RelrEnt);
    }

    /// Returns PLT relocations encoded as DT_REL when DT_PLTREL selects DT_REL.
    [[nodiscard]]
    auto dynamic_plt_rel_table() const noexcept -> ustl::Option<Relocations> {
        if (plt_relocation_type() == DynTag::Rel) {
            return relocation_table_from_tags<Rel>(DynTag::JmpRel, DynTag::PltRelSz,
                                                   DynTag::RelEnt);
        }
        return ustl::NONE;
    }

    /// Returns PLT relocations encoded as DT_RELA when DT_PLTREL selects DT_RELA.
    [[nodiscard]]
    auto dynamic_plt_rela_table() const noexcept -> ustl::Option<RelaRelocations> {
        if (plt_relocation_type() == DynTag::Rela) {
            return relocation_table_from_tags<Rela>(DynTag::JmpRel, DynTag::PltRelSz,
                                                    DynTag::RelaEnt);
        }
        return ustl::NONE;
    }

    [[nodiscard]]
    auto bytes_at_virtual_address(u64 virtual_address, usize count) const noexcept
        -> ustl::Option<ByteRange> {
        if (count == 0) {
            return ustl::NONE;
        }
        auto const range_size = to_u64(count);
        if (range_size.is_none() ||
            virtual_address > std::numeric_limits<u64>::max() - range_size.value()) {
            return ustl::NONE;
        }
        auto const virtual_end = virtual_address + range_size.value();
        ustl::Option<ByteRange> mapping;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::Load || segment.p_filesz > segment.p_memsz) {
                continue;
            }
            auto const file_size = static_cast<u64>(segment.p_filesz);
            auto const virtual_begin = static_cast<u64>(segment.p_vaddr);
            if (file_size == 0 || virtual_begin > std::numeric_limits<u64>::max() - file_size ||
                virtual_address < virtual_begin || virtual_end > virtual_begin + file_size) {
                continue;
            }

            auto const file_offset = to_usize(segment.p_offset);
            auto const virtual_delta = to_usize(virtual_address - virtual_begin);
            if (file_offset.is_none() || virtual_delta.is_none() ||
                file_offset.value() > std::numeric_limits<usize>::max() - virtual_delta.value()) {
                return ustl::NONE;
            }
            auto const bytes = m_stream.bytes(file_offset.value() + virtual_delta.value(), count);
            if (bytes.is_err() || mapping.is_some()) {
                return ustl::NONE;
            }
            mapping = bytes.unwrap();
        }
        return mapping;
    }

    [[nodiscard]]
    static constexpr auto tag(Dynamic const &entry) noexcept -> DynTag {
        return static_cast<DynTag>(static_cast<u64>(entry.d_tag));
    }

    [[nodiscard]]
    static constexpr auto value(Dynamic const &entry) noexcept -> u64 {
        return static_cast<u64>(entry.d_val);
    }

  private:
    constexpr DynamicImageView(BinaryStream stream, Segments segments, Dynamics dynamics,
                               usize count, ByteOrder byte_order) noexcept
        : m_stream(stream)
        , m_segments(segments)
        , m_dynamics(dynamics)
        , m_count(count)
        , m_byte_order(byte_order) {}

    [[nodiscard]]
    auto unique_tag_value(DynTag wanted) const noexcept -> ustl::Option<u64> {
        ustl::Option<u64> result;
        for (auto const entry: entries()) {
            if (tag(entry) != wanted) {
                continue;
            }
            if (result.is_some()) {
                return ustl::NONE;
            }
            result = value(entry);
        }
        return result;
    }

    [[nodiscard]]
    auto contains_tag(DynTag wanted) const noexcept -> bool {
        for (auto const entry: entries()) {
            if (tag(entry) == wanted) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]]
    auto sysv_hash_layout() const noexcept -> ustl::Option<SysvHashLayout> {
        using Word = u32;
        constexpr usize HEADER_WORDS = 2;
        constexpr usize HEADER_BYTES = HEADER_WORDS * sizeof(Word);

        auto const address = unique_tag_value(DynTag::Hash);
        if (address.is_none()) {
            return ustl::NONE;
        }
        auto const header_bytes = bytes_at_virtual_address(address.value(), HEADER_BYTES);
        if (header_bytes.is_none()) {
            return ustl::NONE;
        }
        auto const decoder = decoder_for<ScalarCodec<Word>>(m_byte_order);
        auto const header =
            TableView<Word>::from_checked_bytes(header_bytes.value(), HEADER_WORDS, decoder);
        if (header.is_none()) {
            return ustl::NONE;
        }
        auto const bucket_count = to_usize((*header)[0]);
        auto const symbol_count = to_usize((*header)[1]);
        if (bucket_count.is_none() || symbol_count.is_none() || bucket_count.value() == 0 ||
            symbol_count.value() == 0 ||
            bucket_count.value() > std::numeric_limits<usize>::max() - HEADER_WORDS ||
            symbol_count.value() >
                std::numeric_limits<usize>::max() - HEADER_WORDS - bucket_count.value()) {
            return ustl::NONE;
        }
        auto const word_count = HEADER_WORDS + bucket_count.value() + symbol_count.value();
        if (word_count > std::numeric_limits<usize>::max() / sizeof(Word)) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), word_count * sizeof(Word));
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return ustl::some(SysvHashLayout{
            .m_entries = entries.value(),
            .m_symbol_count = symbol_count.value(),
        });
    }

    [[nodiscard]]
    auto gnu_hash_layout() const noexcept -> ustl::Option<GnuHashLayout> {
        using Word = u32;
        using BloomWord = typename Traits::Addr;
        constexpr usize HEADER_WORDS = 4;
        constexpr usize HEADER_BYTES = HEADER_WORDS * sizeof(Word);

        auto const address = unique_tag_value(DynTag::GnuHash);
        if (address.is_none()) {
            return ustl::NONE;
        }
        auto const tail = bytes_to_file_backed_segment_end(address.value());
        if (tail.is_none() || tail->size() < HEADER_BYTES) {
            return ustl::NONE;
        }
        auto const word_decoder = decoder_for<ScalarCodec<Word>>(m_byte_order);
        auto const header = TableView<Word>::from_checked_bytes(
            ByteRange(tail->data(), HEADER_BYTES), HEADER_WORDS, word_decoder);
        if (header.is_none()) {
            return ustl::NONE;
        }
        auto const bucket_count = to_usize((*header)[0]);
        auto const symbol_offset = to_usize((*header)[1]);
        auto const bloom_count = to_usize((*header)[2]);
        auto const bloom_shift = (*header)[3];
        if (bucket_count.is_none() || symbol_offset.is_none() || bloom_count.is_none() ||
            bucket_count.value() == 0 || bloom_count.value() == 0 ||
            (bloom_count.value() & (bloom_count.value() - 1)) != 0 ||
            bloom_shift >= std::numeric_limits<Word>::digits) {
            return ustl::NONE;
        }

        auto prefix_size = HEADER_BYTES;
        if (!extend_size(prefix_size, bloom_count.value(), sizeof(BloomWord)) ||
            !extend_size(prefix_size, bucket_count.value(), sizeof(Word)) ||
            prefix_size > tail->size()) {
            return ustl::NONE;
        }
        auto const bucket_size = bucket_count.value() * sizeof(Word);
        auto const buckets = TableView<Word>::from_checked_bytes(
            ByteRange(tail->data() + HEADER_BYTES + bloom_count.value() * sizeof(BloomWord),
                      bucket_size),
            bucket_count.value(), word_decoder);
        if (buckets.is_none()) {
            return ustl::NONE;
        }

        auto const chain_bytes = tail->size() - prefix_size;
        auto const chain_count = chain_bytes / sizeof(Word);
        auto const chains = TableView<Word>::from_checked_bytes(
            ByteRange(tail->data() + prefix_size, chain_count * sizeof(Word)), chain_count,
            word_decoder);
        if (chains.is_none()) {
            return ustl::NONE;
        }

        auto symbol_count = symbol_offset.value();
        for (auto const bucket: buckets.value()) {
            if (bucket == 0) {
                continue;
            }
            auto const first_symbol = to_usize(bucket);
            if (first_symbol.is_none() || first_symbol.value() < symbol_offset.value()) {
                return ustl::NONE;
            }
            auto symbol_index = first_symbol.value();
            while (true) {
                auto const chain_index = symbol_index - symbol_offset.value();
                auto const chain = chains->at(chain_index);
                if (chain.is_none() || symbol_index == std::numeric_limits<usize>::max()) {
                    return ustl::NONE;
                }
                ++symbol_index;
                if ((chain.value() & 1U) != 0) {
                    if (symbol_index > symbol_count) {
                        symbol_count = symbol_index;
                    }
                    break;
                }
            }
        }
        if (symbol_count == 0 || symbol_count < symbol_offset.value()) {
            return ustl::NONE;
        }
        auto const hash_chain_count = symbol_count - symbol_offset.value();
        if (!extend_size(prefix_size, hash_chain_count, sizeof(Word))) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), prefix_size);
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return ustl::some(GnuHashLayout{
            .m_entries = entries.value(),
            .m_symbol_count = symbol_count,
        });
    }

    template <typename Entry>
    [[nodiscard]]
    auto relocation_table_from_tags(DynTag address_tag, DynTag size_tag,
                                    DynTag entry_size_tag) const noexcept
        -> ustl::Option<RelocationTableView<Traits, Entry>> {
        using Table = RelocationTableView<Traits, Entry>;

        auto const address = unique_tag_value(address_tag);
        auto const size = unique_tag_value(size_tag);
        auto const entry_size = unique_tag_value(entry_size_tag);
        if (address.is_none() || size.is_none() || entry_size.is_none()) {
            return ustl::NONE;
        }
        auto const byte_count = to_usize(size.value());
        if (byte_count.is_none() || byte_count.value() == 0 ||
            byte_count.value() % sizeof(Entry) != 0 || entry_size.value() != sizeof(Entry)) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), byte_count.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        auto const relocations = Table::from(entries.value(), m_byte_order);
        if (relocations.is_none()) {
            return ustl::NONE;
        }

        ustl::Option<Symbols> symbols;
        for (usize index = 0; index < relocations->size(); ++index) {
            auto const relocation = relocations->relocation_at(index);
            if (relocation.is_none()) {
                return ustl::NONE;
            }
            auto const symbol_index = static_cast<usize>(Table::symbol_index(relocation.value()));
            if (symbol_index == 0) {
                continue;
            }
            if (symbols.is_none()) {
                symbols = dynamic_symbols();
            }
            if (symbols.is_none() || symbol_index >= symbols->size()) {
                return ustl::NONE;
            }
        }
        return relocations;
    }

    template <typename Value>
    [[nodiscard]]
    auto scalar_table_from_tags(DynTag address_tag, DynTag size_tag,
                                DynTag entry_size_tag) const noexcept
        -> ustl::Option<TableView<Value>> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);

        auto const address = unique_tag_value(address_tag);
        auto const size = unique_tag_value(size_tag);
        auto const entry_size = unique_tag_value(entry_size_tag);
        if (address.is_none() || size.is_none() || entry_size.is_none()) {
            return ustl::NONE;
        }
        auto const byte_count = to_usize(size.value());
        if (byte_count.is_none() || byte_count.value() == 0 ||
            byte_count.value() % sizeof(Value) != 0 || entry_size.value() != sizeof(Value)) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), byte_count.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        auto const decoder = decoder_for<ScalarCodec<Value>>(m_byte_order);
        return TableView<Value>::from_checked_bytes(entries.value(),
                                                    byte_count.value() / sizeof(Value), decoder);
    }

    [[nodiscard]]
    auto function_array_from_tags(DynTag address_tag, DynTag size_tag) const noexcept
        -> ustl::Option<FunctionArray> {
        auto const address = unique_tag_value(address_tag);
        auto const size = unique_tag_value(size_tag);
        if (address.is_none() || size.is_none()) {
            return ustl::NONE;
        }
        auto const byte_count = to_usize(size.value());
        if (byte_count.is_none() || byte_count.value() == 0) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), byte_count.value());
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return FunctionArray::from(entries.value(), m_byte_order);
    }

    [[nodiscard]]
    auto plt_relocation_type() const noexcept -> DynTag {
        auto const encoded = unique_tag_value(DynTag::PltRel);
        if (encoded.is_none()) {
            return DynTag::Unknown;
        }
        auto const type = static_cast<DynTag>(encoded.value());
        if (type == DynTag::Rel || type == DynTag::Rela) {
            return type;
        }
        return DynTag::Unknown;
    }

    [[nodiscard]]
    auto symbols_with_count(usize count) const noexcept -> ustl::Option<Symbols> {
        auto const address = unique_tag_value(DynTag::SymTab);
        auto const entry_size = unique_tag_value(DynTag::Syment);
        auto const strings = dynamic_strings();
        if (address.is_none() || entry_size.is_none() || entry_size.value() != sizeof(Symbol) ||
            strings.is_none() || count == 0 ||
            count > std::numeric_limits<usize>::max() / sizeof(Symbol)) {
            return ustl::NONE;
        }
        auto const entries = bytes_at_virtual_address(address.value(), count * sizeof(Symbol));
        if (entries.is_none()) {
            return ustl::NONE;
        }
        return Symbols::from(entries.value(), strings.value(), m_byte_order);
    }

    [[nodiscard]]
    auto bytes_to_file_backed_segment_end(u64 virtual_address) const noexcept
        -> ustl::Option<ByteRange> {
        ustl::Option<ByteRange> mapping;
        for (auto const segment: m_segments) {
            if (segment.type() != SegType::Load || segment.p_filesz > segment.p_memsz) {
                continue;
            }
            auto const file_size = static_cast<u64>(segment.p_filesz);
            auto const virtual_begin = static_cast<u64>(segment.p_vaddr);
            if (file_size == 0 || virtual_begin > std::numeric_limits<u64>::max() - file_size ||
                virtual_address < virtual_begin || virtual_address >= virtual_begin + file_size) {
                continue;
            }
            auto const file_offset = to_usize(segment.p_offset);
            auto const virtual_delta = to_usize(virtual_address - virtual_begin);
            auto const remaining = to_usize(file_size - (virtual_address - virtual_begin));
            if (file_offset.is_none() || virtual_delta.is_none() || remaining.is_none() ||
                file_offset.value() > std::numeric_limits<usize>::max() - virtual_delta.value()) {
                return ustl::NONE;
            }
            auto const bytes =
                m_stream.bytes(file_offset.value() + virtual_delta.value(), remaining.value());
            if (bytes.is_err() || mapping.is_some()) {
                return ustl::NONE;
            }
            mapping = bytes.unwrap();
        }
        return mapping;
    }

    [[nodiscard]]
    static constexpr auto extend_size(usize &size, usize count, usize element_size) noexcept
        -> bool {
        if (count > std::numeric_limits<usize>::max() / element_size) {
            return false;
        }
        auto const bytes = count * element_size;
        if (size > std::numeric_limits<usize>::max() - bytes) {
            return false;
        }
        size += bytes;
        return true;
    }

    template <typename Value>
    [[nodiscard]]
    static constexpr auto to_usize(Value value) noexcept -> ustl::Option<usize> {
        static_assert(std::is_integral_v<Value> && std::is_unsigned_v<Value>);
        if constexpr (sizeof(Value) > sizeof(usize)) {
            if (value > static_cast<Value>(std::numeric_limits<usize>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<usize>(value));
    }

    [[nodiscard]]
    static constexpr auto to_u64(usize value) noexcept -> ustl::Option<u64> {
        if constexpr (sizeof(usize) > sizeof(u64)) {
            if (value > static_cast<usize>(std::numeric_limits<u64>::max())) {
                return ustl::NONE;
            }
        }
        return ustl::some(static_cast<u64>(value));
    }

    template <typename Codec>
    [[nodiscard]]
    static constexpr auto decoder_for(ByteOrder byte_order) noexcept
        -> decltype(&Codec::decode_little) {
        if (byte_order == ByteOrder::Little) {
            return &Codec::decode_little;
        }
        return &Codec::decode_big;
    }

    [[nodiscard]]
    static auto terminator_index(Dynamics const &dynamics) noexcept -> ustl::Option<usize> {
        for (usize index = 0; index < dynamics.size(); ++index) {
            if (tag(dynamics[index]) != DynTag::DtNull) {
                continue;
            }
            for (usize remaining = index + 1; remaining < dynamics.size(); ++remaining) {
                if (tag(dynamics[remaining]) != DynTag::DtNull) {
                    return ustl::NONE;
                }
            }
            return ustl::some(static_cast<usize>(index));
        }
        return ustl::NONE;
    }

    BinaryStream m_stream;
    Segments m_segments;
    Dynamics m_dynamics;
    usize m_count{0};
    ByteOrder m_byte_order{ByteOrder::Little};
};
} // namespace eftl::elf

#endif // EFTL_ELF_DYNAMIC_IMAGE_HPP
