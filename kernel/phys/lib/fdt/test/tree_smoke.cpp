// fdt::Tree against blobs built by hand: a well-formed one read every way the
// library offers, then the same blob broken one way at a time. Each break must
// be refused with the status that names it, and none may read outside the blob.

#include <initializer_list>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include <cstdio>
#include <cstdlib>

#include <fdt/tree.hpp>

namespace {
using fdt::Status;
using fdt::Token;
using fdt::Tree;

auto require(bool const condition,
             std::source_location const where = std::source_location::current()) -> void {
    if (!condition) {
        std::fprintf(stderr, "tree_smoke: failed at line %u\n", where.line());
        std::abort();
    }
}

auto append_be32(std::vector<u8> &bytes, u32 const value) -> void {
    bytes.push_back(static_cast<u8>(value >> 24));
    bytes.push_back(static_cast<u8>(value >> 16));
    bytes.push_back(static_cast<u8>(value >> 8));
    bytes.push_back(static_cast<u8>(value));
}

auto append_be64(std::vector<u8> &bytes, u64 const value) -> void {
    append_be32(bytes, static_cast<u32>(value >> 32));
    append_be32(bytes, static_cast<u32>(value));
}

auto put_be32(std::vector<u8> &bytes, usize const offset, u32 const value) -> void {
    bytes[offset] = static_cast<u8>(value >> 24);
    bytes[offset + 1] = static_cast<u8>(value >> 16);
    bytes[offset + 2] = static_cast<u8>(value >> 8);
    bytes[offset + 3] = static_cast<u8>(value);
}

auto pad_to_four(std::vector<u8> &bytes) -> void {
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }
}

/// Builds a blob: the header, the reservation block, the structure block and
/// the strings block, in that order. Tokens are appended one at a time, so a
/// test can write a stream the library must refuse.
class Builder final {
  public:
    auto word(u32 const value) -> Builder & {
        append_be32(m_structure, value);
        return *this;
    }

    auto raw(std::vector<u8> const &bytes) -> Builder & {
        m_structure.insert(m_structure.end(), bytes.begin(), bytes.end());
        return *this;
    }

    auto begin(std::string_view const name) -> Builder & {
        word(static_cast<u32>(Token::BeginNode));
        m_structure.insert(m_structure.end(), name.begin(), name.end());
        m_structure.push_back(0);
        pad_to_four(m_structure);
        return *this;
    }

    auto end() -> Builder & {
        return word(static_cast<u32>(Token::EndNode));
    }

    auto nop() -> Builder & {
        return word(static_cast<u32>(Token::Nop));
    }

    auto finish() -> Builder & {
        return word(static_cast<u32>(Token::End));
    }

    auto prop(std::string_view const name, std::vector<u8> const &value) -> Builder & {
        word(static_cast<u32>(Token::Prop));
        word(static_cast<u32>(value.size()));
        word(name_offset(name));
        raw(value);
        pad_to_four(m_structure);
        return *this;
    }

    auto prop_u32(std::string_view const name, u32 const value) -> Builder & {
        std::vector<u8> bytes;
        append_be32(bytes, value);
        return prop(name, bytes);
    }

    auto prop_u64(std::string_view const name, u64 const value) -> Builder & {
        std::vector<u8> bytes;
        append_be64(bytes, value);
        return prop(name, bytes);
    }

    auto prop_string(std::string_view const name, std::string_view const value) -> Builder & {
        std::vector<u8> bytes(value.begin(), value.end());
        bytes.push_back(0);
        return prop(name, bytes);
    }

    auto prop_cells(std::string_view const name, std::initializer_list<u32> const cells)
        -> Builder & {
        std::vector<u8> bytes;
        for (auto const cell: cells) {
            append_be32(bytes, cell);
        }
        return prop(name, bytes);
    }

