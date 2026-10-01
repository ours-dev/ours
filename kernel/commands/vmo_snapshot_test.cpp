/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// `vmosnapshot.test`: check what a VmCowPages that hangs under another one
// sees, by building small trees by hand and asking their cursors for pages.
//
// What it covers, and only that:
//
//   - a read of a node with a parent answers with the nearest ancestor's page,
//     on loan and read-only, and allocates nothing in the node;
//   - what an ancestor does not have, and what lies past a node's limit,
//     reads as the shared zero page -- which is checked to be all zeros, so
//     this is also the one test of ZERO_PAGE;
//   - the parent offset and the limit compose down a chain of nodes, each
//     with its own;
//   - a hidden node hands its pages out read-only and refuses a writer;
//   - every node of a tree answers the same lock, and a node that has lost its
//     handle to its parent still has it;
//   - a node with no relatives still answers a read of a hole as it always
//     did, with a page of its own.
//
// What it cannot cover yet: a node's own page winning over its ancestors'
// (a node with a parent has no way to get a page of its own until a write
// copies one), a write, a clone, a destruction that merges. Each of those is a
// later step of .agents/tasks/vmo-snapshot-child and adds its checks here.
//
// Why a console command: the tree has no in-kernel unittest runtime yet (see
// usercopy.test), and these are kernel objects that no host test can build.
//
// The name has no underscore for the reason usercopy.test's does not: gktl's
// command table does not encode '_'.
#include <ours/arch/aspace_layout.hpp>
#include <ours/cpu.hpp>
#include <ours/lockdep.hpp>
#include <ours/mm/gaf.hpp>
#include <ours/mm/memmap.hpp>
#include <ours/mm/page_request.hpp>
#include <ours/mm/physmap.hpp>
#include <ours/mm/vm_area.hpp>
#include <ours/mm/vm_aspace.hpp>
#include <ours/mm/vm_cow_pages.hpp>
#include <ours/mm/vm_fault.hpp>
#include <ours/mm/vm_mapping.hpp>
#include <ours/mm/vm_object_paged.hpp>
#include <ours/mm/vmm.hpp>
#include <ours/task/auto_preempt_disabler.hpp>
#include <ours/task/thread.hpp>
#include <ours/time/clock.hpp>

#include <atomic>

#include <cstring>

#include <arch/spin_hint.hpp>
#include <gktl/command.hpp>
#include <heap/new.hpp>
#include <ktl/result.hpp>
#include <logz4/log.hpp>
#include <user_copy/user_copy.hpp>
#include <ustl/rc.hpp>

namespace ours {
namespace {
    using mm::VmCowPages;
    using mm::VmPage;
    using Node = ustl::Arc<VmCowPages>;
    using Found = VmCowPages::Cursor::RequireResult;

    struct Tally {
        usize passed = 0;
        usize failed = 0;
    };

    auto verdict(Tally &tally, bool const ok, char const *const name) -> void {
        if (ok) {
            tally.passed += 1;
            log::info("vmosnapshot.test: pass  {}", name);
            return;
        }
        tally.failed += 1;
        log::error("vmosnapshot.test: FAIL  {}", name);
    }

