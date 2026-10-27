/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef FDT_TREE_HPP
#define FDT_TREE_HPP 1

#include <ours/status.hpp>

#include <string_view>

#include <fdt/format.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>
#include <ustl/views/span.hpp>

namespace fdt {
using ours::Status;

using Bytes = ustl::views::Span<u8 const>;

template <typename T>
using Result = ustl::Result<T, Status>;

/// One (address, size) pair of a `reg` property.
struct RegEntry final {
    u64 address;
    u64 size;
};

/// A property: a name and the raw bytes of its value. The bytes mean whatever
/// the node's binding says they mean; the accessors below decode the shapes
/// that recur (a cell, two cells, a string, a `reg` pair). A cell is 4 bytes.
class Property final {
  public:
    static constexpr usize CELL_SIZE = 4;

    [[nodiscard]]
    auto name() const -> std::string_view {
        return m_name;
    }

    [[nodiscard]]
    auto bytes() const -> Bytes {
        return m_value;
    }

    [[nodiscard]]
    auto cell_count() const -> usize {
        return m_value.size() / CELL_SIZE;
    }

    /// The value as exactly one 32-bit cell.
    [[nodiscard]]
    auto as_u32() const -> Result<u32> {
        if (m_value.size() != sizeof(u32)) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(load_be32(m_value.data()));
    }

    /// The value as exactly two cells, high one first.
    [[nodiscard]]
    auto as_u64() const -> Result<u64> {
        if (m_value.size() != sizeof(u64)) {
            return ustl::err(Status::InvArgs);
        }
        return ustl::ok(load_be64(m_value.data()));
    }

    /// The value as exactly one string. The value must end in a NUL and hold no
    /// other, so a string list (`compatible`) is refused rather than silently
    /// cut to its first entry.
    [[nodiscard]]
    auto as_string() const -> Result<std::string_view> {
        if (m_value.empty() || m_value.back() != 0) {
            return ustl::err(Status::InvArgs);
        }
        for (usize i = 0; i + 1 < m_value.size(); ++i) {
            if (m_value[i] == 0) {
                return ustl::err(Status::InvArgs);
            }
        }
        return ustl::ok(
            std::string_view(reinterpret_cast<char const *>(m_value.data()), m_value.size() - 1));
    }

    /// `count` cells (one or two) starting at cell `index`, combined into one
    /// number, high cell first.
    [[nodiscard]]
    auto cells(usize const index, usize const count) const -> Result<u64> {
        if (count != 1 && count != 2) {
            return ustl::err(Status::InvArgs);
        }
        if (index > cell_count() || count > cell_count() - index) {
            return ustl::err(Status::OutOfRange);
        }
        auto const *const at = m_value.data() + index * CELL_SIZE;
        if (count == 1) {
            return ustl::ok(u64{load_be32(at)});
        }
        return ustl::ok(load_be64(at));
    }

    /// Entry `index` of a `reg` property, given the `#address-cells` and
    /// `#size-cells` of the node's parent: the property is a run of
    /// (address, size) pairs of that many cells each. A size of zero cells
    /// reads as size 0.
    [[nodiscard]]
    auto reg(usize const address_cells, usize const size_cells, usize const index) const
        -> Result<RegEntry> {
        if (address_cells < 1 || address_cells > 2 || size_cells > 2) {
            return ustl::err(Status::InvArgs);
        }
        auto const stride = address_cells + size_cells;
        if (index >= cell_count() / stride) {
            return ustl::err(Status::OutOfRange);
        }

        auto const first = index * stride;
        auto const address = cells(first, address_cells);
        if (address.is_err()) {
            return ustl::err(address.unwrap_err());
        }
        auto size = u64{0};
        if (size_cells != 0) {
            auto const decoded = cells(first + address_cells, size_cells);
            if (decoded.is_err()) {
                return ustl::err(decoded.unwrap_err());
            }
            size = decoded.unwrap();
        }
        return ustl::ok(RegEntry{.address = address.unwrap(), .size = size});
    }

  private:
    friend class Node;

    Property(std::string_view const name, Bytes const value) : m_name(name), m_value(value) {}

    std::string_view m_name;
    Bytes m_value;
};

namespace detail {
    /// One token of the structure block, decoded.
    struct Item final {
        Token token;

        /// Offset of the token after this one.
        usize next;

        /// BeginNode: the node's name. Prop: the property's name.
        std::string_view name{};

        /// Prop: the property's value.
        Bytes value{};
    };

