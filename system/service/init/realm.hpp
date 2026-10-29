/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef INIT_REALM_HPP
#define INIT_REALM_HPP 1

#include <ours/signals.hpp>
#include <ours/status.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

#include "boot_server.hpp"
#include "environment.hpp"
#include <component/decl.hpp>
#include <component/route.hpp>
#include <loop/loop.hpp>
#include <ustl/option.hpp>

/// The component tree the root keeps: its instances, the boot resolver that
/// fills them in, the ELF runner that starts them, and the supervision of
/// their processes (component-framework design, "Events: starting a
/// component" and "Events: a component exiting").
namespace init {
/// The root manifest's URL. It is fixed, since no parent's manifest names
/// the root (D26).
inline constexpr auto ROOT_URL = std::string_view{"fuchsia-boot:///#meta/root.cm"};

/// Where an instance is in its life (design "States, ownership and
/// locking"). There is no state past Exited: instances here are never
/// destroyed.
enum class InstanceState : u8 {
    /// Created by its parent's resolve; its manifest not read yet.
    Unresolved,
    /// Its manifest decoded and its children created.
    Resolved,
    /// Its process running.
    Started,
    /// Resolved, and done: its process exited, or never started. It is never
    /// moved back to Resolved to be started again by its next route; routes
    /// to it answer NotFound (D23).
    Exited,
};

class Realm;
class LauncherServer;
class ResourceServer;

/// One component instance: the root's record of it. It satisfies
/// component::Instance, so route() walks a tree of these.
///
/// Instances live in the Realm's fixed pool and are never destroyed (D27),
/// so a pointer to one stays valid for good. Its children are contiguous in
/// the pool, allocated together when it resolves.
class Instance final {
  public:
    constexpr Instance() = default;

    // What component::Instance asks for.

    /// The parent, or nullptr for the root.
    [[nodiscard]]
    auto parent() const -> Instance * {
        return m_parent;
    }

    /// Its index in its parent's children, the index an offer's target names.
    [[nodiscard]]
    auto index() const -> u32 {
        return m_index;
    }

    /// The decoded manifest; only on a resolved instance (traps otherwise).
    [[nodiscard]]
    auto decl() const -> component::Decl const & {
        return *m_decl;
    }

    /// Resolves the instance if it is not yet: Realm::resolve().
    [[nodiscard]]
    auto resolve() -> ours::Status;

    /// The child at |index| of decl().children(); only on a resolved
    /// instance, with an index the decoder checked.
    [[nodiscard]]
    auto child(u32 index) -> Instance &;

    // The root's own.

    /// Its name in its parent's manifest; empty for the root.
    [[nodiscard]]
    auto name() const -> std::string_view {
        return m_name;
    }

    [[nodiscard]]
    auto state() const -> InstanceState {
        return m_state;
    }

  private:
    friend class Realm;

    Realm *m_realm = nullptr;
    Instance *m_parent = nullptr;
    u32 m_index = 0;
    /// Views of the parent's manifest, which lies in the BootFS mapping for
    /// good; ROOT_URL for the root.
    std::string_view m_name;
    std::string_view m_url;
    InstanceState m_state = InstanceState::Unresolved;
    ustl::Option<component::Decl> m_decl;
    /// Where its children start in the Realm's pool.
    u32 m_first_child = 0;

    /// The client end of its outgoing directory, made at resolve for an
    /// instance with a program: the Loop's, and Invalid once its slot has
    /// closed it. A route that ends at this instance forwards its Open here
    /// (Realm::forward()).
    ours::Uandle m_outgoing = ours::Uandle::Invalid;
    /// The server end, held until the start hands it on as DirectoryRequest.
    ours::Uandle m_directory_request = ours::Uandle::Invalid;
    /// The process, while it runs: the Loop's, closed by its TERMINATED
    /// slot.
    ours::Uandle m_process = ours::Uandle::Invalid;
};

/// The servers of the root's built-in protocols (design "built-ins"): what
/// a /svc Open whose route ends at a built-in is bound to. Each
/// ResourceServer serves the ours.resource protocol its field is named after.
struct BuiltinServers {
    LauncherServer *launcher;
    ResourceServer *debug_resource;
    ResourceServer *irq_resource;
    ResourceServer *ioport_resource;
    ResourceServer *sampling_resource;
    /// ours.kernel.RootJob, which hands out the root job.
    ResourceServer *root_job;
};

/// The instance pool, the boot resolver, the ELF runner, and the router of
/// every component's /svc.
///
/// One thread runs all of it, inside the Loop's callbacks or before the Loop
/// starts (D16), so nothing is locked: a callback runs to completion before
/// the next one starts.
class Realm final: private ioservice::LoopServer::Delegate {
  public:
    /// A realm whose instances live in |pool|, reading manifests and
    /// programs from |boot|, waiting on |loop|, and binding /svc requests
    /// routed to a built-in protocol to |builtins|. All of them must outlive
    /// it. It makes itself |boot|'s Delegate, so that every Open of a /svc
    /// entry reaches connect().
    Realm(std::span<Instance> pool, BootServer &boot, loop::Loop &loop,
          Environment const &environment, BuiltinServers const &builtins);

