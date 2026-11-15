/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
//
// Declared options for console commands. A command builds an Options in its
// handler, names and types the words it accepts, and hands the console's
// words to parse(); the answer is either every value filled or one rejection
// naming the rule the line broke and the word that broke it. This replaces
// the hand-written word loops, strcmp chains and digit parsers each command
// used to carry.
//
// The grammar is the tree's one grammar: every word is `name` (a boolean set
// to true) or `name=value`. No dashes, no quoting -- the shell has already
// removed the spaces, so everything else is part of the token. All tables
// are fixed-size, sized to the console's sixteen-word cap: a command line
// cannot carry more words than the tables hold, and nothing here allocates.
//
// This header is pure string arithmetic over the console's words, free of
// kernel headers, so the host tests exercise the same parsing a command runs.
// The reporting half -- the pinned error lines and the generated usage -- is
// <gktl/report.hpp>, which needs the log.
#ifndef GKTL_OPTIONS_HPP
#define GKTL_OPTIONS_HPP 1

#include <gktl/command.hpp>
#include <gktl/types.hpp>

#include <array>
#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>

#include <ours/assert.hpp>
#include <ours/status.hpp>
#include <ustl/option.hpp>
#include <ustl/result.hpp>

namespace gktl {

/// One console word: `name=value`, or `name` alone. A bare word carries an
/// empty |value| and |has_value| false; `name=` carries an empty |value| and
/// |has_value| true, and the two mean different things to a boolean option.
struct Word {
    std::string_view name;
    std::string_view value;
    bool has_value{false};
};

/// The words one command line can carry: the console splits a line into at
/// most sixteen arguments including the command itself, so fifteen remain for
/// the options; sixteen leaves slack for a host test driving the full table.
inline constexpr usize MAX_OPTIONS = 16;

/// Splits one token on its first `=`. None when the name is empty (`=x`,
/// `=`, the empty word), which no option can be named.
[[nodiscard]]
constexpr auto parse_word(std::string_view const token) -> ustl::Option<Word> {
    auto const equals = token.find('=');
    if (equals == std::string_view::npos) {
        if (token.empty()) {
            return ustl::NONE;
        }
        return Word{.name = token, .value = std::string_view{}, .has_value = false};
    }
    auto const name = token.substr(0, equals);
    if (name.empty()) {
        return ustl::NONE;
    }
    return Word{.name = name, .value = token.substr(equals + 1), .has_value = true};
}

/// Parses an unsigned decimal or hexadecimal digit run: digits only, no sign,
/// no separators, |base| 10 or 16. InvArgs for an empty value, a character
/// that is not a digit of |base|, or a value beyond u64's range. The one
/// digit primitive every number-shaped word goes through; the ktest runner
/// plans parse through it too.
[[nodiscard]]
constexpr auto parse_u64(std::string_view const digits, u64 const base = 10)
    -> ustl::Result<u64, ours::Status> {
    ASSERT(base == 10 || base == 16, "parse_u64 reads base 10 or 16, not {}", base);
    if (digits.empty()) {
        return ustl::err(ours::Status::InvArgs);
    }
    auto parsed = u64{0};
    for (auto const c: digits) {
        auto digit = u64{0};
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a') + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A') + 10;
        } else {
            return ustl::err(ours::Status::InvArgs);
        }
        if (parsed > (~u64{0} - digit) / base) {
            return ustl::err(ours::Status::InvArgs);
        }
        parsed = parsed * base + digit;
    }
    return ustl::ok(parsed);
}

/// The value half of a boolean option: `1`/`true`/`yes` or `0`/`false`/`no`,
/// and none for anything else, an empty value included. The same strict word
/// list `obi::CommandLineView::parse_bool` accepts for boot options, for the
/// same reason it states there: a word that meant true to one command and
/// false to another would be a console that quietly did half of what it was
/// told. The bare `name` half -- no `=` at all -- is true, and that half is
/// the parse loop's to answer.
[[nodiscard]]
constexpr auto parse_bool_strict(std::string_view const value) -> ustl::Option<bool> {
    if (value == "1" || value == "true" || value == "yes") {
        return ustl::Option<bool>(true);
    }
    if (value == "0" || value == "false" || value == "no") {
        return ustl::Option<bool>(false);
    }
    return ustl::NONE;
}

