/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "user_bootfs.hpp"

#include <bootfs/format.hpp>
#include <logz4/log.hpp>
#include <ours/boot/options.hpp>
#include <ours/mm/cfg.hpp>
#include <ustl/mem/align.hpp>

namespace userabi {
using namespace ours;

namespace {
    /// The whitelist's name in the kernel package, as kernel_package()
    /// installs it (//kernel/kernel_package.gni, `user_bootfs = true`): one
    /// BootFS name per line, generated from the `bootfs_user_visible` GN
    /// metadata.
    constexpr std::string_view WHITELIST_FILE{"config/user_bootfs"};

    /// Where every file's data starts, and what it is padded out to; see
    /// make_user_bootfs().
    constexpr u64 DATA_ALIGNMENT = PAGE_SIZE;

    /// Whether |name| is the kernel's own, which user space never gets
    /// whatever the whitelist says.
    ///
    /// Two tests, because each misses something the other catches. The
    /// kernel's directory covers every file that is private for no reason
    /// but where it lives -- including ones added after this was written,
    /// which is the whole point of refusing by place rather than by kind.
    /// The file names cover a kernel image in a directory other than the one
    /// booted: a second kernel in the same package, or the one phys really
    /// booted when `kernel.name` was overridden from the bootloader (which
    /// vmours does not see; boot::BootOptions::kernel_name()).
    [[nodiscard]]
    auto is_kernel_private(std::string_view const name, std::string_view const kernel_name)
        -> bool {
        if (!kernel_name.empty() && name.starts_with(kernel_name) &&
            (name.size() == kernel_name.size() || name[kernel_name.size()] == '/')) {
            return true;
        }
        auto const slash = name.rfind('/');
        auto const base = slash == std::string_view::npos ? name : name.substr(slash + 1);
        return base == phys::KernelPackage::IMAGE_ELF || base == phys::KernelPackage::CODE_PATCHES;
    }

    /// Calls |fn(name)| for each name in the whitelist |list|, in order: one
    /// per line, empty lines skipped. The file ends in a newline, and a list
    /// with nothing marked is empty; neither is an error.
    template <typename Fn>
    auto for_each_line(std::string_view const list, Fn &&fn) -> void {
        usize start = 0;
        while (start < list.size()) {
            auto end = list.find('\n', start);
            if (end == std::string_view::npos) {
                end = list.size();
            }
            if (end > start) {
                fn(list.substr(start, end - start), start);
            }
            start = end + 1;
        }
    }