    /// A node name matches a requested one when they are equal, or when the
    /// request has no unit address (`memory`) and the node has one
    /// (`memory@80000000`).
    inline auto name_matches(std::string_view const node_name, std::string_view const wanted)
        -> bool {
        if (node_name == wanted) {
            return true;
        }
        if (wanted.find('@') != std::string_view::npos) {
            return false;
        }
        return node_name.size() > wanted.size() && node_name.starts_with(wanted) &&
               node_name[wanted.size()] == '@';
    }

    /// The structure and strings blocks, and the one way either is read.
    ///
    /// Every read is bounds-checked against the block it falls in and reports
    /// failure rather than reading on. `Tree::from()` walks the whole structure
    /// block once through here, so a malformed blob is refused up front; the
    /// accessors that come later go through the same checks, so they cannot read
    /// out of bounds even if the memory changed after that walk.
    class Blocks final {
      public:
        Blocks(Bytes const structure, Bytes const strings)
            : m_structure(structure), m_strings(strings) {}

        /// The offset one past the last byte of the structure block.
        [[nodiscard]]
        auto end_offset() const -> usize {
            return m_structure.size();
        }

        [[nodiscard]]
        auto item_at(usize const offset) const -> Result<Item> {
            auto const size = m_structure.size();
            if (offset % TOKEN_ALIGN != 0 || offset > size || size - offset < TOKEN_ALIGN) {
                return ustl::err(Status::OutOfRange);
            }

            auto const *const base = m_structure.data();
            auto const body = offset + TOKEN_ALIGN;
            switch (load_be32(base + offset)) {
                case static_cast<u32>(Token::BeginNode): {
                    auto const name = c_string(m_structure, body);
                    if (name.is_none()) {
                        return ustl::err(Status::InvArgs);
                    }
                    auto const next = align_up(body + name.value().size() + 1, TOKEN_ALIGN);
                    if (next > size) {
                        return ustl::err(Status::InvArgs);
                    }
                    return ustl::ok(
                        Item{.token = Token::BeginNode, .next = next, .name = name.value()});
                }
                case static_cast<u32>(Token::Prop): {
                    // len and nameoff, then the value.
                    if (size - body < 2 * sizeof(u32)) {
                        return ustl::err(Status::InvArgs);
                    }
                    auto const length = usize{load_be32(base + body)};
                    auto const name_offset = usize{load_be32(base + body + sizeof(u32))};
                    auto const value_offset = body + 2 * sizeof(u32);
                    if (length > size - value_offset) {
                        return ustl::err(Status::InvArgs);
                    }
                    auto const next = align_up(value_offset + length, TOKEN_ALIGN);
                    if (next > size) {
                        return ustl::err(Status::InvArgs);
                    }
                    auto const name = c_string(m_strings, name_offset);
                    if (name.is_none()) {
                        return ustl::err(Status::InvArgs);
                    }
                    return ustl::ok(Item{
                        .token = Token::Prop,
                        .next = next,
                        .name = name.value(),
                        .value = Bytes(base + value_offset, length),
                    });
                }
                case static_cast<u32>(Token::EndNode): {
                    return ustl::ok(Item{.token = Token::EndNode, .next = body});
                }
                case static_cast<u32>(Token::Nop): {
                    return ustl::ok(Item{.token = Token::Nop, .next = body});
                }
                case static_cast<u32>(Token::End): {
                    return ustl::ok(Item{.token = Token::End, .next = body});
                }
                default: {
                    return ustl::err(Status::InvArgs);
                }
            }
        }

        /// Given the offset of a BeginNode, the offset just past its matching
        /// EndNode: the node, its properties and all its descendants skipped.
        [[nodiscard]]
        auto skip_subtree(usize const begin) const -> Result<usize> {
            auto depth = usize{0};
            auto offset = begin;
            while (true) {
                auto const item = item_at(offset);
                if (item.is_err()) {
                    return ustl::err(item.unwrap_err());
                }
                switch (item.unwrap().token) {
                    case Token::BeginNode: {
                        ++depth;
                        break;
                    }
                    case Token::EndNode: {
                        --depth;
                        if (depth == 0) {
                            return ustl::ok(item.unwrap().next);
                        }
                        break;
                    }
                    case Token::End: {
                        return ustl::err(Status::InvArgs);
                    }
                    case Token::Prop:
                    case Token::Nop: {
                        break;
                    }
                }
                offset = item.unwrap().next;
            }
        }

