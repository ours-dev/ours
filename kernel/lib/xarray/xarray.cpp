/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#include <ours/lib/xarray.hpp>

#include <new>

namespace ours::lib {
namespace {
    constexpr auto bit(usize offset) -> u64 {
        return 1_u64 << offset;
    }

    constexpr auto first_index(usize slot, u8 shift) -> XArray::Index {
        return XArray::Index(slot) << shift;
    }
} // namespace anonymous

    XArray::~XArray() {
        clear();
    }

    auto XArray::load(Index index) const -> Object * {
        return load_entry(index).as_object();
    }

    auto XArray::load_entry(Index index) const -> Entry {
        auto const head = m_head;
        if (head.is_null()) {
            return Entry::null();
        }

        if (!head.is_node()) {
            return index == 0 ? head : Entry::null();
        }

        auto const *slot = descend(node_from_entry(head), index);
        return slot == nullptr ? Entry::null() : *slot;
    }

    auto XArray::store(Index index, Object *object, StoreMode mode) -> Status {
        if (object == nullptr) {
            return Status::InvalidArgument;
        }
        if ((usize(object) & Entry::kInternalMask) != 0) {
            return Status::InvalidArgument;
        }

        auto const old = load_entry(index);
        if (mode == StoreMode::Create && !old.is_null()) {
            return Status::AlreadyExists;
        }
        if (mode == StoreMode::Update && old.is_null()) {
            return Status::NotFound;
        }

        if (auto const status = ensure_root(index); status != Status::Ok) {
            return status;
        }

        auto *target_slot = ensure_slot(index);
        if (target_slot == nullptr) {
            return Status::NoMemory;
        }

        auto const entry = Entry::object(object);
        if (m_head.is_node()) {
            auto *node = node_from_entry(m_head);
            auto *leaf = node;
            while (leaf->shift != 0) {
                auto const offset = slot(index, leaf->shift);
                leaf = node_from_entry(leaf->slots[offset]);
            }

            auto const offset = slot(index, 0);
            if (leaf->slots[offset].is_null()) {
                ++leaf->count;
            }
            leaf->slots[offset] = entry;
            recalculate_slot_marks(leaf, offset);
        } else {
            m_head = entry;
        }

        return Status::Ok;
    }

    auto XArray::erase(Index index) -> Object * {
        return erase_entry(index).as_object();
    }

    auto XArray::erase_entry(Index index) -> Entry {
        if (m_head.is_null()) {
            return Entry::null();
        }
        if (!m_head.is_node()) {
            if (index != 0) {
                return Entry::null();
            }

            auto const old = m_head;
            m_head = Entry::null();
            return old;
        }

        auto *node = node_from_entry(m_head);
        auto *target_slot = descend(node, index);
        if (target_slot == nullptr || target_slot->is_null()) {
            return Entry::null();
        }

        auto const old = *target_slot;
        *target_slot = Entry::null();

        auto *leaf = node;
        while (leaf->shift != 0) {
            auto const offset = slot(index, leaf->shift);
            leaf = node_from_entry(leaf->slots[offset]);
        }

        auto const offset = slot(index, 0);
        --leaf->count;
        recalculate_slot_marks(leaf, offset);
        prune_empty(leaf);

        if (m_head.is_node() && node_from_entry(m_head)->count == 0) {
            destroy_node(node_from_entry(m_head));
            m_head = Entry::null();
        }

        return old;
    }

    auto XArray::reserve(Index, Order) -> Status {
        return Status::NotSupported;
    }

    auto XArray::clear() -> void {
        destroy_entry(m_head);
        m_head = Entry::null();
    }

    auto XArray::empty() const -> bool {
        return m_head.is_null();
    }

    auto XArray::set_mark(Index index, Mark mark) -> Status {
        if (!m_head.is_node() && index == 0 && !m_head.is_null()) {
            auto const old = m_head;
            auto *root = create_node(nullptr, 0, 0);
            if (root == nullptr) {
                return Status::NoMemory;
            }
            root->slots[0] = old;
            root->count = 1;
            m_head = Entry::node(root);
        } else if (!m_head.is_node()) {
            return Status::NotFound;
        }

        auto *node = node_from_entry(m_head);
        auto *target_slot = descend(node, index);
        if (target_slot == nullptr || target_slot->is_null()) {
            return Status::NotFound;
        }

        auto *leaf = node;
        while (leaf->shift != 0) {
            auto const offset = slot(index, leaf->shift);
            leaf = node_from_entry(leaf->slots[offset]);
        }
        set_slot_mark(leaf, slot(index, 0), mark);
        return Status::Ok;
    }