    /// Calls |fn(name, entry)| for each file of |package| the whitelist
    /// |list| grants user space, in list order.
    ///
    /// Run twice by make_user_bootfs(), once to size the VMO and once to fill
    /// it, and deterministic, so both passes see the same files. Only the
    /// first logs (|report|): the refusals would otherwise be printed twice.
    template <typename Fn>
    auto for_each_granted(phys::KernelPackage const &package, std::string_view const list,
                          std::string_view const kernel_name, bool const report, Fn &&fn) -> void {
        for_each_line(list, [&](std::string_view const name, usize const at) {
            if (is_kernel_private(name, kernel_name)) {
                if (report) {
                    log::error("userabi: '{}' is on the BootFS whitelist but is the kernel's "
                               "own ({}={}); refused",
                               name, boot::BootOptions::KERNEL_NAME_OPTION, kernel_name);
                }
                return;
            }

            // The first listing of a name stands and any later one is
            // dropped, so the BootFS never holds two entries of one name --
            // bootfs::View::find() answers AlreadyExists to that, and every
            // reader would lose the file.
            auto listed_earlier = false;
            for_each_line(list.substr(0, at), [&](std::string_view const earlier, usize) {
                listed_earlier = listed_earlier || earlier == name;
            });
            if (listed_earlier) {
                if (report) {
                    log::error("userabi: '{}' is on the BootFS whitelist twice", name);
                }
                return;
            }

            auto const found = package.find(name);
            if (found.is_err()) {
                if (report) {
                    log::error("userabi: '{}' could not be looked up in the kernel package: {}",
                               name, to_string(found.unwrap_err()));
                }
                return;
            }
            if (found.unwrap().is_none()) {
                if (report) {
                    log::error("userabi: '{}' is on the BootFS whitelist but not in the kernel "
                               "package",
                               name);
                }
                return;
            }
            fn(name, found.unwrap().value());
        });
    }
} // namespace

auto make_user_bootfs(phys::KernelPackage const &package, std::string_view const kernel_name,
                      object::KernelHandle<object::VmoDispatcher> &out, Rights &rights_out)
    -> Status {
    auto const whitelist = package.file(WHITELIST_FILE);
    if (whitelist.is_err()) {
        // No list means nothing is granted. This is the package not having
        // opted in (kernel_package(user_bootfs = true)), and the answer that
        // fails towards giving less.
        log::error("userabi: no BootFS whitelist '{}' in the kernel package ({}); user space "
                   "gets no BootFS",
                   WHITELIST_FILE, to_string(whitelist.unwrap_err()));
        return whitelist.unwrap_err();
    }
    auto const bytes = whitelist.unwrap();
    auto const list = std::string_view(reinterpret_cast<char const *>(bytes.data()), bytes.size());

    // Pass one: how many files, how many bytes of names, how many of data.
    // Each file's data takes whole pages, so the data size is the sum of the
    // rounded-up sizes, and nothing needs remembering between the passes.
    u32 file_count = 0;
    u64 name_table_size = 0;
    u64 data_size = 0;
    for_each_granted(package, list, kernel_name, true,
                     [&](std::string_view const name, bootfs::Entry const &entry) {
                         file_count += 1;
                         name_table_size += name.size();
                         data_size += ustl::mm::align_up(entry.data_size, DATA_ALIGNMENT);
                     });
    if (file_count == 0) {
        // bootfs::View refuses a BootFS with no files, so there is nothing
        // sensible to build.
        log::error("userabi: the BootFS whitelist grants no files; user space gets no BootFS");
        return Status::NotFound;
    }

    // Header | Entry[file_count] | names | data, as bootfs/format.hpp lays it
    // out. The total is page-sized by construction, and the VMO created for
    // it is exactly that big, which bootfs::View::from() checks.
    auto const entry_offset = u64{sizeof(bootfs::Header)};
    auto const name_offset = entry_offset + u64{file_count} * sizeof(bootfs::Entry);
    auto const data_offset = ustl::mm::align_up(name_offset + name_table_size, DATA_ALIGNMENT);
    auto const total_size = data_offset + data_size;

    object::KernelHandle<object::VmoDispatcher> vmo;
    Rights rights;
    // Committed whole (VmoDispatcher::create_committed()), so every page is
    // there, zeroed, before a byte is written: the gaps between the name table
    // and the data, and after each file, read as zero rather than as whatever
    // the frames last held. Not pinned, so that a program can snapshot a file
    // in it: that is how its data segment is made. It used to be Pinned, by
    // create_paged(), and a snapshot of it answered BadState.
    auto status = object::VmoDispatcher::create_committed(total_size, vmo, rights);
    if (Status::Ok != status) {
        log::error("userabi: could not create a {}-byte BootFS VMO: {}", total_size,
                   to_string(status));
        return status;
    }
    auto &dispatcher = *vmo.dispatcher();

    // Pass two: write the entries, names and data. The first failure is kept
    // and the rest of the pass skipped; there is no partial BootFS to hand
    // out, so the VMO is simply dropped.
    u32 index = 0;
    auto name_cursor = u32{0};
    auto data_cursor = data_offset;
    for_each_granted(
        package, list, kernel_name, false,
        [&](std::string_view const name, bootfs::Entry const &source) {
            if (Status::Ok != status) {
                return;
            }
            auto const entry = bootfs::Entry{
                .name_offset = name_cursor,
                .name_size = static_cast<u16>(name.size()),
                .flags = 0,
                .data_offset = data_cursor,
                .data_size = source.data_size,
                .alignment = DATA_ALIGNMENT,
            };
            auto const data = package.bootfs().file_data(source);
            status =
                dispatcher.write(&entry, entry_offset + u64{index} * sizeof(entry), sizeof(entry));
            if (Status::Ok == status) {
                status = dispatcher.write(name.data(), name_offset + name_cursor, name.size());
            }
            if (Status::Ok == status && !data.empty()) {
                status = dispatcher.write(data.data(), data_cursor, data.size());
            }
            index += 1;
            name_cursor += static_cast<u32>(name.size());
            data_cursor += ustl::mm::align_up(source.data_size, DATA_ALIGNMENT);
        });

    // The header last, so that a VMO that got this far without it would not
    // even parse.
    auto const header = bootfs::Header{
        .magic = bootfs::Header::MAGIC,
        .file_count = file_count,
        .name_table_size = static_cast<u32>(name_table_size),
        .reserved = 0,
        .total_size = total_size,
        .reserved2 = 0,
    };
    if (Status::Ok == status) {
        status = dispatcher.write(&header, 0, sizeof(header));
    }
    if (Status::Ok != status) {
        log::error("userabi: could not write the BootFS VMO: {}", to_string(status));
        return status;
    }

    // Read the header back through the same interface user space will use
    // (vmo_read goes through VmoDispatcher::read()), rather than trusting the
    // writes: this is the one check that the bytes landed in the VMO and not
    // just in a buffer on the way.
    bootfs::Header readback{};
    status = dispatcher.read(&readback, 0, sizeof(readback));
    if (Status::Ok != status || readback.magic != header.magic ||
        readback.file_count != header.file_count || readback.total_size != header.total_size) {
        log::error("userabi: the BootFS VMO does not read back what was written: {}",
                   to_string(status));
        return Status::InternalError;
    }
    log::info("userabi: bootfs vmo {} bytes, {} files, magic {:#x}", total_size,
              readback.file_count, readback.magic);

    rights_out = rights & USER_BOOTFS_RIGHTS;
    out = ustl::move(vmo);
    return Status::Ok;
}
} // namespace userabi