    Realm(Realm const &) = delete;
    auto operator=(Realm const &) -> Realm & = delete;

    /// Makes the root instance from ROOT_URL and resolves it (design "Events:
    /// boot", step 5). A failure is the root's own, which D28 has the caller
    /// log and halt on: no meta/root.cm, or one that fails to decode.
    [[nodiscard]]
    auto resolve_root() -> ours::Status;

    /// Starts the root's eager children, one after another in manifest
    /// order (step 6). A child that fails to start is logged and the next
    /// one still starts: one broken child does not keep its siblings down.
    auto start_root() -> void;

  private:
    friend class Instance;

    /// The boot resolver: reads |instance|'s manifest from /boot by direct
    /// call, decodes it, and creates its children Unresolved; for a manifest
    /// with a program it also makes the outgoing-directory pair. All or
    /// nothing: on failure the instance stays Unresolved and nothing was
    /// taken from any pool.
    ///
    /// NoResource once the pool has no room for its children. route()
    /// terminates only because of that (component/route.hpp): each step
    /// down a manifest that names itself as a child resolves an instance
    /// one level deeper, and so takes at least one more instance from the
    /// pool. So this never grows the pool and never evicts a resolved
    /// instance to make room.
    [[nodiscard]]
    auto resolve(Instance &instance) -> ours::Status;

    /// Starts |instance|, which is resolved: runs its program, if it has
    /// one, then starts its eager children. Failures are logged here.
    auto start(Instance &instance) -> void;

    /// The ELF runner: builds |instance|'s namespace by routing its
    /// directory uses and serving its /svc, starts its program with
    /// launch::start() by direct call (I2), and writes its processargs.
    [[nodiscard]]
    auto run(Instance &instance) -> ours::Status;

    /// Makes |instance|'s /svc: a directory beside /boot with a service node
    /// per protocol use, served on |server| with r* (design "Events:
    /// starting a component", step 3). On failure |server| is still the
    /// caller's.
    [[nodiscard]]
    auto serve_svc(Instance &instance, ours::Uandle server) -> ours::Status;

    /// An Open of the service node |service| of some component's /svc, on
    /// |channel|, which this owns from the call on. Routes the use |service|
    /// names again, as every Open does (D11), and binds |channel| to the
    /// built-in it ends at, forwards it to the provider it ends at, or
    /// closes it with the route's epitaph (design "Events: a component
    /// connecting to a protocol through /svc").
    auto connect(u64 service, ours::Uandle channel) -> void override;

    /// A reply to a client of /boot or of some /svc could not be written, and
    /// its connection ends: the root says so.
    auto write_failed(ours::Status status) -> void override;

    /// Writes Open("svc/<name>", PROTOCOL_SERVICE, |channel|) on
    /// |provider|'s outgoing directory, and does not wait, as
    /// OutgoingConnector::send does; a provider that has exited answers
    /// NotFound (D23). Consumes |channel|.
    auto forward(Instance &provider, std::string_view name, ours::Uandle channel) -> void;

    /// The slot on an outgoing directory's client end.
    static auto on_outgoing(void *context, ours::Signals observed) -> loop::Action;

    /// The TERMINATED slot on a component's process.
    static auto on_terminated(void *context, ours::Signals observed) -> loop::Action;

    std::span<Instance> m_pool;
    /// The first m_used instances of the pool are in use, the root first.
    usize m_used = 0;
    BootServer *m_boot;
    loop::Loop *m_loop;
    Environment m_environment;
    BuiltinServers m_builtins;
};
} // namespace init

#endif // #ifndef INIT_REALM_HPP