      private:
        /// The string starting at `start` in `block`, without its NUL; none if
        /// the block ends before one.
        static auto c_string(Bytes const block, usize const start)
            -> ustl::Option<std::string_view> {
            for (auto i = start; i < block.size(); ++i) {
                if (block[i] == 0) {
                    return ustl::Option<std::string_view>(std::string_view(
                        reinterpret_cast<char const *>(block.data() + start), i - start));
                }
            }
            return ustl::NONE;
        }

        Bytes m_structure;
        Bytes m_strings;
    };

    /// `#address-cells` and `#size-cells` when a node does not say.
    constexpr usize DEFAULT_ADDRESS_CELLS = 2;
    constexpr usize DEFAULT_SIZE_CELLS = 1;

    /// More than two cells (64 bits) of address or size is not something this
    /// reader decodes.
    constexpr usize MAX_CELLS = 2;
} // namespace detail

/// A node of the tree: a name, properties, and child nodes.
///
/// A node is a small value (a few pointers and an offset) that can be copied
/// freely. It borrows the blob and must not outlive it. Properties are read
/// before any child, the order the format writes them in; a property listed
/// after a child is not seen.
class Node final {
  public:
    /// The children of a node, in the order they were written.
    class Children final {
      public:
        [[nodiscard]]
        auto next() -> ustl::Option<Node> {
            while (true) {
                auto const item = m_blocks.item_at(m_cursor);
                if (item.is_err()) {
                    return stop();
                }
                switch (item.unwrap().token) {
                    case Token::Prop:
                    case Token::Nop: {
                        m_cursor = item.unwrap().next;
                        break;
                    }
                    case Token::BeginNode: {
                        // Continue after the child's whole subtree, so the next call
                        // yields its sibling, not its first child.
                        auto const after = m_blocks.skip_subtree(m_cursor);
                        if (after.is_err()) {
                            return stop();
                        }
                        m_cursor = after.unwrap();
                        return ustl::Option<Node>(
                            Node(m_blocks, item.unwrap().name, item.unwrap().next));
                    }
                    case Token::EndNode:
                    case Token::End: {
                        return stop();
                    }
                }
            }
        }

      private:
        friend class Node;

        Children(detail::Blocks const blocks, usize const cursor)
            : m_blocks(blocks), m_cursor(cursor) {}

        /// Park the cursor at the end of the block, where every later read fails
        /// and so every later call ends here again.
        auto stop() -> ustl::Option<Node> {
            m_cursor = m_blocks.end_offset();
            return ustl::NONE;
        }

        detail::Blocks m_blocks;
        usize m_cursor;
    };

    /// The node's name, with its unit address if it has one: `memory@80000000`.
    /// The root's name is empty.
    [[nodiscard]]
    auto name() const -> std::string_view {
        return m_name;
    }

    [[nodiscard]]
    auto property(std::string_view const name) const -> ustl::Option<Property> {
        auto offset = m_body;
        while (true) {
            auto const item = m_blocks.item_at(offset);
            if (item.is_err()) {
                return ustl::NONE;
            }
            switch (item.unwrap().token) {
                case Token::Prop: {
                    if (item.unwrap().name == name) {
                        return ustl::Option<Property>(
                            Property(item.unwrap().name, item.unwrap().value));
                    }
                    break;
                }
                case Token::Nop: {
                    break;
                }
                case Token::BeginNode:
                case Token::EndNode:
                case Token::End: {
                    return ustl::NONE;
                }
            }
            offset = item.unwrap().next;
        }
    }

    [[nodiscard]]
    auto children() const -> Children {
        return Children(m_blocks, m_body);
    }

    /// The first child `detail::name_matches` accepts for `wanted`.
    [[nodiscard]]
    auto child(std::string_view const wanted) const -> ustl::Option<Node> {
        auto children = this->children();
        while (true) {
            auto const node = children.next();
            if (node.is_none()) {
                return ustl::NONE;
            }
            if (detail::name_matches(node.value().name(), wanted)) {
                return node;
            }
        }
    }

    /// The number of cells the children's addresses take, from this node's
    /// `#address-cells`.
    [[nodiscard]]
    auto address_cells() const -> Result<usize> {
        return cell_width("#address-cells", detail::DEFAULT_ADDRESS_CELLS);
    }

    /// The number of cells the children's sizes take, from `#size-cells`.
    [[nodiscard]]
    auto size_cells() const -> Result<usize> {
        return cell_width("#size-cells", detail::DEFAULT_SIZE_CELLS);
    }

