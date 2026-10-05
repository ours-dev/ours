/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "realm.hpp"

#include <ours/object_info.hpp>
#include <ours/process_args.hpp>
#include <ours/rights.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>

#include <algorithm>
#include <array>
#include <utility>

#include <cstring>

#include "launcher_server.hpp"
#include "resource_server.hpp"
#include <elfload/diagnostics.hpp>
#include <launch/launch.hpp>
#include <ort/debug.hpp>
#include <ours_process/ours_process.hpp>
#include <ours_resource/ours_resource.hpp>

namespace init {
namespace {
    using component::Kind;
    using ours::HandleType;
    using ours::Signals;
    using ours::Status;
    using ours::Uandle;

    /// What a child's URL starts with (D26): the rest is the manifest's path
    /// in /boot, with /boot as the package directory.
    constexpr auto BOOT_URL_PREFIX = std::string_view{"fuchsia-boot:///#"};

    /// Where a component's program is, as its argv[0] names it: its path in
    /// the root's /boot, which is the package directory of every component.
    constexpr auto BOOT_PREFIX = std::string_view{"/boot/"};

    /// How many arguments after argv[0] a manifest may give its program.
    constexpr auto MAX_ARGS = usize{64};

    /// What `from: "parent"` means at the root (design "built-ins"): the
    /// directory `boot`, /boot itself with rx*, and the protocols the root
    /// serves itself: ours.process.Launcher (launcher_server.hpp), and
    /// ours.resource.Debug, ours.resource.Irq and ours.resource.Ioport
    /// (resource_server.hpp), which hand out the Debug resource and the roots
    /// of the interrupt lines and the I/O ports.
    constexpr auto BUILTINS = std::array{
        component::Builtin{
            .kind = Kind::Directory,
            .name = "boot",
            .rights = ustl::Option<component::Rights>(component::Rights::RxStarDir),
        },
        component::Builtin{
            .kind = Kind::Protocol,
            .name = ours_process::LAUNCHER_NAME,
            .rights = ustl::NONE,
        },
        component::Builtin{
            .kind = Kind::Protocol,
            .name = ours_resource::DEBUG_PROTOCOL.name,
            .rights = ustl::NONE,
        },
        component::Builtin{
            .kind = Kind::Protocol,
            .name = ours_resource::IRQ_PROTOCOL.name,
            .rights = ustl::NONE,
        },
        component::Builtin{
            .kind = Kind::Protocol,
            .name = ours_resource::IOPORT_PROTOCOL.name,
            .rights = ustl::NONE,
        },
    };

    /// Where every protocol use lives in the namespace (D3): a component's
    /// /svc, whose entries are the uses' names.
    constexpr auto SVC_PATH = std::string_view{"/svc"};
    constexpr auto SVC_PREFIX = std::string_view{"/svc/"};

    /// Where a provider serves a protocol in its outgoing directory: the
    /// root forwards an Open of /svc/<name> to svc/<name> there.
    constexpr auto OUTGOING_SVC_PREFIX = std::string_view{"svc/"};

    /// The rights a /svc directory is served with: r*, which holds CONNECT,
    /// what opening a protocol entry needs, and TRAVERSE, and grants nothing
    /// that could write.
    constexpr auto SVC_RIGHTS = ours_io::R_STAR_DIR;

    /// The value a /svc service node is added with, and connect() takes back
    /// apart: which instance of the pool, and which of its uses.
    constexpr auto service_key(usize const instance, u32 const use) -> u64 {
        return (static_cast<u64>(instance) << 32) | use;
    }

    // A route's rights are ours.io's Operations by their wire values
    // (component/format.hpp), which is what a connection holds.
    static_assert(std::to_underlying(component::Rights::RStarDir) ==
                  std::to_underlying(ours_io::R_STAR_DIR));
    static_assert(std::to_underlying(component::Rights::RxStarDir) ==
                  std::to_underlying(ours_io::RX_STAR_DIR));
    static_assert(std::to_underlying(component::Rights::All) == 0x1FF);