    // The analysis cannot tie `node->lock()` to the `m_lock` that make_cursor()
    // requires: it sees two expressions through two calls of Arc::operator->().
    // The lock is taken on the line above and held to the end of the function.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto lookup(Node &node, bool const will_write, usize const page_index) -> ktl::Result<Found> {
        Guard guard{node->lock()};
        auto cursor = node->make_cursor(page_index * PAGE_SIZE, PAGE_SIZE);
        if (!cursor) {
            return ktl::err(cursor.unwrap_err());
        }
        mm::MultiPageRequest request;
        return cursor->require_page(will_write, 1, &request);
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_pages(Node &node) -> usize {
        Guard guard{node->lock()};
        return node->nr_pages_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_children(Node &node) -> usize {
        Guard guard{node->lock()};
        return node->nr_children_locked();
    }

    // The clone helpers hold the tree's lock through `tree`, any node in it, and
    // look at another node of the same tree through a plain pointer: the
    // analysis cannot tell that the two are under one mutex.
    TA_NO_THREAD_SAFETY_ANALYSIS
    auto parent_of(Node &node) -> VmCowPages const * {
        Guard guard{node->lock()};
        return node->parent_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto parent_of_node(Node &tree, VmCowPages const *const node) -> VmCowPages const * {
        Guard guard{tree->lock()};
        return node->parent_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto pages_in(Node &tree, VmCowPages const *const node) -> usize {
        Guard guard{tree->lock()};
        return node->nr_pages_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto children_in(Node &tree, VmCowPages const *const node) -> usize {
        Guard guard{tree->lock()};
        return node->nr_children_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto clone_raw(Node &source, u64 const offset, usize const size, Node &out) -> Status {
        Guard guard{source->lock()};
        return source->create_clone_locked(offset, size, ~u64{0}, &out);
    }

    auto clone_of(Node &source, usize const offset_pages, usize const pages, Node &out) -> Status {
        return clone_raw(source, offset_pages * PAGE_SIZE, pages * PAGE_SIZE, out);
    }

    auto bytes_of(VmPage *const page) -> u8 * {
        return mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(page));
    }

    /// Writes |marker| over a whole page of |node| through the cursor, the way
    /// a write would, and answers the page.
    auto fill(Node &node, usize const page_index, u8 const marker) -> VmPage * {
        auto found = lookup(node, true, page_index);
        if (!found) {
            return nullptr;
        }
        std::memset(bytes_of(found->page), marker, PAGE_SIZE);
        return found->page;
    }

    auto holds_only(VmPage *const page, u8 const marker) -> bool {
        auto const *const bytes = bytes_of(page);
        for (usize i = 0; i < PAGE_SIZE; ++i) {
            if (bytes[i] != marker) {
                return false;
            }
        }
        return true;
    }

    /// The page |node| answers a read of |page_index| with, read-only and on
    /// loan from |lender|; and nothing was allocated for it in |node|.
    auto reads_lenders_page(Node &node, usize const page_index, VmPage *const lender) -> bool {
        auto const before = nr_pages(node);
        auto found = lookup(node, false, page_index);
        return found && found->page == lender && !found->writable && nr_pages(node) == before;
    }

    auto reads_zero_page(Node &node, usize const page_index) -> bool {
        auto const before = nr_pages(node);
        auto found = lookup(node, false, page_index);
        return found && found->page == mm::get_zero_page() && !found->writable &&
               nr_pages(node) == before;
    }

    auto make(Node &out, usize const pages) -> bool {
        return VmCowPages::create(mm::GAF_USER, pages * PAGE_SIZE, &out) == Status::Ok;
    }

    auto make_child(Node const &parent, usize const parent_page, usize const limit_pages,
                    usize const pages, bool const hidden, Node &out) -> bool {
        auto flags = VmCowPages::Flags::None;
        if (hidden) {
            flags = VmCowPages::Flags::Hidden;
        }
        return VmCowPages::create_child(parent, parent_page * PAGE_SIZE, limit_pages * PAGE_SIZE,
                                        pages * PAGE_SIZE, flags, &out) == Status::Ok;
    }

    // Whether the page at |va| is mapped, and with what.
    auto pte_flags(ustl::Arc<mm::VmAspace> &aspace, VirtAddr const va)
        -> ustl::Option<mm::MmuFlags> {
        PhysAddr pa = 0;
        mm::MmuFlags flags{};
        if (aspace->arch_aspace().query(va, &pa, &flags) != Status::Ok) {
            return ustl::NONE;
        }
        return ustl::some(flags);
    }

    auto is_writable(ustl::Arc<mm::VmAspace> &aspace, VirtAddr const va) -> bool {
        auto const flags = pte_flags(aspace, va);
        return flags.is_some() && !!(*flags & mm::MmuFlags::Writable);
    }

    auto is_executable(ustl::Arc<mm::VmAspace> &aspace, VirtAddr const va) -> bool {
        auto const flags = pte_flags(aspace, va);
        return flags.is_some() && !!(*flags & mm::MmuFlags::Executable);
    }

    auto is_readable(ustl::Arc<mm::VmAspace> &aspace, VirtAddr const va) -> bool {
        auto const flags = pte_flags(aspace, va);
        return flags.is_some() && !!(*flags & mm::MmuFlags::Readable);
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto remove_write(ustl::Arc<mm::VmObjectPaged> &vmo, u64 const offset, u64 const len) -> void {
        Guard guard{vmo->lock()};
        vmo->remove_write_locked(offset, len);
    }

    // Removing Write from the page tables of the mappings of an object, from the
    // object's side: which pages it reaches, what it leaves, and what it does
    // not touch.
    auto check_remove_write(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the remove-write checks is made");
            return;
        }
        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto RW_AT = USER_ASPACE_BASE + MB(16);
        constexpr auto RO_AT = RW_AT + 8 * PAGE_SIZE;
        constexpr auto LAZY_AT = RO_AT + 8 * PAGE_SIZE;

        ustl::Arc<mm::VmObjectPaged> vmo;
        ustl::Arc<mm::VmObjectPaged> ro_vmo;
        ustl::Arc<mm::VmObjectPaged> lazy_vmo;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &vmo) == Status::Ok &&
                          mm::VmObjectPaged::create(mm::GAF_USER, 2 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &ro_vmo) == Status::Ok &&
                          mm::VmObjectPaged::create(mm::GAF_USER, 2 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &lazy_vmo) == Status::Ok;
        verdict(tally, made, "three VMOs for the remove-write checks are made");
        if (!made) {
            return;
        }

        // Committed, so the page tables hold entries to change.
        auto writable = root->map_with_vmo(mm::VmPlacement::at(RW_AT), 3 * PAGE_SIZE, 0, rw, PERMS,
                                           vmo, mm::VmMapOption::Populate, "remove-write rw");
        auto read_only = root->map_with_vmo(mm::VmPlacement::at(RO_AT), 2 * PAGE_SIZE, 0, ro, PERMS,
                                            ro_vmo, mm::VmMapOption::Populate, "remove-write ro");
        // Mapped, not committed: no entries yet.
        auto lazy = root->map_with_vmo(mm::VmPlacement::at(LAZY_AT), 2 * PAGE_SIZE, 0, rw, PERMS,
                                       lazy_vmo, mm::VmMapOption::None, "remove-write lazy");
        verdict(tally, writable.is_ok() && read_only.is_ok() && lazy.is_ok(),
                "the mappings are made");
        if (!(writable.is_ok() && read_only.is_ok() && lazy.is_ok())) {
            return;
        }
        verdict(
            tally,
            is_writable(aspace, RW_AT) && is_writable(aspace, RW_AT + PAGE_SIZE) &&
                is_writable(aspace, RW_AT + 2 * PAGE_SIZE) && !is_writable(aspace, RO_AT) &&
                is_readable(aspace, RO_AT),
            "the committed mappings have entries: writable in the writable one, not in the other");

        // One page of three. What is left is exactly what the entry had, less
        // Write: nothing else changes, and above all nothing is added, which is
        // what makes this different from protect().
        auto const before = pte_flags(aspace, RW_AT + PAGE_SIZE);
        remove_write(vmo, PAGE_SIZE, PAGE_SIZE);
        verdict(
            tally,
            is_writable(aspace, RW_AT) && !is_writable(aspace, RW_AT + PAGE_SIZE) &&
                is_readable(aspace, RW_AT + PAGE_SIZE) &&
                is_writable(aspace, RW_AT + 2 * PAGE_SIZE),
            "removing Write from one page takes it from that page only, and leaves it readable");
        auto const after = pte_flags(aspace, RW_AT + PAGE_SIZE);
        verdict(tally,
                before.is_some() && after.is_some() &&
                    *after == (*before & ~mm::MmuFlags::Writable),
                "and leaves the entry exactly as it was, less Write");

        // Idempotent, and a range that names nothing the mapping shows changes nothing.
        // Doing it again is a restriction of an entry that already lacks what is taken,
        // which must leave it as it is, not set it to what is kept.
        remove_write(vmo, PAGE_SIZE, PAGE_SIZE);
        remove_write(vmo, 3 * PAGE_SIZE, 4 * PAGE_SIZE);
        auto const again = pte_flags(aspace, RW_AT + PAGE_SIZE);
        verdict(tally,
                is_writable(aspace, RW_AT) && is_writable(aspace, RW_AT + 2 * PAGE_SIZE) &&
                    again.is_some() && *again == *after,
                "doing it again, or for a range past the object, changes nothing at all");

        // The other objects' mappings are not touched by this one's.
        remove_write(ro_vmo, 0, 2 * PAGE_SIZE);
        verdict(tally,
                is_writable(aspace, RW_AT) && !is_writable(aspace, RO_AT) &&
                    is_readable(aspace, RO_AT),
                "a read-only mapping keeps what it has, and another object's is not reached");

        // A mapping with no entries yet: nothing to change, and nothing goes wrong.
        remove_write(lazy_vmo, 0, 2 * PAGE_SIZE);
        verdict(tally, !pte_flags(aspace, LAZY_AT).is_some(),
                "a mapping with no entries is left with none");

        // The whole object, with a length that reaches the end of the address space.
        remove_write(vmo, 0, ~u64{0});
        verdict(tally,
                !is_writable(aspace, RW_AT) && !is_writable(aspace, RW_AT + PAGE_SIZE) &&
                    !is_writable(aspace, RW_AT + 2 * PAGE_SIZE) && is_readable(aspace, RW_AT) &&
                    is_readable(aspace, RW_AT + 2 * PAGE_SIZE),
                "the whole object, with a length that does not fit, takes Write from every page");

        // Two mappings of one object that show different parts of it: the whole,
        // and two pages from its page 1. A range is in the object's bytes, so it
        // lands on each mapping at that mapping's own offset.
        ustl::Arc<mm::VmObjectPaged> shared_vmo;
        constexpr auto FULL_AT = LAZY_AT + 8 * PAGE_SIZE;
        constexpr auto WIN_AT = FULL_AT + 8 * PAGE_SIZE;
        auto const shared_made =
            mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                      &shared_vmo) == Status::Ok;
        verdict(tally, shared_made, "an object to map twice is made");
        if (shared_made) {
            auto full =
                root->map_with_vmo(mm::VmPlacement::at(FULL_AT), 3 * PAGE_SIZE, 0, rw, PERMS,
                                   shared_vmo, mm::VmMapOption::Populate, "remove-write full");
            auto window =
                root->map_with_vmo(mm::VmPlacement::at(WIN_AT), 2 * PAGE_SIZE, PAGE_SIZE, rw, PERMS,
                                   shared_vmo, mm::VmMapOption::Populate, "remove-write window");
            verdict(tally, full.is_ok() && window.is_ok(), "the whole and the window are mapped");
            if (full.is_ok() && window.is_ok()) {
                remove_write(shared_vmo, 2 * PAGE_SIZE, PAGE_SIZE);
                verdict(tally,
                        is_writable(aspace, FULL_AT) && is_writable(aspace, FULL_AT + PAGE_SIZE) &&
                            !is_writable(aspace, FULL_AT + 2 * PAGE_SIZE) &&
                            is_writable(aspace, WIN_AT) && !is_writable(aspace, WIN_AT + PAGE_SIZE),
                        "the object's page 2 loses Write in both mappings, at the address each "
                        "shows it at");
            }
        }

        verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");
    }

    // Puts the calling thread on |next| and loads its page tables, as usercopy.test
    // does, so that soft_fault() resolves a fault against the test's address
    // space. Preemption is off across the two steps: the scheduler loads the
    // page tables from the thread's address space on every switch.
    auto attach(mm::VmAspace *const next) -> mm::VmAspace * {
        task::AutoPreemptDisabler no_preemption;
        auto *const thread = task::Thread::Current::get();
        auto *const previous = thread->aspace();
        thread->set_aspace(next);
        mm::VmAspace::switch_aspace(previous, next);
        return previous;
    }

    auto entry_pa(ustl::Arc<mm::VmAspace> &aspace, VirtAddr const va) -> PhysAddr {
        PhysAddr pa = 0;
        mm::MmuFlags flags{};
        if (aspace->arch_aspace().query(va, &pa, &flags) != Status::Ok) {
            return 0;
        }
        return pa;
    }

    // Protection only narrows the page table entries, and a permission a mapping
    // is given is added by the first access that needs it, in the fault path:
    // which is what keeps a page that was made read-only for a reason from being
    // made writable by anyone asking for the permissions the mapping has.
    auto check_narrowing_protect(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test protect", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the protect checks is made");
            return;
        }
        auto *const previous = attach(aspace.as_ptr_mut());

        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        auto const ro = mm::MmuFlags::Readable | mm::MmuFlags::User;
        constexpr auto AT = USER_ASPACE_BASE + MB(16);
        constexpr auto WRITE = mm::VmfCause::Write;

        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 2 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &vmo) == Status::Ok;
        verdict(tally, made, "an object for the protect checks is made");
        if (made) {
            static constexpr u8 PATTERN[4] = {0xde, 0xad, 0xbe, 0xef};
            auto mapped = root->map_with_vmo(mm::VmPlacement::at(AT), 2 * PAGE_SIZE, 0, rw, PERMS,
                                             vmo, mm::VmMapOption::Populate, "protect checks");
            verdict(tally, mapped.is_ok() && vmo->write(PATTERN, 0, sizeof(PATTERN)) == Status::Ok,
                    "it is mapped writable and holds a pattern");
            if (mapped.is_ok()) {
                auto mapping = mapped.unwrap();
                auto const pa0 = entry_pa(aspace, AT);

                // A page made read-only for a reason stays read-only through a protect()
                // that asks for what the mapping already allows.
                remove_write(vmo, 0, PAGE_SIZE);
                verdict(tally, mapping->protect(AT, 2 * PAGE_SIZE, rw) == Status::Ok,
                        "protect() to the permissions the mapping has is accepted");
                verdict(tally, !is_writable(aspace, AT) && is_writable(aspace, AT + PAGE_SIZE),
                        "and does not give Write back to the page that lost it");

                // The permission is added by the access that needs it: same frame, same bytes.
                verdict(tally, task::Thread::Current::soft_fault(AT, WRITE) == Status::Ok,
                        "a write to the read-only page is resolved by the fault path");
                u8 back[4] = {};
                verdict(tally,
                        is_writable(aspace, AT) && entry_pa(aspace, AT) == pa0 &&
                            vmo->read(back, 0, sizeof(back)) == Status::Ok &&
                            std::memcmp(back, PATTERN, sizeof(back)) == 0,
                        "which leaves it writable, on the same frame, with the bytes it had");

                // Narrowing takes effect at once, on every entry.
                verdict(tally, mapping->protect(AT, 2 * PAGE_SIZE, ro) == Status::Ok,
                        "protect() to read-only is accepted");
                verdict(tally,
                        !is_writable(aspace, AT) && !is_writable(aspace, AT + PAGE_SIZE) &&
                            is_readable(aspace, AT) && is_readable(aspace, AT + PAGE_SIZE),
                        "and takes Write from every entry at once");
                verdict(tally,
                        task::Thread::Current::soft_fault(AT + PAGE_SIZE, WRITE) != Status::Ok &&
                            !is_writable(aspace, AT + PAGE_SIZE),
                        "a write to a mapping that no longer allows it is refused, and the entry "
                        "stays");

                // Widening again does not touch the entries; the write does.
                verdict(tally,
                        mapping->protect(AT, 2 * PAGE_SIZE, rw) == Status::Ok &&
                            !is_writable(aspace, AT + PAGE_SIZE),
                        "protect() back to read-write leaves the entries read-only");
                verdict(tally,
                        task::Thread::Current::soft_fault(AT + PAGE_SIZE, WRITE) == Status::Ok &&
                            is_writable(aspace, AT + PAGE_SIZE) && !is_writable(aspace, AT),
                        "and the first write to a page adds Write to that page only");
            }
        }

