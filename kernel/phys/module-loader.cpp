#include <ours/phys/module-loader.hpp>

#include <bootfs/view.hpp>
#include <obi/kernel-package.hpp>
#include <ours/phys/build-id.hpp>
#include <ours/phys/exports.hpp>
#include <ours/phys/handoff.hpp>
#include <ours/phys/init.hpp>
#include <ours/phys/print.hpp>
#include <ustl/mem/align.hpp>

#include <cstring>
#include <string_view>

namespace ours::phys {
namespace {
    /// The module loaded when `kernel.phys.next` is absent or names a file
    /// the kernel package does not carry.
    constexpr std::string_view kDefaultModule{"physboot"};

    [[nodiscard]]
    auto bootmem_allocate(usize const size, PhysAddr const alignment) -> bootmem::Range {
        auto const mem = global_bootmem();
        auto const allocation = mem->allocate({
            .size = size,
            .alignment = alignment,
            .lower = mem->min_address(),
            .upper = mem->max_address(),
            .node = bootmem::kAnyNode,
            .direction = bootmem::AllocationDirection::TopDown,
        });
        if (allocation.is_err()) {
            panic("No enough memory to place phys module");
        }
        return allocation.unwrap();
    }
} // namespace

auto ModuleLoader::init(obi::BootObi const &boot) -> Status {
    // Locate the single StorageKernel item (BootObi::from() has already
    // asserted there is exactly one).
    auto const &package = boot.package();
    for (usize index = 0; index < package.item_count(); ++index) {
        auto const item = package.item(index);
        if (item.is_err()) {
            return item.unwrap_err();
        }
        auto const &view = item.unwrap();
        if (view.type() != obi::ItemType::StorageKernel) {
            continue;
        }

        // Decompress into bootmem-owned scratch.  Ownership of the buffer
        // moves to the module via PhysLoadHandoff; it is never released.
        auto const scratch = bootmem_allocate(view.memory_size(), PAGE_SIZE);
        auto const storage = ustl::views::Span<u8>(
            reinterpret_cast<u8 *>(scratch.base), scratch.size);
        auto const materialized = obi::KernelPackage::from(view, storage);
        if (materialized.is_err()) {
            return materialized.unwrap_err();
        }

        m_storage_base = scratch.base;
        m_storage_size = scratch.size;
        return Status::Ok;
    }
    return Status::NotFound;
}

auto ModuleLoader::load(BootOptions const &options) -> Status {
    auto const storage = bootfs::View::Bytes(reinterpret_cast<u8 const *>(m_storage_base),
                                             m_storage_size);
    auto const bootfs = bootfs::View::from(storage);
    if (bootfs.is_err()) {
        return bootfs.unwrap_err();
    }
    auto const &files = bootfs.unwrap();

    auto name = std::string_view(options.next_module);
    auto found = files.find(name);
    if (found.is_err()) {
        return found.unwrap_err();
    }
    if (found.unwrap().is_none()) {
        // A miss on the default module is fatal; a miss on an explicitly
        // requested one warns and falls back (the fallback is not silent).
        if (name == kDefaultModule) {
            panic("phys module '{}' missing from kernel package", options.next_module);
        }
        println("phys module '{}' not found, falling back to '{}'",
                options.next_module);
        name = kDefaultModule;
        found = files.find(name);
        if (found.is_err()) {
            return found.unwrap_err();
        }
        if (found.unwrap().is_none()) {
            panic("phys module '{}' missing from kernel package", name);
        }
    }
    auto const module_bytes = files.file_data(found.unwrap().value());

    auto const init_status = m_image.init({module_bytes.data(), module_bytes.size()},
                                          module_bytes.size());
    if (init_status != Status::Ok) {
        return init_status;
    }

    auto const total = ustl::mm::align_up(m_image.size(), PAGE_SIZE);
    auto const placement = bootmem_allocate(total, PAGE_SIZE);
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
    // own_build_id() (ours/phys/build-id.hpp) reads this physload's own
    // copy back out of its own image right now, so the two can be compared.
    auto const interp = m_image.section(".interp");
    auto pt_interp =
        std::string_view(reinterpret_cast<char const *>(interp.data()), interp.size());
    if (!pt_interp.empty() && pt_interp.back() == '\0') {
        pt_interp.remove_suffix(1); // elf-interp.S null-terminates the string.
    }
    auto const expected_build_id = own_build_id();
    if (expected_build_id.empty() || pt_interp.empty() || pt_interp != expected_build_id) {
        println("phys module '{}' PT_INTERP '{}' does not match this physload's build ID '{}'",
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
    auto const record = m_image.section(PhysModule::kSection);
    if (record.size() < sizeof(PhysModule)) {
        println("phys module '{}' declares no {} descriptor", name, PhysModule::kSection);
        return Status::NotFound;
    }
    auto const *descriptor = reinterpret_cast<PhysModule const *>(record.data());
    if (descriptor->abi_version != PhysModule::kAbiVersion) {
        println("phys module '{}' speaks module ABI {}, physload speaks {}", name,
                descriptor->abi_version, PhysModule::kAbiVersion);
        return Status::Unsupported;
    }
    if (descriptor->init == nullptr) {
        println("phys module '{}' declares a null entry", name);
        return Status::InvalidArgument;
    }

    m_descriptor = descriptor;
    dprintln("Loaded phys module '{}' at 0x{:X}, entry 0x{:X}", descriptor->name, target,
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
    using Initializer = auto (*)() -> void;

    auto const array = m_image.section(".init_array");
    auto const *entries = reinterpret_cast<Initializer const *>(array.data());
    for (auto index = usize{0}; index < array.size() / sizeof(Initializer); ++index) {
        if (entries[index] != nullptr) {
            entries[index]();
        }
    }
}

} // namespace ours::phys
