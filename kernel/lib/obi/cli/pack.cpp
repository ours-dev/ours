/// obit pack — read an .obi.rsp response file and produce an OBI container.
///
/// The .rsp contains one argument per line.  obit processes them
/// left-to-right with a state machine: --reset clears the block state
/// (prefix, files-type, compression), subsequent flags set it, and
/// --files <fini> / --type=container <path> / --type=executable <path>
/// emit content under the current state.
///
/// --files blocks accumulate per files_type for the whole .rsp and are
/// flushed once at the end, so blocks of the same type merge into a single
/// item regardless of the --reset boundaries GN emits between obi_input()
/// blocks.  The kernel storage is flushed last so the compressed package
/// trails the direct items; PackageBuilder::build() then stable-sorts the
/// Executable item to index 0 (see package-builder.hpp).

#include <obi/package_builder.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <map>
#include <print>
#include <sstream>

#include "command.hpp"
#include "common.hpp"
#include "lz4_block.hpp"
#include <bootfs/builder.hpp>
#include <cxxopts.hpp>

namespace obit {
namespace {

    // Every ItemSource/bootfs::File holds a non-owning Span, so any buffer
    // fed into the builder must outlive PackageBuilder::build().  A deque
    // never moves or frees an existing element on insertion, so pointers
    // handed out via arena element .data() stay valid for the arena's
    // lifetime — i.e. for the rest of pack_from_rsp().
    using Arena = std::deque<std::vector<u8>>;

    struct PackState {
        std::string prefix;
        std::string files_type = "bootfs";
        bool compressed_flag = false;
    };

    // Merge all --files blocks (grouped by files_type) into BootFS blobs
    // (obit is where OBI packing and the BootFS format get composed — the
    // obi/ and bootfs/ libraries stay independent of each other), then
    // flush them as STORAGE_KERNEL / STORAGE_BOOTFS items.  Prefixes are
    // applied to file paths inside the BootFS.  Compression is decided per
    // merged item: a --compressed flag in any contributing block compresses
    // the whole blob, so blocks of one files_type must agree in practice
    // (GN's kernel_package() always emits --compressed=lz4).
    struct BootFsAccumulator {
        bootfs::Builder builder;
        bool compressed = false;
    };

    auto add_fini_to_bootfs(BootFsAccumulator &acc, std::string const &fini_path,
                            std::string const &prefix, Arena &arena) -> int {
        auto fini_data = read_file(fini_path);
        if (!fini_data) {
            std::println(std::cerr, "obit: cannot read fini: {}", fini_path);
            return 2;
        }
        auto text =
            std::string(reinterpret_cast<char const *>(fini_data->data()), fini_data->size());
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            auto dest = line.substr(0, eq);
            auto src = line.substr(eq + 1);

            auto file_data = read_file(src);
            if (!file_data) {
                std::println(std::cerr, "obit: cannot read: {}", src);
                return 2;
            }
            arena.push_back(std::move(*file_data));
            auto const &owned = arena.back();
            // Apply prefix to the path inside the BootFS.
            std::string path = prefix.empty() ? dest : prefix + "/" + dest;
            acc.builder.add_file(path, {owned.data(), owned.size()});
        }
        return 0;
    }

    auto flush_bootfs(obi::PackageBuilder &builder, BootFsAccumulator &acc,
                      std::string const &files_type, Arena &arena) -> int {
        if (acc.builder.file_count() == 0) {
            return 0;
        }
        auto blob = acc.builder.build();
        if (blob.empty()) {
            return 0;
        }

        obi::ItemType otype =
            (files_type == "bootfs") ? obi::ItemType::StorageBootFs : obi::ItemType::StorageKernel;

        obi::ItemSource s;
        s.name = (files_type == "bootfs") ? "bootfs" : "kernel";
        s.type = otype;
        s.format = obi::ItemFormat::BootFs;
        s.memory_size = blob.size();
        s.flags = obi::ItemFlags::Required;

        if (acc.compressed) {
            // Upstream LZ4, checked against the boot chain's decoder before
            // it goes in (lz4_compress_block()).
            auto compressed = lz4_compress_block({blob.data(), blob.size()});
            if (compressed.is_err()) {
                std::println(std::cerr, "obit: cannot compress item '{}': status {}", s.name,
                             static_cast<int>(compressed.unwrap_err()));
                return 1;
            }
            arena.push_back(std::move(compressed.unwrap()));
            auto const &owned = arena.back();
            s.data = {owned.data(), owned.size()};
            s.compression = obi::Compression::Lz4;
        } else {
            arena.push_back(std::move(blob));
            auto const &owned = arena.back();
            s.data = {owned.data(), owned.size()};
        }

        if (auto result = builder.add_item(s); result.is_err()) {
            std::println(std::cerr, "obit: cannot add item '{}': status {}", s.name,
                         static_cast<int>(result.unwrap_err()));
            return 1;
        }
        return 0;
    }

