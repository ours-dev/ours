/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LAUNCH_PROCESS_LAUNCHER_HPP
#define LAUNCH_PROCESS_LAUNCHER_HPP 1

#include <array>
#include <span>
#include <string_view>

#include <ours/process_args.hpp>
#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>
#include <ours_process/ours_process.hpp>
#include <ustl/result.hpp>
#include <ustl/util/enum_bits.hpp>
#include <wire/wire.hpp>

namespace launch {
/// The handle_info of the handles a launcher adds to every processargs it
/// writes, after the client's and in this order: the new process's root
/// region and the vDSO's VMO (component-framework design D19). ProcessSelf,
/// ThreadSelf and StackVmo are not added: start() closes them or never
/// passes them (launch.hpp).
inline constexpr auto LAUNCHER_HANDLE_INFO = std::array{
    ours::handle_info(ours::HandleType::VmaRoot, 0),
    ours::handle_info(ours::HandleType::VmoVdso, 0),
};

/// A program that a Launch asked for and that the state has judged: what
/// the start hook of LauncherOps is handed.
struct Startup {
    /// The job the process goes under, from LaunchInfo. Not consumed: the
    /// state closes it once the hook has returned, as start() never takes it.
    ours::Uandle job;

    /// The process's name, from LaunchInfo: at most
    /// ours_process::MAX_NAME_LEN bytes, no NUL.
    std::string_view name;

    /// The program, from LaunchInfo. Not consumed, like |job|.
    ours::Uandle executable;

    /// The processargs the program is to get, encoded by
    /// ours::encode_process_args(): the arguments, no environment, the
    /// namespace paths, and a handle_info for each of |handles| followed by
    /// LAUNCHER_HANDLE_INFO.
    std::span<u8 const> process_args;

    /// What the Add* calls gave, in the order of the processargs'
    /// handle_info. Consumed: the hook owns them from the call on.
    std::span<ours::Uandle const> handles;
};

/// What a ProcessLauncherState needs from the program that serves the
/// connection: the system calls, which this library does not make, so that
/// the state is the same code on the host, where its tests run, as in the
/// root. A function pointer and its argument, as elfload::Diagnostics is.
struct LauncherOps {
    /// Starts the program |startup| describes and returns its process.
    ///
    /// It takes every handle in |startup.handles|, whatever it answers: it
    /// writes them on the new process's bootstrap channel with
    /// |startup.process_args|, followed by its own in the order of
    /// LAUNCHER_HANDLE_INFO, or it closes them. In the root this is a channel
    /// pair, launch::start() and that channel_write.
    auto (*start)(void *context, Startup const &startup)
        -> ustl::Result<ours::Uandle, ours::Status> = nullptr;

    /// Closes |handle|, which the state holds and passes on to nobody.
    auto (*close)(void *context, ours::Uandle handle) -> void = nullptr;

    /// Handed to both, as it is.
    void *context = nullptr;
};

/// One ours.process.Launcher connection, as its server keeps it: what the
/// client's Add* calls have given so far, and the judging of each message
/// it sends.
///
/// The root has no heap (user programs have none, system/lib/ort), so the
/// state is fixed: a string arena, MAX_ARGS arguments, and MAX_HANDLES
/// handles with their handle_info (design D13). An Add* that does not fit is
/// not refused on the spot: the Add* calls are one-way and have no reply to
/// carry a refusal, so it sets a mark, and the Launch after it answers
/// InvArgs, as it does for a processargs too large to send. The client
/// learns of it from Launch's status.
///
/// It owns every handle a message brings, from the moment receive() is
/// given the message: each one is passed on to the start hook or closed
/// through LauncherOps::close, exactly once. The process a Launch's reply
/// carries comes from the hook, and is the caller's.
///
/// One thread uses it at a time, as the root runs every callback on one
/// thread (design D16); nothing here locks.
class ProcessLauncherState final {
  public:
    /// The most handles the Add* calls may give. A processargs message
    /// carries at most CHANNEL_MAX_MSG_HANDLES handles, and the launcher
    /// adds LAUNCHER_HANDLE_INFO's: 64 - 2.
    static constexpr auto MAX_HANDLES =
        usize{ours::CHANNEL_MAX_MSG_HANDLES} - LAUNCHER_HANDLE_INFO.size();

    /// The most arguments AddArgs may give. A limit of this state's, which
    /// keeps where each argument lies in a fixed array, not of the
    /// protocol's, whose only bound is the message size. The name-based
    /// protocol this one replaced had the same limit, MAX_STRINGS.
    static constexpr auto MAX_ARGS = usize{64};

    /// The most bytes of strings -- the arguments and the namespace paths,
    /// each with its NUL -- the Add* calls may give: what a processargs
    /// message has room for after its header and the launcher's own
    /// handle_info. Each handle a client adds takes four more bytes from the
    /// same message, which only Launch can count; it answers InvArgs when the
    /// whole does not fit.
    static constexpr auto MAX_STRING_BYTES = usize{ours::CHANNEL_MAX_MSG_BYTES} -
                                             sizeof(ours::ProcArgs) -
                                             (LAUNCHER_HANDLE_INFO.size() * sizeof(u32));

    /// What the server does after receive().
    enum class Outcome : u8 {
        /// An Add* call was taken in. Nothing to write; read the next
        /// message.
        Continue,
        /// A Launch was served, well or not: write the reply.
        Reply,
        /// The message was malformed, or is no Launcher request: close the
        /// connection, without an epitaph. Everything the state held has
        /// been closed.
        Close,
    };