    /// Append text to the strings block with no NUL after it, and return where
    /// it starts: a name that runs off the end of the block.
    auto unterminated_name(std::string_view const text) -> u32 {
        auto const offset = static_cast<u32>(m_strings.size());
        m_strings.append(text);
        return offset;
    }

    auto reserve(u64 const address, u64 const size) -> Builder & {
        append_be64(m_reservations, address);
        append_be64(m_reservations, size);
        return *this;
    }

    /// Leave `bytes` of padding before the structure block. The block itself is
    /// intact, so an odd gap is a structure block that starts off a token
    /// boundary and nothing else is wrong with the blob.
    auto structure_gap(usize const bytes) -> Builder & {
        m_structure_gap = bytes;
        return *this;
    }

    [[nodiscard]]
    auto build() const -> std::vector<u8> {
        std::vector<u8> blob(fdt::HEADER_SIZE, 0);

        auto const reservation_offset = blob.size();
        blob.insert(blob.end(), m_reservations.begin(), m_reservations.end());
        append_be64(blob, 0);
        append_be64(blob, 0);

        blob.insert(blob.end(), m_structure_gap, 0);
        auto const structure_offset = blob.size();
        blob.insert(blob.end(), m_structure.begin(), m_structure.end());

        auto const strings_offset = blob.size();
        blob.insert(blob.end(), m_strings.begin(), m_strings.end());

        put_be32(blob, fdt::header::MAGIC_OFFSET, fdt::MAGIC);
        put_be32(blob, fdt::header::TOTAL_SIZE, static_cast<u32>(blob.size()));
        put_be32(blob, fdt::header::STRUCTURE_OFFSET, static_cast<u32>(structure_offset));
        put_be32(blob, fdt::header::STRINGS_OFFSET, static_cast<u32>(strings_offset));
        put_be32(blob, fdt::header::RESERVATION_OFFSET, static_cast<u32>(reservation_offset));
        put_be32(blob, fdt::header::VERSION_OFFSET, fdt::VERSION);
        put_be32(blob, fdt::header::LAST_COMPATIBLE_VERSION, 16);
        put_be32(blob, fdt::header::BOOT_CPU, 0);
        put_be32(blob, fdt::header::STRINGS_SIZE, static_cast<u32>(m_strings.size()));
        put_be32(blob, fdt::header::STRUCTURE_SIZE, static_cast<u32>(m_structure.size()));
        return blob;
    }

  private:
    /// Property names are shared: each distinct name is stored once.
    auto name_offset(std::string_view const name) -> u32 {
        auto const needle = std::string(name) + '\0';
        auto const found = m_strings.find(needle);
        if (found != std::string::npos) {
            return static_cast<u32>(found);
        }
        auto const offset = static_cast<u32>(m_strings.size());
        m_strings += needle;
        return offset;
    }

    std::vector<u8> m_structure;
    std::vector<u8> m_reservations;
    std::string m_strings;
    usize m_structure_gap = 0;
};