    auto to_operations(component::Rights const rights) -> ours_io::Operations {
        return static_cast<ours_io::Operations>(std::to_underlying(rights));
    }

    /// An instance's moniker for the log: its names from the root down,
    /// joined with '/', and "." for the root.
    class Moniker final {
      public:
        explicit Moniker(Instance const &instance) {
            if (instance.parent() == nullptr) {
                prepend(".");
                return;
            }
            // Leaf first, since the buffer fills from its end. Each ancestor
            // is an instance of the pool, so the depth is below its size; a
            // name the decoder let through longer than the compiler allows
            // could still overflow, and is cut short with "...".
            for (auto const *at = &instance; at->parent() != nullptr; at = at->parent()) {
                if (at != &instance && !prepend("/")) {
                    break;
                }
                if (!prepend(at->name())) {
                    auto _ = prepend("...");
                    break;
                }
            }
        }

        [[nodiscard]]
        auto view() const -> std::string_view {
            return {m_bytes.data() + m_start, m_bytes.size() - m_start};
        }

      private:
        auto prepend(std::string_view const text) -> bool {
            if (text.size() > m_start) {
                return false;
            }
            m_start -= text.size();
            std::memcpy(m_bytes.data() + m_start, text.data(), text.size());
            return true;
        }

        std::array<char, MAX_INSTANCES *(component::MAX_NAME_LENGTH + 1)> m_bytes{};
        usize m_start = m_bytes.size();
    };

    auto verb_name(component::Verb const verb) -> std::string_view {
        switch (verb) {
            case component::Verb::Offer:
                return "offer";
            case component::Verb::Expose:
                return "expose";
            case component::Verb::Declare:
                return "declaration";
            case component::Verb::Use:
                return "use";
        }
        return "hop";
    }

    /// Logs that |moniker|'s use |use| routes nowhere, and where the walk
    /// stopped: one line per failed route, so that a use that leads nowhere
    /// is found from the log alone (design "Events: a component connecting
    /// to a protocol through /svc", step 5).
    auto log_route_error(std::string_view const moniker, component::Use const &use,
                         component::RouteError<Instance> const &error) -> void {
        auto at = std::string_view{"the built-ins"};
        auto at_moniker = ustl::Option<Moniker>{};
        if (error.at != nullptr) {
            at_moniker = ustl::Option<Moniker>(Moniker(*error.at));
            at = at_moniker->view();
        }
        auto _ = ort::debug_line({"init: ", moniker, "'s use of \"", use.source_name,
                                  "\" routes nowhere: ", ours::to_string(error.status), " at ", at,
                                  " (", verb_name(error.verb), " of \"", error.name, "\")"});
    }

    /// The handles a component starts with, gathered while its start is
    /// built, in the order the processargs lists them. They are closed on
    /// every way out but the one write that hands them over.
    class Handed final {
      public:
        Handed() = default;
        Handed(Handed const &) = delete;
        auto operator=(Handed const &) -> Handed & = delete;

        ~Handed() {
            for (auto index = usize{0}; index < m_count; ++index) {
                // Each is this process's own; a close that fails has no one
                // to tell.
                auto _ = ours::handle_close(m_handles[index]);
            }
        }

        /// Adds |handle|, described by |info|. NoResource when a processargs
        /// message could not carry it; |handle| is closed then, as it would
        /// have been with the rest.
        [[nodiscard]]
        auto add(Uandle const handle, u32 const info) -> Status {
            if (m_count == m_handles.size()) {
                auto _ = ours::handle_close(handle);
                return Status::NoResource;
            }
            m_handles[m_count] = handle;
            m_infos[m_count] = info;
            m_count += 1;
            return Status::Ok;
        }

        [[nodiscard]]
        auto handles() const -> std::span<Uandle const> {
            return std::span{m_handles}.first(m_count);
        }

