#include <ours/phys/kernel_package.hpp>
#include <ours/phys/kernel_package_loader.hpp>

#include <cstring>

namespace ours::phys {
namespace {
    /// Compose "<directory>/<name>" into a fixed buffer; the kernel-name
    /// buffer in BootOptions bounds the input, so 160 bytes cover it.
    auto join_path(char *const buffer, usize const capacity, std::string_view const directory,
                   std::string_view const name) -> Status {
        if (directory.empty() || name.empty() || directory.size() + 1 + name.size() >= capacity) {
            return Status::InvArgs;
        }
        auto *cursor = buffer;
        for (auto const character: directory) {
            *cursor++ = character;
        }
        *cursor++ = '/';
        for (auto const character: name) {
            *cursor++ = character;
        }
        *cursor = '\0';
        return Status::Ok;
    }
} // namespace

auto KernelPackageLoader::init(Bytes const storage, PhysAddr const package_base,
                               usize const package_size, std::string_view const kernel_dir)
    -> Status {
    // 1. View the BootFS physload materialized (no decompression here).
    auto const bootfs = bootfs::View::from(storage);
    if (bootfs.is_err()) {
        return bootfs.unwrap_err();
    }
    auto const &files = bootfs.unwrap();

    // 2. Locate the kernel ELF inside the kernel-name directory.
    char path[160];
    auto const joined = join_path(path, sizeof(path), kernel_dir, KernelPackage::IMAGE_ELF);
    if (joined != Status::Ok) {
        return joined;
    }
    auto const found = files.find(path);
    if (found.is_err()) {
        return found.unwrap_err();
    }
    if (found.unwrap().is_none()) {
        return Status::NotFound;
    }
    kernel_ = files.file_data(found.unwrap().value());

    // 3. Retain the outer OBI range and the materialized package for the
    //    kernel-side handoff. phys runs identity-mapped, so the storage's
    //    address is its physical one.
    handoff_.package = {
        .base = package_base,
        .size = package_size,
        .present = true,
    };
    handoff_.kernel_storage = {
        .base = PhysAddr{reinterpret_cast<usize>(storage.data())},
        .size = storage.size(),
        .present = true,
    };
    return Status::Ok;
}
} // namespace ours::phys
