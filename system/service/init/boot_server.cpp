/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "boot_server.hpp"

#include <ours/rights.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include "programs.hpp"
#include <bootfs/format.hpp>
#include <bootfs/view.hpp>
#include <ort/debug.hpp>

namespace init {
namespace {
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    auto report(std::string_view const what, std::string_view const name, Status const status)
        -> void {
        auto _ = ort::debug_line({what, name, ": ", ours::to_string(status)});
    }

    /// Whether the BootFS name |name| lies under the top-level directory
    /// |directory|: "bin/shell" under "bin", and neither "bin" itself nor
    /// "binary/x".
    auto lies_under(std::string_view const name, std::string_view const directory) -> bool {
        return name.size() > directory.size() && name.starts_with(directory) &&
               name[directory.size()] == '/';
    }
} // namespace

BootServer::BootServer(Storage &storage, loop::Loop &loop)
    : m_tree(storage.nodes)
    , m_server(m_tree, loop, ioservice::Syscalls{.handle_duplicate = ours::handle_duplicate},
               ioservice::LoopServer::Memory{
                   .connections = storage.connections,
                   .bindings = storage.bindings,
                   .request_bytes = storage.request_bytes,
                   .request_handles = storage.request_handles,
                   .reply = &storage.reply,
               }) {}

auto BootServer::ingest(std::span<u8 const> const mapping, Uandle const bootfs, Uandle const vmex)
    -> ustl::Result<Ingested, Status> {
    auto const view = bootfs::View::from({mapping.data(), mapping.size()});
    if (view.is_err()) {
        return ustl::err(view.unwrap_err());
    }
    if (!has_room(Share::Boot)) {
        return ustl::err(Status::NoResource);
    }
    auto const root = m_tree.add_root();
    if (root.is_err()) {
        return ustl::err(root.unwrap_err());
    }
    took(Share::Boot);
    m_root = root.unwrap();

    auto ingested = Ingested{.files = 0, .executable = 0};
    for (auto index = u32{0}; index < view->file_count(); ++index) {
        auto const entry = view->file(index).unwrap();
        auto const name = view->file_name(entry);

        // The directories on the way, made as they are first met. A name
        // the tree refuses -- empty, ".", "..", too long -- leaves the entry
        // out: nothing could open it anyway.
        auto parent = m_root;
        auto rest = name;
        auto status = Status::Ok;
        for (auto slash = rest.find('/'); slash != std::string_view::npos; slash = rest.find('/')) {
            auto const segment = rest.substr(0, slash);
            rest.remove_prefix(slash + 1);
            if (auto const found = m_tree.lookup(parent, segment); found.is_ok()) {
                parent = found.unwrap();
                continue;
            }
            if (!has_room(Share::Boot)) {
                status = Status::NoResource;
                break;
            }
            auto const made = m_tree.add_directory(parent, segment);
            if (made.is_err()) {
                status = made.unwrap_err();
                break;
            }
            took(Share::Boot);
            parent = made.unwrap();
        }
        if (status == Status::NoResource) {
            return ustl::err(status);
        }
        if (status != Status::Ok) {
            report("init: /boot leaves out ", name, status);
            continue;
        }

        if (!has_room(Share::Boot)) {
            return ustl::err(Status::NoResource);
        }
        // The file's own VMO: a slice of the BootFS over its bytes, which
        // View has checked start on a page and share no page with another
        // file. A slice of a read-only VMO is read-only too.
        auto slice = Uandle::Invalid;
        if (auto const sliced = ours::vmo_create_child(bootfs, ours::VmoChildOptions::Slice,
                                                       entry.data_offset, entry.data_size, &slice);
            sliced != Status::Ok) {
            report("init: /boot cannot slice ", name, sliced);
            continue;
        }
        // Under bin/, executable: decided by path, with bin/ the only
        // executable directory until there are shared libraries (S3). The call
        // consumes the slice and drops Write, which a slice of the BootFS never
        // had.
        auto const executable = lies_under(name, user_programs::DIRECTORY);
        auto vmo = slice;
        if (executable) {
            if (auto const replaced = ours::vmo_replace_as_executable(slice, vmex, &vmo);
                replaced != Status::Ok) {
                report("init: /boot cannot make executable ", name, replaced);
                continue;
            }
        }
        auto const added = m_tree.add_file(
            parent, rest,
            ioservice::VmoFile{
                .vmo = vmo,
                .bytes = {view->file_data(entry).data(), view->file_data(entry).size()},
                .executable = executable,
            });
        if (added.is_err()) {
            auto _ = ours::handle_close(vmo);
            if (added.unwrap_err() == Status::NoResource) {
                return ustl::err(Status::NoResource);
            }
            report("init: /boot leaves out ", name, added.unwrap_err());
            continue;
        }
        took(Share::Boot);
        ingested.files += 1;
        if (executable) {
            ingested.executable += 1;
        }
    }
    return ustl::ok(ingested);
}

auto BootServer::file(std::string_view const path) const
    -> ustl::Result<ioservice::VmoFile, Status> {
    auto const found = m_tree.lookup(m_root, path);
    if (found.is_err()) {
        return ustl::err(found.unwrap_err());
    }
    auto const file = m_tree.file(found.unwrap());
    if (file.is_none()) {
        return ustl::err(Status::NotFile);
    }
    return ustl::ok(*file);
}

auto BootServer::serve(Uandle const channel, std::string_view const subdir,
                       ours_io::Operations const rights) -> Status {
    auto node = m_root;
    if (!subdir.empty()) {
        auto const found = m_tree.lookup(m_root, subdir);
        if (found.is_err()) {
            return found.unwrap_err();
        }
        node = found.unwrap();
    }
    return serve(channel, node, rights);
}

auto BootServer::serve(Uandle const channel, ioservice::NodeId const directory,
                       ours_io::Operations const rights) -> Status {
    return m_server.serve(channel, directory, rights);
}

auto BootServer::add_svc_directory() -> ustl::Result<ioservice::NodeId, Status> {
    if (!has_room(Share::Svc)) {
        return ustl::err(Status::NoResource);
    }
    auto const made = m_tree.add_root();
    if (made.is_ok()) {
        took(Share::Svc);
    }
    return made;
}

auto BootServer::add_service(ioservice::NodeId const directory, std::string_view const name,
                             u64 const service) -> Status {
    if (!has_room(Share::Svc)) {
        return Status::NoResource;
    }
    auto const added = m_tree.add_service(directory, name, service);
    if (added.is_err()) {
        return added.unwrap_err();
    }
    took(Share::Svc);
    return Status::Ok;
}

auto BootServer::has_room(Share const share) const -> bool {
    if (share == Share::Boot) {
        return m_boot_nodes < MAX_BOOT_NODES;
    }
    return m_svc_nodes < MAX_SVC_NODES;
}

auto BootServer::took(Share const share) -> void {
    if (share == Share::Boot) {
        m_boot_nodes += 1;
    } else {
        m_svc_nodes += 1;
    }
}
} // namespace init