/// A custom parse hook for a number-shaped option whose words carry more than
/// plain digits -- `0x`-prefixed seeds, for one. It answers the value's u64 or
/// why it could not. A value that is really several values (`shard=M/N`) is
/// not one u64 and does not pretend: declare it a string option and parse it
/// in the handler, where the structured shape is known.
using U64Parse = auto (*)(std::string_view) -> ustl::Result<u64, ours::Status>;

/// Which shape an option's words carry and its slot stores.
enum class OptionKind : u8 {
    /// Unsigned digits through parse_u64, or through the option's hook.
    U64,

    /// A bare word, or a strict `1/true/yes` vs `0/false/no` value.
    Bool,

    /// The word as it stands, `=` and spaces already handled by the shell.
    Str,
};

/// One stored value: whichever member the option's kind names. Reading a
/// member the kind does not name is a misuse the kind check at the accessor
/// catches; the union itself is dumb storage.
struct Value {
    union {
        u64 i;
        bool b;
        std::string_view s;
    };

    constexpr Value() : i{0} {}
    constexpr Value(u64 const value) : i{value} {}
    constexpr Value(bool const value) : b{value} {}
    constexpr Value(std::string_view const value) : s{value} {}
};
static_assert(std::is_trivially_copyable_v<Value>);

/// What the option's declaration and its slot both store for choices: the
/// words an option accepts, as a view over a static table the command owns.
struct Choice {
    std::string_view const *words{nullptr};
    usize count{0};

    [[nodiscard]]
    constexpr auto accepts(std::string_view const word) const -> bool {
        for (auto i = usize{0}; i < count; ++i) {
            if (words[i] == word) {
                return true;
            }
        }
        return false;
    }
};

/// Which rule a rejected line broke, and -- in |name| and |word| -- the
/// option and the console word that broke it. One fault maps to one pinned
/// error line; see rejection_message() and <gktl/report.hpp>.
enum class OptionFault : u8 {
    /// The word names an option the command did not declare, and the command
    /// declared no pass-through.
    UnknownOption,

    /// The value is not of the option's shape: not digits for a u64, not a
    /// strict boolean word, or a bare word over an option that has neither
    /// default nor boolean shape.
    BadValue,

    /// The value is well-formed but not one of the option's choices.
    BadChoice,

    /// The run ended and a required option was never given.
    MissingRequired,

    /// More words than a command line can carry reached the parse.
    TooManyWords,

    /// The word has an empty name (`=x`, `=`); nothing can be named that.
    MalformedWord,
};

struct Rejection {
    OptionFault fault;
    std::string_view name;
    std::string_view word;
};

/// The fault's fixed phrasing, one row per fault in the enum's order: the
/// data, not a switch -- a fault the enum grows must grow its phrase in the
/// same row, and the static_assert below refuses a row count that does not
/// answer for every fault.
inline constexpr std::array FAULT_MESSAGES{
    std::string_view{"has no option named"},
    std::string_view{"could not read a value for"},
    std::string_view{"is not one of the choices for"},
    std::string_view{"needs"},
    std::string_view{"carries more words than a command line can hold"},
    std::string_view{"carries a word with an empty name"},
};
static_assert(FAULT_MESSAGES.size() == std::to_underlying(OptionFault::MalformedWord) + 1);

/// The fault's fixed phrasing, the one word of every error line that names
/// the rule rather than the input. The report composes the line around it.
[[nodiscard]]
constexpr auto rejection_message(OptionFault const fault) -> std::string_view {
    return FAULT_MESSAGES[std::to_underlying(fault)];
}

/// Appends to a bounded buffer, answering what fits: the honest truncation a
/// fragment too long for |out| suffers, the same contract the console's own
/// 128-byte lines put on everything a command prints.
constexpr auto append(char *const out, usize const cap, usize &at, std::string_view const text)
    -> void {
    for (auto const c: text) {
        if (at == cap) {
            return;
        }
        out[at] = c;
        at += 1;
    }
}