    auto XArray::clear_mark(Index index, Mark mark) -> Status {
        if (!m_head.is_node()) {
            return index == 0 && !m_head.is_null() ? Status::Ok : Status::NotFound;
        }

        auto *node = node_from_entry(m_head);
        auto *target_slot = descend(node, index);
        if (target_slot == nullptr || target_slot->is_null()) {
            return Status::NotFound;
        }

        auto *leaf = node;
        while (leaf->shift != 0) {
            auto const offset = slot(index, leaf->shift);
            leaf = node_from_entry(leaf->slots[offset]);
        }
        clear_slot_mark(leaf, slot(index, 0), mark);
        return Status::Ok;
    }

    auto XArray::marked(Index index, Mark mark) const -> bool {
        if (!m_head.is_node()) {
            return false;
        }

        auto const *node = node_from_entry(m_head);
        while (true) {
            auto const offset = slot(index, node->shift);
            if ((node->marks[mark_index(mark)] & bit(offset)) == 0) {
                return false;
            }
            if (node->shift == 0) {
                return true;
            }
            auto const entry = node->slots[offset];
            if (!entry.is_node()) {
                return false;
            }
            node = node_from_entry(entry);
        }
    }

    auto XArray::find(Cursor &cursor) const -> Object * {
        return find_from(m_head, 0, cursor.index(), nullptr, cursor).as_object();
    }

    auto XArray::find_marked(Cursor &cursor, Mark mark) const -> Object * {
        return find_from(m_head, 0, cursor.index(), &mark, cursor).as_object();
    }

    auto XArray::max_shift(Index index) -> u8 {
        auto shift = 0_u8;
        while ((index >> shift) >= kChunkSize) {
            shift += kChunkShift;
        }
        return shift;
    }

    auto XArray::node_from_entry(Entry entry) -> Node * {
        return reinterpret_cast<Node *>(entry.bits() & ~Entry::kInternalMask);
    }

    auto XArray::create_node(Node *parent, u8 shift, u8 offset) -> Node * {
        auto *node = new (std::nothrow) Node;
        if (node == nullptr) {
            return nullptr;
        }

        node->parent = parent;
        node->shift = shift;
        node->offset = offset;
        return node;
    }

    auto XArray::destroy_node(Node *node) -> void {
        delete node;
    }

    auto XArray::destroy_entry(Entry entry) -> void {
        if (!entry.is_node()) {
            return;
        }

        auto *node = node_from_entry(entry);
        for (auto &slot : node->slots) {
            destroy_entry(slot);
        }
        destroy_node(node);
    }

    auto XArray::descend(Node *node, Index index) -> Entry * {
        while (true) {
            auto const offset = slot(index, node->shift);
            auto *entry = &node->slots[offset];
            if (node->shift == 0) {
                return entry;
            }
            if (!entry->is_node()) {
                return nullptr;
            }
            node = node_from_entry(*entry);
        }
    }

    auto XArray::descend(Node const *node, Index index) -> Entry const * {
        while (true) {
            auto const offset = slot(index, node->shift);
            auto const *entry = &node->slots[offset];
            if (node->shift == 0) {
                return entry;
            }
            if (!entry->is_node()) {
                return nullptr;
            }
            node = node_from_entry(*entry);
        }
    }

    auto XArray::any_marks(Node const *node) -> u64 {
        auto marks = 0_u64;
        for (auto mark : node->marks) {
            marks |= mark;
        }
        return marks;
    }

    auto XArray::recalculate_slot_marks(Node *node, usize offset) -> void {
        auto const entry = node->slots[offset];
        for (auto i = 0_usize; i < kMaxMarks; ++i) {
            auto const mask = bit(offset);
            if (entry.is_node() && (node_from_entry(entry)->marks[i] != 0)) {
                node->marks[i] |= mask;
            } else {
                node->marks[i] &= ~mask;
            }
        }

        if (node->parent != nullptr) {
            recalculate_slot_marks(node->parent, node->offset);
        }
    }

