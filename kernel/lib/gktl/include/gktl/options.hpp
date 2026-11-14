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

/// Splits one token on its first `=`. False when the name is empty (`=x`,
/// `=`), which no option can be named.
[[nodiscard]]
constexpr auto parse_word(std::string_view const token, Word &out) -> bool {
    auto const equals = token.find('=');
    if (equals == std::string_view::npos) {
        if (token.empty()) {
            return false;
        }
        out = Word{.name = token, .value = std::string_view{}, .has_value = false};
        return true;
    }
    auto const name = token.substr(0, equals);
    if (name.empty()) {
        return false;
    }
    out = Word{.name = name, .value = token.substr(equals + 1), .has_value = true};
    return true;
}

/// Parses an unsigned decimal or hexadecimal digit run: digits only, no sign,
/// no separators, |base| 10 or 16. InvArgs for an empty value, a character
/// that is not a digit of |base|, or a value beyond u64's range. The one
/// digit primitive every number-shaped word goes through; the ktest runner
/// plans parse through it too.
[[nodiscard]]
constexpr auto parse_u64(std::string_view const digits, u64 const base = 10)
    -> ustl::Result<u64, ours::Status> {
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

/// The fault's fixed phrasing, the one word of every error line that names
/// the rule rather than the input. The report composes the line around it.
[[nodiscard]]
constexpr auto rejection_message(OptionFault const fault) -> std::string_view {
    switch (fault) {
        case OptionFault::UnknownOption:
            return "has no option named";
        case OptionFault::BadValue:
            return "could not read a value for";
        case OptionFault::BadChoice:
            return "is not one of the choices for";
        case OptionFault::MissingRequired:
            return "needs";
        case OptionFault::TooManyWords:
            return "carries more words than a command line can hold";
        case OptionFault::MalformedWord:
            return "carries a word with an empty name";
    }
    return "was rejected";
}

/// One declared option: its name and shape, its constraints, and where its
/// value lands. Built only through Options::add's fluent calls; a command
/// never writes one by hand.
struct OptionDescriptor {
    std::string_view name{};
    OptionKind kind{};
    bool required{false};
    bool has_default{false};
    Value default_{};
    U64Parse parse{nullptr};
    Choice choices{};
    char const *help{nullptr};
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
        for (auto i = usize{0}; i < count_; ++i) {
            if (names_[i] != name) {
                continue;
            }
            ASSERT(kind_of(i) == kind_of<T>(), "option '{}' read as the wrong shape", name);
            return read_as<T>(values_[i]);
        }
        return T{};
    }

    /// Whether the line carried |name|, whatever its value.
    [[nodiscard]]
    auto has(std::string_view const name) const -> bool {
        for (auto i = usize{0}; i < count_; ++i) {
            if (names_[i] == name) {
                return true;
            }
        }
        return false;
    }

    /// The words no declared option claimed, in the order the line carried
    /// them. Empty unless the command declared a pass-through.
    [[nodiscard]]
    auto passthrough() const -> std::span<Word const> {
        return {passthrough_, passthrough_count_};
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

    [[nodiscard]]
    constexpr auto kind_of(usize const slot) const -> OptionKind {
        return kinds_[slot];
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

    std::string_view names_[MAX_OPTIONS]{};
    OptionKind kinds_[MAX_OPTIONS]{};
    Value values_[MAX_OPTIONS]{};
    usize count_{0};

    Word passthrough_[MAX_OPTIONS]{};
    usize passthrough_count_{0};
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
            ASSERT(descriptor_->kind == OptionKind::U64, "u64 default on a non-u64 option");
            descriptor_->default_ = Value{value};
            descriptor_->has_default = true;
            return *this;
        }

        auto defaults_to(bool const value) -> Arg & {
            ASSERT(descriptor_->kind == OptionKind::Bool, "bool default on a non-bool option");
            descriptor_->default_ = Value{value};
            descriptor_->has_default = true;
            return *this;
        }

        auto defaults_to(std::string_view const value) -> Arg & {
            ASSERT(descriptor_->kind == OptionKind::Str, "string default on a non-string option");
            descriptor_->default_ = Value{value};
            descriptor_->has_default = true;
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
            descriptor_->required = true;
            return *this;
        }

        /// The words the option accepts, out of a table the command owns --
        /// a static constexpr array in the command, not a brace list: a
        /// brace list's backing array dies with the statement that built it,
        /// and the acceptance check would be reading it dead. A value
        /// outside the table is a BadChoice. Only a Str option takes
        /// choices; the words a number accepts are its digits, not a list.
        auto choices(std::span<std::string_view const> const words) -> Arg & {
            ASSERT(descriptor_->kind == OptionKind::Str, "choices on a non-string option");
            descriptor_->choices = Choice{words.data(), words.size()};
            return *this;
        }

        /// The number-shaped option's own parse: `0x`-prefixed seeds over
        /// plain digits, for one. Only a U64 option takes a hook.
        auto parse(U64Parse const hook) -> Arg & {
            ASSERT(descriptor_->kind == OptionKind::U64, "parse hook on a non-u64 option");
            descriptor_->parse = hook;
            return *this;
        }

        /// The option's one-line help, printed by the generated usage.
        auto help(char const *const text) -> Arg & {
            descriptor_->help = text;
            return *this;
        }

        /// The option's effective value in |parsed|. Reads as |T|: u64 or
        /// usize for a U64 option, bool for a Bool option, std::string_view
        /// for a Str option.
        template <typename T>
        [[nodiscard]]
        auto value(Parsed const &parsed) const -> T {
            return parsed.get<T>(descriptor_->name);
        }

        /// The option's name, for the commands that name their options in
        /// their own error lines.
        [[nodiscard]]
        auto name() const -> std::string_view {
            return descriptor_->name;
        }

      private:
        friend class Options;

        Arg(OptionDescriptor *descriptor) : descriptor_{descriptor} {}

        OptionDescriptor *descriptor_;
    };

    explicit Options(std::string_view const cmd) : cmd_{cmd} {}

    Options(Options const &) = delete;
    auto operator=(Options const &) -> Options & = delete;

    /// Declares an option named |name| reading as |T|. Two options of one
    /// name are an authoring bug the assertion catches at build-the-options
    /// time, where the second add() runs. The answer is the handle the
    /// constraints chain on; a command that reads its values through
    /// Parsed::get may drop it.
    template <typename T>
    auto add(std::string_view const name) -> Arg {
        for (auto i = usize{0}; i < count_; ++i) {
            ASSERT(descriptors_[i].name != name, "option '{}' declared twice", name);
        }
        ASSERT(count_ != MAX_OPTIONS, "more options declared than a command line carries");
        auto &descriptor = descriptors_[count_];
        descriptor = OptionDescriptor{.name = name, .kind = kind_of<T>()};
        count_ += 1;
        return Arg{&descriptor};
    }

    /// Unclaimed words are kept for the command instead of rejected. This is
    /// how the one command that forwards options to something else -- the
    /// test runner, whose options belong to the tests -- declares that it
    /// meant to; every other command's unknown word is an error.
    auto pass_through() -> void {
        pass_through_ = true;
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
            auto entry = Word{};
            if (!parse_word(word, entry)) {
                return ustl::err(Rejection{OptionFault::MalformedWord, {}, word});
            }
            auto *const descriptor = find(entry.name);
            if (descriptor == nullptr) {
                if (pass_through_) {
                    parsed.passthrough_[parsed.passthrough_count_] = entry;
                    parsed.passthrough_count_ += 1;
                    continue;
                }
                return ustl::err(Rejection{OptionFault::UnknownOption, entry.name, word});
            }
            auto const slot = read(*descriptor, entry, parsed);
            if (slot.is_err()) {
                return ustl::err(slot.unwrap_err());
            }
        }
        for (auto i = usize{0}; i < count_; ++i) {
            if (descriptors_[i].required && !parsed.has(descriptors_[i].name)) {
                return ustl::err(Rejection{OptionFault::MissingRequired, descriptors_[i].name, {}});
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

    /// The same, from the console's shape: argv[0] is the command itself and
    /// is not an option.
    [[nodiscard]]
    auto parse(int const argc, char **const argv) const -> ustl::Result<Parsed, Rejection> {
        auto words = std::array<std::string_view, MAX_OPTIONS>{};
        auto count = usize{0};
        for (auto i = 1; i < argc && argv[i] != nullptr; ++i) {
            words[count] = argv[i];
            count += 1;
        }
        return parse(std::span<std::string_view const>{words.data(), count});
    }

    /// The command name the error lines and the usage open with.
    [[nodiscard]]
    auto cmd() const -> std::string_view {
        return cmd_;
    }

    /// The declarations, in the order they were added, for the usage printer.
    [[nodiscard]]
    auto descriptors() const -> std::span<OptionDescriptor const> {
        return {descriptors_, count_};
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
        for (auto i = usize{0}; i < count_; ++i) {
            if (descriptors_[i].name == name) {
                return &descriptors_[i];
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
        auto value = Value{};
        // What the error line quotes as "given": the value half when the
        // word carried one, the bare name when it did not -- a bare number
        // option given no default is named by its name, a value that failed
        // its parse by its value.
        auto const given = entry.has_value ? entry.value : entry.name;
        if (!entry.has_value) {
            // A bare word: true for a boolean, the declared default for
            // anything else -- that is how `shuffle` means its default seed.
            // Bare over an option with neither shape nor default reads as
            // nothing the option promised, and that is a bad value.
            if (descriptor.kind == OptionKind::Bool) {
                value = Value{true};
            } else if (descriptor.has_default) {
                value = descriptor.default_;
            } else {
                return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, given});
            }
        } else {
            switch (descriptor.kind) {
                case OptionKind::U64: {
                    auto const number = descriptor.parse != nullptr ? descriptor.parse(entry.value)
                                                                    : parse_u64(entry.value);
                    if (number.is_err()) {
                        return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, given});
                    }
                    value = Value{number.unwrap()};
                    break;
                }
                case OptionKind::Bool: {
                    auto const flag = parse_bool_strict(entry.value);
                    if (flag.is_none()) {
                        return ustl::err(Rejection{OptionFault::BadValue, descriptor.name, given});
                    }
                    value = Value{flag.value()};
                    break;
                }
                case OptionKind::Str: {
                    if (descriptor.choices.count != 0 && !descriptor.choices.accepts(entry.value)) {
                        return ustl::err(
                            Rejection{OptionFault::BadChoice, descriptor.name, entry.value});
                    }
                    value = Value{entry.value};
                    break;
                }
            }
        }
        auto slot = parsed.count_;
        for (auto i = usize{0}; i < parsed.count_; ++i) {
            if (parsed.names_[i] == descriptor.name) {
                slot = i;
                break;
            }
        }
        parsed.names_[slot] = descriptor.name;
        parsed.kinds_[slot] = descriptor.kind;
        parsed.values_[slot] = value;
        if (slot == parsed.count_) {
            parsed.count_ += 1;
        }
        return ustl::ok();
    }

    std::string_view cmd_;
    OptionDescriptor descriptors_[MAX_OPTIONS]{};
    usize count_{0};
    bool pass_through_{false};
};

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
    switch (descriptor.kind) {
        case OptionKind::U64:
            append(out, cap, at, "=N");
            break;
        case OptionKind::Bool:
            break;
        case OptionKind::Str: {
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
            break;
        }
    }
    if (!descriptor.required) {
        append(out, cap, at, "]");
    }
    return std::string_view{out, at};
}

} // namespace gktl

#endif // #ifndef GKTL_OPTIONS_HPP