  private:
    friend class Tree;

    Node(detail::Blocks const blocks, std::string_view const name, usize const body)
        : m_blocks(blocks), m_name(name), m_body(body) {}

    [[nodiscard]]
    auto cell_width(std::string_view const name, usize const fallback) const -> Result<usize> {
        auto const found = property(name);
        if (found.is_none()) {
            return ustl::ok(fallback);
        }
        auto const width = found.value().as_u32();
        if (width.is_err()) {
            return ustl::err(width.unwrap_err());
        }
        if (width.unwrap() > detail::MAX_CELLS) {
            return ustl::err(Status::NotSupported);
        }
        return ustl::ok(usize{width.unwrap()});
    }

    detail::Blocks m_blocks;
    std::string_view m_name;

    /// Offset of the first token after the BeginNode and its name: the node's
    /// first property, or its first child, or its EndNode.
    usize m_body;
};

/// A device tree blob, checked and ready to read.
///
/// The blob comes from firmware, so it is input to be distrusted, not a
/// structure to be trusted: `from()` refuses anything whose header, blocks or
/// token stream do not hold together, and what it accepts is read through
/// bounds-checked accessors. It does not allocate; the tree is read in place
/// and borrows the blob, which must stay where it is.
class Tree final {
  public:
    /// A range the blob asks to be kept clear of, from the reservation block.
    struct Reservation final {
        u64 address;
        u64 size;
    };

    /// Check the blob at `data`, of which `size` bytes are readable, and
    /// return a view of it. The blob's own `totalsize` says where it ends and
    /// may be smaller than `size`.
    [[nodiscard]]
    static auto from(u8 const *const data, usize const size) -> Result<Tree> {
        if (data == nullptr || size < HEADER_SIZE) {
            return ustl::err(Status::InvArgs);
        }
        if (load_be32(data + header::MAGIC_OFFSET) != MAGIC) {
            return ustl::err(Status::InvArgs);
        }
        auto const total = usize{load_be32(data + header::TOTAL_SIZE)};
        if (total < HEADER_SIZE || total > size) {
            return ustl::err(Status::InvArgs);
        }
        if (load_be32(data + header::VERSION_OFFSET) < VERSION ||
            load_be32(data + header::LAST_COMPATIBLE_VERSION) > VERSION) {
            return ustl::err(Status::NotSupported);
        }

        auto const blob = Bytes(data, total);
        auto const structure = block(blob, load_be32(data + header::STRUCTURE_OFFSET),
                                     load_be32(data + header::STRUCTURE_SIZE), TOKEN_ALIGN);
        auto const strings = block(blob, load_be32(data + header::STRINGS_OFFSET),
                                   load_be32(data + header::STRINGS_SIZE), 1);
        if (structure.is_none() || strings.is_none()) {
            return ustl::err(Status::InvArgs);
        }

        auto const reservation_offset = usize{load_be32(data + header::RESERVATION_OFFSET)};
        auto const reservation_count = count_reservations(blob, reservation_offset);
        if (reservation_count.is_err()) {
            return ustl::err(reservation_count.unwrap_err());
        }

        auto const blocks = detail::Blocks(structure.value(), strings.value());
        auto const root_body = walk(blocks);
        if (root_body.is_err()) {
            return ustl::err(root_body.unwrap_err());
        }
        return ustl::ok(Tree(blob, Node(blocks, std::string_view{}, root_body.unwrap()),
                             reservation_offset, reservation_count.unwrap(),
                             load_be32(data + header::BOOT_CPU)));
    }

    /// The whole blob, `totalsize` bytes: what a consumer that is about to
    /// claim memory has to keep out of its way.
    [[nodiscard]]
    auto blob() const -> Bytes {
        return m_blob;
    }

    /// The physical ID of the CPU the firmware booted on, from the header.
    [[nodiscard]]
    auto boot_cpu() const -> u32 {
        return m_boot_cpu;
    }

    [[nodiscard]]
    auto root() const -> Node {
        return m_root;
    }

