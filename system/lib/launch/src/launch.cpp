/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/launch.hpp>
#include <launch/plan.hpp>
#include <launch/program.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include <elfload/binder.hpp>
#include <elfload/image.hpp>
#include <elfload/program.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <ustl/mem/align.hpp>

namespace launch {
namespace {
    using ours::HandleValue;
    using ours::Status;
    using ours::VirtAddr;
    using ours::VmOptions;

    /// The stack a program starts with when it does not ask for a size, and
    /// the unmapped page below every stack that turns running off its end into
    /// a fault. 64 KiB, as the kernel's userboot gives the first program
    /// (USER_STACK_SIZE) and the interface design (3.4) sets.
    constexpr usize DEFAULT_STACK_SIZE = 64 * 1024;
    constexpr usize STACK_GUARD = PAGE_SIZE;

    /// The stack |program| gets: the size it asks for with PT_GNU_STACK,
    /// rounded up to a page, or the default when it asks for none -- the rule
    /// of Zircon's process_builder, with a smaller default. No cap: a stack is
    /// the program's own address space and memory, taken only as it is
    /// touched, and a size there is no room for fails at vma_allocate.
    /// OutOfRange for a size that would wrap, rounded or with its guard.
    auto stack_size_of(Program const &program) -> ustl::Result<usize, Status> {
        auto const requested = program.stack_size();
        if (requested == 0) {
            return ustl::ok(DEFAULT_STACK_SIZE);
        }
        auto const rounded = ustl::mm::align_up(requested, u64{PAGE_SIZE});
        if (rounded < requested || rounded > ~u64{0} - STACK_GUARD) {
            return ustl::err(Status::OutOfRange);
        }
        return ustl::ok(usize(rounded));
    }

    /// One handle this function made, closed when it goes out of scope unless
    /// it was handed on. Local to the launcher, the one place that needs it.
    class Owned {
      public:
        Owned() = default;
        Owned(Owned const &) = delete;
        auto operator=(Owned const &) -> Owned & = delete;

        ~Owned() {
            if (m_value != ours::HANDLE_INVALID) {
                static_cast<void>(ours::handle_close(m_value));
            }
        }

        /// Where a system call writes the handle it makes.
        [[nodiscard]]
        auto out() -> HandleValue * {
            return &m_value;
        }

        [[nodiscard]]
        auto get() const -> HandleValue {
            return m_value;
        }

        /// Hands the handle on; this no longer closes it.
        [[nodiscard]]
        auto release() -> HandleValue {
            return std::exchange(m_value, ours::HANDLE_INVALID);
        }

      private:
        HandleValue m_value = ours::HANDLE_INVALID;
    };

    /// The x86-64 ABI wants %rsp + 8 sixteen-byte aligned at a function's
    /// entry, as if a call had just pushed its return address. Zircon's
    /// compute_initial_stack_pointer() and the kernel's userboot do the same.
    constexpr auto initial_stack_pointer(VirtAddr const top) -> VirtAddr {
        return (top & ~VirtAddr{15}) - 8;
    }

    /// The pages an ELF's PT_LOAD segments cover, from the first page of the
    /// lowest to the end of the last page of the highest: what the image's
    /// addresses count from, and how much address space it takes.
    struct Span {
        u64 first = 0;
        u64 size = 0;
    };

    auto span_of(std::span<Segment const> const segments) -> Span {
        auto lowest = ~u64{0};
        auto highest = u64{0};
        for (auto const &segment: segments) {
            lowest = std::min(lowest, segment.vaddr);
            highest = std::max(highest, segment.vaddr + segment.mem_size);
        }
        auto const first = ustl::mm::align_down(lowest, PAGE_SIZE);
        return {.first = first, .size = ustl::mm::align_up(highest, PAGE_SIZE) - first};
    }

    /// Where the binder's writes go: the plan says which VMO holds each page of
    /// the image, so a write at an offset of the image is a vmo_write to that
    /// VMO, split where it crosses from one to the next (the snapshot to the
    /// zero-fill VMO of the same segment, say). A write into a page that comes
    /// straight from the file is refused: the file is shared by every process
    /// running the program, and this kernel does not do text relocations.
    struct Writer {
        Plan const *plan;
        u64 first; // The link-time address an offset counts from.
        std::array<HandleValue, Plan::MAX_SEGMENTS> const *snapshots;
        std::array<HandleValue, Plan::MAX_SEGMENTS> const *zeros;

