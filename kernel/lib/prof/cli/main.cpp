/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prof-fold: turn a prof.dump serial export into folded stacks for
// flamegraph.pl (https://github.com/brendangregg/FlameGraph).
//
//   prof-fold --elf <kernel.elf> --samples <dump.txt> [--base 0x0] > folded.txt
//   flamegraph.pl folded.txt > flame.svg
//
// The kernel records raw, un-symbolized IPs (with the call stacks already
// unwound in-kernel via .eh_frame); this tool resolves them against the
// kernel ELF symbol table.

#include <prof/format.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstring>
#include <cxxabi.h>

#include <cxxopts.hpp>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

/// Best-effort Itanium demangling; the kernel stores mangled C++ names.
[[nodiscard]]
auto demangle(std::string_view mangled) -> std::string {
    int status = 0;
    char *raw = abi::__cxa_demangle(std::string(mangled).c_str(), nullptr, nullptr, &status);
    if (raw == nullptr || status != 0) {
        return std::string(mangled);
    }
    std::string const result = raw;
    std::free(raw);
    return result;
}

struct Symbol final {
    u64 addr;
    u64 size;
    std::string name;
};

class SymbolTable final {
  public:
    void add(Symbol symbol) {
        m_symbols.push_back(std::move(symbol));
    }

    void sort() {
        std::sort(m_symbols.begin(), m_symbols.end(),
                  [](Symbol const &a, Symbol const &b) { return a.addr < b.addr; });
    }

    [[nodiscard]]
    auto lookup(u64 addr) const -> std::string {
        // Rightmost symbol at or below addr; inside its extent wins.
        auto it = std::upper_bound(m_symbols.begin(), m_symbols.end(), addr,
                                   [](u64 value, Symbol const &s) { return value < s.addr; });
        if (it == m_symbols.begin()) {
            return "";
        }
        --it;
        if (it->size != 0 && addr >= it->addr + it->size) {
            return "";
        }
        if (it->name.empty()) {
            return "";
        }
        return it->name;
    }

  private:
    std::vector<Symbol> m_symbols;
};

// Minimal ELF64 reader: everything prof-fold needs is the .symtab.
class ElfFile final {
  public:
    explicit ElfFile(std::vector<u8> bytes) : m_bytes(std::move(bytes)) {}

    [[nodiscard]]
    auto valid() const -> bool {
        return m_bytes.size() >= sizeof(ElfFileHeader) && std::memcmp(m_bytes.data(),
                                                                      "\x7f"
                                                                      "ELF",
                                                                      4) == 0;
    }

    auto read_symbols(SymbolTable &out) const -> bool {
        auto const *ehdr = reinterpret_cast<ElfFileHeader const *>(m_bytes.data());
        if (ehdr->e_shoff == 0 || ehdr->e_shentsize < sizeof(ElfSectionHeader)) {
            return false;
        }
        auto const *shdrs =
            reinterpret_cast<ElfSectionHeader const *>(m_bytes.data() + ehdr->e_shoff);
        if (ehdr->e_shoff + static_cast<u64>(ehdr->e_shnum) * sizeof(ElfSectionHeader) >
            m_bytes.size()) {
            return false;
        }

        for (usize i = 0; i < ehdr->e_shnum; ++i) {
            auto const &section = shdrs[i];
            if (section.sh_type != SHT_SYMTAB) {
                continue;
            }
            if (section.sh_link >= ehdr->e_shnum) {
                continue;
            }
            auto const &strtab = shdrs[section.sh_link];
            auto const *syms =
                reinterpret_cast<ElfSymbol const *>(m_bytes.data() + section.sh_offset);
            auto const count = section.sh_size / sizeof(ElfSymbol);
            for (usize j = 0; j < count; ++j) {
                auto const &sym = syms[j];
                auto const type = sym.st_info & 0xF;
                if (type != STT_FUNC || sym.st_value == 0) {
                    continue;
                }
                Symbol symbol;
                symbol.addr = sym.st_value;
                symbol.size = sym.st_size;
                symbol.name = demangle(string_at(strtab, sym.st_name));
                out.add(std::move(symbol));
            }
        }
        return true;
    }