/// Appends |value| in decimal. There is no <charconv> in this environment, so
/// the digits go in by hand, least significant first out of a scratch array.
constexpr auto append_decimal(u64 const value, char *const out, usize const cap, usize &at)
    -> void {
    if (value == 0) {
        append(out, cap, at, "0");
        return;
    }
    char digits[20];
    auto count = usize{0};
    auto rest = value;
    while (rest != 0) {
        digits[count] = static_cast<char>('0' + rest % 10);
        rest /= 10;
        count += 1;
    }
    for (auto i = count; i != 0; --i) {
        append(out, cap, at, std::string_view{&digits[i - 1], 1});
    }
}

/// One declared option: its name and shape, its constraints, and where its
/// value lands. Built only through Options::add's fluent calls; a command
/// never writes one by hand.
struct OptionDescriptor {
    std::string_view name{};
    OptionKind kind{};
    bool required{false};
    bool has_default{false};
    Value default_value{};
    U64Parse parse{nullptr};
    Choice choices{};
    char const *help{nullptr};
};

/// One shape's behavior, as build-time polymorphism: what reading a word of
/// this kind answers, what a bare word of it means, how its usage fragment
/// and its default render. The three rows below are the only place in the
/// library that asks "which shape is this" -- the parse, the usage fragments
/// and the report all go through the table, so a shape the enum grows is one
/// row here and nowhere else.
struct U64Shape {
    static auto read_value(OptionDescriptor const &descriptor, Word const &entry)
        -> ustl::Result<Value, Rejection> {
        auto number = parse_u64(entry.value);
        if (descriptor.parse != nullptr) {
            number = descriptor.parse(entry.value);
        }
        if (number.is_err()) {
            return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, entry.value});
        }
        return ustl::ok(Value{number.unwrap()});
    }

    static auto read_bare(OptionDescriptor const &descriptor) -> ustl::Result<Value, Rejection> {
        if (descriptor.has_default) {
            return ustl::ok(descriptor.default_value);
        }
        return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, descriptor.name});
    }

    static auto usage_inner(OptionDescriptor const &, char *const out, usize const cap, usize &at)
        -> void {
        append(out, cap, at, "=N");
    }

    static auto default_text(Value const &value, char *const out, usize const cap, usize &at)
        -> void {
        append_decimal(value.i, out, cap, at);
    }
};

struct BoolShape {
    static auto read_value(OptionDescriptor const &descriptor, Word const &entry)
        -> ustl::Result<Value, Rejection> {
        auto const flag = parse_bool_strict(entry.value);
        if (flag.is_none()) {
            return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, entry.value});
        }
        return ustl::ok(Value{flag.value()});
    }

    /// A bare boolean word is the word set true -- presence is the boolean.
    static auto read_bare(OptionDescriptor const &) -> ustl::Result<Value, Rejection> {
        return ustl::ok(Value{true});
    }

    static auto usage_inner(OptionDescriptor const &, char *const, usize const, usize &) -> void {}

    static auto default_text(Value const &value, char *const out, usize const cap, usize &at)
        -> void {
        append(out, cap, at, value.b ? "true" : "false");
    }
};

struct StrShape {
    static auto read_value(OptionDescriptor const &descriptor, Word const &entry)
        -> ustl::Result<Value, Rejection> {
        if (descriptor.choices.count != 0 && !descriptor.choices.accepts(entry.value)) {
            return ustl::err(Rejection{OptionFault::BadChoice, descriptor.name, entry.value});
        }
        return ustl::ok(Value{entry.value});
    }

    static auto read_bare(OptionDescriptor const &descriptor) -> ustl::Result<Value, Rejection> {
        if (descriptor.has_default) {
            return ustl::ok(descriptor.default_value);
        }
        return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, descriptor.name});
    }

    static auto usage_inner(OptionDescriptor const &descriptor, char *const out, usize const cap,
                            usize &at) -> void {
        append(out, cap, at, "=");
        if (descriptor.choices.count != 0) {
            append(out, cap, at, "<");
            for (auto i = usize{0}; i < descriptor.choices.count; ++i) {
                if (i != 0) {
                    append(out, cap, at, "|");
                }
                append(out, cap, at, descriptor.choices.words[i]);
            }
            append(out, cap, at, ">");
        } else {
            append(out, cap, at, "<text>");
        }
    }

    static auto default_text(Value const &value, char *const out, usize const cap, usize &at)
        -> void {
        append(out, cap, at, "'");
        append(out, cap, at, value.s);
        append(out, cap, at, "'");
    }
};

