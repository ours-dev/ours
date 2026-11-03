/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <launch/launch.hpp>
#include <launch/plan.hpp>
#include <launch/program.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>

#include <elfload/binder.hpp>
#include <elfload/image.hpp>
#include <elfload/program.hpp>
#include <ours/clock_page.hpp>
#include <ours/syscall.hpp>
#include <ours/vdso.hpp>
#include <ustl/mem/align.hpp>

namespace launch {
namespace {
    using ours::Status;
    using ours::Uandle;
    using ours::VirtAddr;
    using ours::VmOptions;

    /// The stack a program starts with when it does not ask for a size, and
    /// the unmapped page below every stack that turns running off its end into
    /// a fault. 64 KiB, as the kernel's userboot gives the first program
    /// (USER_STACK_SIZE) and the interface design (3.4) sets.
    constexpr usize DEFAULT_STACK_SIZE = 64 * 1024;
    constexpr usize STACK_GUARD = PAGE_SIZE;

    /// The stack |program| gets: the size it asks for with PT_GNU_STACK,
    /// rounded up to a page, or the default when it asks for none. No cap: a
    /// stack is the program's own address space and memory, taken only as it
    /// is touched, and a size there is no room for fails at vma_allocate.
    /// OutOfRange for a size that would wrap, rounded or with its guard.
    auto stack_size_of(Program const &program) -> ustl::Result<usize, Status> {
        auto const requested = program.stack_size();
        if (requested == 0) {
            return ustl::ok(DEFAULT_STACK_SIZE);
        }
        auto const rounded = ustl::round_up(requested, u64{PAGE_SIZE});
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
            if (m_value != ours::Uandle::Invalid) {
                auto _ = ours::handle_close(m_value);
            }
        }

        /// Where a system call writes the handle it makes.
        [[nodiscard]]
        auto out() -> Uandle * {
            return &m_value;
        }

        [[nodiscard]]
        auto get() const -> Uandle {
            return m_value;
        }

        /// Hands the handle on; this no longer closes it.
        [[nodiscard]]
        auto release() -> Uandle {
            return std::exchange(m_value, ours::Uandle::Invalid);
        }

      private:
        Uandle m_value = ours::Uandle::Invalid;
    };