        static auto write(void *const context, usize const offset, void const *const bytes,
                          usize const size) -> Status {
            auto const &self = *static_cast<Writer const *>(context);
            auto const *source = static_cast<u8 const *>(bytes);
            auto address = self.first + offset;
            auto left = size;
            while (left != 0) {
                Mapping const *holder = nullptr;
                for (auto const &mapping: self.plan->mappings()) {
                    if (address >= mapping.vaddr && address - mapping.vaddr < mapping.size) {
                        holder = &mapping;
                        break;
                    }
                }
                if (holder == nullptr || holder->source == Source::File) {
                    return Status::InvArgs;
                }
                auto vmo = (*self.zeros)[holder->segment];
                if (holder->source == Source::Snapshot) {
                    vmo = (*self.snapshots)[holder->segment];
                }
                auto const into = address - holder->vaddr;
                auto const piece = std::min<u64>(left, holder->size - into);
                if (auto const status =
                        ours::vmo_write(vmo, source, holder->vmo_offset + into, usize(piece));
                    status != Status::Ok) {
                    return status;
                }
                source += piece;
                address += piece;
                left -= usize(piece);
            }
            return Status::Ok;
        }
    };

    auto options_for(Perm const perm) -> VmOptions {
        auto options = VmOptions::PermRead | VmOptions::Specific;
        if (perm == Perm::RW) {
            options |= VmOptions::PermWrite;
        }
        if (perm == Perm::RX) {
            options |= VmOptions::PermExecute;
        }
        return options;
    }

