#include <ours/phys/exports.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/kernel_package.hpp>
#include <ours/phys/module_loader.hpp>

#include <span>
#include <string_view>

#include <cstring>

#include "build_id_note.hpp"
#include "detail/boot_alloc.hpp"
#include "physload.hpp"
#include <bootfs/view.hpp>
#include <logz4/log.hpp>
#include <ustl/mem/align.hpp>

namespace ours::phys {
namespace {
    /// The module loaded when `kernel.phys.next` is absent or names a file
    /// the kernel package does not carry.
    constexpr std::string_view DEFAULT_MODULE{"physboot"};

    extern "C" char const __build_id_note_start[];
    extern "C" char const __build_id_note_end[];

    /// physload's own GNU build ID, as lowercase hex with no separators --
    /// the same format kernel_elf_interp.gni bakes into a
    /// physload_module()'s PT_INTERP (kernel/phys/generate-elf-interp.py's
    /// `.lower()`).
    ///
    /// Compared against a loaded module's PT_INTERP before calling into it
    /// (see load() below): the module and physload share raw struct
    /// layouts across their boundary with no ABI versioning of their own,
    /// so the one thing that makes that safe is a guarantee they came from
    /// the very same link -- which this build-id comparison is what
    /// actually enforces, rather than merely documents.
    ///
    /// Reads `.note.gnu.build-id` out of this image's own memory (phys.ld
    /// places it in a segment that survives packaging).  Returns an empty
    /// view if the note is missing or malformed -- the caller decides
    /// whether that is fatal; this function does not panic. The only
    /// caller here is ModuleLoader::load()'s PT_INTERP check.
    [[nodiscard]]
    auto own_build_id() -> std::string_view {
        // Plain zero-initialized storage: constant-initialized (no
        // dynamic-init guard machinery), and safe to refill on every call
        // since physload is single-threaded at every point this can be
        // reached from.
        static char buffer[41]{}; // sha1 = 20 bytes = 40 hex chars + NUL

        auto const notes =
            std::span(reinterpret_cast<u8 const *>(__build_id_note_start),
                      static_cast<usize>(__build_id_note_end - __build_id_note_start));
        auto const length = hex_encode_build_id(notes, buffer);
        return {buffer, length};
    }
} // namespace

auto ModuleLoader::init(BootObi const &boot) -> Status {
    // Locate the single StorageKernel item (BootObi::from() has already
    // asserted there is exactly one).
    auto const item = boot.kernel_package_item();
    if (item.is_err()) {
        return item.unwrap_err();
    }

    // Decompress into bootmem-owned scratch.  Ownership of the buffer
    // moves to the module via PhysLoadHandoff; it is never released.
    auto const scratch =
        detail::allocate_top_down(item.unwrap().memory_size(), PAGE_SIZE,
                                  bootmem::MemoryType::PhysImage, "phys module");
    auto const storage = ustl::views::Span<u8>(reinterpret_cast<u8 *>(scratch.base), scratch.size);
    auto const materialized = boot.kernel_package(storage);
    if (materialized.is_err()) {
        return materialized.unwrap_err();
    }

    m_storage_base = scratch.base;
    m_storage_size = scratch.size;
    return Status::Ok;
}

auto ModuleLoader::load(BootOptions const &options) -> Status {
    auto const storage =
        bootfs::View::Bytes(reinterpret_cast<u8 const *>(m_storage_base), m_storage_size);
    auto const bootfs = bootfs::View::from(storage);
    if (bootfs.is_err()) {
        return bootfs.unwrap_err();
    }
    auto const &files = bootfs.unwrap();

    auto name = options.next_module();
    auto found = files.find(name);
    if (found.is_err()) {
        return found.unwrap_err();
    }
    if (found.unwrap().is_none()) {
        // A miss on the default module is fatal; a miss on an explicitly
        // requested one warns and falls back (the fallback is not silent).
        if (name == DEFAULT_MODULE) {
            panic("phys module '{}' missing from kernel package", name);
        }
        log::info("phys module '{}' not found, falling back to '{}'", name, DEFAULT_MODULE);
        name = DEFAULT_MODULE;
        found = files.find(name);
        if (found.is_err()) {
            return found.unwrap_err();
        }
        if (found.unwrap().is_none()) {
            panic("phys module '{}' missing from kernel package", name);
        }
    }
    auto const module_bytes = files.file_data(found.unwrap().value());

    auto const init_status = m_image.init({module_bytes.data(), module_bytes.size()});
    if (init_status != Status::Ok) {
        return init_status;
    }

    auto const total = ustl::mm::align_up(m_image.size(), PAGE_SIZE);
    auto const placement = detail::allocate_top_down(total, PAGE_SIZE,
                                                     bootmem::MemoryType::PhysImage,
                                                     "phys module");
    auto const target = placement.base;

    if (auto const placed = m_image.load_at(target); placed.is_err()) {
        return placed.unwrap_err();
    }

    // Refuse before doing anything else with this image.  Everything from
    // here on -- symbol binding, the PhysModule descriptor, and every call
    // the module's own logic makes back into physload -- trusts that both
    // images agree on raw struct layouts with no ABI versioning of their
    // own.  That trust is only sound if they came from the exact same
    // build: kernel_elf_interp.gni bakes this physload's build ID into
    // every physload_module()'s PT_INTERP at build time, and
    // own_build_id() above reads this physload's own copy back out of its
    // own image right now, so the two can be compared.
    auto const interp = m_image.section(".interp");
    auto pt_interp = std::string_view(reinterpret_cast<char const *>(interp.data()), interp.size());
    if (!pt_interp.empty() && pt_interp.back() == '\0') {
        pt_interp.remove_suffix(1); // elf-interp.S null-terminates the string.
    }
    auto const expected_build_id = own_build_id();
    if (expected_build_id.empty() || pt_interp.empty() || pt_interp != expected_build_id) {
        log::info("phys module '{}' PT_INTERP '{}' does not match this physload's build ID '{}'",
                  name, pt_interp, expected_build_id);
        return Status::Unsupported;
    }

    // The module links at base 0, so the relocation bias is the placement.
    // Beyond that it arrives with its imports unresolved: physload owns the
    // phys runtime, and the module reaches it through the export table
    // rather than carrying a second copy (ours/phys/exports.hpp).
    SymbolBinder const binder{
        .context = nullptr,
        .resolve = [](void *, std::string_view const name) -> void const * {
            return resolve_export(name);
        },
    };
    if (auto const status = m_image.relocate_and_bind(target, binder); status != Status::Ok) {
        return status;
    }

    // What to run is the module's own declaration, not an address in the
    // ELF header: PHYS_MODULE() put a descriptor in .phys.module and the
    // pass above fixed up the pointers inside it.
    auto const record = m_image.section(PhysModule::SECTION);
    if (record.size() < sizeof(PhysModule)) {
        log::info("phys module '{}' declares no {} descriptor", name, PhysModule::SECTION);
        return Status::NotFound;
    }
    auto const *descriptor = reinterpret_cast<PhysModule const *>(record.data());
    if (descriptor->abi_version != PhysModule::ABI_VERSION) {
        log::info("phys module '{}' speaks module ABI {}, physload speaks {}", name,
                  descriptor->abi_version, PhysModule::ABI_VERSION);
        return Status::Unsupported;
    }
    if (descriptor->init == nullptr) {
        log::info("phys module '{}' declares a null entry", name);
        return Status::InvArgs;
    }

    m_descriptor = descriptor;
    log::debug("Loaded phys module '{}' at 0x{:X}, entry 0x{:X}", descriptor->name, target,
               reinterpret_cast<usize>(descriptor->init));
    return Status::Ok;
}

auto ModuleLoader::start(PhysLoadHandoff const &handoff) -> void {
    if (m_descriptor == nullptr) {
        panic("No phys module loaded");
    }

    // The module's own constructors, over its own image.  physload ran its
    // own in its own image; the objects those built -- the console list, the
    // allocator, the address space -- are the ones the module is bound to,
    // so nothing here re-creates them.
    run_initializers();

    // Architecture-specific post-load setup lives on physload's side now
    // that the module has no startup code of its own.
    arch_on_phys_load_handoff(handoff);

    m_descriptor->init(handoff);

    // The descriptor's contract says init() never returns; it is a plain
    // function pointer, so the compiler cannot know that.
    panic("phys module '{}' returned from its entry", m_descriptor->name);
}

auto ModuleLoader::run_initializers() -> void {
    using Initializer = auto (*)()->void;

    auto const array = m_image.section(".init_array");
    auto const *entries = reinterpret_cast<Initializer const *>(array.data());
    for (auto index = usize{0}; index < array.size() / sizeof(Initializer); ++index) {
        if (entries[index] != nullptr) {
            entries[index]();
        }
    }
}

} // namespace ours::phys