    /// What receive() made of one message.
    struct Received {
        Outcome outcome;

        /// With Reply: the txid of the Launch it answers.
        u32 txid = 0;

        /// With Reply: the answer. On Ok, |reply.process| is the caller's,
        /// to send with the reply or close.
        ours_process::LaunchReply reply = {.status = ours::Status::Ok};
    };

    /// |ops| must have both hooks; the state calls them for as long as it
    /// lives.
    ///
    /// constexpr, so that a program can keep its connections in a constinit
    /// global: nothing runs a user program's dynamic initialisers (no
    /// .init_array walk, //system/lib/ort), so a pool that needed a
    /// constructor to run would start out as zeros, with null hooks. The root
    /// keeps its Launcher connections that way (component-framework step 14).
    constexpr explicit ProcessLauncherState(LauncherOps const &ops) : m_ops(ops) {}

    /// Closes whatever is still held.
    ///
    /// A global holding states must be [[clang::no_destroy]]: this
    /// destructor is not trivial, so as a plain global it would be
    /// registered with __cxa_atexit, which ort does not define, since a user
    /// program's globals are never torn down. The root's pool is; a
    /// connection that ends is reset() when its peer closes, not destroyed.
    ~ProcessLauncherState() {
        reset();
    }

    // It owns handles, so a copy would close them twice; and the root keeps
    // its connections in fixed pools (design D27), so none has to move.
    ProcessLauncherState(ProcessLauncherState const &) = delete;
    auto operator=(ProcessLauncherState const &) -> ProcessLauncherState & = delete;
    ProcessLauncherState(ProcessLauncherState &&) = delete;
    auto operator=(ProcessLauncherState &&) -> ProcessLauncherState & = delete;

    /// Takes in one message the client wrote, and every handle in it,
    /// whatever it answers:
    ///
    ///   - AddArgs, AddHandles and AddNames are added to what is held, or set
    ///     a mark when they do not fit or name a handle type the launcher
    ///     refuses; their handles are then closed at once. Continue.
    ///   - Launch is judged, its program started through LauncherOps::start
    ///     unless a mark or the judging says InvArgs, and the state reset
    ///     whatever the answer, so that each Launch starts from nothing.
    ///     Reply.
    ///   - Anything else -- a message that is malformed, a reply, an ordinal
    ///     that is not one of the four -- resets the state and closes the
    ///     message's handles. Close.
    ///
    /// |scratch| is where a Launch's processargs is encoded, and must hold
    /// CHANNEL_MAX_MSG_BYTES; it must not overlap |message|.
    [[nodiscard]]
    auto receive(wire::Message message, std::span<u8> scratch) -> Received;

    /// Back to the state of a new connection: every handle held is closed,
    /// every string and mark forgotten. The server calls this when the
    /// connection goes away (PEER_CLOSED); receive() calls it after every
    /// Launch and before Close.
    auto reset() -> void;

  private:
    /// Facts about what the Add* calls gave that make the next Launch
    /// answer InvArgs.
    enum class Marks : u8 {
        None = 0,
        /// An Add* call did not fit: more handles, arguments or bytes than
        /// this state holds.
        Overflow = 1 << 0,
        /// An Add* call named a handle type the launcher adds itself or that
        /// AddNames alone may give, or set a handle_info's reserved bits.
        Refused = 1 << 1,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(Marks);

    /// Where one string lies in m_strings, its NUL left out.
    struct Piece {
        u16 offset;
        u16 size;
    };
    static_assert(MAX_STRING_BYTES <= 0xFFFF);

    auto add_args(ours_process::PackedStrings args) -> void;
    auto add_handles(ours_process::AddHandlesRequest const &request) -> void;
    auto add_names(ours_process::AddNamesRequest const &request) -> void;
    auto launch(ours_process::LaunchRequest const &request, std::span<u8> scratch) -> Received;

    /// Judges what is held against |info|, encodes the processargs into
    /// |scratch| and calls the start hook. The hook's answer, or InvArgs.
    auto start(ours_process::LaunchInfo const &info, std::span<u8> scratch)
        -> ustl::Result<ours::Uandle, ours::Status>;

    /// Copies |strings| to the end of m_strings and records where each one
    /// lies in |pieces|, from |count| on. The caller has checked that they
    /// fit.
    auto store_strings(ours_process::PackedStrings strings, std::span<Piece> pieces, usize count)
        -> void;

    [[nodiscard]]
    auto string_at(Piece piece) const -> std::string_view;

    /// Closes every valid handle of |handles|.
    auto close_all(std::span<ours::Uandle const> handles) const -> void;

    LauncherOps m_ops;

    /// The arguments and the namespace paths, each followed by its NUL, in
    /// the order they came.
    std::array<char, MAX_STRING_BYTES> m_strings{};
    usize m_strings_size = 0;

    std::array<Piece, MAX_ARGS> m_args{};
    usize m_num_args = 0;

    /// One path per AddNames entry; name |i| is the one whose NsDir
    /// handle_info has the argument |i|. Every name brings a handle, so
    /// there are never more names than handles.
    std::array<Piece, MAX_HANDLES> m_names{};
    usize m_num_names = 0;

    /// The handles, from AddHandles and AddNames in the order they came,
    /// each with its handle_info at the same index.
    std::array<ours::Uandle, MAX_HANDLES> m_handles{};
    std::array<u32, MAX_HANDLES> m_handle_info{};
    usize m_num_handles = 0;

    Marks m_marks = Marks::None;
};
} // namespace launch

#endif // #ifndef LAUNCH_PROCESS_LAUNCHER_HPP