  private:
    struct ElfFileHeader final {
        u8 e_ident[16];
        u16 e_type;
        u16 e_machine;
        u32 e_version;
        u64 e_entry;
        u64 e_phoff;
        u64 e_shoff;
        u32 e_flags;
        u16 e_ehsize;
        u16 e_phentsize;
        u16 e_phnum;
        u16 e_shentsize;
        u16 e_shnum;
        u16 e_shstrndx;
    };

    struct ElfSectionHeader final {
        u32 sh_name;
        u32 sh_type;
        u64 sh_flags;
        u64 sh_addr;
        u64 sh_offset;
        u64 sh_size;
        u32 sh_link;
        u32 sh_info;
        u64 sh_addralign;
        u64 sh_entsize;
    };

    struct ElfSymbol final {
        u32 st_name;
        u8 st_info;
        u8 st_other;
        u16 st_shndx;
        u64 st_value;
        u64 st_size;
    };

    static constexpr u32 SHT_SYMTAB = 2;
    static constexpr u8 STT_FUNC = 2;

    [[nodiscard]] [[nodiscard]]
    auto string_at(ElfSectionHeader const &strtab, u32 offset) const -> std::string {
        if (offset >= strtab.sh_size) {
            return "";
        }
        u64 const start = strtab.sh_offset + offset;
        u64 const end = strtab.sh_offset + strtab.sh_size;
        if (start >= m_bytes.size()) {
            return "";
        }
        usize length = 0;
        while (start + length < end && start + length < m_bytes.size() &&
               m_bytes[start + length] != 0) {
            ++length;
        }
        return {reinterpret_cast<char const *>(m_bytes.data() + start), length};
    }

    std::vector<u8> m_bytes;
};

[[nodiscard]]
auto read_file(char const *path) -> std::optional<std::vector<u8>> {
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    u8 buffer[65536];
    usize n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + n);
    }
    std::fclose(file);
    return bytes;
}

[[nodiscard]]
auto hex_nibble(char c) -> int {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

[[nodiscard]]
auto hex_decode(std::string_view text, u8 *out, usize capacity) -> usize {
    if (text.size() % 2 != 0 || text.size() / 2 > capacity) {
        return 0;
    }
    for (usize i = 0; i < text.size(); i += 2) {
        int const hi = hex_nibble(text[i]);
        int const lo = hex_nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i / 2] = static_cast<u8>((hi << 4) | lo);
    }
    return text.size() / 2;
}

struct Dump final {
    prof::DumpHeader header;
    std::vector<prof::SampleRecord> records;
};

[[nodiscard]]
auto parse_dump(std::vector<u8> const &bytes) -> std::optional<Dump> {
    Dump dump;
    bool have_header = false;

    usize line_begin = 0;
    while (line_begin <= bytes.size()) {
        usize line_end = line_begin;
        while (line_end < bytes.size() && bytes[line_end] != '\n' && bytes[line_end] != '\r') {
            ++line_end;
        }

        auto const line = reinterpret_cast<char const *>(bytes.data()) + line_begin;
        std::string_view text(line, line_end - line_begin);
        if (text.starts_with("PROF1 ")) {
            if (hex_decode(text.substr(6), reinterpret_cast<u8 *>(&dump.header),
                           sizeof(dump.header)) != sizeof(dump.header) ||
                dump.header.magic != prof::DUMP_MAGIC) {
                return std::nullopt;
            }
            have_header = true;
        } else if (text.starts_with("SAMP ")) {
            prof::SampleRecord record = {};
            auto const n =
                hex_decode(text.substr(5), reinterpret_cast<u8 *>(&record), sizeof(record));
            if (n == sizeof(record) && record.magic == prof::SAMPLE_MAGIC &&
                record.depth <= prof::MAX_STACK_DEPTH) {
                dump.records.push_back(record);
            }
        }
        // Anything else: console chatter between/around lines — skip it.

        if (line_end >= bytes.size()) {
            break;
        }
        line_begin = line_end + 1;
        while (line_begin < bytes.size() &&
               (bytes[line_begin] == '\r' || bytes[line_begin] == '\n')) {
            ++line_begin;
        }
    }

    if (!have_header) {
        return std::nullopt;
    }
    return dump;
}

