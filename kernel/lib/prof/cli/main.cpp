/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// prof-fold: turn a sample dump captured from the console into folded
// stacks for flamegraph.pl (https://github.com/brendangregg/FlameGraph).
//
// A dump is one `PROF1 <hex>` line for the header and one line per record:
// `SAMP <hex>`, as the kernel shell's former prof.dump wrote them, or
// `SAMPB <base64>`, as a program writes them through debug_write, whose
// 256-byte limit a record's 304 hex digits would not fit (examples/profile).
//
//   prof-fold --elf <kernel.elf> --samples <dump.txt> [--base 0x0]
//             [--debug-json out/<dir>/<image>.debug.json] > folded.txt
//   flamegraph.pl folded.txt > flame.svg
//
// The kernel records raw, un-symbolized IPs (kernel stacks unwound in the
// kernel via .eh_frame, user stacks walked by frame pointers); this tool
// resolves the kernel ones against the kernel ELF symbol table. A user one
// is resolved when the program that took the samples said where its code
// lies -- a `PROFP <koid>` line, then its modules and segments in LLVM's
// symbolizer markup ({{{module:...}}}, {{{mmap:...}}}) -- and --debug-json
// names an unstripped ELF with that module's build ID: the image's
// <image>.debug.json, which `gn gen` writes beside it. Any other is printed
// as the address it is.

#include <prof/format.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstring>
#include <cxxabi.h>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

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

[[nodiscard]]
auto base64_value(char c) -> int {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

/// Decodes standard, padded base64 into |out|; the bytes written, or 0 for
/// text that is not base64 or does not fit.
[[nodiscard]]
auto base64_decode(std::string_view text, u8 *out, usize capacity) -> usize {
    if (text.size() % 4 != 0) {
        return 0;
    }
    usize written = 0;
    for (usize i = 0; i < text.size(); i += 4) {
        u32 group = 0;
        usize bytes = 3;
        for (usize j = 0; j < 4; ++j) {
            auto const c = text[i + j];
            // Padding only at the end of the last group.
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                bytes = std::min(bytes, j - 1);
                group <<= 6;
                continue;
            }
            auto const value = base64_value(c);
            if (value < 0) {
                return 0;
            }
            group = (group << 6) | static_cast<u32>(value);
        }
        if (written + bytes > capacity) {
            return 0;
        }
        for (usize k = 0; k < bytes; ++k) {
            out[written++] = static_cast<u8>(group >> (16 - 8 * k));
        }
    }
    return written;
}

/// The sizes of a version 1 dump: no pid in a record, so its ips start at
/// 24, and no num_unmatched in the header.
constexpr auto V1_HEADER_SIZE = usize{32};
constexpr auto V1_RECORD_SIZE = usize{152};
constexpr auto V1_IPS_OFFSET = usize{24};

/// A record of the dump's |version|, from the |n| bytes decoded into |bytes|,
/// as today's layout; empty when it is not one record's worth.
auto decode_record(std::span<u8 const> const bytes, usize const n, u16 const version)
    -> std::optional<prof::SampleRecord> {
    prof::SampleRecord record = {};
    if (version == 1) {
        if (n != V1_RECORD_SIZE) {
            return std::nullopt;
        }
        std::memcpy(&record, bytes.data(), V1_IPS_OFFSET);
        std::memcpy(record.ips, bytes.data() + V1_IPS_OFFSET, V1_RECORD_SIZE - V1_IPS_OFFSET);
        return record;
    }
    if (n != sizeof(record)) {
        return std::nullopt;
    }
    std::memcpy(&record, bytes.data(), sizeof(record));
    return record;
}

/// One {{{mmap:...}}} of a process: |size| bytes from |start| in its memory
/// hold its module |module| from module-relative address |relative| on.
struct Mapping final {
    u64 start;
    u64 size;
    u32 module;
    u64 relative;
};

