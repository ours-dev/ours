/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ioservice/outgoing.hpp>

#include <algorithm>
#include <array>

#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

namespace ioservice {
namespace {
    using ours::Status;
    using ours::Uandle;

    /// How much of a refused path the line shows: a path may be as long as
    /// ours.io allows, which is more than one debug_write takes. A longer
    /// path is cut (printable_path()).
    constexpr auto MAX_SHOWN_PATH = usize{128};

    /// A line being assembled in the room one debug_write takes, with no
    /// allocation: the pieces are copied in, and a piece that does not fit is
    /// cut. Written out in one call, so that the line cannot be split by
    /// another program printing at the same moment.
    class Line final {
      public:
        auto append(std::string_view const text) -> void {
            // One byte stays free for the newline.
            auto const room = m_text.size() - 1 - m_size;
            auto const count = std::min(text.size(), room);
            std::copy_n(text.data(), count, m_text.data() + m_size);
            m_size += count;
        }

        /// Ends the line and writes it to the debug console.
        [[nodiscard]]
        auto write() -> Status {
            m_text[m_size] = '\n';
            m_size += 1;
            return ours::debug_write(m_text.data(), m_size);
        }

      private:
        std::array<char, ours::MAX_DEBUG_WRITE> m_text{};
        usize m_size = 0;
    };
} // namespace

Outgoing::Outgoing(Storage &storage, loop::Loop &loop)
    : m_storage(&storage)
    , m_tree(storage.nodes)
    , m_server(m_tree, loop, Syscalls{.handle_duplicate = ours::handle_duplicate},
               LoopServer::Memory{
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

auto Outgoing::add_protocol(std::string_view const name, Handler &handler) -> Status {
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
    m_storage->handlers[m_protocols] = &handler;
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
    m_storage->handlers[service]->connect(channel);
}

auto Outgoing::open_refused(NodeId, std::string_view const path, Status const status) -> void {
    // Every refusal, not only a name that is not served: the others (a path
    // that is not valid, rights the connection does not hold, flags that do
    // not fit the node) are as silent to the provider, and the status says
    // which it was.
    //
    // The path is the client's own bytes, so it goes in through
    // printable_path(): a client cannot end the line and write another.
    std::array<char, MAX_SHOWN_PATH> shown{};
    auto line = Line{};
    line.append("ioservice: the outgoing directory refused an Open of \"");
    line.append(printable_path(path, shown));
    line.append("\": ");
    line.append(ours::to_string(status));
    // A console that does not take the line leaves nothing to do about it.
    auto _ = line.write();
}
} // namespace ioservice