    /// The node at `path`: names separated by `/`, from the root. A name may
    /// leave out its unit address (`/memory`), in which case the first node
    /// that has one matches.
    [[nodiscard]]
    auto find(std::string_view const path) const -> ustl::Option<Node> {
        if (path.empty() || path.front() != '/') {
            return ustl::NONE;
        }

        auto node = m_root;
        auto rest = path.substr(1);
        while (!rest.empty()) {
            auto const slash = rest.find('/');
            auto const component = rest.substr(0, slash);
            if (slash == std::string_view::npos) {
                rest = std::string_view{};
            } else {
                rest = rest.substr(slash + 1);
            }
            if (component.empty()) {
                continue;
            }

            auto const next = node.child(component);
            if (next.is_none()) {
                return ustl::NONE;
            }
            node = next.value();
        }
        return ustl::Option<Node>(node);
    }

    [[nodiscard]]
    auto reservation_count() const -> usize {
        return m_reservation_count;
    }

    /// Entry `index` of the reservation block; none past the last.
    [[nodiscard]]
    auto reservation(usize const index) const -> ustl::Option<Reservation> {
        if (index >= m_reservation_count) {
            return ustl::NONE;
        }
        auto const *const at =
            m_blob.data() + m_reservation_offset + index * RESERVATION_ENTRY_SIZE;
        return ustl::Option<Reservation>(
            Reservation{.address = load_be64(at), .size = load_be64(at + sizeof(u64))});
    }

  private:
    Tree(Bytes const blob, Node const root, usize const reservation_offset,
         usize const reservation_count, u32 const boot_cpu)
        : m_blob(blob)
        , m_root(root)
        , m_reservation_offset(reservation_offset)
        , m_reservation_count(reservation_count)
        , m_boot_cpu(boot_cpu) {}

    /// The `length` bytes at `offset` in `blob`, if they lie inside it and
    /// `offset` is aligned.
    static auto block(Bytes const blob, usize const offset, usize const length,
                      usize const alignment) -> ustl::Option<Bytes> {
        if (offset % alignment != 0 || offset > blob.size() || length > blob.size() - offset) {
            return ustl::NONE;
        }
        return ustl::Option<Bytes>(blob.subspan(offset, length));
    }

    /// How many entries the reservation block holds before its terminating
    /// pair of zeros, which must be inside the blob.
    static auto count_reservations(Bytes const blob, usize const offset) -> Result<usize> {
        if (offset % RESERVATION_ALIGN != 0 || offset > blob.size()) {
            return ustl::err(Status::InvArgs);
        }

        auto count = usize{0};
        auto cursor = offset;
        while (true) {
            if (blob.size() - cursor < RESERVATION_ENTRY_SIZE) {
                return ustl::err(Status::InvArgs);
            }
            auto const *const at = blob.data() + cursor;
            if (load_be64(at) == 0 && load_be64(at + sizeof(u64)) == 0) {
                return ustl::ok(count);
            }
            ++count;
            cursor += RESERVATION_ENTRY_SIZE;
        }
    }

    /// Read the whole structure block once, token by token, and refuse it unless
    /// it is one root node (nameless) holding properly nested nodes and
    /// properties, followed by End as the very last token. On success, the
    /// offset of the root's first token after its name.
    static auto walk(detail::Blocks const &blocks) -> Result<usize> {
        auto offset = usize{0};
        auto depth = usize{0};
        auto seen_root = false;
        auto root_body = usize{0};
        while (true) {
            auto const item = blocks.item_at(offset);
            if (item.is_err()) {
                return ustl::err(item.unwrap_err());
            }
            switch (item.unwrap().token) {
                case Token::BeginNode: {
                    if (depth == 0) {
                        if (seen_root || !item.unwrap().name.empty()) {
                            return ustl::err(Status::InvArgs);
                        }
                        seen_root = true;
                        root_body = item.unwrap().next;
                    }
                    if (depth == MAX_DEPTH) {
                        return ustl::err(Status::OutOfRange);
                    }
                    ++depth;
                    break;
                }
                case Token::EndNode: {
                    if (depth == 0) {
                        return ustl::err(Status::InvArgs);
                    }
                    --depth;
                    break;
                }
                case Token::Prop: {
                    if (depth == 0) {
                        return ustl::err(Status::InvArgs);
                    }
                    break;
                }
                case Token::Nop: {
                    break;
                }
                case Token::End: {
                    if (depth != 0 || !seen_root || item.unwrap().next != blocks.end_offset()) {
                        return ustl::err(Status::InvArgs);
                    }
                    return ustl::ok(root_body);
                }
            }
            offset = item.unwrap().next;
        }
    }

    Bytes m_blob;
    Node m_root;
    usize m_reservation_offset;
    usize m_reservation_count;
    u32 m_boot_cpu;
};

} // namespace fdt

#endif // FDT_TREE_HPP