namespace internal {

    struct ShapeRow {
        using ReadValue = auto (*)(OptionDescriptor const &, Word const &)
            -> ustl::Result<Value, Rejection>;
        using ReadBare = auto (*)(OptionDescriptor const &) -> ustl::Result<Value, Rejection>;
        using UsageInner = auto (*)(OptionDescriptor const &, char *, usize const, usize &) -> void;
        using DefaultText = auto (*)(Value const &, char *, usize const, usize &) -> void;

        ReadValue read_value;
        ReadBare read_bare;
        UsageInner usage_inner;
        DefaultText default_text;
    };

    /// The table the one kind lookup answers through, indexed by the enum's
    /// own order.
    inline constexpr ShapeRow SHAPES[]{
        {&U64Shape::read_value, &U64Shape::read_bare, &U64Shape::usage_inner,
         &U64Shape::default_text},
        {&BoolShape::read_value, &BoolShape::read_bare, &BoolShape::usage_inner,
         &BoolShape::default_text},
        {&StrShape::read_value, &StrShape::read_bare, &StrShape::usage_inner,
         &StrShape::default_text},
    };
    static_assert(SHAPES[std::to_underlying(OptionKind::Str)].read_value == &StrShape::read_value);

    [[nodiscard]]
    constexpr auto shape_row(OptionKind const kind) -> ShapeRow const & {
        return SHAPES[std::to_underlying(kind)];
    }

} // namespace internal

/// One landed option: the name, the shape that fills it, the value, and
/// whether the line carried it -- one object, so the four facts travel
/// together and cannot fall out of step the way parallel arrays can.
struct Slot {
    std::string_view name{};
    OptionKind kind{};
    Value value{};
    bool given{false};
};

/// The answer parse() returns when the line is accepted: every declared
/// option's effective value -- the given one, or the declaration's default,
/// or zero for an optional option given neither -- plus the words a
/// pass-through command left unclaimed. The values alias the console line
/// where they are strings, and are only valid while the command runs, the
/// same lifetime the console's argv has.
class Parsed {
  public:
    /// |name|'s effective value. |T| is u64 or usize for a U64 option, bool
    /// for a Bool option, std::string_view for a Str option; anything else is
    /// a compile error, and a T that does not match the declaration's kind is
    /// a misuse the assertion catches. An option the line did not carry and
    /// that declares no default reads as zero -- a command that needs the
    /// distinction between "given" and "zero" asks has() instead.
    template <typename T>
    [[nodiscard]]
    auto get(std::string_view const name) const -> T {
        static_assert(std::is_same_v<T, u64> || std::is_same_v<T, usize> ||
                          std::is_same_v<T, bool> || std::is_same_v<T, std::string_view>,
                      "an option reads as u64, usize, bool or std::string_view");
        for (auto i = usize{0}; i < m_count; ++i) {
            if (m_slots[i].name != name) {
                continue;
            }
            ASSERT(m_slots[i].kind == kind_of<T>(), "option '{}' read as the wrong shape", name);
            return read_as<T>(m_slots[i].value);
        }
        return T{};
    }

    /// Whether the line carried |name|, whatever its value.
    [[nodiscard]]
    auto has(std::string_view const name) const -> bool {
        // Whether the LINE carried the option, not whether a value answers
        // for it: a defaulted option the line did not mention reads as its
        // default through get() and answers false here, which is what lets
        // the runner tell `shuffle` from its seed -- the runner's own
        // options are the ones it acts on, the defaulted rest are the
        // declaration's.
        for (auto i = usize{0}; i < m_count; ++i) {
            if (m_slots[i].name == name && m_slots[i].given) {
                return true;
            }
        }
        return false;
    }

    /// The words no declared option claimed, in the order the line carried
    /// them. Empty unless the command declared a pass-through.
    [[nodiscard]]
    auto passthrough() const -> std::span<Word const> {
        return {m_passthrough, m_passthrough_count};
    }