        // Execute is added the same way: an instruction fetch from a page whose entry
        // lost it is what puts it back, after the mapping has been given it again.
        ustl::Arc<mm::VmObjectPaged> code;
        constexpr auto CODE_AT = AT + 8 * PAGE_SIZE;
        auto const rx = mm::MmuFlags::Readable | mm::MmuFlags::Executable | mm::MmuFlags::User;
        auto const code_made = mm::VmObjectPaged::create(mm::GAF_USER, PAGE_SIZE,
                                                         mm::VmoFlags::Lazy, &code) == Status::Ok;
        if (code_made) {
            auto code_mapped = root->map_with_vmo(mm::VmPlacement::at(CODE_AT), PAGE_SIZE, 0, rx,
                                                  PERMS, code, mm::VmMapOption::Populate, "code");
            verdict(tally, code_mapped.is_ok() && is_executable(aspace, CODE_AT),
                    "an executable mapping has an executable entry");
            if (code_mapped.is_ok()) {
                auto code_mapping = code_mapped.unwrap();
                verdict(tally,
                        code_mapping->protect(CODE_AT, PAGE_SIZE, ro) == Status::Ok &&
                            !is_executable(aspace, CODE_AT) &&
                            code_mapping->protect(CODE_AT, PAGE_SIZE, rx) == Status::Ok &&
                            !is_executable(aspace, CODE_AT),
                        "protect() to read-only takes Execute from the entry and back to "
                        "executable does not give it back");
                verdict(tally,
                        task::Thread::Current::soft_fault(CODE_AT, mm::VmfCause::Instruction) ==
                                Status::Ok &&
                            is_executable(aspace, CODE_AT) && !is_writable(aspace, CODE_AT),
                        "an instruction fetch adds Execute to the entry, and no Write");
            }
        }

        attach(previous);
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the protect checks is destroyed");
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto nr_attached(ustl::Arc<mm::VmCowPages> const &cow) -> usize {
        Guard guard{const_cast<ustl::Arc<mm::VmCowPages> &>(cow)->lock()};
        return cow->nr_attached_locked();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS
    auto cow_remove_write(ustl::Arc<mm::VmCowPages> const &cow, u64 const offset, u64 const len)
        -> void {
        Guard guard{const_cast<ustl::Arc<mm::VmCowPages> &>(cow)->lock()};
        const_cast<ustl::Arc<mm::VmCowPages> &>(cow)->remove_write_locked(offset, len);
    }

    // Objects over the same pages: a slice shares its parent's pages and has its own
    // mappings, and an operation on the pages reaches all of them.
    auto check_attachments(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test attach", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the attachment checks is made");
            return;
        }
        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        constexpr auto WHOLE_AT = USER_ASPACE_BASE + MB(16);
        constexpr auto SLICE_AT = WHOLE_AT + 8 * PAGE_SIZE;
        constexpr auto FIRST_AT = SLICE_AT + 8 * PAGE_SIZE;

        ustl::Arc<mm::VmObjectPaged> whole;
        ustl::Arc<mm::VmObjectPaged> slice;
        ustl::Arc<mm::VmObjectPaged> gone;
        auto const made =
            mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy, &whole) ==
                Status::Ok &&
            whole->create_child_slice(PAGE_SIZE, 2 * PAGE_SIZE, &slice) == Status::Ok &&
            whole->create_child_slice(0, PAGE_SIZE, &gone) == Status::Ok;
        verdict(tally, made, "an object, a slice of its last two pages, and a slice of its first");
        if (!made) {
            return;
        }
        auto const cow = whole->cow_pages();
        verdict(tally,
                slice->cow_pages() == cow && gone->cow_pages() == cow && nr_attached(cow) == 3,
                "all three are over the same pages, and the pages know all three");
        gone = nullptr;
        verdict(tally, nr_attached(cow) == 2, "an object that dies is off the list of the pages");

        auto whole_map =
            root->map_with_vmo(mm::VmPlacement::at(WHOLE_AT), 3 * PAGE_SIZE, 0, rw, PERMS, whole,
                               mm::VmMapOption::Populate, "attach whole");
        auto slice_map =
            root->map_with_vmo(mm::VmPlacement::at(SLICE_AT), 2 * PAGE_SIZE, 0, rw, PERMS, slice,
                               mm::VmMapOption::Populate, "attach slice");
        // A slice of the first page, mapped too: it is over the pages but shows none the
        // operation below is about, and it is made and dropped while the mappings live.
        ustl::Arc<mm::VmObjectPaged> first;
        auto const first_made = whole->create_child_slice(0, PAGE_SIZE, &first) == Status::Ok;
        verdict(tally, first_made, "a slice of the first page is made");
        if (!first_made) {
            return;
        }
        auto first_map = root->map_with_vmo(mm::VmPlacement::at(FIRST_AT), PAGE_SIZE, 0, rw, PERMS,
                                            first, mm::VmMapOption::Populate, "attach first");
        verdict(tally, whole_map.is_ok() && slice_map.is_ok() && first_map.is_ok(),
                "the object and both slices are mapped");
        if (!(whole_map.is_ok() && slice_map.is_ok() && first_map.is_ok())) {
            return;
        }

        // The pages' second page: the object's page 1, the slice's page 0.
        cow_remove_write(cow, PAGE_SIZE, PAGE_SIZE);
        verdict(tally,
                is_writable(aspace, WHOLE_AT) && !is_writable(aspace, WHOLE_AT + PAGE_SIZE) &&
                    is_writable(aspace, WHOLE_AT + 2 * PAGE_SIZE) &&
                    !is_writable(aspace, SLICE_AT) && is_writable(aspace, SLICE_AT + PAGE_SIZE),
                "an operation on the pages takes Write from the page in the object's mapping "
                "and in the slice's, each at its own offset, and from no other");
        verdict(tally, is_writable(aspace, FIRST_AT),
                "and not from a slice that does not show that page");