    // Embed a pre-built OBI container by copying its items.
    auto ingest_container(obi::PackageBuilder &builder, std::string const &path, Arena &arena)
        -> int {
        auto data = read_file(path);
        if (!data) {
            std::println(std::cerr, "obit: cannot read container: {}", path);
            return 2;
        }
        arena.push_back(std::move(*data));
        auto const &owned = arena.back();
        auto container = obi::ContainerView::from(owned.data(), owned.size());
        if (container.is_err()) {
            // Not yet an OBI container — embed as a raw executable item
            // (the physload case: raw binary, not yet wrapped).
            auto fname = fs::path(path).filename().string();
            // Strip extension for the item name.
            auto dot = fname.find('.');
            auto name = (dot != std::string::npos) ? fname.substr(0, dot) : fname;

            obi::ItemSource s;
            s.name = name;
            s.type = obi::ItemType::Executable;
            s.format = obi::ItemFormat::Raw;
            s.data = {owned.data(), owned.size()};
            s.memory_size = owned.size();
            s.flags = obi::ItemFlags::Required;
            if (auto result = builder.add_item(s); result.is_err()) {
                std::println(std::cerr, "obit: cannot add item '{}': status {}", s.name,
                             static_cast<int>(result.unwrap_err()));
                return 1;
            }
            return 0;
        }
        // Pre-built OBI container: copy each item.
        auto const &c = container.unwrap();
        for (usize i = 0; i < c.item_count(); ++i) {
            auto item = c.item(i);
            if (item.is_err()) {
                return 1;
            }
            auto const &v = item.unwrap();
            obi::ItemSource s;
            s.name = std::string(v.name());
            s.type = v.type();
            s.format = v.format();
            s.data = v.payload();
            s.memory_size = v.memory_size();
            s.alignment = v.alignment();
            s.compression = v.compression();
            s.flags = v.flags();
            if (auto result = builder.add_item(s); result.is_err()) {
                std::println(std::cerr, "obit: cannot add item '{}': status {}", s.name,
                             static_cast<int>(result.unwrap_err()));
                return 1;
            }
        }
        return 0;
    }

    // Embed a stripped ELF as the container's Executable item — item 0 by
    // PackageBuilder's sort order, i.e. the object the boot shim loads
    // first.  The item name is the file basename without its first
    // extension, matching ingest_container()'s raw-image convention.
    auto add_executable_item(obi::PackageBuilder &builder, std::string const &path, Arena &arena)
        -> int {
        // NOLINTNEXTLINE(readability-identifier-naming) — CAMEL_CASE is the obi/ constant convention (MAGIC et al.)
        static constexpr std::array<u8, 4> ELF_MAGIC{0x7F, 'E', 'L', 'F'};

        auto data = read_file(path);
        if (!data) {
            std::println(std::cerr, "obit: cannot read executable: {}", path);
            return 2;
        }
        if (data->size() < ELF_MAGIC.size() ||
            !std::equal(ELF_MAGIC.begin(), ELF_MAGIC.end(), data->begin())) {
            std::println(std::cerr, "obit: not an ELF file: {}", path);
            return 2;
        }
        auto const fname = fs::path(path).filename().string();
        auto const dot = fname.find('.');
        auto name = fname;
        if (dot != std::string::npos) {
            name = fname.substr(0, dot);
        }

        arena.push_back(std::move(*data));
        auto const &owned = arena.back();
        obi::ItemSource s;
        s.name = name;
        s.type = obi::ItemType::Executable;
        s.format = obi::ItemFormat::Elf;
        s.data = {owned.data(), owned.size()};
        s.memory_size = owned.size();
        s.flags = obi::ItemFlags::Required;
        if (auto result = builder.add_item(s); result.is_err()) {
            std::println(std::cerr, "obit: cannot add item '{}': status {}", s.name,
                         static_cast<int>(result.unwrap_err()));
            return 1;
        }
        return 0;
    }