  private:
    template <typename T>
    [[nodiscard]]
    static constexpr auto kind_of() -> OptionKind {
        if constexpr (std::is_same_v<T, bool>) {
            return OptionKind::Bool;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return OptionKind::Str;
        } else {
            return OptionKind::U64;
        }
    }

    template <typename T>
    [[nodiscard]]
    static constexpr auto read_as(Value const &value) -> T {
        if constexpr (std::is_same_v<T, bool>) {
            return value.b;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return value.s;
        } else {
            return static_cast<T>(value.i);
        }
    }

    friend class Options;

    Slot m_slots[MAX_OPTIONS]{};
    usize m_count{0};

    Word m_passthrough[MAX_OPTIONS]{};
    usize m_passthrough_count{0};
};

/// A command's declared options. The command builds one in its handler frame,
// names every word it accepts through add<T>, and hands the line's words to
// parse(). It is used in place: add() answers a handle into this object's
// own table, so copying or moving an Options would strand the handles, and
// is deleted to keep that from compiling.
//
// The tables are sized to the console's word cap and together with the
// parse()'s answer they are a few kilobytes of frame -- fine for a command
// that reports and returns, heavy for one whose handler hosts long work on
// its own stack: the ktest runner runs every test inside its command frame,
// and there its options sit in file-scope storage instead, the way the
// runner's other tables do, the one-run-at-a-time guard making the storage
// exclusive.
class Options {
  public:
    /// A handle onto one declared option, for the fluent constraints and for
    /// reading the option's value out of a Parsed without repeating its name.
    class Arg {
      public:
        /// The value the option reads as when the line does not carry it.
        /// The shape must match the option's; a mismatch is a misuse the
        /// assertion catches.
        auto defaults_to(u64 const value) -> Arg & {
            ASSERT(m_descriptor->kind == OptionKind::U64, "u64 default on a non-u64 option");
            m_descriptor->default_value = Value{value};
            m_descriptor->has_default = true;
            return *this;
        }

        auto defaults_to(bool const value) -> Arg & {
            ASSERT(m_descriptor->kind == OptionKind::Bool, "bool default on a non-bool option");
            m_descriptor->default_value = Value{value};
            m_descriptor->has_default = true;
            return *this;
        }

        auto defaults_to(std::string_view const value) -> Arg & {
            ASSERT(m_descriptor->kind == OptionKind::Str, "string default on a non-string option");
            m_descriptor->default_value = Value{value};
            m_descriptor->has_default = true;
            return *this;
        }

        /// The string-literal spelling of defaults_to, and not an accident of
        /// overload resolution: a bare literal would otherwise bind to the
        /// bool overload through pointer-to-bool, and a `defaults_to("time")`
        /// on a Str option would quietly mean `defaults_to(true)`.
        auto defaults_to(char const *const value) -> Arg & {
            return defaults_to(std::string_view{value});
        }

        /// The run stops with MissingRequired unless the line carried the
        /// option. A required option needs no default.
        auto required() -> Arg & {
            m_descriptor->required = true;
            return *this;
        }

        /// The words the option accepts, out of a table the command owns --
        /// a static constexpr array in the command, not a brace list: a
        /// brace list's backing array dies with the statement that built it,
        /// and the acceptance check would be reading it dead. A value
        /// outside the table is a BadChoice. Only a Str option takes
        /// choices; the words a number accepts are its digits, not a list.
        auto choices(std::span<std::string_view const> const words) -> Arg & {
            ASSERT(m_descriptor->kind == OptionKind::Str, "choices on a non-string option");
            m_descriptor->choices = Choice{words.data(), words.size()};
            return *this;
        }

        /// The number-shaped option's own parse: `0x`-prefixed seeds over
        /// plain digits, for one. Only a U64 option takes a hook.
        auto parse(U64Parse const hook) -> Arg & {
            ASSERT(m_descriptor->kind == OptionKind::U64, "parse hook on a non-u64 option");
            m_descriptor->parse = hook;
            return *this;
        }

        /// The option's one-line help, printed by the generated usage.
        auto help(char const *const text) -> Arg & {
            m_descriptor->help = text;
            return *this;
        }