/// What a process said about its code, after its `PROFP <koid>` line: its
/// modules' build IDs by markup module ID, and where each was loaded.
struct Process final {
    std::map<u32, std::string> build_ids;
    std::vector<Mapping> mappings;
};

struct Dump final {
    prof::DumpHeader header;
    std::vector<prof::SampleRecord> records;
    /// By koid, which is the pid the process's user records carry.
    std::map<u64, Process> processes;
};

/// The fields of the markup element |text| -- `{{{name:field:...}}}`, the
/// name first -- or none when it is not one.
auto markup_fields(std::string_view text) -> std::optional<std::vector<std::string_view>> {
    if (!text.starts_with("{{{") || !text.ends_with("}}}")) {
        return std::nullopt;
    }
    text = text.substr(3, text.size() - 6);
    std::vector<std::string_view> fields;
    for (usize colon = 0; (colon = text.find(':')) != std::string_view::npos;) {
        fields.push_back(text.substr(0, colon));
        text.remove_prefix(colon + 1);
    }
    fields.push_back(text);
    return fields;
}

/// |text| as a number, in decimal or with 0x in hexadecimal, as the markup
/// writes them; none when it is not one.
auto markup_number(std::string_view const text) -> std::optional<u64> {
    try {
        usize used = 0;
        auto const value = std::stoull(std::string(text), &used, 0);
        if (used != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (std::exception const &) {
        return std::nullopt;
    }
}

/// Takes in a markup line for the process the last `PROFP` named, when
/// there was one: a reset forgets what it said, a module gives a build ID,
/// an mmap a loaded segment. Other elements, and malformed ones, are
/// skipped, as console chatter is.
auto take_markup(std::vector<std::string_view> const &fields, Process &process) -> void {
    if (fields[0] == "reset") {
        process = {};
        return;
    }
    // {{{module:<id>:<name>:elf:<build id>}}}
    if (fields[0] == "module" && fields.size() == 5 && fields[3] == "elf") {
        auto const id = markup_number(fields[1]);
        if (id) {
            process.build_ids[static_cast<u32>(*id)] = std::string(fields[4]);
        }
        return;
    }
    // {{{mmap:<start>:<size>:load:<module id>:<flags>:<relative>}}}
    if (fields[0] == "mmap" && fields.size() == 7 && fields[3] == "load") {
        auto const start = markup_number(fields[1]);
        auto const size = markup_number(fields[2]);
        auto const module = markup_number(fields[4]);
        auto const relative = markup_number(fields[6]);
        if (start && size && module && relative) {
            process.mappings.push_back(Mapping{
                .start = *start,
                .size = *size,
                .module = static_cast<u32>(*module),
                .relative = *relative,
            });
        }
    }
}

[[nodiscard]]
auto parse_dump(std::vector<u8> const &bytes) -> std::optional<Dump> {
    Dump dump;
    bool have_header = false;
    // The process the markup lines are about: the last `PROFP <koid>`.
    Process *describing = nullptr;

    usize line_begin = 0;
    while (line_begin <= bytes.size()) {
        usize line_end = line_begin;
        while (line_end < bytes.size() && bytes[line_end] != '\n' && bytes[line_end] != '\r') {
            ++line_end;
        }

        auto const line = reinterpret_cast<char const *>(bytes.data()) + line_begin;
        std::string_view text(line, line_end - line_begin);
        if (text.starts_with("PROF1 ")) {
            // A version 1 header is the first 32 bytes of today's; what it
            // lacks stays zero.
            dump.header = {};
            auto const n = hex_decode(text.substr(6), reinterpret_cast<u8 *>(&dump.header),
                                      sizeof(dump.header));
            if (n < V1_HEADER_SIZE || dump.header.magic != prof::DUMP_MAGIC ||
                n != (dump.header.version == 1 ? V1_HEADER_SIZE : sizeof(dump.header))) {
                return std::nullopt;
            }
            have_header = true;
        } else if (have_header && (text.starts_with("SAMP ") || text.starts_with("SAMPB "))) {
            std::array<u8, sizeof(prof::SampleRecord)> bytes{};
            auto const n = text.starts_with("SAMPB ")
                               ? base64_decode(text.substr(6), bytes.data(), bytes.size())
                               : hex_decode(text.substr(5), bytes.data(), bytes.size());
            auto const record = decode_record(bytes, n, dump.header.version);
            if (record && record->magic == prof::SAMPLE_MAGIC &&
                record->depth <= prof::MAX_STACK_DEPTH) {
                dump.records.push_back(*record);
            }
        } else if (text.starts_with("PROFP ")) {
            auto const koid = markup_number(text.substr(6));
            describing = koid ? &dump.processes[*koid] : nullptr;
        } else if (auto const fields = markup_fields(text); fields && describing != nullptr) {
            take_markup(*fields, *describing);
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

/// The unstripped ELFs an image's <image>.debug.json lists, by build ID, and
/// the symbols of each, read the first time a frame needs them.
class UserModules final {
  public:
    /// Reads |path|, the list `gn gen` writes beside an image: one entry per
    /// binary in it, with "debug", the unstripped ELF, and "elf_build_id", a
    /// file holding its build ID as hex, both relative to the build
    /// directory the list lies in. Entries without both are skipped: the
    /// image's own entry, and binaries the build gives no build ID.
    auto load(std::filesystem::path const &path) -> bool {
        std::ifstream file(path);
        if (!file) {
            return false;
        }
        auto const entries = nlohmann::json::parse(file, nullptr, false);
        if (!entries.is_array()) {
            return false;
        }
        auto const directory = path.parent_path();
        for (auto const &entry: entries) {
            if (!entry.contains("debug") || !entry.contains("elf_build_id")) {
                continue;
            }
            std::ifstream id_file(directory / entry["elf_build_id"].get<std::string>());
            std::string id;
            if (id_file >> id) {
                m_paths[id] = directory / entry["debug"].get<std::string>();
            }
        }
        return true;
    }

    /// How many ELFs the lists named.
    [[nodiscard]]
    auto size() const -> usize {
        return m_paths.size();
    }

    /// The function at module-relative address |address| in the module with
    /// build ID |id|; empty when no list named that module, its ELF cannot
    /// be read, or no function holds the address.
    auto lookup(std::string const &id, u64 const address) -> std::string {
        auto found = m_tables.find(id);
        if (found == m_tables.end()) {
            found = m_tables.emplace(id, read(id)).first;
        }
        return found->second ? found->second->lookup(address) : std::string();
    }

  private:
    [[nodiscard]]
    auto read(std::string const &id) const -> std::optional<SymbolTable> {
        auto const path = m_paths.find(id);
        if (path == m_paths.end()) {
            return std::nullopt;
        }
        auto bytes = read_file(path->second.string().c_str());
        if (!bytes) {
            return std::nullopt;
        }
        ElfFile const elf(std::move(*bytes));
        SymbolTable table;
        if (!elf.valid() || !elf.read_symbols(table)) {
            return std::nullopt;
        }
        table.sort();
        return table;
    }

    std::map<std::string, std::filesystem::path> m_paths;
    std::map<std::string, std::optional<SymbolTable>> m_tables;
};

/// The function |address| lies in, in the user memory of the process with
/// koid |pid|: found through the mmap that holds it, whose module-relative
/// address plus the offset into it is an address in the module's ELF.
/// Empty when the process said nothing of that address, or its module
/// cannot be resolved.
auto resolve_user(Dump const &dump, UserModules &modules, u64 const pid, u64 const address)
    -> std::string {
    auto const process = dump.processes.find(pid);
    if (process == dump.processes.end()) {
        return "";
    }
    for (auto const &mapping: process->second.mappings) {
        if (address - mapping.start >= mapping.size) {
            continue;
        }
        auto const id = process->second.build_ids.find(mapping.module);
        if (id == process->second.build_ids.end()) {
            return "";
        }
        return modules.lookup(id->second, address - mapping.start + mapping.relative);
    }
    return "";
}

auto fold(Dump const &dump, SymbolTable const &symbols, UserModules &modules, u64 extra_base)
    -> void {
    std::map<std::string, u64> folded;

    for (auto const &record: dump.records) {
        // The process first, so that a flame graph groups each program's
        // threads; a version 1 dump has no pid, and a kernel thread's is 0.
        std::string key = dump.header.version >= 2 ? "pid " + std::to_string(record.pid) + ";" : "";
        key += "tid " + std::to_string(record.tid);
        if (record.depth == 0) {
            key += ";<empty>";
        }
        // Outermost frame first: flamegraph.pl reads a line from the root of
        // the stack to its leaf, while a record lists its pc first. Written
        // leaf first, every graph came out upside down, each sampled
        // function at the bottom with its callers stacked on top.
        bool const from_user =
            (record.flags & prof::RecordFlags::FromUser) != prof::RecordFlags::None;
        for (usize i = record.depth; i-- > 0;) {
            key += ";";
            // Every frame but the first is a return address, just past its
            // call; the call, which is what names the caller, is a byte back.
            // A call that ends a function would otherwise name the next one.
            auto const call_back = static_cast<u64>(i != 0);
            // A user frame is resolved against its own program's ELF, through
            // what the process said of its code, or printed as the address it
            // is. Never against the kernel's: the load base is the kernel
            // image's, and the kernel's symbols name nothing in user space.
            if (from_user) {
                auto name = resolve_user(dump, modules, record.pid, record.ips[i] - call_back);
                if (name.empty()) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "0x%llx",
                                  static_cast<unsigned long long>(record.ips[i]));
                    name = buf;
                }
                key += name;
                if (i == 0) {
                    key += " [user]";
                }
                continue;
            }
            u64 const ip = record.ips[i] - dump.header.load_base - extra_base;
            auto name = symbols.lookup(ip - call_back);
            if (name.empty()) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(ip));
                name = buf;
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

    cxxopts::Options options("prof-fold", "Symbolize sampler dumps into folded stacks");
    options.add_options()("e,elf", "kernel ELF image (with symbol table)",
                          cxxopts::value<std::string>())(
        "s,samples", "console capture of a dump (PROF1 and SAMP or SAMPB lines)",
        cxxopts::value<std::string>())("b,base", "extra base to subtract from IPs",
                                       cxxopts::value<std::string>()->default_value("0"))(
        "d,debug-json",
        "the image's <image>.debug.json, naming the unstripped ELFs user frames resolve against",
        cxxopts::value<std::string>());
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

        UserModules modules;
        if (result.count("debug-json") != 0) {
            auto const path = result["debug-json"].as<std::string>();
            if (!modules.load(path)) {
                std::fprintf(stderr, "prof-fold: cannot read %s as a debug list\n", path.c_str());
                return 1;
            }
        }

        std::fprintf(stderr,
                     "prof-fold: %u samples (%u dropped, %llu ticks unmatched), load base 0x%llx\n",
                     static_cast<unsigned>(dump->header.num_records),
                     static_cast<unsigned>(dump->header.num_dropped),
                     static_cast<unsigned long long>(dump->header.num_unmatched),
                     static_cast<unsigned long long>(dump->header.load_base));
        std::fprintf(stderr, "prof-fold: %zu processes described their code, %zu ELFs listed\n",
                     dump->processes.size(), modules.size());
        fold(*dump, symbols, modules, extra_base);
        return 0;
    } catch (std::exception const &e) {
        std::fprintf(stderr, "prof-fold: %s\n", e.what());
        return 1;
    }
}