        [[nodiscard]]
        auto infos() const -> std::span<u32 const> {
            return std::span{m_infos}.first(m_count);
        }

        /// The handles were written away: channel_write consumes them,
        /// whatever it answers.
        auto handed_over() -> void {
            m_count = 0;
        }

      private:
        std::array<Uandle, ours::CHANNEL_MAX_MSG_HANDLES> m_handles{};
        std::array<u32, ours::CHANNEL_MAX_MSG_HANDLES> m_infos{};
        usize m_count = 0;
    };

    /// launch::start()'s failures, on the debug console: only the errors,
    /// each naming the step.
    auto print_launch_error(void *, elfload::Report const &report) -> void {
        if (report.severity != elfload::Severity::Error) {
            return;
        }
        std::string_view separator;
        std::string_view name;
        if (!report.name.empty()) {
            separator = ": ";
            name = {report.name.data(), report.name.size()};
        }
        auto _ = ort::debug_line(
            {"init: launch failed: ", {report.what.data(), report.what.size()}, separator, name});
    }

    /// Closes |channel| with an epitaph carrying |status|, the way a refused
    /// Open ends. The client may be gone already; the channel closes either
    /// way.
    auto close_with_epitaph(Uandle const channel, Status const status) -> void {
        std::array<u8, wire::STATUS_FORM_SIZE> bytes{};
        if (auto const epitaph = wire::encode_epitaph(bytes, status); epitaph.is_ok()) {
            auto const message = epitaph.unwrap();
            auto _ =
                ours::channel_write(channel, ours::ChannelWriteOptions::None, message.bytes.data(),
                                    static_cast<u32>(message.bytes.size()), nullptr, 0);
        }
        auto _ = ours::handle_close(channel);
    }
} // namespace

auto Instance::resolve() -> Status {
    return m_realm->resolve(*this);
}

auto Instance::child(u32 const index) -> Instance & {
    // The decoder checked every child index against the table, and the
    // children were allocated with it; past it is a bug, and traps.
    if (index >= decl().children().size()) {
        __builtin_trap();
    }
    return m_realm->m_pool[m_first_child + index];
}

Realm::Realm(std::span<Instance> const pool, BootServer &boot, loop::Loop &loop,
             Environment const &environment, BuiltinServers const &builtins)
    : m_pool(pool), m_boot(&boot), m_loop(&loop), m_environment(environment), m_builtins(builtins) {
    // Every /svc lives in /boot's tree, and an Open of one of its entries
    // comes back here to be routed.
    boot.set_connector({.connect = &Realm::on_connect, .context = this});
}

auto Realm::resolve_root() -> Status {
    if (m_pool.empty()) {
        return Status::NoResource;
    }
    auto &root = m_pool[0];
    root = Instance{};
    root.m_realm = this;
    root.m_url = ROOT_URL;
    m_used = 1;
    return resolve(root);
}

auto Realm::start_root() -> void {
    start(m_pool[0]);
}

auto Realm::resolve(Instance &instance) -> Status {
    if (instance.m_state != InstanceState::Unresolved) {
        return Status::Ok;
    }
    // The boot resolver: the URL names a file of /boot, read by direct call
    // (I2), since the root holds /boot itself.
    if (!instance.m_url.starts_with(BOOT_URL_PREFIX)) {
        return Status::InvArgs;
    }
    auto const file = m_boot->file(instance.m_url.substr(BOOT_URL_PREFIX.size()));
    if (file.is_err()) {
        return file.unwrap_err();
    }
    auto const decoded = component::Decl::decode(file->bytes);
    if (decoded.is_err()) {
        return decoded.unwrap_err();
    }
    auto const &decl = decoded.unwrap();

    // Room for every child first, before anything is made: an instance
    // resolves whole or not at all. This refusal is also what bounds
    // route(), which resolves children as it walks down (see resolve()'s
    // comment in realm.hpp); the pool only ever grows by this line.
    auto const children = usize{decl.children().size()};
    if (children > m_pool.size() - m_used) {
        return Status::NoResource;
    }

    // A component with a program gets its outgoing directory now, so that
    // a route can forward into it before it runs: the server end waits for
    // the start, holding what arrives meanwhile. Only a component with a
    // program gets the pair: without one, nothing would ever serve it. The
    // client end gets a slot, for READABLE and PEER_CLOSED.
    if (decl.program().is_some()) {
        auto outgoing = Uandle::Invalid;
        auto request = Uandle::Invalid;
        if (auto const status =
                ours::channel_create(ours::ChannelCreateOptions::None, &outgoing, &request);
            status != Status::Ok) {
            return status;
        }
        auto const added = m_loop->add(outgoing, Signals::Readable | Signals::PeerClosed,
                                       &Realm::on_outgoing, &instance);
        if (added.is_err()) {
            auto _ = ours::handle_close(outgoing);
            auto _ = ours::handle_close(request);
            return added.unwrap_err();
        }
        instance.m_outgoing = outgoing;
        instance.m_directory_request = request;
    }

    // The children, Unresolved, contiguous in the pool.
    instance.m_first_child = static_cast<u32>(m_used);
    for (auto index = u32{0}; index < children; ++index) {
        auto const declared = decl.children()[index];
        auto &child = m_pool[m_used + index];
        child = Instance{};
        child.m_realm = this;
        child.m_parent = &instance;
        child.m_index = index;
        child.m_name = declared.name;
        child.m_url = declared.url;
    }
    m_used += children;
    instance.m_decl = decl;
    instance.m_state = InstanceState::Resolved;
    return Status::Ok;
}

auto Realm::start(Instance &instance) -> void {
    if (instance.m_state != InstanceState::Resolved) {
        return;
    }
    if (instance.decl().program().is_some()) {
        if (auto const status = run(instance); status != Status::Ok) {
            auto const moniker = Moniker(instance);
            auto _ = ort::debug_line(
                {"init: cannot start ", moniker.view(), ": ", ours::to_string(status)});
            // Never started, and never started again (D23): routes to it
            // answer NotFound. Its outgoing directory's server end goes,
            // which closes the client end's slot in turn.
            instance.m_state = InstanceState::Exited;
            if (instance.m_directory_request != Uandle::Invalid) {
                auto _ = ours::handle_close(instance.m_directory_request);
                instance.m_directory_request = Uandle::Invalid;
            }
            return;
        }
    }
    // Its eager children, in manifest order: every child is eager (D4), the
    // compiler writes no other startup.
    for (auto index = u32{0}; index < instance.decl().children().size(); ++index) {
        auto &child = instance.child(index);
        if (auto const status = child.resolve(); status != Status::Ok) {
            auto const moniker = Moniker(child);
            auto _ = ort::debug_line(
                {"init: cannot resolve ", moniker.view(), ": ", ours::to_string(status)});
            continue;
        }
        start(child);
    }
}

auto Realm::run(Instance &instance) -> Status {
    auto const &decl = instance.decl();
    auto const program = *decl.program();
    auto const moniker = Moniker(instance);

    // The program's file, by direct call into /boot (I2). Only a file the
    // root made executable at ingest can be run: launch::start() maps it
    // with Execute, which a VMO without the right refuses.
    auto const file = m_boot->file(program.binary);
    if (file.is_err()) {
        return file.unwrap_err();
    }
    if (!file->executable) {
        return Status::AccessDenied;
    }

    // argv: /boot/<binary>, then the manifest's arguments.
    std::array<char, BOOT_PREFIX.size() + component::MAX_PATH_LENGTH> path_bytes{};
    if (program.binary.size() > component::MAX_PATH_LENGTH) {
        return Status::InvArgs;
    }
    std::memcpy(path_bytes.data(), BOOT_PREFIX.data(), BOOT_PREFIX.size());
    std::memcpy(path_bytes.data() + BOOT_PREFIX.size(), program.binary.data(),
                program.binary.size());
    std::array<std::string_view, 1 + MAX_ARGS> argv{};
    argv[0] = {path_bytes.data(), BOOT_PREFIX.size() + program.binary.size()};
    auto argc = usize{1};
    for (auto const arg: decl.args()) {
        if (argc == argv.size()) {
            return Status::OutOfRange;
        }
        argv[argc++] = arg;
    }

    Handed handed;
    auto job = Uandle::Invalid;
    if (auto const status =
            ours::handle_duplicate(m_environment.job, ours::Rights::SameRights, &job);
        status != Status::Ok) {
        return status;
    }
    if (auto const status = handed.add(job, ours::handle_info(HandleType::JobDefault, 0));
        status != Status::Ok) {
        return status;
    }

    // The namespace: one entry per directory use, routed now (D8), and
    // always from the built-in `boot` under D3. The root serves the entry's
    // server end itself, at the joined subdirectory with the use's rights;
    // when the route fails, the entry is closed with an epitaph saying why,
    // before the component runs, and the component finds it so on its first
    // Open (fsio::open()). One name per directory use, and /svc.
    std::array<std::string_view, MAX_DIRECTORY_USES + 1> names{};
    auto num_names = usize{0};
    auto has_protocols = false;
    for (auto const use: decl.uses()) {
        if (use.kind != Kind::Directory) {
            // Protocol uses live under /svc, made below.
            has_protocols = true;
            continue;
        }
        if (num_names == MAX_DIRECTORY_USES) {
            return Status::NoResource;
        }
        auto client = Uandle::Invalid;
        auto server = Uandle::Invalid;
        if (auto const status =
                ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
            status != Status::Ok) {
            return status;
        }
        auto served = Status::Ok;
        auto const routed = component::route(instance, use, BUILTINS);
        if (routed.is_err()) {
            served = routed.unwrap_err().status;
            log_route_error(moniker.view(), use, routed.unwrap_err());
        } else if (routed->provider != nullptr || routed->rights.is_none()) {
            // Under D3 a directory comes only from the built-in `boot`, and
            // a directory use always states rights: the compiler refuses
            // anything else, and the decoder does not.
            served = Status::NotSupported;
        } else {
            served = m_boot->serve(server, routed->subdir.view(), to_operations(*routed->rights));
        }
        if (served != Status::Ok) {
            if (routed.is_ok()) {
                // Routed, but not served: a subdir /boot does not have, a
                // file where a directory has to be, or no room for the
                // connection. The component reads the same epitaph as for a
                // failed route, so the root's log is where the two differ.
                auto _ = ort::debug_line({"init: ", moniker.view(), "'s use of \"", use.source_name,
                                          "\" cannot be served: ", ours::to_string(served)});
            }
            close_with_epitaph(server, served);
        }
        names[num_names] = use.target_path;
        if (auto const status = handed.add(
                client, ours::handle_info(HandleType::NsDir, static_cast<u16>(num_names)));
            status != Status::Ok) {
            return status;
        }
        num_names += 1;
    }

    // /svc, when it uses any protocol: one directory with an entry per
    // protocol use, served by the root, where each Open is routed when it
    // comes (D10, D11). A /svc the root cannot serve is closed with an
    // epitaph saying why, as a namespace entry it cannot route is, and the
    // component still starts.
    if (has_protocols) {
        auto client = Uandle::Invalid;
        auto server = Uandle::Invalid;
        if (auto const status =
                ours::channel_create(ours::ChannelCreateOptions::None, &client, &server);
            status != Status::Ok) {
            return status;
        }
        if (auto const served = serve_svc(instance, server); served != Status::Ok) {
            auto _ = ort::debug_line(
                {"init: ", moniker.view(), "'s /svc cannot be served: ", ours::to_string(served)});
            close_with_epitaph(server, served);
        }
        names[num_names] = SVC_PATH;
        if (auto const status = handed.add(
                client, ours::handle_info(HandleType::NsDir, static_cast<u16>(num_names)));
            status != Status::Ok) {
            return status;
        }
        num_names += 1;
    }

    // Its outgoing directory's server end, made at resolve.
    if (instance.m_directory_request != Uandle::Invalid) {
        auto const request = std::exchange(instance.m_directory_request, Uandle::Invalid);
        if (auto const status =
                handed.add(request, ours::handle_info(HandleType::DirectoryRequest, 0));
            status != Status::Ok) {
            return status;
        }
    }
    auto vdso = Uandle::Invalid;
    if (auto const status =
            ours::handle_duplicate(m_environment.vdso, ours::Rights::SameRights, &vdso);
        status != Status::Ok) {
        return status;
    }
    if (auto const status = handed.add(vdso, ours::handle_info(HandleType::VmoVdso, 0));
        status != Status::Ok) {
        return status;
    }

    // The processargs, written before the process exists, so that a message
    // that cannot be encoded starts nothing. Only its root region is still
    // to come, last, from start(): its info is listed already.
    std::array<u32, ours::CHANNEL_MAX_MSG_HANDLES> infos{};
    auto const listed = handed.infos();
    if (listed.size() == infos.size()) {
        return Status::NoResource;
    }
    std::ranges::copy(listed, infos.begin());
    infos[listed.size()] = ours::handle_info(HandleType::VmaRoot, 0);
    std::array<u8, ours::CHANNEL_MAX_MSG_BYTES> message{};
    auto length = u32{0};
    if (auto const status = ours::encode_process_args(
            message, std::span{infos}.first(listed.size() + 1), std::span{argv}.first(argc), {},
            std::span{names}.first(num_names), &length);
        status != Status::Ok) {
        return status;
    }

    auto here = Uandle::Invalid;
    auto there = Uandle::Invalid;
    if (auto const status = ours::channel_create(ours::ChannelCreateOptions::None, &here, &there);
        status != Status::Ok) {
        return status;
    }
    auto const started = launch::start({
        .job = m_environment.job,
        .name = instance.name(),
        .file = file->vmo,
        .vma = m_environment.vma,
        .vdso = m_environment.vdso,
        .vdso_file = m_environment.vdso_file,
        .bootstrap = there,
        .diagnostics = {.report = print_launch_error, .context = nullptr},
    });
    if (started.is_err()) {
        auto _ = ours::handle_close(here);
        return started.unwrap_err();
    }
    if (auto const status =
            handed.add(started->root_vma, ours::handle_info(HandleType::VmaRoot, 0));
        status != Status::Ok) {
        // Cannot happen: room for it was checked above. The process waits
        // for a processargs that will not come, and ends when |here| closes.
        auto _ = ours::handle_close(here);
        auto _ = ours::handle_close(started->process);
        return status;
    }
    auto const handles = handed.handles();
    auto const written =
        ours::channel_write(here, ours::ChannelWriteOptions::None, message.data(), length,
                            handles.data(), static_cast<u32>(handles.size()));
    handed.handed_over();
    auto _ = ours::handle_close(here);
    if (written != Status::Ok) {
        // The runtime finds the bootstrap channel closed with nothing on it
        // and exits; the process is not this instance's any more.
        auto _ = ours::handle_close(started->process);
        return written;
    }

    // Supervised from here: its TERMINATED slot owns the process handle.
    instance.m_state = InstanceState::Started;
    instance.m_process = started->process;
    if (auto const added =
            m_loop->add(started->process, Signals::Terminated, &Realm::on_terminated, &instance);
        added.is_err()) {
        auto _ = ort::debug_line(
            {"init: cannot watch ", moniker.view(), ": ", ours::to_string(added.unwrap_err())});
        auto _ = ours::handle_close(started->process);
        instance.m_process = Uandle::Invalid;
    }
    auto _ = ort::debug_line({"init: started ", moniker.view()});
    return Status::Ok;
}

auto Realm::serve_svc(Instance &instance, Uandle const server) -> Status {
    auto const &decl = instance.decl();
    auto protocols = usize{0};
    for (auto const use: decl.uses()) {
        if (use.kind == Kind::Protocol) {
            protocols += 1;
        }
    }
    // The bound that keeps every /svc inside MAX_SVC_NODES: an instance
    // starts once (D23), with at most this many entries.
    if (protocols > MAX_PROTOCOL_USES) {
        return Status::NoResource;
    }
    auto const directory = m_boot->add_svc_directory();
    if (directory.is_err()) {
        return directory.unwrap_err();
    }
    // One entry per protocol use, named as the use names its path, under the
    // key connect() takes apart. A component's /svc is made once, when it
    // starts, and never changes: nodes of a vfs tree are never removed.
    auto const pool_index = static_cast<usize>(&instance - m_pool.data());
    auto index = u32{0};
    for (auto const use: decl.uses()) {
        auto const use_index = index;
        index += 1;
        if (use.kind != Kind::Protocol) {
            continue;
        }
        // The compiler writes every protocol use at /svc/<name> (D3); the
        // decoder does not check it.
        if (!use.target_path.starts_with(SVC_PREFIX)) {
            return Status::InvArgs;
        }
        if (auto const status =
                m_boot->add_service(*directory, use.target_path.substr(SVC_PREFIX.size()),
                                    service_key(pool_index, use_index));
            status != Status::Ok) {
            return status;
        }
    }
    return m_boot->serve(server, *directory, SVC_RIGHTS);
}

auto Realm::on_connect(void *const context, u64 const service, Uandle const channel) -> void {
    static_cast<Realm *>(context)->connect(service, channel);
}

auto Realm::connect(u64 const service, Uandle const channel) -> void {
    // The key serve_svc() gave the entry. Both halves name what it named
    // then: instances are never destroyed, and a decl never changes.
    auto const pool_index = static_cast<usize>(service >> 32);
    auto const use_index = static_cast<u32>(service);
    if (pool_index >= m_used || use_index >= m_pool[pool_index].decl().uses().size()) {
        close_with_epitaph(channel, Status::NotFound);
        return;
    }
    auto &instance = m_pool[pool_index];
    auto const use = instance.decl().uses()[use_index];

    // Routed again on every Open (D11): nothing is cached between two Opens
    // of one entry, so a route always reflects the tree as it is now.
    auto const routed = component::route(instance, use, BUILTINS);
    if (routed.is_err()) {
        log_route_error(Moniker(instance).view(), use, routed.unwrap_err());
        close_with_epitaph(channel, routed.unwrap_err().status);
        return;
    }
    if (routed->provider != nullptr) {
        forward(*routed->provider, routed->name, channel);
        return;
    }

    // A built-in: a fresh connection of its server, one per Open, so that
    // one client's connection state never meets another's. No security
    // policy is asked first; ours has none. Routing is the only gate, so a
    // route that reached a resource protocol is all it takes to be handed
    // that resource.
    auto status = Status::NotFound;
    if (routed->name == ours_process::LAUNCHER_NAME) {
        status = m_builtins.launcher->bind(channel);
    } else if (routed->name == ours_resource::DEBUG_PROTOCOL.name) {
        status = m_builtins.debug_resource->bind(channel);
    } else if (routed->name == ours_resource::IRQ_PROTOCOL.name) {
        status = m_builtins.irq_resource->bind(channel);
    } else if (routed->name == ours_resource::IOPORT_PROTOCOL.name) {
        status = m_builtins.ioport_resource->bind(channel);
    }
    if (status != Status::Ok) {
        // A full pool answers NoResource on the request's server end (D27).
        auto _ = ort::debug_line({"init: cannot serve ", routed->name, " to ",
                                  Moniker(instance).view(), ": ", ours::to_string(status)});
        close_with_epitaph(channel, status);
    }
}

auto Realm::forward(Instance &provider, std::string_view const name, Uandle const channel) -> void {
    // A provider that has exited answers NotFound (D23), and so does one
    // whose outgoing directory's slot has closed: it is gone, or never had
    // one to serve from.
    if (provider.m_state == InstanceState::Exited || provider.m_outgoing == Uandle::Invalid) {
        close_with_epitaph(channel, Status::NotFound);
        return;
    }
    // svc/<name> in its outgoing directory. A provider not started yet --
    // a later sibling -- finds the Open queued on the server end it gets at
    // its start, as resolve() arranged.
    std::array<char, OUTGOING_SVC_PREFIX.size() + ours_io::MAX_NAME_LENGTH> path_bytes{};
    if (name.size() > ours_io::MAX_NAME_LENGTH) {
        close_with_epitaph(channel, Status::NotFound);
        return;
    }
    std::memcpy(path_bytes.data(), OUTGOING_SVC_PREFIX.data(), OUTGOING_SVC_PREFIX.size());
    std::memcpy(path_bytes.data() + OUTGOING_SVC_PREFIX.size(), name.data(), name.size());
    std::array<u8, wire::MAX_MESSAGE_BYTES> bytes{};
    std::array<Uandle, 1> handles{};
    auto const encoded = ours_io::encode_open(
        bytes, handles,
        ours_io::OpenRequest{
            .flags = ours_io::Flags::ProtocolService,
            .path = {path_bytes.data(), OUTGOING_SVC_PREFIX.size() + name.size()},
            .object = channel,
        });
    if (encoded.is_err()) {
        close_with_epitaph(channel, encoded.unwrap_err());
        return;
    }
    // Fire and forget (I1): from here the client and the provider talk
    // directly. A full or closed outgoing channel drops the Open, and the
    // channel with it, which the client sees as PEER_CLOSED with no
    // epitaph.
    auto const message = encoded.unwrap();
    if (auto const written =
            ours::channel_write(provider.m_outgoing, ours::ChannelWriteOptions::None,
                                message.bytes.data(), static_cast<u32>(message.bytes.size()),
                                message.handles.data(), static_cast<u32>(message.handles.size()));
        written != Status::Ok) {
        auto _ = ort::debug_line({"init: cannot forward ", name, " to ", Moniker(provider).view(),
                                  ": ", ours::to_string(written)});
    }
}

auto Realm::on_outgoing(void *const context, Signals) -> loop::Action {
    // A provider never writes on its outgoing directory's client end, so
    // whatever came is closed with it, before a provider can park messages
    // full of handles there; PEER_CLOSED means the provider is gone (FS
    // §4.3's o_c). Either way the slot closes the handle (I3), and the
    // instance stops naming it.
    auto *const instance = static_cast<Instance *>(context);
    instance->m_outgoing = Uandle::Invalid;
    return loop::Action::Close;
}

auto Realm::on_terminated(void *const context, Signals) -> loop::Action {
    // The code it ended with -- the shell's too, since it is a component --
    // TERMINATED first, then object_get_info: the return code is final only
    // once the process has ended.
    auto *const instance = static_cast<Instance *>(context);
    auto const moniker = Moniker(*instance);
    ours::InfoProcess info{};
    if (auto const status = ours::object_get_info(instance->m_process, ours::InfoTopic::Process,
                                                  &info, sizeof(info), nullptr, nullptr);
        status != Status::Ok) {
        auto _ = ort::debug_line(
            {"init: ", moniker.view(), " exited; reading its code = ", ours::to_string(status)});
    } else {
        char code[21];
        auto _ = ort::debug_line({"init: ", moniker.view(), " exited with ",
                                  ort::signed_decimal(info.return_code, code)});
    }
    // Back to Resolved, marked exited, never started again (D23). The
    // process handle is the slot's, and closes with it (I3); everything else
    // the component held closes itself, each in its own slot's callback.
    instance->m_state = InstanceState::Exited;
    instance->m_process = Uandle::Invalid;
    return loop::Action::Close;
}
} // namespace init