    // Enforce the boot topology contract when --bootable is given: item 0
    // must be the Executable entry point and the container must carry
    // exactly one StorageKernel package.  phys::BootObi::from() (built on
    // top of this container format, not part of it) rejects anything else
    // at boot time; this turns that into a build-time error.
    auto validate_bootable(std::vector<u8> const &container) -> int {
        auto view = obi::ContainerView::from(container.data(), container.size());
        if (view.is_err()) {
            std::println(std::cerr, "obit: --bootable: built container is invalid: {}",
                         static_cast<u32>(view.unwrap_err()));
            return 1;
        }
        auto const &items = view.unwrap();

        auto const first = items.item(0);
        if (first.is_err() || first.unwrap().type() != obi::ItemType::Executable) {
            std::println(std::cerr, "obit: --bootable: item 0 must be the Executable entry point");
            return 1;
        }

        usize kernel_packages = 0;
        for (usize index = 0; index < items.item_count(); ++index) {
            auto const item = items.item(index);
            if (item.is_err()) {
                std::println(std::cerr, "obit: --bootable: item {} is invalid", index);
                return 1;
            }
            if (item.unwrap().type() == obi::ItemType::StorageKernel) {
                kernel_packages += 1;
            }
        }
        if (kernel_packages != 1) {
            std::println(std::cerr,
                         "obit: --bootable: expected exactly one StorageKernel item, found {}",
                         kernel_packages);
            return 1;
        }
        return 0;
    }

    auto pack_from_rsp(std::string const &rsp_path, std::string const &output_path,
                       [[maybe_unused]] std::string const &compressed_global,
                       std::string const &bootable) -> int {
        // TODO: compressed_global (container-level compression, e.g. the
        // whole .obi post-processed with zstd) is accepted for CLI/GN
        // compatibility but not yet implemented.
        std::ifstream rsp_file(rsp_path);
        if (!rsp_file) {
            std::println(std::cerr, "obit: cannot read rsp: {}", rsp_path);
            return 2;
        }
        std::vector<std::string> args;
        std::string line;
        while (std::getline(rsp_file, line)) {
            if (!line.empty() && line[0] != '#') {
                args.push_back(line);
            }
        }

        obi::PackageBuilder builder;
        PackState state;
        std::map<std::string, BootFsAccumulator> accumulators;
        Arena arena;
        int rc = 0;

        for (std::size_t i = 0; i < args.size(); ++i) {
            auto const &arg = args[i];

            // --reset closes one obi_input() block: only the block state is
            // dropped.  The per-files_type accumulators deliberately survive
            // so same-type blocks merge into a single item (see the file
            // header comment).
            if (arg == "--reset") {
                state = PackState{};
            } else if (arg.starts_with("--prefix=")) {
                state.prefix = arg.substr(9);
            } else if (arg.starts_with("--files-type=")) {
                state.files_type = arg.substr(13);
            } else if (arg == "--compressed" || arg.starts_with("--compressed=")) {
                // Only lz4 is implemented for BootFS item compression today
                // (see flush_bootfs); any algorithm name just turns it on.
                state.compressed_flag = true;
                if (!state.files_type.empty()) {
                    accumulators[state.files_type].compressed = true;
                }
            } else if (arg == "--files") {
                if (i + 1 < args.size()) {
                    rc = add_fini_to_bootfs(accumulators[state.files_type], args[++i], state.prefix,
                                            arena);
                    if (rc != 0) {
                        return rc;
                    }
                }
            } else if (arg.starts_with("--files=")) {
                rc = add_fini_to_bootfs(accumulators[state.files_type], arg.substr(8), state.prefix,
                                        arena);
                if (rc != 0) {
                    return rc;
                }
            } else if (arg == "--type=container") {
                if (i + 1 < args.size()) {
                    rc = ingest_container(builder, args[++i], arena);
                    if (rc != 0) {
                        return rc;
                    }
                }
            } else if (arg == "--type=executable") {
                if (i + 1 < args.size()) {
                    rc = add_executable_item(builder, args[++i], arena);
                    if (rc != 0) {
                        return rc;
                    }
                }
            } else if (arg.starts_with("--entry=")) {
                // Collect cmdline entries into a single CONFIG item.
                std::string entry = arg.substr(8);
                thread_local static std::vector<std::string> cmdline_entries;
                thread_local static bool cmdline_seen = false;
                if (!cmdline_seen) {
                    cmdline_entries.clear();
                    cmdline_seen = true;
                }
                cmdline_entries.push_back(entry);

                // Check if next arg is also --entry; if not, flush.
                bool more_entries = (i + 1 < args.size() && args[i + 1].starts_with("--entry="));
                if (!more_entries) {
                    std::string joined;
                    for (std::size_t j = 0; j < cmdline_entries.size(); ++j) {
                        if (j > 0) {
                            joined += " ";
                        }
                        joined += cmdline_entries[j];
                    }
                    arena.push_back(std::vector<u8>(joined.begin(), joined.end()));
                    auto const &entry_bytes = arena.back();
                    obi::ItemSource s;
                    s.name = "cmdline";
                    s.type = obi::ItemType::Config;
                    s.format = obi::ItemFormat::Raw;
                    s.data = {entry_bytes.data(), entry_bytes.size()};
                    s.memory_size = entry_bytes.size();
                    s.flags = obi::ItemFlags::Required;
                    if (auto result = builder.add_item(s); result.is_err()) {
                        std::println(std::cerr, "obit: cannot add item '{}': status {}", s.name,
                                     static_cast<int>(result.unwrap_err()));
                        return 1;
                    }
                    cmdline_seen = false;
                }
            }
        }

        // Flush the accumulated BootFS blobs.  Deferred to the end of the
        // .rsp so same-type blocks merge (see the file header comment); the
        // kernel storage is flushed last so the compressed package trails
        // the direct entry-point and cmdline items.
        for (auto &[ftype, acc]: accumulators) {
            if (ftype == "kernel-storage") {
                continue;
            }
            rc = flush_bootfs(builder, acc, ftype, arena);
            if (rc != 0) {
                return rc;
            }
        }
        if (accumulators.contains("kernel-storage")) {
            rc = flush_bootfs(builder, accumulators["kernel-storage"], "kernel-storage", arena);
            if (rc != 0) {
                return rc;
            }
        }

        auto container = builder.build(/*crc=*/false);
        if (container.empty()) {
            std::println(std::cerr, "obit: no items to pack");
            return 1;
        }

        if (!bootable.empty()) {
            rc = validate_bootable(container);
            if (rc != 0) {
                return rc;
            }
        }

        if (!write_file(output_path, {container.data(), container.size()})) {
            std::println(std::cerr, "obit: cannot write: {}", output_path);
            return 2;
        }
        return 0;
    }