    /// A step that failed, said through the caller's diagnostics before its
    /// status goes back, as Zircon's VmarLoader reports each failed map and
    /// clone: the status alone cannot tell which step stopped, nor a system
    /// call's InvArgs from the binder's. |value| is the segment or the
    /// address the step was working on, when there is one.
    auto failed(Request const &request, std::string_view const step, Status const status,
                ustl::Option<u64> const value = {}) -> Status {
        request.diagnostics({.what = step, .name = ours::to_string(status), .value = value});
        return status;
    }
} // namespace

auto start(Request const &request) -> ustl::Result<Started, Status> {
    // |bootstrap| is consumed whatever the answer, so it is owned from here and
    // closed on every way out but the one that hands it to the process.
    Owned bootstrap;
    *bootstrap.out() = request.bootstrap;

    // 1. Judged, read and planned before anything exists: refusing a file then
    // leaves nothing to take down. The judgement is the kernel loader's own.
    if (auto const status = elfload::check_program(
            request.diagnostics, {request.file_bytes.data(), request.file_bytes.size()});
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const program = Program::read(request.file_bytes);
    if (program.is_err()) {
        return ustl::err(failed(request, "the launcher cannot take the program's headers",
                                program.unwrap_err()));
    }
    auto const planned = Plan::build(program.unwrap().segments(), program.unwrap().relro(),
                                     request.file_bytes.size());
    if (planned.is_err()) {
        return ustl::err(
            failed(request, "the program's segments cannot be planned", planned.unwrap_err()));
    }
    auto const &plan = planned.unwrap();
    auto const vdso_program = Program::read(request.vdso_file);
    if (vdso_program.is_err()) {
        return ustl::err(
            failed(request, "the vDSO's headers cannot be read", vdso_program.unwrap_err()));
    }
    auto const image = span_of(program.unwrap().segments());
    auto const vdso_span = span_of(vdso_program.unwrap().segments());
    auto const stack_size = stack_size_of(program.unwrap());
    if (stack_size.is_err()) {
        return ustl::err(failed(request, "the stack PT_GNU_STACK asks for is too large",
                                stack_size.unwrap_err(), program.unwrap().stack_size()));
    }

    // 2. The process. Until process_start, closing the last handle to it -- which
    // every early return below does -- destroys it with everything mapped in
    // it (K23a).
    Owned process;
    Owned root;
    if (auto const status =
            ours::process_create(request.job, request.name.data(), request.name.size(),
                                 ours::ProcessCreateOptions::None, process.out(), root.out());
        status != Status::Ok) {
        return ustl::err(failed(request, "process_create", status));
    }

    // 3. The image's place: one sub-region as long as every segment together,
    // where the kernel finds room, so that the segments keep their places
    // relative to each other wherever it lands. CanMapSpecific, because each
    // segment goes at its own offset in it: what Zircon's
    // VmarLoader::AllocateVmar() asks for, and for the same reason.
    Owned image_region;
    VirtAddr image_base = 0;
    if (auto const status =
            ours::vma_allocate(root.get(),
                               VmOptions::CanMapRead | VmOptions::CanMapWrite |
                                   VmOptions::CanMapExecute | VmOptions::CanMapSpecific,
                               0, usize(image.size), image_region.out(), &image_base);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_allocate of the image's region", status));
    }

    // The VMOs the plan asks for, and the Copy of each partial page.
    std::array<Owned, Plan::MAX_SEGMENTS> snapshot_owners;
    std::array<Owned, Plan::MAX_SEGMENTS> zero_owners;
    std::array<HandleValue, Plan::MAX_SEGMENTS> snapshots{};
    std::array<HandleValue, Plan::MAX_SEGMENTS> zeros{};
    for (u32 index = 0; index < program.unwrap().segments().size(); ++index) {
        auto const snapshot = plan.snapshot(index);
        if (snapshot.size != 0) {
            if (auto const status = ours::vmo_create_child(
                    request.file, ours::VmoChildOptions::SnapshotOnWrite, snapshot.file_offset,
                    snapshot.size, snapshot_owners[index].out());
                status != Status::Ok) {
                return ustl::err(
                    failed(request, "vmo_create_child of a segment's snapshot", status, index));
            }
        }
        if (auto const size = plan.zero_size(index); size != 0) {
            if (auto const status =
                    ours::vmo_create(size, ours::VmoOptions::None, zero_owners[index].out());
                status != Status::Ok) {
                return ustl::err(
                    failed(request, "vmo_create of a segment's zero-fill", status, index));
            }
        }
        snapshots[index] = snapshot_owners[index].get();
        zeros[index] = zero_owners[index].get();
    }
    for (auto const &copy: plan.copies()) {
        if (auto const status =
                ours::vmo_write(zeros[copy.segment], request.file_bytes.data() + copy.file_offset,
                                copy.vmo_offset, usize(copy.size));
            status != Status::Ok) {
            return ustl::err(
                failed(request, "vmo_write of a segment's partial page", status, copy.segment));
        }
    }

    // 4. The vDSO, by segment and sealed, in a sub-region of its own so that
    // its segments keep their places relative to each other: its data read
    // only, its code read and execute. The kernel registers the code alone as
    // this process's vDSO range, and refuses an executable mapping of
    // anything else of it. Then the program's imports, bound to it; the
    // relocations go into the VMOs above, before anything of the program is
    // mapped, so every mapping can be made with its final protection.
    Owned vdso_region;
    VirtAddr vdso_base = 0;
    if (auto const status = ours::vma_allocate(
            root.get(),
            VmOptions::CanMapRead | VmOptions::CanMapExecute | VmOptions::CanMapSpecific, 0,
            usize(vdso_span.size), vdso_region.out(), &vdso_base);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_allocate for the vDSO", status));
    }
    for (auto const &segment: vdso_program.unwrap().segments()) {
        // The VMO lays the vDSO out from its first page, as the sub-region
        // does, so a segment is at the same offset in both.
        auto const first = ustl::mm::align_down(segment.vaddr, PAGE_SIZE);
        auto const end = ustl::mm::align_up(segment.vaddr + segment.mem_size, PAGE_SIZE);
        auto const offset = first - vdso_span.first;
        auto options = VmOptions::PermRead | VmOptions::Specific | VmOptions::Sealed;
        if ((segment.flags & PF_X) != 0) {
            options |= VmOptions::PermExecute;
        }
        VirtAddr at = 0;
        if (auto const status = ours::vma_map(vdso_region.get(), options, usize(offset),
                                              request.vdso, offset, usize(end - first), &at);
            status != Status::Ok) {
            return ustl::err(failed(request, "vma_map of a vDSO segment", status, offset));
        }
    }
    elfload::Image const vdso_image{
        .base = vdso_base,
        .span = usize(vdso_span.size),
        .entry = 0,
        .writer = {},
        .image_base = vdso_span.first,
    };
    auto const binder = elfload::Binder::from(
        request.diagnostics, {request.vdso_file.data(), request.vdso_file.size()}, vdso_image);
    if (binder.is_err()) {
        return ustl::err(binder.unwrap_err());
    }
    Writer writer{.plan = &plan, .first = image.first, .snapshots = &snapshots, .zeros = &zeros};
    auto const bias = image_base - image.first;
    elfload::Image const program_image{
        .base = image_base,
        .span = usize(image.size),
        .entry = program.unwrap().entry() + bias,
        .writer = {.write = &Writer::write, .context = &writer},
        .image_base = image.first,
    };
    if (auto const status = binder.unwrap().bind(
            request.diagnostics, {request.file_bytes.data(), request.file_bytes.size()},
            program_image);
        status != Status::Ok) {
        return ustl::err(status);
    }

    // 5. The program, each mapping at its offset in the sub-region.
    for (auto const &mapping: plan.mappings()) {
        auto vmo = request.file;
        if (mapping.source == Source::Snapshot) {
            vmo = snapshots[mapping.segment];
        }
        if (mapping.source == Source::Zero) {
            vmo = zeros[mapping.segment];
        }
        VirtAddr at = 0;
        if (auto const status = ours::vma_map(image_region.get(), options_for(mapping.perm),
                                              usize(mapping.vaddr - image.first), vmo,
                                              mapping.vmo_offset, usize(mapping.size), &at);
            status != Status::Ok) {
            return ustl::err(failed(request, "vma_map of a segment", status, mapping.vaddr));
        }
    }

    // 6. The stack, in a sub-region of its own: a guard page that allows nothing,
    // then the stack above it. Running off the stack's end faults on the
    // guard instead of writing into whatever the kernel placed below. Both go
    // at chosen offsets in it, hence CanMapSpecific.
    Owned stack_region;
    VirtAddr stack_region_base = 0;
    if (auto const status = ours::vma_allocate(
            root.get(), VmOptions::CanMapRead | VmOptions::CanMapWrite | VmOptions::CanMapSpecific,
            0, STACK_GUARD + stack_size.unwrap(), stack_region.out(), &stack_region_base);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_allocate of the stack's region", status));
    }
    Owned guard;
    VirtAddr guard_at = 0;
    if (auto const status = ours::vma_allocate(stack_region.get(), VmOptions::Specific, 0,
                                               STACK_GUARD, guard.out(), &guard_at);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_allocate of the stack's guard", status));
    }
    Owned stack;
    if (auto const status =
            ours::vmo_create(stack_size.unwrap(), ours::VmoOptions::None, stack.out());
        status != Status::Ok) {
        return ustl::err(failed(request, "vmo_create of the stack", status));
    }
    VirtAddr stack_bottom = 0;
    if (auto const status = ours::vma_map(
            stack_region.get(), VmOptions::PermRead | VmOptions::PermWrite | VmOptions::Specific,
            STACK_GUARD, stack.get(), 0, stack_size.unwrap(), &stack_bottom);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_map of the stack", status));
    }

    // 7. The first thread, and the start. process_start consumes |bootstrap|
    // whatever it answers: into the process, or closed when it fails.
    Owned thread;
    if (auto const status =
            ours::thread_create(process.get(), request.name.data(), request.name.size(),
                                ours::ThreadCreateOptions::None, thread.out());
        status != Status::Ok) {
        return ustl::err(failed(request, "thread_create", status));
    }
    if (auto const status =
            ours::process_start(process.get(), thread.get(), program_image.entry,
                                initial_stack_pointer(stack_bottom + stack_size.unwrap()),
                                bootstrap.release(), vdso_base);
        status != Status::Ok) {
        return ustl::err(failed(request, "process_start", status));
    }

    // Everything else this made goes out of scope and is closed: the mappings
    // hold their VMOs, and the address space holds its regions.
    return ustl::ok(Started{.process = process.release(), .root_vma = root.release()});
}
} // namespace launch
