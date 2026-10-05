/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PC_PS2_KEYMAP_HPP
#define PC_PS2_KEYMAP_HPP 1

#include <string_view>
#include <utility>

#include <ustl/option.hpp>
#include <ustl/util/enum_bits.hpp>

/// What the keyboard's bytes mean: the keys of scancode set 1, and the
/// decoder that turns a stream of its bytes into the keys pressed.
namespace pc_ps2 {
/// The keys the scancode tables name that type no character. They are
/// numbered from 0x80, past every character the tables hold, so that a Key
/// is one byte whichever kind it is.
enum class NamedKey : u8 {
    Enter = 0x80,
    Escape,
    Backspace,
    Tab,
    LeftShift,
    RightShift,
    LeftControl,
    RightControl,
    LeftAlt,
    RightAlt,
    LeftWindows,
    RightWindows,
    Menu,
    CapsLock,
    NumLock,
    ScrollLock,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    PrintScreen,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    Up,
    Down,
    Left,
    Right,
    KeypadEnter,
    KeypadDivide,
    KeypadMinus,
    KeypadPlus,
    KeypadPeriod,
    Keypad0,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
};

/// One key, as a scancode table names it: no key, a key that types a
/// printable character, or a named key.
class Key final {
  public:
    /// No key: what a table holds for a code no key sends.
    constexpr Key() = default;

    /// The key that types |character|, a printable ASCII character. Not
    /// explicit, so that a table lists a character as it is.
    constexpr Key(char const character) : m_code(static_cast<u8>(character)) {}

    /// Not explicit either, for the same reason.
    constexpr Key(NamedKey const key) : m_code(std::to_underlying(key)) {}

    [[nodiscard]]
    constexpr auto is_none() const -> bool {
        return m_code == 0;
    }

    /// The name the log gives it: the character a key types, "space" for the
    /// space bar, and a named key's name in lower case, words joined by
    /// hyphens: "left-shift".
    [[nodiscard]]
    auto name() const -> std::string_view;

    friend constexpr auto operator==(Key, Key) -> bool = default;

  private:
    u8 m_code = 0;
};

/// Turns the bytes of scancode set 1 into the keys they press, one byte at a
/// time.
///
/// Set 1 is what the controller hands on with translation on, whichever set
/// the keyboard itself speaks. A key's make code, sent when it goes down, is
/// a number from 0x01 to 0x7F; its break code, sent when it comes up, is the
/// same number with bit 7 set. 0xE0 in front of either says that the number
/// is an extended key's, a key the first keyboards did not have: the
/// numbers repeat, so the right Ctrl is 0xE0 0x1D where the left one is
/// 0x1D.
class Decoder final {
  public:
    /// The key |code| presses, if it presses one. A break code, the 0xE0
    /// prefix and a code no key sends press none. A shift key is tracked
    /// whether it goes down or up, so that while either is held a key that
    /// types a character types its shifted one.
    [[nodiscard]]
    auto feed(u8 code) -> ustl::Option<Key>;

  private:
    /// What the decoder carries from one byte to the next.
    enum class State : u8 {
        None = 0,
        LeftShift = 0_bit,
        RightShift = 1_bit,
        /// The last byte was 0xE0: the next is an extended key's.
        Extended = 2_bit,
    };
    USTL_ENABLE_INNER_ENUM_BITMASK(State);

    State m_state = State::None;
};
} // namespace pc_ps2

#endif // #ifndef PC_PS2_KEYMAP_HPP
