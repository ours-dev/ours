#include <ours/phys/module-loader.hpp>

#include <bootfs/view.hpp>
#include <obi/kernel-package.hpp>
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

    auto const entry = m_image.load_at(target);
    if (entry.is_err()) {
        return entry.unwrap_err();
    }
    // The module links at base 0, so the relocation bias is the placement.
    m_image.relocate(target);
    m_entry = entry.unwrap();
    dprintln("Loaded phys module '{}' at 0x{:X}, entry 0x{:X}", options.next_module, target,
             m_entry);
    return Status::Ok;
}

} // namespace ours::phys
