/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <svc/outgoing.hpp>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace svc {
namespace {
    using ours::Status;
    using ours::Uandle;
} // namespace

Outgoing::Outgoing(Storage &storage, loop::Loop &loop)
    : m_storage(&storage)
    , m_tree(storage.nodes)
    , m_server(m_tree, loop, ioservice::Syscalls{.handle_duplicate = ours::handle_duplicate},
               ioservice::LoopServer::Memory{
                   .connections = storage.connections,
                   .bindings = storage.bindings,
                   .request_bytes = storage.request_bytes,
                   .request_handles = storage.request_handles,
                   .reply = &storage.reply,
               },
               this)
    // The tree's first two nodes, which Storage sizes room for: neither add
    // can fail, and unwrap() traps if one ever does.
    , m_root(m_tree.add_root().unwrap())
    , m_svc(m_tree.add_directory(m_root, SVC_DIRECTORY).unwrap()) {}

auto Outgoing::add_protocol(std::string_view const name, Handler const handler) -> Status {
    if (handler.connect == nullptr) {
        return Status::InvArgs;
    }
    if (m_protocols == MAX_PROTOCOLS) {
        return Status::NoResource;
    }
    // The service node's value is the handler's index, which connect() takes
    // back. The handler is stored only once the node is in, so a refused
    // name leaves nothing behind.
    auto const added = m_tree.add_service(m_svc, name, m_protocols);
    if (added.is_err()) {
        return added.unwrap_err();
    }
    m_storage->handlers[m_protocols] = handler;
    m_protocols += 1;
    return Status::Ok;
}

auto Outgoing::serve(Uandle const directory_request) -> Status {
    return m_server.serve(directory_request, m_root, OUTGOING_RIGHTS);
}

auto Outgoing::connect(u64 const service, Uandle const channel) -> void {
    // An Open of svc/<name>: the protocol's handler owns the channel from
    // here. The value is an index add_protocol() gave out, and the tree holds
    // no other service node; one past them would be a bug, and its channel
    // only closes.
    if (service >= m_protocols) {
        // Nothing can be done about a close that fails.
        auto _ = ours::handle_close(channel);
        return;
    }
    auto const &handler = m_storage->handlers[service];
    handler.connect(handler.context, channel);
}
} // namespace svc
