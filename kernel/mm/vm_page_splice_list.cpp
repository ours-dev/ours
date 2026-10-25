/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <ours/mm/pmm.hpp>
#include <ours/mm/vm_page_splice_list.hpp>

namespace ours::mm {
VmPageSpliceList::VmPageSpliceList(usize const nr_pages) : m_held(), m_nr_pages(nr_pages) {}

VmPageSpliceList::~VmPageSpliceList() {
    // What was never popped is still this list's: the pages go back to the
    // allocator, the markers and gaps are nothing to give back.
    for (auto at = m_next_held; at < m_held.size(); ++at) {
        if (auto *const page = m_held[at].slot.page()) {
            free_frames(page, 0);
        }
    }
}

auto VmPageSpliceList::reserve(usize const nr_held) -> Status {
    if (m_held.reserve(m_held.size() + nr_held).is_err()) {
        return Status::OutOfMem;
    }
    return Status::Ok;
}

auto VmPageSpliceList::append(usize const index, Slot const slot) -> Status {
    DEBUG_ASSERT(index < m_nr_pages, "slot {} is past the run of {}", index, m_nr_pages);
    DEBUG_ASSERT(m_held.empty() || m_held[m_held.size() - 1].index < index,
                 "slots are appended in index order, each once");
    DEBUG_ASSERT(m_position == 0, "a list being read is not appended to");
    if (m_held.push_back(Held{.index = index, .slot = slot}).is_err()) {
        return Status::OutOfMem;
    }
    return Status::Ok;
}

auto VmPageSpliceList::append_page(usize const index, VmPage *const page) -> Status {
    DEBUG_ASSERT(page, "a gap is appended by appending nothing");
    return append(index, Slot::of_page(page, VmPageList::Tag::Owned));
}

auto VmPageSpliceList::append_zero_marker(usize const index) -> Status {
    return append(index, Slot::zero_marker());
}

auto VmPageSpliceList::pop() -> Slot {
    DEBUG_ASSERT(m_position < m_nr_pages, "popped past the end of the run");
    auto const index = m_position;
    m_position += 1;
    if (m_next_held < m_held.size() && m_held[m_next_held].index == index) {
        auto const slot = m_held[m_next_held].slot;
        m_next_held += 1;
        return slot;
    }
    return Slot();
}

} // namespace ours::mm