        /// The option's effective value in |parsed|. Reads as |T|: u64 or
        /// usize for a U64 option, bool for a Bool option, std::string_view
        /// for a Str option.
        template <typename T>
        [[nodiscard]]
        auto value(Parsed const &parsed) const -> T {
            return parsed.get<T>(m_descriptor->name);
        }

        /// The option's name, for the commands that name their options in
        /// their own error lines.
        [[nodiscard]]
        auto name() const -> std::string_view {
            return m_descriptor->name;
        }

      private:
        friend class Options;

        Arg(OptionDescriptor *descriptor) : m_descriptor{descriptor} {}

        OptionDescriptor *m_descriptor;
    };

    explicit Options(std::string_view const cmd) : m_cmd{cmd} {}

    Options(Options const &) = delete;
    auto operator=(Options const &) -> Options & = delete;

    /// Declares an option named |name| reading as |T|. Two options of one
    /// name are an authoring bug the assertion catches at build-the-options
    /// time, where the second add() runs. The answer is the handle the
    /// constraints chain on; a command that reads its values through
    /// Parsed::get may drop it.
    template <typename T>
    auto add(std::string_view const name) -> Arg {
        for (auto i = usize{0}; i < m_count; ++i) {
            ASSERT(m_descriptors[i].name != name, "option '{}' declared twice", name);
        }
        ASSERT(m_count != MAX_OPTIONS, "more options declared than a command line carries");
        auto &descriptor = m_descriptors[m_count];
        descriptor = OptionDescriptor{.name = name, .kind = kind_of<T>()};
        m_count += 1;
        return Arg{&descriptor};
    }

    /// Unclaimed words are kept for the command instead of rejected. This is
    /// how the one command that forwards options to something else -- the
    /// test runner, whose options belong to the tests -- declares that it
    /// meant to; every other command's unknown word is an error.
    auto pass_through() -> void {
        m_pass_through = true;
    }

    /// Parses the words after the command. The answer is every value filled,
    /// or the first rule the line broke with the word that broke it.
    [[nodiscard]]
    auto parse(std::span<std::string_view const> const words) const
        -> ustl::Result<Parsed, Rejection> {
        if (words.size() > MAX_OPTIONS) {
            return ustl::err(Rejection{OptionFault::TooManyWords, {}, {}});
        }
        auto parsed = Parsed{};
        for (auto const word: words) {
            auto const entry = parse_word(word);
            if (entry.is_none()) {
                return ustl::err(Rejection{OptionFault::MalformedWord, {}, word});
            }
            auto const &word_value = entry.value();
            auto *const descriptor = find(word_value.name);
            if (descriptor == nullptr) {
                if (m_pass_through) {
                    parsed.m_passthrough[parsed.m_passthrough_count] = word_value;
                    parsed.m_passthrough_count += 1;
                    continue;
                }
                return ustl::err(Rejection{OptionFault::UnknownOption, word_value.name, word});
            }
            auto const slot = read(*descriptor, word_value, parsed);
            if (slot.is_err()) {
                return ustl::err(slot.unwrap_err());
            }
        }
        for (auto i = usize{0}; i < m_count; ++i) {
            if (m_descriptors[i].required && !parsed.has(m_descriptors[i].name)) {
                return ustl::err(
                    Rejection{OptionFault::MissingRequired, m_descriptors[i].name, {}});
            }
            // A defaulted option the line did not mention still answers with
            // its default -- the declaration promised that value and the
            // usage line prints it -- but answers has() false: the
            // distinction between "given" and "defaulted" is what the
            // runner's all-only rule and a handler's own checks read.
            if (m_descriptors[i].has_default && !parsed.has(m_descriptors[i].name)) {
                parsed.m_slots[parsed.m_count] = Slot{.name = m_descriptors[i].name,
                                                      .kind = m_descriptors[i].kind,
                                                      .value = m_descriptors[i].default_value,
                                                      .given = false};
                parsed.m_count += 1;
            }
        }
        return ustl::ok(parsed);
    }