    class PackCommand final: public AutoCommand<PackCommand> {
      public:
        PackCommand()
            : AutoCommand("pack", "create an OBI container from a .rsp file",
                          "@RESPONSE.rsp --output OUT [--compressed=X] [--bootable=ARCH]") {}

        auto execute(std::vector<std::string> const &args) const -> int override {
            cxxopts::Options opts("obit pack", "create an OBI container from a .rsp file");
            auto adder = opts.add_options();
            adder("rsp", "response file (prefixed with @)", cxxopts::value<std::string>());
            adder("output", "output OBI path", cxxopts::value<std::string>());
            adder("compressed", "compression algorithm (e.g. lz4)",
                  cxxopts::value<std::string>()->default_value(""));
            adder("bootable", "bootable CPU architecture",
                  cxxopts::value<std::string>()->default_value(""));
            opts.parse_positional({"rsp"});
            opts.positional_help("@RESPONSE.rsp");

            auto const argv = make_argv("obit pack", args);
            cxxopts::ParseResult parsed;
            try {
                parsed = opts.parse(static_cast<int>(argv.size()), argv.data());
            } catch (std::exception const &ex) {
                std::println(std::cerr, "obit pack: {}", ex.what());
                return 2;
            }

            if (parsed.count("rsp") == 0 || parsed.count("output") == 0) {
                std::println(std::cerr, "obit pack: expected @RESPONSE.rsp --output OUT");
                return 2;
            }

            auto rsp_path = parsed["rsp"].as<std::string>();
            if (rsp_path.starts_with("@")) {
                rsp_path = rsp_path.substr(1);
            }
            return pack_from_rsp(rsp_path, parsed["output"].as<std::string>(),
                                 parsed["compressed"].as<std::string>(),
                                 parsed["bootable"].as<std::string>());
        }
    };

} // namespace
} // namespace obit