    /// The x86-64 ABI wants %rsp + 8 sixteen-byte aligned at a function's
    /// entry, as if a call had just pushed its return address. The kernel's
    /// userboot does the same.
    constexpr auto initial_stack_pointer(VirtAddr const top) -> VirtAddr {
        return VirtAddr{std::to_underlying(top) & ~15_usize} - 8;
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
        auto const first = ustl::round_down(lowest, PAGE_SIZE);
        return {.first = first, .size = ustl::round_up(highest, PAGE_SIZE) - first};
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
        std::array<Uandle, Plan::MAX_SEGMENTS> const *snapshots;
        std::array<Uandle, Plan::MAX_SEGMENTS> const *zeros;

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
    /// status goes back: the status alone cannot tell which step stopped, nor
    /// a system call's InvArgs from the binder's. |value| is the segment or
    /// the address the step was working on, when there is one.
    auto failed(Request const &request, std::string_view const step, Status const status,
                ustl::Option<u64> const value = {}) -> Status {
        request.diagnostics({.what = step, .name = ours::to_string(status), .value = value});
        return status;
    }

    /// The program's file, mapped whole into this process for as long as
    /// start() reads it, and unmapped when this goes out of scope -- on every
    /// way out of start(), refusals included, so a launch leaves nothing of
    /// the program in this address space.
    ///
    /// One mapping of the whole VMO, at its size: the headers and tables are
    /// read in place, and the destructor unmaps it. It maps read and execute
    /// rather than read-only, on purpose: see map().
    class MappedFile {
      public:
        MappedFile() = default;
        MappedFile(MappedFile const &) = delete;
        auto operator=(MappedFile const &) -> MappedFile & = delete;

        ~MappedFile() {
            if (m_bytes.empty()) {
                return;
            }
            // The answer is ignored: none is expected (the range is exactly
            // one mapping this made, not sealed, in a region this process
            // holds), and if the region were gone the mapping would be gone
            // with it.
            auto _ = ours::vma_unmap(m_vma, VirtAddr{reinterpret_cast<usize>(m_bytes.data())},
                                     m_bytes.size());
        }

        /// Maps all of |request|'s file into its |vma|, at the size
        /// vmo_get_size answers, with PermRead and PermExecute. The status of
        /// the step that failed, reported as start() reports its own; Ok and
        /// bytes() the file otherwise.
        ///
        /// Execute is asked for although nothing here runs the file, because
        /// vma_map grants it only when the VMO's handle carries Execute
        /// (AccessDenied otherwise), and a handle gets Execute only from
        /// vmex (vmo_replace_as_executable) or, for the vDSO, from the
        /// kernel; a slice keeps its parent's, a snapshot loses it. That
        /// alone does not make the VMO read-only -- vmex takes Write off the
        /// handle it makes, not off the others -- but on the dev image the
        /// one holder of vmex makes only BootFS files executable, and nobody
        /// holds the BootFS writable. So every file that gets past this is
        /// one nobody can write, and that is what makes reading it in place
        /// safe. A VMO the client made itself, and still holds writable,
        /// could change under the parser between a check and the read it
        /// guards -- a double fetch, in the root of the system once the
        /// Launcher lives there (component-framework design, D14 and I6).
        /// The refusal comes before a byte is read, which is the point of
        /// asking here rather than when the code is mapped into the new
        /// process.
        ///
        /// The size is the VMO's, whole pages (vmo_get_size), never one the
        /// caller states: it bounds every offset Plan::build() and the
        /// binder check, and only the kernel can say it truthfully. For a
        /// slice of a BootFS file it runs past the file's end to the end of
        /// its last page, which the BootFS fills with zeros.
        [[nodiscard]]
        auto map(Request const &request) -> Status {
            u64 size = 0;
            if (auto const status = ours::vmo_get_size(request.file, &size); status != Status::Ok) {
                return failed(request, "vmo_get_size of the program's file", status);
            }
            VirtAddr address{};
            if (auto const status =
                    ours::vma_map(request.vma, VmOptions::PermRead | VmOptions::PermExecute, 0,
                                  request.file, 0, usize(size), &address);
                status != Status::Ok) {
                return failed(request, "mapping the program's file into this process", status);
            }
            m_vma = request.vma;
            m_bytes = {reinterpret_cast<u8 const *>(address), usize(size)};
            return Status::Ok;
        }

        /// The file, read in place; empty until map() answered Ok.
        [[nodiscard]]
        auto bytes() const -> std::span<u8 const> {
            return m_bytes;
        }

      private:
        Uandle m_vma = ours::Uandle::Invalid;
        std::span<u8 const> m_bytes;
    };
} // namespace

auto start(Request const &request) -> ustl::Result<Started, Status> {
    // |bootstrap| is consumed whatever the answer, so it is owned from here and
    // closed on every way out but the one that hands it to the process.
    Owned bootstrap;
    *bootstrap.out() = request.bootstrap;

    // 0. The file, mapped read and execute so that one nobody can write is the
    // only kind read at all (MappedFile::map()). Declared before everything
    // below, it is unmapped after all of it, on every way out.
    MappedFile mapped;
    if (auto const status = mapped.map(request); status != Status::Ok) {
        return ustl::err(status);
    }
    auto const file = mapped.bytes();

    // 1. Judged, read and planned before anything exists: refusing a file then
    // leaves nothing to take down. The judgement is the kernel loader's own.
    if (auto const status = elfload::check_program(request.diagnostics, {file.data(), file.size()});
        status != Status::Ok) {
        return ustl::err(status);
    }
    auto const program = Program::read(file);
    if (program.is_err()) {
        return ustl::err(failed(request, "the launcher cannot take the program's headers",
                                program.unwrap_err()));
    }
    auto const planned =
        Plan::build(program.unwrap().segments(), program.unwrap().relro(), file.size());
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
    // segment goes at its own offset in it.
    Owned image_region;
    VirtAddr image_base{};
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
    std::array<Uandle, Plan::MAX_SEGMENTS> snapshots{};
    std::array<Uandle, Plan::MAX_SEGMENTS> zeros{};
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
        if (auto const status = ours::vmo_write(zeros[copy.segment], file.data() + copy.file_offset,
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
    VirtAddr vdso_base{};
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
        auto const first = ustl::round_down(segment.vaddr, PAGE_SIZE);
        auto const end = ustl::round_up(segment.vaddr + segment.mem_size, PAGE_SIZE);
        auto const offset = first - vdso_span.first;
        auto options = VmOptions::PermRead | VmOptions::Specific | VmOptions::Sealed;
        if ((segment.flags & PF_X) != 0) {
            options |= VmOptions::PermExecute;
        }
        VirtAddr at{};
        if (auto const status = ours::vma_map(vdso_region.get(), options, usize(offset),
                                              request.vdso, offset, usize(end - first), &at);
            status != Status::Ok) {
            return ustl::err(failed(request, "vma_map of a vDSO segment", status, offset));
        }
    }

    // The clock page, at the address the vDSO loads. Same place in every
    // process because TimeValues.counter is one published word. Missing
    // when the kernel did not make the VMO; the vDSO then traps.
    if (request.clock_page != Uandle::Invalid) {
        constexpr auto OPTIONS =
            VmOptions::PermRead | VmOptions::Specific | VmOptions::Sealed | VmOptions::MapRange;
        auto const offset = usize(ours::CLOCK_PAGE_BASE - ours::PROCESS_ASPACE_BASE);
        VirtAddr at{};
        if (auto const status = ours::vma_map(root.get(), OPTIONS, offset, request.clock_page, 0,
                                              usize(PAGE_SIZE), &at);
            status != Status::Ok) {
            return ustl::err(failed(request, "vma_map of the clock page", status));
        }
    }
    elfload::Image const vdso_image{
        .base = vdso_base,
        .span = usize(vdso_span.size),
        .entry = ours::VirtAddr{0},
        .writer = {},
        .image_base = VirtAddr{vdso_span.first},
    };
    auto const binder = elfload::Binder::from(
        request.diagnostics, {request.vdso_file.data(), request.vdso_file.size()}, vdso_image);
    if (binder.is_err()) {
        return ustl::err(binder.unwrap_err());
    }
    Writer writer{.plan = &plan, .first = image.first, .snapshots = &snapshots, .zeros = &zeros};
    auto const bias = image_base - VirtAddr{image.first};
    elfload::Image const program_image{
        .base = image_base,
        .span = usize(image.size),
        .entry = VirtAddr{program.unwrap().entry()} + bias,
        .writer = {.write = &Writer::write, .context = &writer},
        .image_base = VirtAddr{image.first},
    };
    if (auto const status =
            binder.unwrap().bind(request.diagnostics, {file.data(), file.size()}, program_image);
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
        VirtAddr at{};
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
    VirtAddr stack_region_base{};
    if (auto const status = ours::vma_allocate(
            root.get(), VmOptions::CanMapRead | VmOptions::CanMapWrite | VmOptions::CanMapSpecific,
            0, STACK_GUARD + stack_size.unwrap(), stack_region.out(), &stack_region_base);
        status != Status::Ok) {
        return ustl::err(failed(request, "vma_allocate of the stack's region", status));
    }
    Owned guard;
    VirtAddr guard_at{};
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
    VirtAddr stack_bottom{};
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
                                bootstrap.release(), std::to_underlying(vdso_base));
        status != Status::Ok) {
        return ustl::err(failed(request, "process_start", status));
    }

    // Everything else this made goes out of scope and is closed: the mappings
    // hold their VMOs, and the address space holds its regions.
    return ustl::ok(Started{.process = process.release(), .root_vma = root.release()});
}
} // namespace launch