    /// The same, from a brace list -- the shape a host test drives a command
    /// line with. The literals convert to string_view as the list builds.
    [[nodiscard]]
    auto parse(std::initializer_list<std::string_view> const words) const
        -> ustl::Result<Parsed, Rejection> {
        return parse(std::span<std::string_view const>{words.begin(), words.size()});
    }

    /// The same, from the invocation the callback was handed: the command's
    /// own word is not an option, and the words are what the line carried.
    [[nodiscard]]
    auto parse(Invocation const &invocation) const -> ustl::Result<Parsed, Rejection> {
        return parse(invocation.words());
    }

    /// The command name the error lines and the usage open with.
    [[nodiscard]]
    auto cmd() const -> std::string_view {
        return m_cmd;
    }

    /// The declarations, in the order they were added, for the usage printer.
    [[nodiscard]]
    auto descriptors() const -> std::span<OptionDescriptor const> {
        return {m_descriptors, m_count};
    }

  private:
    template <typename T>
    [[nodiscard]]
    static constexpr auto kind_of() -> OptionKind {
        if constexpr (std::is_same_v<T, bool>) {
            return OptionKind::Bool;
        } else if constexpr (std::is_same_v<T, std::string_view>) {
            return OptionKind::Str;
        } else {
            static_assert(std::is_same_v<T, u64> || std::is_same_v<T, usize>,
                          "an option reads as u64, usize, bool or std::string_view");
            return OptionKind::U64;
        }
    }

    [[nodiscard]]
    auto find(std::string_view const name) const -> OptionDescriptor const * {
        for (auto i = usize{0}; i < m_count; ++i) {
            if (m_descriptors[i].name == name) {
                return &m_descriptors[i];
            }
        }
        return nullptr;
    }

    /// Reads one word into |parsed| for |descriptor|, answering the slot it
    /// landed in or the rejection that stopped the line. A word naming an
    /// option that already landed overwrites it: the last occurrence on the
    /// line wins, which is what a reader repeating an option means by it.
    [[nodiscard]]
    auto read(OptionDescriptor const &descriptor, Word const &entry, Parsed &parsed) const
        -> ustl::Result<void, Rejection> {
        // The shape's row answers what the word's value -- or its bare
        // presence -- means; this only lands the slot. A bare word is true
        // for a boolean and the declared default for anything else, which is
        // how `shuffle` means its default seed; bare over an option with
        // neither shape default nor boolean reads as nothing the option
        // promised, and that is the row's bad value.
        auto const &shape = internal::shape_row(descriptor.kind);
        auto value =
            entry.has_value ? shape.read_value(descriptor, entry) : shape.read_bare(descriptor);
        if (value.is_err()) {
            return ustl::err(value.unwrap_err());
        }
        auto slot = parsed.m_count;
        for (auto i = usize{0}; i < parsed.m_count; ++i) {
            if (parsed.m_slots[i].name == descriptor.name) {
                slot = i;
                break;
            }
        }
        parsed.m_slots[slot] = Slot{.name = descriptor.name,
                                    .kind = descriptor.kind,
                                    .value = value.unwrap(),
                                    .given = true};
        if (slot == parsed.m_count) {
            parsed.m_count += 1;
        }
        return ustl::ok();
    }

    std::string_view m_cmd;
    OptionDescriptor m_descriptors[MAX_OPTIONS]{};
    usize m_count{0};
    bool m_pass_through{false};
};

/// The usage fragment one option renders as: `how=<panic|panic-locked>` for a
/// required string option with choices, `[rounds=N]` for an optional number,
/// `[csv]` for an optional boolean. Built into |out|, which a 112-byte buffer
/// fits for any option a command line can carry.
[[nodiscard]]
inline auto usage_word(OptionDescriptor const &descriptor, char *const out, usize const cap)
    -> std::string_view {
    auto at = usize{0};
    if (!descriptor.required) {
        append(out, cap, at, "[");
    }
    append(out, cap, at, descriptor.name);
    internal::shape_row(descriptor.kind).usage_inner(descriptor, out, cap, at);
    if (!descriptor.required) {
        append(out, cap, at, "]");
    }
    return std::string_view{out, at};
}

} // namespace gktl

#endif // #ifndef GKTL_OPTIONS_HPP