/// A small machine: a memory node, two harts under /cpus, a /chosen with a
/// 64-bit initrd range, one reservation.
auto sample() -> Builder {
    Builder builder;
    builder.begin("")
        .prop_u32("#address-cells", 2)
        .prop_u32("#size-cells", 2)
        .prop_string("model", "ours,test")
        .begin("memory@80000000")
        .prop_string("device_type", "memory")
        .prop_cells("reg", {0, 0x8000'0000, 0, 0x1000'0000})
        .end()
        .begin("cpus")
        .prop_u32("#address-cells", 1)
        .prop_u32("#size-cells", 0)
        .prop_u32("timebase-frequency", 10'000'000)
        .begin("cpu@0")
        .prop_u32("reg", 0)
        .begin("interrupt-controller")
        .end()
        .end()
        .begin("cpu@1")
        .prop_u32("reg", 1)
        .end()
        .end()
        .begin("chosen")
        .prop_u64("linux,initrd-start", 0x8820'0000)
        .prop_u64("linux,initrd-end", 0x8820'1000)
        .end()
        .end()
        .finish();
    builder.reserve(0x8000'0000, 0x4'0000);
    return builder;
}

auto parse(std::vector<u8> const &blob) -> fdt::Result<Tree> {
    return Tree::from(blob.data(), blob.size());
}

auto expect_refused(std::vector<u8> const &blob, Status const expected,
                    std::source_location const where = std::source_location::current()) -> void {
    auto const parsed = parse(blob);
    if (parsed.is_ok() || parsed.unwrap_err() != expected) {
        std::fprintf(stderr, "tree_smoke: line %u: not refused with the expected status\n",
                     where.line());
        std::abort();
    }
}

auto well_formed_blob_reads_back() -> void {
    auto const blob = sample().build();
    auto const parsed = parse(blob);
    require(parsed.is_ok());
    auto const &tree = parsed.unwrap();
    require(tree.blob().size() == blob.size());
    require(tree.boot_cpu() == 0);
    require(tree.root().name().empty());
    require(tree.root().property("model").value().as_string().unwrap() == "ours,test");

    // Reservation block.
    require(tree.reservation_count() == 1);
    require(tree.reservation(0).value().address == 0x8000'0000);
    require(tree.reservation(0).value().size == 0x4'0000);
    require(tree.reservation(1).is_none());

    // Children come in the order they were written, and a child's own children
    // are skipped over, not yielded.
    auto children = tree.root().children();
    require(children.next().value().name() == "memory@80000000");
    require(children.next().value().name() == "cpus");
    require(children.next().value().name() == "chosen");
    require(children.next().is_none());
    require(children.next().is_none());

    auto cpus = tree.find("/cpus").value().children();
    require(cpus.next().value().name() == "cpu@0");
    require(cpus.next().value().name() == "cpu@1");
    require(cpus.next().is_none());

    // Paths: exact, without a unit address, nested, with stray slashes.
    require(tree.find("/").value().name().empty());
    require(tree.find("/memory@80000000").is_some());
    require(tree.find("/memory").value().name() == "memory@80000000");
    require(tree.find("//cpus///cpu@1/").value().name() == "cpu@1");
    require(tree.find("/cpus/cpu").value().name() == "cpu@0");
    require(tree.find("/cpus/cpu@0/interrupt-controller").is_some());
    require(tree.find("/mem").is_none());
    require(tree.find("/memory@0").is_none());
    require(tree.find("cpus").is_none());
    require(tree.find("").is_none());

    // A property is found on its own node and nowhere else.
    require(tree.find("/cpus").value().property("reg").is_none());
    require(tree.find("/cpus/cpu@1").value().property("reg").value().as_u32().unwrap() == 1);
    require(tree.root().property("timebase-frequency").is_none());
}

auto properties_decode() -> void {
    auto const blob = sample().build();
    auto const tree = parse(blob).unwrap();

    auto const memory = tree.find("/memory").value();
    require(memory.property("device_type").value().as_string().unwrap() == "memory");
    require(memory.property("device_type").value().as_u32().is_err());

    auto const reg = memory.property("reg").value();
    require(reg.cell_count() == 4);
    require(reg.cells(1, 1).unwrap() == 0x8000'0000);
    require(reg.cells(0, 2).unwrap() == 0x8000'0000);
    require(reg.cells(2, 2).unwrap() == 0x1000'0000);
    require(reg.cells(3, 2).unwrap_err() == Status::OutOfRange);
    require(reg.cells(5, 1).unwrap_err() == Status::OutOfRange);
    require(reg.cells(0, 3).unwrap_err() == Status::InvArgs);
    require(reg.cells(0, 0).unwrap_err() == Status::InvArgs);

    // reg, given the parent's widths: (2, 2) gives one entry; (1, 1) reads the
    // same bytes as two.
    auto const first = reg.reg(2, 2, 0).unwrap();
    require(first.address == 0x8000'0000 && first.size == 0x1000'0000);
    require(reg.reg(2, 2, 1).unwrap_err() == Status::OutOfRange);
    require(reg.reg(1, 1, 1).unwrap().address == 0);
    require(reg.reg(1, 1, 1).unwrap().size == 0x1000'0000);
    require(reg.reg(1, 1, 2).unwrap_err() == Status::OutOfRange);
    require(reg.reg(0, 1, 0).unwrap_err() == Status::InvArgs);
    require(reg.reg(3, 1, 0).unwrap_err() == Status::InvArgs);
    require(reg.reg(2, 3, 0).unwrap_err() == Status::InvArgs);

    // A size of zero cells reads as size 0.
    require(reg.reg(2, 0, 0).unwrap().size == 0);
    require(reg.reg(2, 0, 1).unwrap().address == 0x1000'0000);

    auto const chosen = tree.find("/chosen").value();
    require(chosen.property("linux,initrd-start").value().as_u64().unwrap() == 0x8820'0000);
    require(chosen.property("linux,initrd-start").value().as_u32().is_err());

    // Cell widths: stated, defaulted, and out of what the reader decodes.
    require(tree.root().address_cells().unwrap() == 2);
    require(tree.find("/cpus").value().address_cells().unwrap() == 1);
    require(tree.find("/cpus").value().size_cells().unwrap() == 0);
    require(tree.find("/chosen").value().address_cells().unwrap() == 2);
    require(tree.find("/chosen").value().size_cells().unwrap() == 1);
}

auto strings_must_be_single_and_terminated() -> void {
    Builder builder;
    builder.begin("")
        .prop("list", {'a', 0, 'b', 0})
        .prop("unterminated", {'a', 'b'})
        .prop("empty", {})
        .prop("bare-nul", {0})
        .end()
        .finish();
    auto const blob = builder.build();
    auto const tree = parse(blob).unwrap();
    auto const root = tree.root();
    require(root.property("list").value().as_string().is_err());
    require(root.property("unterminated").value().as_string().is_err());
    require(root.property("empty").value().as_string().is_err());
    require(root.property("bare-nul").value().as_string().unwrap().empty());
}

auto nops_are_skipped() -> void {
    Builder builder;
    builder.nop()
        .begin("")
        .nop()
        .prop_u32("a", 1)
        .nop()
        .begin("child")
        .nop()
        .prop_u32("b", 2)
        .end()
        .nop()
        .end()
        .nop()
        .finish();
    auto const blob = builder.build();
    auto const tree = parse(blob).unwrap();
    require(tree.root().property("a").value().as_u32().unwrap() == 1);
    require(tree.find("/child").value().property("b").value().as_u32().unwrap() == 2);
}

auto a_property_after_a_child_is_not_seen() -> void {
    // The format writes properties first. A reader that kept scanning past a
    // child would find "late" and be wrong about which node it belongs to.
    Builder builder;
    builder.begin("").prop_u32("early", 1).begin("child").end().prop_u32("late", 2).end().finish();
    auto const blob = builder.build();
    auto const tree = parse(blob).unwrap();
    require(tree.root().property("early").is_some());
    require(tree.root().property("late").is_none());
}

auto reservations() -> void {
    {
        Builder builder;
        builder.begin("").end().finish();
        auto const blob = builder.build();
        require(parse(blob).unwrap().reservation_count() == 0);
    }
    {
        Builder builder;
        builder.begin("").end().finish();
        builder.reserve(1, 2).reserve(3, 4).reserve(5, 6);
        auto const blob = builder.build();
        auto const tree = parse(blob).unwrap();
        require(tree.reservation_count() == 3);
        require(tree.reservation(2).value().address == 5);
        require(tree.reservation(2).value().size == 6);
    }
    {
        // A pair with one half zero is a reservation, not the terminator.
        Builder builder;
        builder.begin("").end().finish();
        builder.reserve(0, 7).reserve(7, 0);
        auto const blob = builder.build();
        require(parse(blob).unwrap().reservation_count() == 2);
    }
}

auto header_is_checked() -> void {
    auto const good = sample().build();
    require(parse(good).is_ok());

    require(Tree::from(nullptr, 100).unwrap_err() == Status::InvArgs);
    require(Tree::from(good.data(), fdt::HEADER_SIZE - 1).unwrap_err() == Status::InvArgs);

    {
        auto blob = good;
        put_be32(blob, fdt::header::MAGIC_OFFSET, 0xd00dfeee);
        expect_refused(blob, Status::InvArgs);
    }
    {
        // totalsize claims more than the caller can read.
        require(Tree::from(good.data(), good.size() - 1).unwrap_err() == Status::InvArgs);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::TOTAL_SIZE, fdt::HEADER_SIZE - 1);
        expect_refused(blob, Status::InvArgs);
    }
    {
        // Bytes after totalsize are not part of the blob and are not read.
        auto blob = good;
        blob.insert(blob.end(), 64, 0xff);
        auto const tree = parse(blob).unwrap();
        require(tree.blob().size() == good.size());
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::VERSION_OFFSET, 16);
        expect_refused(blob, Status::NotSupported);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::LAST_COMPATIBLE_VERSION, fdt::VERSION + 1);
        expect_refused(blob, Status::NotSupported);
    }
    {
        // A newer blob that an older reader can still read is accepted.
        auto blob = good;
        put_be32(blob, fdt::header::VERSION_OFFSET, fdt::VERSION + 1);
        put_be32(blob, fdt::header::LAST_COMPATIBLE_VERSION, fdt::VERSION);
        require(parse(blob).is_ok());
    }

    auto const total = static_cast<u32>(good.size());
    {
        auto blob = good;
        put_be32(blob, fdt::header::STRUCTURE_OFFSET, total + 4);
        expect_refused(blob, Status::InvArgs);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::STRUCTURE_SIZE, total);
        expect_refused(blob, Status::InvArgs);
    }
    {
        // The structure block starts on a token boundary: the same block, one
        // byte further on, is refused for that alone, and four bytes further on
        // is accepted.
        auto builder = sample();
        require(parse(builder.structure_gap(4).build()).is_ok());
        expect_refused(builder.structure_gap(1).build(), Status::InvArgs);
        expect_refused(builder.structure_gap(6).build(), Status::InvArgs);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::STRINGS_OFFSET, total + 1);
        expect_refused(blob, Status::InvArgs);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::STRINGS_SIZE, total);
        expect_refused(blob, Status::InvArgs);
    }
    {
        // Offset plus size that wraps a 32-bit sum is still out of range.
        auto blob = good;
        put_be32(blob, fdt::header::STRINGS_OFFSET, 0xffff'fff8);
        put_be32(blob, fdt::header::STRINGS_SIZE, 0x10);
        expect_refused(blob, Status::InvArgs);
    }
    {
        // The reservation block starts on an 8-byte boundary. The bytes from
        // the header to the structure block are all zero here, so a block read
        // from the misaligned offset would find its terminator at once: only
        // the alignment rule refuses it.
        Builder builder;
        builder.begin("").end().finish().structure_gap(16);
        auto blob = builder.build();
        put_be32(blob, fdt::header::RESERVATION_OFFSET, static_cast<u32>(fdt::HEADER_SIZE + 8));
        require(parse(blob).is_ok());
        put_be32(blob, fdt::header::RESERVATION_OFFSET, static_cast<u32>(fdt::HEADER_SIZE + 4));
        expect_refused(blob, Status::InvArgs);
    }
    {
        // The reservation block has no terminator before the blob ends.
        auto blob = good;
        put_be32(blob, fdt::header::RESERVATION_OFFSET, (total - 8) & ~7u);
        expect_refused(blob, Status::InvArgs);
    }
    {
        auto blob = good;
        put_be32(blob, fdt::header::RESERVATION_OFFSET, total + 8);
        expect_refused(blob, Status::InvArgs);
    }
}

auto structure_is_checked() -> void {
    {
        // No End token.
        Builder builder;
        builder.begin("").end();
        expect_refused(builder.build(), Status::OutOfRange);
    }
    {
        // Nothing at all.
        expect_refused(Builder{}.build(), Status::OutOfRange);
    }
    {
        // End before the root closes.
        Builder builder;
        builder.begin("").finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A node left open.
        Builder builder;
        builder.begin("").begin("a").end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // One EndNode too many.
        Builder builder;
        builder.begin("").end().end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A property before the root.
        Builder builder;
        builder.prop_u32("a", 1).begin("").end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A property after the root has closed.
        Builder builder;
        builder.begin("").end().prop_u32("a", 1).finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // Two roots.
        Builder builder;
        builder.begin("").end().begin("").end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // The root has no name.
        Builder builder;
        builder.begin("root").end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // Bytes after End.
        Builder builder;
        builder.begin("").end().finish().word(0);
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A token that is not one.
        Builder builder;
        builder.begin("").word(7).end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A node name with no NUL before the block ends.
        Builder builder;
        builder.word(static_cast<u32>(Token::BeginNode)).raw({'a', 'b', 'c', 'd'});
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A property cut off inside its own header.
        Builder builder;
        builder.begin("").word(static_cast<u32>(Token::Prop)).word(4);
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A value longer than what is left of the block.
        Builder builder;
        builder.begin("").word(static_cast<u32>(Token::Prop)).word(100).word(0);
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A length so large that adding it to the offset would wrap.
        Builder builder;
        builder.begin("").word(static_cast<u32>(Token::Prop)).word(0xffff'ffff).word(0);
        expect_refused(builder.build(), Status::InvArgs);
    }
    {
        // A name offset outside the strings block.
        Builder bad;
        bad.begin("")
            .prop_u32("a", 1)
            .word(static_cast<u32>(Token::Prop))
            .word(0)
            .word(9999)
            .end()
            .finish();
        expect_refused(bad.build(), Status::InvArgs);
    }
    {
        // A name that runs off the end of the strings block.
        Builder builder;
        builder.begin("");
        auto const offset = builder.unterminated_name("oops");
        builder.word(static_cast<u32>(Token::Prop)).word(0).word(offset).end().finish();
        expect_refused(builder.build(), Status::InvArgs);
    }
}

auto nesting_is_bounded() -> void {
    auto const nested = [](usize const levels) {
        Builder builder;
        for (usize i = 0; i < levels; ++i) {
            if (i == 0) {
                builder.begin("");
            } else {
                builder.begin("n");
            }
        }
        for (usize i = 0; i < levels; ++i) {
            builder.end();
        }
        builder.finish();
        return builder.build();
    };
    require(parse(nested(fdt::MAX_DEPTH)).is_ok());
    expect_refused(nested(fdt::MAX_DEPTH + 1), Status::OutOfRange);

    // Reading a deep tree does not recurse: the deepest node is found and its
    // siblings skipped without growing the stack.
    auto const blob = nested(fdt::MAX_DEPTH);
    auto const tree = parse(blob).unwrap();
    auto path = std::string();
    for (usize i = 1; i < fdt::MAX_DEPTH; ++i) {
        path += "/n";
    }
    require(tree.find(path).is_some());
    require(tree.find(path + "/n").is_none());
}

} // namespace

auto main() -> int {
    well_formed_blob_reads_back();
    properties_decode();
    strings_must_be_single_and_terminated();
    nops_are_skipped();
    a_property_after_a_child_is_not_seen();
    reservations();
    header_is_checked();
    structure_is_checked();
    nesting_is_bounded();
    return 0;
}
