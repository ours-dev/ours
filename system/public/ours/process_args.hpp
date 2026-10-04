/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef OURS_PROCESS_ARGS_HPP
#define OURS_PROCESS_ARGS_HPP 1

// processargs: the one message a new process is started with.
//
// A process begins with a single handle in %rdi, its bootstrap channel, and
// the first message on that channel carries everything else it is given --
// the handles, each tagged with what it is for, and its arguments,
// environment and namespace names. One register can say only one thing;
// this message can say as much as a channel message holds.
//
// The layout is Zircon's zx_proc_args (zircon/system/public/zircon/
// processargs.h), field for field, and the handle types keep Zircon's
// numbers:
//
//   [ProcArgs][u32 handle_info x num_handles][args][environ][names]
//
// Every *_off counts bytes from the start of the message. Each string section
// is |*_num| NUL-terminated strings back to back, with no length prefix and
// no padding anywhere. The message's i-th handle is described by the i-th
// handle_info. What differs from Zircon is the protocol word, which is ours
// (PROCARGS_PROTOCOL) so that nothing mistakes this for a message it could
// exchange with Zircon.
//
// There is one encoder, encode_process_args() below, and one decoder,
// ort::ProcessArgs (//system/lib/ort); everything that starts a
// process -- the kernel for the first one, the launcher for the rest --
// writes the message with the former (design 3.3).

#include <ours/status.hpp>
#include <ours/syscall.hpp>
#include <ours/types.hpp>

#include <span>
#include <string_view>

#include <cstddef>
#include <cstring>

namespace ours {
/// The fixed head of a processargs message. Zircon zx_proc_args, in the same
/// order and of the same size.
struct ProcArgs {
    /// PROCARGS_PROTOCOL.
    u32 protocol;
    /// PROCARGS_VERSION.
    u32 version;

    /// Where the handle_info array starts. Its length is the number of handles
    /// the message carries, which the message itself does not repeat.
    u32 handle_info_off;

    /// The command line, argv[0] first.
    u32 args_off;
    u32 args_num;

    /// Conventionally NAME=VALUE, but nothing checks that.
    u32 environ_off;
    u32 environ_num;