        cow_remove_write(cow, 0, ~u64{0});
        verdict(tally,
                !is_writable(aspace, WHOLE_AT) && !is_writable(aspace, WHOLE_AT + 2 * PAGE_SIZE) &&
                    !is_writable(aspace, SLICE_AT + PAGE_SIZE) && !is_writable(aspace, FIRST_AT),
                "the whole range takes it from every mapping of every object over the pages");

        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the attachment checks is destroyed");
    }

    auto holds_bytes(Node &node, u8 const (&bytes)[4]) -> bool {
        auto found = lookup(node, false, 0);
        return found && std::memcmp(mm::PhysMap::phys_to_virt<u8>(mm::frame_to_phys(found->page)),
                                    bytes, sizeof(bytes)) == 0;
    }

    // The clone of the pages of a mapped object, end to end: Write comes off the
    // object's mappings when the pages become shared, and the first write through
    // the mapping gets a page of its own, while the clone keeps the old one.
    auto check_clone_of_mapped_object(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test clone", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the clone checks is made");
            return;
        }
        auto *const previous = attach(aspace.as_ptr_mut());

        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        constexpr auto AT = USER_ASPACE_BASE + MB(16);

        ustl::Arc<mm::VmObjectPaged> vmo;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &vmo) == Status::Ok;
        verdict(tally, made, "an object to clone is made");
        if (made) {
            static constexpr u8 BEFORE[4] = {0x11, 0x22, 0x33, 0x44};
            static constexpr u8 AFTER[4] = {0xaa, 0xbb, 0xcc, 0xdd};
            auto mapped = root->map_with_vmo(mm::VmPlacement::at(AT), 3 * PAGE_SIZE, 0, rw, PERMS,
                                             vmo, mm::VmMapOption::Populate, "clone checks");
            verdict(tally, mapped.is_ok() && vmo->write(BEFORE, 0, sizeof(BEFORE)) == Status::Ok,
                    "it is mapped writable and holds a pattern");
            if (mapped.is_ok()) {
                auto const old_pa = entry_pa(aspace, AT);
                Node pages = vmo->cow_pages();
                Node clone;
                verdict(tally, clone_of(pages, 0, 3, clone) == Status::Ok,
                        "the pages of the object are cloned");
                verdict(tally,
                        !is_writable(aspace, AT) && !is_writable(aspace, AT + PAGE_SIZE) &&
                            !is_writable(aspace, AT + 2 * PAGE_SIZE) && is_readable(aspace, AT) &&
                            entry_pa(aspace, AT) == old_pa,
                        "Write is off every entry of the object's mapping, on the frames it had");

                // The mapping still allows the write; the fault path resolves it with a page
                // of the object's own.
                verdict(tally,
                        task::Thread::Current::soft_fault(AT, mm::VmfCause::Write) == Status::Ok,
                        "a write through the mapping is resolved");
                verdict(tally,
                        is_writable(aspace, AT) && entry_pa(aspace, AT) != old_pa &&
                            !is_writable(aspace, AT + PAGE_SIZE),
                        "the entry is writable on a frame of its own, and the other pages are not");
                u8 seen[4] = {};
                verdict(tally,
                        vmo->read(seen, 0, sizeof(seen)) == Status::Ok &&
                            std::memcmp(seen, BEFORE, sizeof(seen)) == 0,
                        "the object's page holds the bytes it had, copied");

                // Writes to the object's page do not reach the clone's.
                verdict(tally, vmo->write(AFTER, 0, sizeof(AFTER)) == Status::Ok,
                        "the object writes its page");
                auto const mine = lookup(pages, false, 0);
                auto const theirs = lookup(clone, false, 0);
                verdict(tally,
                        mine && theirs && mine->page != theirs->page && mine->writable &&
                            !theirs->writable && holds_bytes(pages, AFTER) &&
                            holds_bytes(clone, BEFORE),
                        "and the clone still sees the bytes from before the clone, in the page "
                        "the object gave up");
            }
        }

        attach(previous);
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the clone checks is destroyed");
    }

    // Four bytes at the start of a page of an object, as a number: what the
    // clone checks write and read.
    auto peek(ustl::Arc<mm::VmObjectPaged> &vmo, usize const page_index) -> u32 {
        u32 word = 0xffff'ffff;
        if (vmo->read(&word, page_index * PAGE_SIZE, sizeof(word)) != Status::Ok) {
            return 0xdead'0001;
        }
        return word;
    }

    auto poke(ustl::Arc<mm::VmObjectPaged> &vmo, usize const page_index, u32 const word) -> bool {
        return vmo->write(&word, page_index * PAGE_SIZE, sizeof(word)) == Status::Ok;
    }

    // The clone of an object, through the object's own interface: what it reads, what
    // is not shared after a write on either side, what it may not see, and what is
    // refused.
    auto check_create_clone(Tally &tally) -> void {
        ustl::Arc<mm::VmObjectPaged> parent;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &parent) == Status::Ok;
        verdict(tally, made, "an object to clone is made");
        if (!made) {
            return;
        }
        verdict(tally, poke(parent, 0, 0x1100'0001) && poke(parent, 1, 0x1200'0002),
                "it holds two pages and a hole");

        ustl::Arc<mm::VmObjectPaged> child;
        verdict(tally, parent->create_clone(0, 3 * PAGE_SIZE, &child) == Status::Ok,
                "it is cloned");
        verdict(tally,
                peek(child, 0) == 0x1100'0001 && peek(child, 1) == 0x1200'0002 &&
                    peek(child, 2) == 0,
                "the clone reads what the object held: its pages, and zero for the hole");

        // Neither sees the other's writes after the clone.
        verdict(tally, poke(child, 0, 0xc000'0001) && poke(parent, 1, 0x9900'0002),
                "the clone writes its page 0 and the object its page 1");
        verdict(tally,
                peek(child, 0) == 0xc000'0001 && peek(parent, 0) == 0x1100'0001 &&
                    peek(parent, 1) == 0x9900'0002 && peek(child, 1) == 0x1200'0002,
                "each reads its own write and the other's page as it was at the clone");

        // A later clone sees the object as it is then; a second clone of the same object.
        ustl::Arc<mm::VmObjectPaged> second;
        verdict(tally, parent->create_clone(PAGE_SIZE, 2 * PAGE_SIZE, &second) == Status::Ok,
                "the object is cloned again, from its page 1");
        verdict(tally,
                peek(second, 0) == 0x9900'0002 && peek(second, 1) == 0 &&
                    peek(child, 1) == 0x1200'0002,
                "the second clone reads the object as it is now, and the first is unchanged");

        // A clone longer than the object reads zero past its end.
        ustl::Arc<mm::VmObjectPaged> longer;
        verdict(tally, parent->create_clone(0, 5 * PAGE_SIZE, &longer) == Status::Ok,
                "a clone longer than the object is made");
        verdict(tally,
                peek(longer, 0) == 0x1100'0001 && peek(longer, 3) == 0 && peek(longer, 4) == 0 &&
                    poke(longer, 4, 0x4400'0004) && peek(longer, 4) == 0x4400'0004 &&
                    peek(parent, 2) == 0,
                "it reads zero past the object's end, and a write there is its own");

        // A clone of a clone.
        ustl::Arc<mm::VmObjectPaged> grand;
        verdict(tally,
                child->create_clone(0, 3 * PAGE_SIZE, &grand) == Status::Ok &&
                    peek(grand, 0) == 0xc000'0001 && peek(grand, 1) == 0x1200'0002,
                "a clone of a clone reads what the clone held");

        // Refusals.
        ustl::Arc<mm::VmObjectPaged> none;
        verdict(tally,
                parent->create_clone(1, PAGE_SIZE, &none) == Status::InvArgs &&
                    parent->create_clone(0, 1, &none) == Status::InvArgs &&
                    parent->create_clone(4 * PAGE_SIZE, PAGE_SIZE, &none) == Status::InvArgs,
                "an offset or size that is not page aligned, and an offset past the end, are "
                "refused");
        ustl::Arc<mm::VmObjectPaged> pinned;
        verdict(tally,
                mm::VmObjectPaged::create(mm::GAF_KERNEL, PAGE_SIZE, mm::VmoFlags::Pinned,
                                          &pinned) == Status::Ok &&
                    pinned->create_clone(0, PAGE_SIZE, &none) == Status::BadState,
                "an object with pinned pages is refused: BadState");

        // A slice: the clone of a window does not see the pages past it.
        ustl::Arc<mm::VmObjectPaged> whole;
        ustl::Arc<mm::VmObjectPaged> slice;
        ustl::Arc<mm::VmObjectPaged> of_slice;
        verdict(tally,
                mm::VmObjectPaged::create(mm::GAF_USER, 4 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                          &whole) == Status::Ok &&
                    poke(whole, 0, 0x2100'0000) && poke(whole, 1, 0x2200'0000) &&
                    poke(whole, 2, 0x2300'0000) && poke(whole, 3, 0x2400'0000) &&
                    whole->create_child_slice(PAGE_SIZE, 2 * PAGE_SIZE, &slice) == Status::Ok,
                "an object and a slice of its pages 1 and 2 are made");
        verdict(tally, slice->create_clone(0, 3 * PAGE_SIZE, &of_slice) == Status::Ok,
                "the slice is cloned, to more than it shows");
        verdict(tally,
                peek(of_slice, 0) == 0x2200'0000 && peek(of_slice, 1) == 0x2300'0000 &&
                    peek(of_slice, 2) == 0,
                "the clone reads the slice's pages, and zero past the slice, not the object's "
                "page after it");
        verdict(tally,
                poke(whole, 1, 0x7700'0000) && poke(slice, 1, 0x7800'0000) &&
                    peek(of_slice, 0) == 0x2200'0000 && peek(of_slice, 1) == 0x2300'0000,
                "a write through the object or the slice does not reach the clone of the slice");
    }

    // The clone behind a mapping: a read maps the shared page read-only, on the frame
    // the object holds; a write to it, or to a page not yet mapped, gets a page of its own.
    auto check_mapped_clone(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test mapped clone", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the mapped clone checks is made");
            return;
        }
        auto *const previous = attach(aspace.as_ptr_mut());

        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        constexpr auto AT = USER_ASPACE_BASE + MB(16);

        ustl::Arc<mm::VmObjectPaged> parent;
        ustl::Arc<mm::VmObjectPaged> child;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 2 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &parent) == Status::Ok &&
                          poke(parent, 0, 0x3100'0000) && poke(parent, 1, 0x3200'0000) &&
                          parent->create_clone(0, 2 * PAGE_SIZE, &child) == Status::Ok;
        verdict(tally, made, "an object with two pages, and a clone of it, are made");
        if (made) {
            // Not committed: nothing is mapped until it is touched.
            auto mapped = root->map_with_vmo(mm::VmPlacement::at(AT), 2 * PAGE_SIZE, 0, rw, PERMS,
                                             child, mm::VmMapOption::None, "mapped clone");
            verdict(tally, mapped.is_ok(),
                    "the clone is mapped read-write, and nothing is in "
                    "the page tables yet");
            if (mapped.is_ok()) {
                Node parent_pages = parent->cow_pages();
                auto const shared = lookup(parent_pages, false, 0);
                verdict(tally,
                        task::Thread::Current::soft_fault(AT, mm::VmfCause::Absent) == Status::Ok,
                        "a read of the clone's page 0 is resolved");
                verdict(tally,
                        shared && entry_pa(aspace, AT) == mm::frame_to_phys(shared->page) &&
                            !is_writable(aspace, AT) && is_readable(aspace, AT),
                        "it maps the page the object holds, read-only, in a mapping that "
                        "allows Write");

                // A write to that entry gets the clone a page of its own.
                verdict(tally,
                        task::Thread::Current::soft_fault(AT, mm::VmfCause::Write) == Status::Ok,
                        "a write to it is resolved");
                verdict(tally,
                        is_writable(aspace, AT) && shared &&
                            entry_pa(aspace, AT) != mm::frame_to_phys(shared->page) &&
                            peek(child, 0) == 0x3100'0000 && peek(parent, 0) == 0x3100'0000,
                        "the entry is writable on another frame, holding the bytes copied, and "
                        "the object's page is as it was");

                // A write to a page that was never mapped goes straight to a page of its own.
                verdict(tally,
                        task::Thread::Current::soft_fault(AT + PAGE_SIZE,
                                                          mm::VmfCause::Write |
                                                              mm::VmfCause::Absent) == Status::Ok &&
                            is_writable(aspace, AT + PAGE_SIZE) && peek(child, 1) == 0x3200'0000,
                        "a write to an unmapped page is mapped writable, on a copy");
            }
        }

        attach(previous);
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the mapped clone checks is destroyed");
    }

    // A clone whose page tables are filled in at once (VmMapping::map()): what they
    // hold is what the cursor says -- the node's own pages writable, the shared ones and
    // the zero page read-only -- without a page copied.
    // and what they hold is what the cursor says -- the node's own pages
    // writable, the shared ones and the zero page read-only -- without a page copied.
    auto check_committed_clone_mapping(Tally &tally) -> void {
        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test committed clone", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the committed clone checks is made");
            return;
        }
        auto root = aspace->root_vma();
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        constexpr auto AT = USER_ASPACE_BASE + MB(16);

        ustl::Arc<mm::VmObjectPaged> parent;
        ustl::Arc<mm::VmObjectPaged> child;
        auto const made = mm::VmObjectPaged::create(mm::GAF_USER, 3 * PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &parent) == Status::Ok &&
                          poke(parent, 0, 0x5100'0000) && poke(parent, 1, 0x5200'0000) &&
                          parent->create_clone(0, 4 * PAGE_SIZE, &child) == Status::Ok &&
                          poke(child, 1, 0x5a00'0000);
        verdict(tally, made,
                "an object of three pages, a clone of it four pages long, and a write to the "
                "clone's page 1 are made");
        if (made) {
            Node parent_pages = parent->cow_pages();
            Node child_pages = child->cow_pages();
            auto const shared = lookup(parent_pages, false, 0);
            auto const own_before = pages_in(child_pages, child_pages.as_ptr());
            verdict(tally, shared && own_before == 1,
                    "the clone holds one page of its own, the one it wrote");

            // Populated the way a program asks for it (VmOptions::MapRange): the page
            // tables are filled in when the mapping is made, and nothing is committed.
            auto mapped = root->map_with_vmo(mm::VmPlacement::at(AT), 4 * PAGE_SIZE, 0, rw, PERMS,
                                             child, mm::VmMapOption::Populate, "populated clone");
            verdict(tally, mapped.is_ok(),
                    "the clone is mapped read-write with Populate, and its page tables are "
                    "filled in");
            if (mapped.is_ok() && shared) {
                auto const zero = mm::frame_to_phys(mm::get_zero_page());
                verdict(tally,
                        entry_pa(aspace, AT) == mm::frame_to_phys(shared->page) &&
                            !is_writable(aspace, AT) && is_readable(aspace, AT),
                        "a page the clone only sees is mapped on the object's frame, read-only");
                verdict(tally,
                        is_writable(aspace, AT + PAGE_SIZE) && is_readable(aspace, AT + PAGE_SIZE),
                        "the page it wrote is mapped writable, in the middle of the run");
                verdict(tally,
                        entry_pa(aspace, AT + 2 * PAGE_SIZE) == zero &&
                            entry_pa(aspace, AT + 3 * PAGE_SIZE) == zero &&
                            !is_writable(aspace, AT + 2 * PAGE_SIZE) &&
                            !is_writable(aspace, AT + 3 * PAGE_SIZE),
                        "a hole of the object and a page past its end are the shared zero page, "
                        "read-only");
                verdict(tally, pages_in(child_pages, child_pages.as_ptr()) == own_before,
                        "and mapping the clone copied nothing: it still holds the one page");
            }
        }
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the committed clone checks is destroyed");
    }

    // A TLB shootdown, run on two CPUs at once. The writer, pinned to one CPU,
    // writes a page through a writable mapping, which leaves that CPU's TLB
    // holding a writable translation of it; the cloner, pinned to another,
    // then clones the object behind the mapping, which takes Write off the
    // entry; the writer writes again. Unless the clone also shot down the
    // writer's translation, that second write goes through the stale one,
    // into the page the clone now shares.
    //
    // The writer holds preemption off from its first write to its second. A
    // thread switched away from this CPU would load another root, which
    // flushes the translation by itself, and the check would pass whether or
    // not anything was shot down. Both writes capture their faults, which may
    // be done with preemption off, so the second one is a status to read
    // rather than a fault to resolve.
    struct ShootdownRun {
        mm::VmAspace *aspace = nullptr;
        ustl::Arc<mm::VmObjectPaged> object;
        ustl::Arc<mm::VmObjectPaged> clone;
        VirtAddr at = 0;
        std::atomic<bool> warm{false};
        std::atomic<bool> cloned{false};
        std::atomic<usize> finished{0};
        bool first_write = false;
        bool second_refused = false;
        bool waited_out = false;
        Status clone_status = Status::Ok;
    };

    constexpr u32 BEFORE_CLONE = 0x6100'0000;
    constexpr u32 AFTER_CLONE = 0x6200'0000;
    constexpr auto SHOOTDOWN_WAIT = Seconds(5);

    auto shootdown_writer(void *const arg) -> i32 {
        auto &run = *static_cast<ShootdownRun *>(arg);
        auto *const previous = attach(run.aspace);
        {
            task::AutoPreemptDisabler stay;
            auto const first =
                user_copy::copy_to_user_capture_faults(run.at, &BEFORE_CLONE, sizeof(u32));
            run.first_write = first.status == Status::Ok && !first.fault;
            run.warm.store(true, std::memory_order_release);

            auto const give_up = time::SteadyClock::now() + SHOOTDOWN_WAIT;
            while (!run.cloned.load(std::memory_order_acquire)) {
                if (time::SteadyClock::now() > give_up) {
                    run.waited_out = true;
                    break;
                }
                arch::spin_hint();
            }
            if (!run.waited_out) {
                auto const second =
                    user_copy::copy_to_user_capture_faults(run.at, &AFTER_CLONE, sizeof(u32));
                run.second_refused = static_cast<bool>(second.fault);
            }
        }
        attach(previous);
        run.finished.fetch_add(1, std::memory_order_release);
        return 0;
    }

    auto shootdown_cloner(void *const arg) -> i32 {
        auto &run = *static_cast<ShootdownRun *>(arg);
        auto const give_up = time::SteadyClock::now() + SHOOTDOWN_WAIT;
        while (!run.warm.load(std::memory_order_acquire) && time::SteadyClock::now() < give_up) {
            arch::spin_hint();
        }
        run.clone_status = run.object->create_clone(0, PAGE_SIZE, &run.clone);
        // Set whatever the clone answered, so that the writer is never left waiting.
        run.cloned.store(true, std::memory_order_release);
        run.finished.fetch_add(1, std::memory_order_release);
        return 0;
    }

    auto spawn_on(char const *const name, task::Thread::StartRoutine const routine,
                  ShootdownRun *const run, CpuNum const cpu) -> bool {
        auto *const thread = task::Thread::spawn(name, routine, run, task::DEFAULT_PRIORITY);
        if (thread == nullptr) {
            return false;
        }
        thread->set_cpu_affinity_hard(CpuMask::of(cpu));
        static_cast<void>(thread->detach());
        static_cast<void>(thread->resume());
        return true;
    }

    auto check_shootdown(Tally &tally) -> void {
        auto const cpus = Cpu::num_online_cpus();
        if (cpus < 2) {
            log::info("vmosnapshot.test: the shootdown checks need two CPUs, have {}", cpus);
            return;
        }

        ustl::Arc<mm::VmAspace> aspace;
        if (auto const status = mm::VmAspace::create(USER_ASPACE_BASE, USER_ASPACE_RESTRICTED_SIZE,
                                                     mm::VmasKind::User, mm::VmasFlags::None,
                                                     "vmosnapshot.test shootdown", &aspace);
            status != Status::Ok) {
            verdict(tally, false, "an address space for the shootdown checks is made");
            return;
        }
        constexpr auto PERMS = mm::VmaFlags::PermMask;
        auto const rw = mm::MmuFlags::Readable | mm::MmuFlags::Writable | mm::MmuFlags::User;
        constexpr auto AT = USER_ASPACE_BASE + MB(16);

        // Heap, not the stack: if a thread never finishes it still points here, and
        // this has to be leaked rather than popped.
        auto *const run = new (mm::GAF_KERNEL) ShootdownRun{};
        auto const made = run != nullptr &&
                          mm::VmObjectPaged::create(mm::GAF_USER, PAGE_SIZE, mm::VmoFlags::Lazy,
                                                    &run->object) == Status::Ok &&
                          poke(run->object, 0, 0x6000'0000) &&
                          aspace->root_vma()
                              ->map_with_vmo(mm::VmPlacement::at(AT), PAGE_SIZE, 0, rw, PERMS,
                                             run->object, mm::VmMapOption::Populate, "shootdown")
                              .is_ok() &&
                          is_writable(aspace, AT);
        verdict(tally, made, "an object of one page is mapped writable, its entry filled in");
        if (!made) {
            delete run;
            verdict(tally, aspace->destroy() == Status::Ok,
                    "the address space of the shootdown checks is destroyed");
            return;
        }
        run->aspace = aspace.as_ptr_mut();
        run->at = AT;

        // Two different CPUs, the writer's not this thread's own. The writer spins
        // with preemption off, so a thread on its CPU does not run until it is
        // done: had this thread been there, it could not have spawned the
        // cloner, and the writer would wait it out. The cloner is spawned first
        // for the same reason. Online CPUs are taken to be numbered from 0.
        CpuNum here = 0;
        {
            task::AutoPreemptDisabler stay;
            here = Cpu::id();
        }
        auto const writer_cpu = static_cast<CpuNum>((here + 1) % cpus);
        auto const cloner_cpu = static_cast<CpuNum>(cpus > 2 ? (here + 2) % cpus : here);
        auto const spawned = spawn_on("shootdown-cloner", shootdown_cloner, run, cloner_cpu) &&
                             spawn_on("shootdown-writer", shootdown_writer, run, writer_cpu);
        auto const started = time::SteadyClock::now();
        auto const expected = spawned ? usize{2} : usize{0};
        while (spawned && run->finished.load(std::memory_order_acquire) < expected &&
               time::SteadyClock::now() < started + 2 * SHOOTDOWN_WAIT) {
            static_cast<void>(
                task::Thread::Current::sleep_for(Milliseconds(10), task::Interruptible::No));
        }
        auto const finished = spawned && run->finished.load(std::memory_order_acquire) == 2;
        verdict(tally, finished, "the writer and the cloner both ran, on two CPUs, and finished");
        if (!finished) {
            // Leaked, with the address space: a thread may still be using both.
            static_cast<void>(aspace.into_raw());
            return;
        }

        verdict(tally, run->first_write && !run->waited_out && run->clone_status == Status::Ok,
                "the writer's first write goes through, and the clone is made while it waits");
        verdict(tally, run->second_refused,
                "its next write, with no switch in between, faults: the clone on the other CPU "
                "shot down the writable translation it held");
        verdict(tally, run->clone && peek(run->clone, 0) == BEFORE_CLONE,
                "the clone holds what the page held when it was made, not a write after it");

        delete run;
        verdict(tally, aspace->destroy() == Status::Ok,
                "the address space of the shootdown checks is destroyed");
    }

    auto vmosnapshot_test_command(int, char **) -> int {
        Tally tally;

        // A parent of three pages: 0 and 1 hold something, 2 is a hole.
        Node parent;
        if (!make(parent, 3)) {
            log::error("vmosnapshot.test: could not make the parent");
            return 1;
        }
        auto *const p0 = fill(parent, 0, 0xA0);
        auto *const p1 = fill(parent, 1, 0xA1);
        verdict(tally, p0 && p1 && p0 != p1 && nr_pages(parent) == 2,
                "the parent holds two pages and a hole");
        if (tally.failed != 0) {
            log::error("vmosnapshot.test: FAILED -- setup");
            return 1;
        }

        // The zero page, on its own: every later "reads as zero" is a claim
        // about this one page.
        verdict(tally, mm::get_zero_page() != nullptr && holds_only(mm::get_zero_page(), 0),
                "ZERO_PAGE exists and is all zeros");

        // A child larger than its parent: sees all three of the parent's pages,
        // and has two more that the parent cannot reach.
        Node child;
        verdict(tally, make_child(parent, 0, 3, 5, false, child), "a child of the parent is made");
        verdict(tally, &child->lock() == &parent->lock(),
                "the child takes the parent's lock, not one of its own");
        verdict(tally, reads_lenders_page(child, 0, p0) && holds_only(p0, 0xA0),
                "the child reads the parent's page 0, read-only, and allocates nothing");
        verdict(tally, reads_lenders_page(child, 1, p1), "and page 1");
        verdict(tally, reads_zero_page(child, 2),
                "a page the parent has a hole for reads as the zero page");
        verdict(tally, reads_zero_page(child, 3) && reads_zero_page(child, 4),
                "and so does everything past the parent's end");
        verdict(tally, nr_pages(child) == 0, "and the child still holds no page of its own");

        // The parent offset: the child's page 0 is the parent's page 1, and its
        // limit cuts it off after two pages.
        Node shifted;
        verdict(tally, make_child(parent, 1, 2, 3, false, shifted), "a child at a parent offset");
        verdict(tally, reads_lenders_page(shifted, 0, p1), "its page 0 is the parent's page 1");
        verdict(tally, reads_zero_page(shifted, 1), "its page 1 is the parent's hole, so zero");
        verdict(tally, reads_zero_page(shifted, 2), "and its page 2 is past its limit");

        // A limit the caller did not check: longer than the child and than what
        // the parent has from that offset. It is clamped, not refused.
        Node clamped;
        verdict(tally, make_child(parent, 1, 100, 8, false, clamped),
                "a child with a limit longer than either side is made");
        verdict(tally, reads_lenders_page(clamped, 0, p1) && reads_zero_page(clamped, 7),
                "and reads as if the limit had been clamped");

        // A misaligned argument is refused.
        Node refused;
        verdict(tally,
                VmCowPages::create_child(parent, 1, PAGE_SIZE, PAGE_SIZE, VmCowPages::Flags::None,
                                         &refused) == Status::InvArgs,
                "an offset that is not page aligned is refused");

        // A chain: the grandchild's offset and limit compose with its parent's.
        Node grandchild;
        verdict(tally, make_child(child, 0, 5, 5, false, grandchild),
                "a child of the child is made");
        verdict(tally, &grandchild->lock() == &parent->lock(),
                "and it is in the same tree, with the same lock");
        verdict(tally,
                reads_lenders_page(grandchild, 0, p0) && reads_lenders_page(grandchild, 1, p1),
                "it reads the pages two hops up");
        verdict(tally, reads_zero_page(grandchild, 3),
                "the middle node's limit stops the walk: page 3 is past the child's limit");
        Node offset_chain;
        verdict(tally, make_child(shifted, 0, 2, 2, false, offset_chain),
                "a child of the shifted child is made");
        verdict(tally, reads_lenders_page(offset_chain, 0, p1),
                "its page 0 is the shifted child's page 0, which is the parent's page 1");

        // Two children of one parent see the same page.
        Node sibling;
        verdict(tally, make_child(parent, 0, 3, 3, false, sibling), "a second child is made");
        verdict(tally, reads_lenders_page(sibling, 0, p0) && reads_lenders_page(child, 0, p0),
                "two children of one parent read the same page");

        // A hidden node.
        Node hidden;
        verdict(tally, make_child(parent, 0, 3, 3, true, hidden) && hidden->is_hidden(),
                "a hidden child is made");
        verdict(tally, !child->is_hidden(), "and an ordinary one is not hidden");
        verdict(tally,
                lookup(hidden, true, 0).is_err() &&
                    lookup(hidden, true, 0).unwrap_err() == Status::BadState,
                "a writer is refused by a hidden node, even where it has a page to show");
        verdict(tally, reads_lenders_page(hidden, 0, p0) && reads_zero_page(hidden, 2),
                "a hidden node reads through to its parent like any other");
        Node below_hidden;
        verdict(tally,
                make_child(hidden, 0, 3, 3, false, below_hidden) &&
                    reads_lenders_page(below_hidden, 1, p1),
                "and a node below it reads through both");

        // A node with no relatives is what it always was.
        Node alone;
        verdict(tally, make(alone, 2), "a node with no relatives is made");
        verdict(tally, &alone->lock() != &parent->lock(), "and it has a lock of its own");
        auto const first = lookup(alone, false, 1);
        verdict(tally,
                first && first->writable && first->page != mm::get_zero_page() &&
                    nr_pages(alone) == 1 && holds_only(first->page, 0),
                "a read of its hole commits a zeroed page of its own, writable");
        auto const again = lookup(alone, false, 1);
        verdict(tally,
                again && again->page == first->page && again->writable && nr_pages(alone) == 1,
                "and the next read finds that page");

        // The list of children: each node made under a parent is on its list,
        // and leaves it when it dies.
        Node family;
        Node kid_a;
        Node kid_b;
        Node kid_c;
        verdict(tally,
                make(family, 2) && make_child(family, 0, 2, 2, false, kid_a) &&
                    make_child(family, 0, 2, 2, false, kid_b) &&
                    make_child(family, 1, 1, 1, true, kid_c),
                "a parent and three children are made");
        verdict(tally, nr_children(family) == 3, "the parent lists all three");
        verdict(tally, nr_children(kid_a) == 0, "and a child with none of its own lists none");
        kid_b = nullptr;
        verdict(tally, nr_children(family) == 2, "a child that dies leaves the list");
        Node kid_a_child;
        verdict(tally, make_child(kid_a, 0, 2, 2, false, kid_a_child),
                "a child of a child is made");
        verdict(tally, nr_children(kid_a) == 1 && nr_children(family) == 2,
                "it is on its own parent's list, not on its grandparent's");
        kid_a = nullptr;
        verdict(tally, nr_children(family) == 2 && reads_zero_page(kid_a_child, 0),
                "a child whose parent handle is dropped stays on the grandparent's list, alive");
        kid_a_child = nullptr;
        verdict(tally, nr_children(family) == 1,
                "and leaves it when it dies, taking its parent with it");
        kid_c = nullptr;
        verdict(tally, nr_children(family) == 0, "the parent ends up with none");

        // The clone. A node that holds pages is cloned: its pages move into a
        // new hidden node above it, and it and the clone hang under that.
        Node origin;
        verdict(tally, make(origin, 3), "a node to clone is made");
        auto *const o0 = fill(origin, 0, 0xC0);
        auto *const o1 = fill(origin, 1, 0xC1);
        verdict(tally, o0 && o1 && pages_in(origin, origin.as_ptr()) == 2,
                "it holds two pages and a hole");
        Node clone1;
        verdict(tally, clone_of(origin, 0, 3, clone1) == Status::Ok, "it is cloned");
        auto const *const shared = parent_of(origin);
        verdict(tally, shared != nullptr && shared->is_hidden() && parent_of(clone1) == shared,
                "the node and its clone hang under one hidden node");
        verdict(tally, &clone1->lock() == &origin->lock(), "in one tree, under one lock");
        verdict(tally, pages_in(origin, origin.as_ptr()) == 0 && pages_in(origin, shared) == 2,
                "the pages moved up into the hidden node, and the node holds none");
        verdict(tally, children_in(origin, shared) == 2, "which lists the node and the clone");
        verdict(tally, o0->share_count() == 1 && o1->share_count() == 1,
                "each page both of them see has one more viewer");
        verdict(tally,
                reads_lenders_page(origin, 0, o0) && reads_lenders_page(clone1, 0, o0) &&
                    reads_lenders_page(clone1, 1, o1) && reads_zero_page(clone1, 2),
                "both read the same pages, and the hole as zero");
        verdict(tally, holds_only(o0, 0xC0) && holds_only(o1, 0xC1),
                "and the bytes are what was written before the clone");

        // A second clone of the same node finds it holds nothing and hangs
        // under the same hidden node: no new one, and no chain.
        Node clone2;
        verdict(tally, clone_of(origin, 0, 3, clone2) == Status::Ok, "it is cloned again");
        verdict(tally,
                parent_of(clone2) == shared && parent_of(origin) == shared &&
                    children_in(origin, shared) == 3 && pages_in(origin, shared) == 2,
                "the second clone hangs under the same hidden node");
        verdict(tally, o0->share_count() == 2 && o1->share_count() == 2,
                "and the pages have one more viewer again");

        // A window: only the page it can see gains a viewer.
        Node window;
        verdict(tally, clone_of(origin, 1, 1, window) == Status::Ok, "a one-page window is cloned");
        verdict(tally,
                reads_lenders_page(window, 0, o1) && o0->share_count() == 2 &&
                    o1->share_count() == 3,
                "its page 0 is the node's page 1, and only that page gained a viewer");

        // A window longer than the node: the rest reads as zero.
        Node longer;
        verdict(tally, clone_of(origin, 0, 5, longer) == Status::Ok,
                "a clone longer than the node");
        verdict(tally,
                reads_lenders_page(longer, 1, o1) && reads_zero_page(longer, 2) &&
                    reads_zero_page(longer, 4) && o0->share_count() == 3 && o1->share_count() == 4,
                "reads the node, then zero past its end");

        // A clone of a clone: the clone holds nothing, so this one hangs there too.
        Node of_clone;
        verdict(tally, clone_of(clone1, 0, 3, of_clone) == Status::Ok, "a clone of a clone");
        verdict(tally,
                parent_of(of_clone) == shared && children_in(origin, shared) == 6 &&
                    reads_lenders_page(of_clone, 0, o0),
                "hangs under the same hidden node, six nodes under it now");

        // Refused.
        Node refused_clone;
        verdict(tally, clone_of(origin, 0, 1, refused_clone) == Status::Ok && refused_clone,
                "a clone is made for the refusals");
        Node refused_hidden;
        Node hidden_source;
        verdict(tally,
                make(hidden_source, 1) && make_child(hidden_source, 0, 1, 1, true, refused_hidden),
                "a hidden node is made");
        verdict(tally, clone_of(refused_hidden, 0, 1, refused_clone) == Status::BadState,
                "a hidden node cannot be cloned");
        {
            Node misaligned;
            verdict(tally, clone_raw(origin, 1, PAGE_SIZE, misaligned) == Status::InvArgs,
                    "an offset that is not page aligned is refused");
            verdict(tally, clone_raw(origin, 0, 1, misaligned) == Status::InvArgs,
                    "and so is a size that is not");
        }

        // A clone of a node with nothing at all still gets a hidden node above,
        // because what the node writes later must not reach the clone.
        Node blank;
        Node blank_clone;
        verdict(tally, make(blank, 2) && clone_of(blank, 0, 2, blank_clone) == Status::Ok,
                "a node with no pages is cloned");
        auto const *const blank_hidden = parent_of(blank);
        verdict(tally,
                blank_hidden != nullptr && blank_hidden->is_hidden() &&
                    parent_of(blank_clone) == blank_hidden && pages_in(blank, blank_hidden) == 0 &&
                    children_in(blank, blank_hidden) == 2 && reads_zero_page(blank, 0) &&
                    reads_zero_page(blank_clone, 1),
                "it gets one, empty, and both read zero");

        // The write. A node that writes a page it sees only through the hidden
        // node above it gets a page of its own: a copy while someone else still
        // sees the page, the page itself when it is the last.
        Node w_origin;
        verdict(tally, make(w_origin, 3), "a node to write through is made");
        auto *const w0 = fill(w_origin, 0, 0xD0);
        auto *const w1 = fill(w_origin, 1, 0xD1);
        Node w_a;
        Node w_b;
        verdict(tally,
                w0 && w1 && clone_of(w_origin, 0, 3, w_a) == Status::Ok &&
                    clone_of(w_origin, 0, 3, w_b) == Status::Ok,
                "it holds two pages and has two clones");
        auto const *const w_hidden = parent_of(w_origin);
        verdict(tally,
                w_hidden != nullptr && pages_in(w_origin, w_hidden) == 2 &&
                    w0->share_count() == 2 && w1->share_count() == 2,
                "three nodes see each page, so each has a count of two");

        // The first writer of a page others still see gets a copy.
        auto const wrote_a = lookup(w_a, true, 0);
        verdict(tally,
                wrote_a && wrote_a->writable && wrote_a->page != w0 &&
                    holds_only(wrote_a->page, 0xD0) && nr_pages(w_a) == 1 &&
                    w0->share_count() == 1 && pages_in(w_origin, w_hidden) == 2,
                "the first writer of a shared page gets a copy of it, and the page stays");
        std::memset(bytes_of(wrote_a->page), 0xEE, PAGE_SIZE);
        verdict(tally,
                holds_only(w0, 0xD0) && reads_lenders_page(w_b, 0, w0) &&
                    reads_lenders_page(w_origin, 0, w0),
                "what it writes does not reach the page the others read");
        auto const again_a = lookup(w_a, false, 0);
        verdict(tally,
                again_a && again_a->page == wrote_a->page && again_a->writable &&
                    holds_only(again_a->page, 0xEE),
                "and it reads its own page afterwards, writable");

        // The second writer forks as well: one other viewer is still there.
        auto const wrote_b = lookup(w_b, true, 0);
        verdict(tally,
                wrote_b && wrote_b->page != w0 && wrote_b->page != wrote_a->page &&
                    holds_only(wrote_b->page, 0xD0) && w0->share_count() == 0 &&
                    pages_in(w_origin, w_hidden) == 2,
                "the second writer forks too, and the count is now zero");

        // The last node that sees the page takes it: nothing is copied.
        auto const wrote_o = lookup(w_origin, true, 0);
        verdict(tally,
                wrote_o && wrote_o->page == w0 && wrote_o->writable &&
                    pages_in(w_origin, w_hidden) == 1 && nr_pages(w_origin) == 1 &&
                    holds_only(w0, 0xD0),
                "the last node that sees a page takes the page itself, and the hidden node "
                "lets go of it");

        // The other order, on page 1: two forks and then the take.
        auto const o1w = lookup(w_origin, true, 1);
        auto const a1w = lookup(w_a, true, 1);
        auto const b1w = lookup(w_b, true, 1);
        verdict(tally,
                o1w && a1w && b1w && o1w->page != w1 && a1w->page != w1 && b1w->page == w1 &&
                    w1->share_count() == 0 && pages_in(w_origin, w_hidden) == 0 &&
                    holds_only(o1w->page, 0xD1) && holds_only(a1w->page, 0xD1) &&
                    holds_only(w1, 0xD1),
                "whoever writes last takes the page, whichever node that is");

        // A hole, and the zero tail, are allocated, not copied.
        auto const hole = lookup(w_a, true, 2);
        verdict(tally,
                hole && hole->writable && hole->page != mm::get_zero_page() &&
                    holds_only(hole->page, 0) && nr_pages(w_a) == 3,
                "a write to a hole gets a zeroed page of its own");
        auto const tail = lookup(longer, true, 4);
        verdict(tally,
                tail && tail->writable && tail->page != mm::get_zero_page() &&
                    holds_only(tail->page, 0) && nr_pages(longer) == 1,
                "and so does a write past the end of what the clone can see");

        // A page a node that is not hidden holds is only ever copied.
        auto const before_pages = nr_pages(parent);
        auto const under_plain = lookup(child, true, 0);
        verdict(tally,
                under_plain && under_plain->page != p0 && holds_only(under_plain->page, 0xA0) &&
                    nr_pages(parent) == before_pages && p0->share_count() == 0 &&
                    nr_pages(child) == 1,
                "a page held by a node that is not hidden is copied, never taken, and its count "
                "is left alone");

        // Two windows of one node that do not overlap: two programs' segments of
        // one boot image. They hang under one hidden node and share nothing but
        // the lock; what each reads, writes and counts is its own.
        Node image;
        verdict(tally, make(image, 6), "a six-page image is made");
        VmPage *img[6] = {};
        for (usize i = 0; i < 6; ++i) {
            img[i] = fill(image, i, static_cast<u8>(0x30 + i));
        }
        Node seg_x;
        Node seg_y;
        verdict(tally,
                img[5] != nullptr && clone_of(image, 1, 2, seg_x) == Status::Ok &&
                    clone_of(image, 4, 2, seg_y) == Status::Ok,
                "two windows of it, pages 1-2 and 4-5, are cloned");
        auto const *const image_hidden = parent_of(image);
        verdict(tally,
                image_hidden != nullptr && parent_of(seg_x) == image_hidden &&
                    parent_of(seg_y) == image_hidden && children_in(image, image_hidden) == 3 &&
                    pages_in(image, image_hidden) == 6,
                "both hang under the one hidden node, which holds all six pages");
        verdict(tally,
                img[0]->share_count() == 0 && img[1]->share_count() == 1 &&
                    img[2]->share_count() == 1 && img[3]->share_count() == 0 &&
                    img[4]->share_count() == 1 && img[5]->share_count() == 1,
                "each window's pages have one more viewer, and the pages in neither have none");
        verdict(tally,
                reads_lenders_page(seg_x, 0, img[1]) && reads_lenders_page(seg_x, 1, img[2]) &&
                    reads_lenders_page(seg_y, 0, img[4]) && reads_lenders_page(seg_y, 1, img[5]),
                "each window reads its own pages of the image");

        // A write in one window does not touch the other's pages or counts.
        auto const x_wrote = lookup(seg_x, true, 0);
        verdict(tally,
                x_wrote && x_wrote->page != img[1] && holds_only(x_wrote->page, 0x31) &&
                    img[1]->share_count() == 0 && img[4]->share_count() == 1 &&
                    img[5]->share_count() == 1 && reads_lenders_page(seg_y, 0, img[4]),
                "a write in the first window forks its page and leaves the second's alone");
        auto const y_wrote = lookup(seg_y, true, 1);
        verdict(tally,
                y_wrote && y_wrote->page != img[5] && holds_only(y_wrote->page, 0x35) &&
                    img[5]->share_count() == 0 && img[2]->share_count() == 1 &&
                    reads_lenders_page(seg_x, 1, img[2]),
                "and a write in the second forks its page and leaves the first's alone");

        // The image itself sees every page, so once a window has left a page
        // the image is the last to see it, and takes it; the pages no window
        // saw, it takes at once.
        auto const image_1 = lookup(image, true, 1);
        auto const image_0 = lookup(image, true, 0);
        auto const image_3 = lookup(image, true, 3);
        verdict(tally,
                image_1 && image_0 && image_3 && image_1->page == img[1] &&
                    image_0->page == img[0] && image_3->page == img[3] &&
                    pages_in(image, image_hidden) == 3 && img[2]->share_count() == 1 &&
                    img[4]->share_count() == 1,
                "the image takes the pages nobody else still sees, and the rest stay shared");
        verdict(tally,
                holds_only(img[0], 0x30) && holds_only(img[1], 0x31) && holds_only(img[3], 0x33) &&
                    reads_lenders_page(seg_x, 1, img[2]) && reads_lenders_page(seg_y, 0, img[4]),
                "and the windows still read the pages that stayed");

        // Death. A leaf that dies is one viewer fewer of what it sees: a page it
        // was the last to see is freed, and the count of any other drops. A hidden
        // node left with one child is merged into it.
        Node dsrc;
        verdict(tally, make(dsrc, 3), "a node for the death checks is made");
        auto *const d0 = fill(dsrc, 0, 0x50);
        auto *const d1 = fill(dsrc, 1, 0x51);
        Node da;
        Node db;
        verdict(tally,
                d0 && d1 && clone_of(dsrc, 0, 3, da) == Status::Ok &&
                    clone_of(dsrc, 0, 3, db) == Status::Ok,
                "it has two pages and two clones");
        auto const *const d_hidden = parent_of(dsrc);
        verdict(tally,
                d_hidden != nullptr && d0->share_count() == 2 && d1->share_count() == 2 &&
                    children_in(dsrc, d_hidden) == 3,
                "three nodes see each page");
        da = nullptr;
        verdict(tally,
                d0->share_count() == 1 && d1->share_count() == 1 &&
                    children_in(dsrc, d_hidden) == 2 && pages_in(dsrc, d_hidden) == 2 &&
                    parent_of(dsrc) == d_hidden,
                "one clone dies: each page has one viewer fewer, and the hidden node, with two "
                "children left, stays");
        db = nullptr;
        verdict(tally,
                parent_of(dsrc) == nullptr && nr_pages(dsrc) == 2 && d0->share_count() == 0 &&
                    d1->share_count() == 0,
                "the other dies: the hidden node is merged away and the node that is left has "
                "no parent");
        auto const d_own = lookup(dsrc, false, 0);
        verdict(tally,
                d_own && d_own->page == d0 && d_own->writable && holds_only(d0, 0x50) &&
                    holds_only(d1, 0x51),
                "and it holds the very pages, writable, not copies");

        // The last viewer of a page frees it, and the hidden node stays while others remain.
        Node ea;
        Node eb;
        Node esrc;
        verdict(tally, make(esrc, 2), "a node for the last-viewer checks is made");
        auto *const e0 = fill(esrc, 0, 0x54);
        verdict(tally,
                e0 && clone_of(esrc, 0, 2, ea) == Status::Ok &&
                    clone_of(esrc, 0, 2, eb) == Status::Ok,
                "it has a page and two clones");
        auto const *const e_hidden = parent_of(esrc);
        auto const e_src_wrote = lookup(esrc, true, 0);
        auto const e_a_wrote = lookup(ea, true, 0);
        verdict(tally,
                e_src_wrote && e_a_wrote && e_src_wrote->page != e0 && e_a_wrote->page != e0 &&
                    e0->share_count() == 0 && pages_in(esrc, e_hidden) == 1,
                "the other two write it, and only the last clone still sees the page in the "
                "hidden node");
        eb = nullptr;
        verdict(tally,
                pages_in(esrc, e_hidden) == 0 && children_in(esrc, e_hidden) == 2 &&
                    parent_of(esrc) == e_hidden,
                "the last viewer dies: the page is freed, and the hidden node stays for the "
                "two that are left");

        // The source dies first, and the window it was cloned for is all that is kept.
        Node fsrc;
        Node fwin;
        verdict(tally, make(fsrc, 4), "a node for the source-dies checks is made");
        VmPage *f[4] = {};
        for (usize i = 0; i < 4; ++i) {
            f[i] = fill(fsrc, i, static_cast<u8>(0x70 + i));
        }
        verdict(tally, f[3] != nullptr && clone_of(fsrc, 1, 1, fwin) == Status::Ok,
                "a one-page window of it is cloned");
        fsrc = nullptr;
        auto const f_own = lookup(fwin, false, 0);
        verdict(tally,
                parent_of(fwin) == nullptr && nr_pages(fwin) == 1 && f_own && f_own->page == f[1] &&
                    f_own->writable && holds_only(f[1], 0x71),
                "the source dies: the window is left with its page, moved and not copied, and no "
                "parent");

        // A merge composes the offsets and limits, and leaves the counts as they were.
        Node rsrc;
        verdict(tally, make(rsrc, 4), "a node for the merge checks is made");
        VmPage *r[4] = {};
        for (usize i = 0; i < 4; ++i) {
            r[i] = fill(rsrc, i, static_cast<u8>(0x60 + i));
        }
        Node rc1;
        verdict(tally, r[3] != nullptr && clone_of(rsrc, 1, 2, rc1) == Status::Ok,
                "a two-page window of it, at page 1, is cloned");
        auto const *const r_top = parent_of(rsrc);
        auto const rc1_wrote = lookup(rc1, true, 0);
        Node rc2;
        verdict(tally, rc1_wrote && clone_of(rc1, 0, 2, rc2) == Status::Ok,
                "the window writes its page 0 and is cloned in turn");
        auto const *const r_mid = parent_of(rc1);
        verdict(tally,
                r_mid != nullptr && r_mid != r_top && parent_of(rc2) == r_mid &&
                    parent_of_node(rsrc, r_mid) == r_top && rc1_wrote->page->share_count() == 1 &&
                    r[2]->share_count() == 2,
                "the clone of a node that holds a page gets a hidden node of its own, below "
                "the first, and counts the pages it sees");
        rc2 = nullptr;
        verdict(tally,
                parent_of(rc1) == r_top && children_in(rsrc, r_top) == 2 &&
                    rc1_wrote->page->share_count() == 0 && r[2]->share_count() == 1 &&
                    nr_pages(rc1) == 1,
                "the clone dies: the hidden node between is merged away, and the window hangs "
                "under the first again with the counts left as if it had never been");
        auto const rc1_own = lookup(rc1, false, 0);
        verdict(tally,
                rc1_own && rc1_own->page == rc1_wrote->page && rc1_own->writable &&
                    holds_only(rc1_own->page, 0x61) && reads_lenders_page(rc1, 1, r[2]),
                "and the window still reads its own page, and the page above it that the "
                "composed offset points to");

        // The new hidden node takes the node's place on its parent's list.
        Node top;
        Node under_top;
        Node under_top_clone;
        verdict(tally,
                make(top, 2) && make_child(top, 0, 2, 2, false, under_top) &&
                    children_in(top, top.as_ptr()) == 1,
                "a node under a parent is made");
        verdict(tally, clone_of(under_top, 0, 2, under_top_clone) == Status::Ok, "and cloned");
        auto const *const between = parent_of(under_top);
        verdict(tally,
                between != nullptr && between->is_hidden() &&
                    parent_of(under_top_clone) == between && children_in(top, top.as_ptr()) == 1 &&
                    children_in(top, between) == 2,
                "the hidden node stands between them, and the parent still lists one child");

        // The hidden node and its pages outlive every handle but one.
        origin = nullptr;
        clone1 = nullptr;
        clone2 = nullptr;
        window = nullptr;
        of_clone = nullptr;
        verdict(tally,
                reads_lenders_page(longer, 0, o0) && reads_lenders_page(longer, 1, o1) &&
                    holds_only(o0, 0xC0),
                "with every other handle dropped the last clone still reads the pages");

        // The last handle to the parent goes; the children keep the tree.
        Node orphan_parent;
        Node orphan;
        auto const made =
            make(orphan_parent, 1) && make_child(orphan_parent, 0, 1, 1, false, orphan);
        verdict(tally, made, "a parent and its child are made for the last check");
        if (made) {
            auto *const kept = fill(orphan_parent, 0, 0xB0);
            auto *const lock = &orphan->lock();
            orphan_parent = nullptr;
            verdict(
                tally,
                kept != nullptr && &orphan->lock() == lock && reads_lenders_page(orphan, 0, kept) &&
                    holds_only(kept, 0xB0),
                "a child whose parent handle is dropped still reads it, and still has its lock");
        }

        check_remove_write(tally);
        check_narrowing_protect(tally);
        check_attachments(tally);
        check_clone_of_mapped_object(tally);
        check_create_clone(tally);
        check_mapped_clone(tally);
        check_committed_clone_mapping(tally);
        check_shootdown(tally);

        if (tally.failed != 0) {
            log::error("vmosnapshot.test: FAILED -- {} of {} checks", tally.failed,
                       tally.passed + tally.failed);
            return 1;
        }
        log::info("vmosnapshot.test: ok -- {} checks", tally.passed);
        return 0;
    }
} // namespace
} // namespace ours

GKTL_COMMAND(VmoSnapshotTest, "vmosnapshot.test", ours::vmosnapshot_test_command,
             "check what a VmCowPages under another one sees: lender's pages, zeros, its lock");