    auto XArray::set_slot_mark(Node *node, usize offset, Mark mark) -> void {
        node->marks[mark_index(mark)] |= bit(offset);
        if (node->parent != nullptr) {
            set_slot_mark(node->parent, node->offset, mark);
        }
    }

    auto XArray::clear_slot_mark(Node *node, usize offset, Mark mark) -> void {
        node->marks[mark_index(mark)] &= ~bit(offset);
        if (node->parent != nullptr && node->marks[mark_index(mark)] == 0) {
            clear_slot_mark(node->parent, node->offset, mark);
        }
    }

    auto XArray::prune_empty(Node *node) -> void {
        while (node != nullptr && node->count == 0 && any_marks(node) == 0) {
            auto *parent = node->parent;
            auto const offset = node->offset;
            destroy_node(node);

            if (parent == nullptr) {
                m_head = Entry::null();
                return;
            }

            parent->slots[offset] = Entry::null();
            --parent->count;
            recalculate_slot_marks(parent, offset);
            node = parent;
        }
    }

    auto XArray::ensure_root(Index index) -> Status {
        auto const needed_shift = max_shift(index);
        if (m_head.is_node()) {
            while (node_from_entry(m_head)->shift < needed_shift) {
                auto *old = node_from_entry(m_head);
                auto *root = create_node(nullptr, old->shift + kChunkShift, 0);
                if (root == nullptr) {
                    return Status::NoMemory;
                }

                auto const offset = 0_usize;
                root->slots[offset] = Entry::node(old);
                root->count = 1;
                old->parent = root;
                old->offset = u8(offset);
                m_head = Entry::node(root);
                recalculate_slot_marks(root, offset);
            }
            return Status::Ok;
        }

        if (needed_shift == 0 && (m_head.is_null() || index == 0)) {
            return Status::Ok;
        }

        auto *root = create_node(nullptr, needed_shift, 0);
        if (root == nullptr) {
            return Status::NoMemory;
        }

        if (!m_head.is_null()) {
            auto *node = root;
            while (node->shift != 0) {
                auto *child = create_node(node, node->shift - kChunkShift, 0);
                if (child == nullptr) {
                    destroy_entry(Entry::node(root));
                    return Status::NoMemory;
                }
                node->slots[0] = Entry::node(child);
                node->count = 1;
                node = child;
            }
            node->slots[0] = m_head;
            node->count = 1;
        }
        m_head = Entry::node(root);
        return Status::Ok;
    }

    auto XArray::ensure_slot(Index index) -> Entry * {
        if (!m_head.is_node()) {
            return &m_head;
        }

        auto *node = node_from_entry(m_head);
        while (node->shift != 0) {
            auto const offset = slot(index, node->shift);
            auto *entry = &node->slots[offset];
            if (entry->is_null()) {
                auto *child = create_node(node, node->shift - kChunkShift, u8(offset));
                if (child == nullptr) {
                    return nullptr;
                }
                *entry = Entry::node(child);
                ++node->count;
            } else if (!entry->is_node()) {
                return nullptr;
            }
            node = node_from_entry(*entry);
        }

        return &node->slots[slot(index, 0)];
    }

    auto XArray::find_from(Entry entry, Index base, Index start, Mark const *mark, Cursor &cursor) const -> Entry {
        if (entry.is_null()) {
            return Entry::null();
        }
        if (!entry.is_node()) {
            if (base < start) {
                return Entry::null();
            }
            cursor.m_index = base + 1;
            cursor.m_shift = 0;
            return entry;
        }

        auto const *node = node_from_entry(entry);
        auto const width = Index(1) << node->shift;
        auto offset = start <= base ? 0_usize : slot(start, node->shift);
        for (; offset < kChunkSize; ++offset) {
            if (mark != nullptr && (node->marks[mark_index(*mark)] & bit(offset)) == 0) {
                continue;
            }

            auto const child_base = base + first_index(offset, node->shift);
            auto const child_end = child_base + width;
            if (child_end <= start) {
                continue;
            }

            auto found = find_from(node->slots[offset], child_base, start, mark, cursor);
            if (!found.is_null()) {
                return found;
            }
        }

        return Entry::null();
    }
} // namespace ours::lib