    /// The paths the NsDir handles are mounted at: an NsDir handle's arg is an
    /// index into these.
    u32 names_off;
    u32 names_num;
};
static_assert(sizeof(ProcArgs) == 36);
static_assert(alignof(ProcArgs) == 4);
static_assert(offsetof(ProcArgs, handle_info_off) == 8);
static_assert(offsetof(ProcArgs, args_off) == 12);
static_assert(offsetof(ProcArgs, environ_off) == 20);
static_assert(offsetof(ProcArgs, names_off) == 28);
static_assert(offsetof(ProcArgs, names_num) == 32);

/// "OPAR" in memory (Ours ProcARgs). Zircon's is "]XPA"; a different word
/// on purpose, since the two are not interchangeable.
inline constexpr auto PROCARGS_PROTOCOL = u32{0x5241'504f};

inline constexpr auto PROCARGS_VERSION = u32{1};

/// What a startup handle is for. Zircon's PA_* values, so that a reader who
/// knows those knows these (design 3.3); types of ours' own go in 0xe0-0xef,
/// which Zircon leaves unused. VmoVdso goes to the process that starts others:
/// it maps the vDSO's VMO into each, and the kernel registers the mapping as
/// that process's vDSO (2026-09-28: it used to be left out, the kernel
/// mapping the vDSO itself).
enum class HandleType : u8 {
    ProcSelf = 0x01,
    ThreadSelf = 0x02,
    JobDefault = 0x03,
    VmaRoot = 0x04,
    VmaLoaded = 0x05,
    VmoVdso = 0x11,
    VmoStack = 0x13,
    VmoExecutable = 0x14,
    VmoBootdata = 0x1a,
    VmoBootfs = 0x1b,
    NsDir = 0x20,
    Fd = 0x30,
    DirectoryRequest = 0x3b,
    LogSink = 0x3d,
    Resource = 0x3f,
    MmioResource = 0x50,
    IrqResource = 0x51,
    IoportResource = 0x52,
    SystemResource = 0x54,

    /// The crash record the previous boot's panic left, as a read-only VMO
    /// holding its text, the panic's message first. Present only when there
    /// was one. Zircon hands its userboot the same thing as PA_VMO_KERNEL_FILE
    /// (0x1c) and tells it apart by the VMO's name, "crashlog"; VMOs have no
    /// names here, so it has a type of its own, in ours' range.
    VmoCrashlog = 0xe0,
    User0 = 0xf0,
    User1 = 0xf1,
    User2 = 0xf2,
};

/// A handle_info: the type in bits 0-7, a type-specific argument in bits
/// 16-31 (an index into names for NsDir, a file descriptor number for Fd,
/// usually 0). Zircon PA_HND().
[[nodiscard]]
constexpr auto handle_info(HandleType const type, u16 const arg) -> u32 {
    return u32(type) | u32(arg) << 16;
}

[[nodiscard]]
constexpr auto handle_info_type(u32 const info) -> HandleType {
    return static_cast<HandleType>(info & 0xff);
}

[[nodiscard]]
constexpr auto handle_info_arg(u32 const info) -> u16 {
    return static_cast<u16>(info >> 16);
}

/// Bits 8-15 of a handle_info, which are unused and must be zero. Zircon's
/// PA_HND() masks them off without checking; the encoder and the decoder
/// here both refuse a value that sets any.
inline constexpr auto HANDLE_INFO_RESERVED = u32{0xff00};

/// How many of the image's command-line options the kernel relays to the
/// first process as environment strings, and how many bytes they may take
/// together, each with its NUL (collect_user_options() in
/// //kernel/lib/userabi/src/userboot.cpp).
///
/// Here, beside the format, because more than the kernel sizes buffers by
/// them. The options travel on -- userboot hands init the ones that are not
/// its own, and init builds a command's argv out of one -- and each hop
/// writes a new message into a fixed buffer. With the bound in one place
/// every buffer is sized from it and fits by construction; spelled out in
/// each program, a kernel that raised its bound would leave the others to
/// find out at run time, as a BufferTooSmall on the boot path.
///
/// Generous for what exists -- two option families -- without letting a
/// runaway command line decide the size of a kernel stack frame.
inline constexpr u32 MAX_USER_OPTIONS = 8;
inline constexpr usize USER_OPTIONS_BYTES = 384;

namespace process_args_detail {
    /// Adds the encoded size of |strings| -- each one's bytes and its NUL --
    /// to |size|.
    [[nodiscard]]
    inline auto add_strings(std::span<std::string_view const> const strings, usize &size)
        -> Status {
        for (auto const string: strings) {
            // A NUL inside a string would end it early, and the reader would
            // find one string more than the writer wrote.
            if (string.find('\0') != std::string_view::npos) {
                return Status::InvArgs;
            }
            size += string.size() + 1;
            if (size > CHANNEL_MAX_MSG_BYTES) {
                return Status::OutOfRange;
            }
        }
        return Status::Ok;
    }

    inline auto put_strings(u8 *const out, usize &at,
                            std::span<std::string_view const> const strings) -> void {
        for (auto const string: strings) {
            std::memcpy(out + at, string.data(), string.size());
            at += string.size();
            out[at++] = 0;
        }
    }
} // namespace process_args_detail

/// Writes a processargs message into |out| and its length into |length|.
///
/// |handle_info| describes the handles the caller will send alongside, in the
/// same order; this writes only the bytes, and the caller passes the handles
/// to channel_write itself. Nothing is allocated, so the kernel can use this
/// on a buffer of its own as well as a program can.
///
///   InvArgs         a string contains a NUL, or a handle_info sets reserved
///                   bits;
///   OutOfRange      more handles, or more bytes, than one channel message
///                   may carry -- a message that could never be sent;
///   BufferTooSmall  it would fit a message but not |out|.
///
/// Nothing is written unless the answer is Ok. The layout is the one
/// Zircon's process_builder writes: the header, the handle_info array
/// straight after it, then args, environ and names, with no padding; an
/// empty section's offset is where it would have started.
[[nodiscard]]
inline auto encode_process_args(std::span<u8> const out, std::span<u32 const> const handle_info,
                                std::span<std::string_view const> const args,
                                std::span<std::string_view const> const environ,
                                std::span<std::string_view const> const names, u32 *const length)
    -> Status {
    if (handle_info.size() > CHANNEL_MAX_MSG_HANDLES) {
        return Status::OutOfRange;
    }
    for (auto const info: handle_info) {
        if ((info & HANDLE_INFO_RESERVED) != 0) {
            return Status::InvArgs;
        }
    }

    // Sizes first, so that a message that cannot be written leaves |out|
    // untouched. Every sum stays below CHANNEL_MAX_MSG_BYTES, which is what
    // makes the narrowing to u32 below exact.
    auto size = usize{sizeof(ProcArgs)};
    auto const handle_info_off = size;
    size += handle_info.size() * sizeof(u32);
    auto const args_off = size;
    if (auto const status = process_args_detail::add_strings(args, size); status != Status::Ok) {
        return status;
    }
    auto const environ_off = size;
    if (auto const status = process_args_detail::add_strings(environ, size); status != Status::Ok) {
        return status;
    }
    auto const names_off = size;
    if (auto const status = process_args_detail::add_strings(names, size); status != Status::Ok) {
        return status;
    }
    if (size > CHANNEL_MAX_MSG_BYTES) {
        return Status::OutOfRange;
    }
    if (size > out.size()) {
        return Status::BufferTooSmall;
    }

    auto const header = ProcArgs{
        .protocol = PROCARGS_PROTOCOL,
        .version = PROCARGS_VERSION,
        .handle_info_off = static_cast<u32>(handle_info_off),
        .args_off = static_cast<u32>(args_off),
        .args_num = static_cast<u32>(args.size()),
        .environ_off = static_cast<u32>(environ_off),
        .environ_num = static_cast<u32>(environ.size()),
        .names_off = static_cast<u32>(names_off),
        .names_num = static_cast<u32>(names.size()),
    };
    std::memcpy(out.data(), &header, sizeof(header));
    if (!handle_info.empty()) {
        std::memcpy(out.data() + handle_info_off, handle_info.data(),
                    handle_info.size() * sizeof(u32));
    }
    auto at = args_off;
    process_args_detail::put_strings(out.data(), at, args);
    process_args_detail::put_strings(out.data(), at, environ);
    process_args_detail::put_strings(out.data(), at, names);

    *length = static_cast<u32>(size);
    return Status::Ok;
}
} // namespace ours

#endif // #ifndef OURS_PROCESS_ARGS_HPP