auto fold(Dump const &dump, SymbolTable const &symbols, u64 extra_base) -> void {
    std::map<std::string, u64> folded;

    for (auto const &record: dump.records) {
        std::string key = "tid " + std::to_string(record.tid);
        if (record.depth == 0) {
            key += ";<empty>";
        }
        for (usize i = 0; i < record.depth; ++i) {
            key += ";";
            u64 const ip = record.ips[i] - dump.header.load_base - extra_base;
            auto name = symbols.lookup(ip);
            if (name.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(ip));
                name = buf;
            }
            if (i == 0 && (record.flags & prof::RecordFlags::RecordFromUspace) !=
                              prof::RecordFlags::RecordNone) {
                name += " [user]";
            }
            key += name;
        }
        ++folded[key];
    }

    std::vector<std::pair<std::string, u64>> rows(folded.begin(), folded.end());
    std::sort(rows.begin(), rows.end(),
              [](auto const &a, auto const &b) { return a.second > b.second; });
    for (auto const &row: rows) {
        std::printf("%s %llu\n", row.first.c_str(), static_cast<unsigned long long>(row.second));
    }
}

} // namespace

auto main(int argc, char **argv) -> int {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    cxxopts::Options options("prof-fold", "Symbolize prof.dump samples into folded stacks");
    options.add_options()("e,elf", "kernel ELF image (with symbol table)",
                          cxxopts::value<std::string>())(
        "s,samples", "prof.dump serial capture (PROF1/SAMP hex lines)",
        cxxopts::value<std::string>())("b,base", "extra base to subtract from IPs",
                                       cxxopts::value<std::string>()->default_value("0"));
    options.parse_positional({"elf", "samples"});
    options.positional_help("<kernel.elf> <dump.txt>");

    try {
        auto const result = options.parse(argc, argv);

        auto elf_bytes = read_file(result["elf"].as<std::string>().c_str());
        if (!elf_bytes) {
            std::fprintf(stderr, "prof-fold: cannot read %s\n",
                         result["elf"].as<std::string>().c_str());
            return 1;
        }
        ElfFile const elf(std::move(*elf_bytes));
        if (!elf.valid()) {
            std::fprintf(stderr, "prof-fold: not an ELF image\n");
            return 1;
        }

        SymbolTable symbols;
        if (!elf.read_symbols(symbols)) {
            std::fprintf(stderr, "prof-fold: malformed ELF (section headers)\n");
            return 1;
        }
        symbols.sort();

        auto dump_bytes = read_file(result["samples"].as<std::string>().c_str());
        if (!dump_bytes) {
            std::fprintf(stderr, "prof-fold: cannot read %s\n",
                         result["samples"].as<std::string>().c_str());
            return 1;
        }
        auto dump = parse_dump(*dump_bytes);
        if (!dump) {
            std::fprintf(stderr, "prof-fold: no PROF1 header found in %s\n",
                         result["samples"].as<std::string>().c_str());
            return 1;
        }

        u64 const extra_base = std::stoull(result["base"].as<std::string>(), nullptr, 0);

        std::fprintf(stderr, "prof-fold: %u samples (%u dropped), load base 0x%llx\n",
                     static_cast<unsigned>(dump->header.num_records),
                     static_cast<unsigned>(dump->header.num_dropped),
                     static_cast<unsigned long long>(dump->header.load_base));
        fold(*dump, symbols, extra_base);
        return 0;
    } catch (std::exception const &e) {
        std::fprintf(stderr, "prof-fold: %s\n", e.what());
        return 1;
    }
}
