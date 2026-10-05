/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include "keymap.hpp"

#include <array>
#include <string_view>

namespace pc_ps2 {
namespace {
    using enum NamedKey;

    /// The prefix byte of an extended key's code.
    constexpr auto EXTENDED_PREFIX = u8{0xE0};

    /// The bit a break code adds to its key's make code.
    constexpr auto BREAK = u8{0x80};

    /// What each make code of set 1 presses, by its number, eight to a row:
    /// the keys of the first keyboards, with no shift key held.
    constexpr auto SET1 = std::array<Key, 128>{
        // 0x00
        Key{}, Escape, '1', '2', '3', '4', '5', '6',
        // 0x08
        '7', '8', '9', '0', '-', '=', Backspace, Tab,
        // 0x10
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
        // 0x18
        'o', 'p', '[', ']', Enter, LeftControl, 'a', 's',
        // 0x20
        'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
        // 0x28
        '\'', '`', LeftShift, '\\', 'z', 'x', 'c', 'v',
        // 0x30
        'b', 'n', 'm', ',', '.', '/', RightShift, '*',
        // 0x38
        LeftAlt, ' ', CapsLock, F1, F2, F3, F4, F5,
        // 0x40
        F6, F7, F8, F9, F10, NumLock, ScrollLock, Keypad7,
        // 0x48
        Keypad8, Keypad9, KeypadMinus, Keypad4, Keypad5, Keypad6, KeypadPlus, Keypad1,
        // 0x50
        Keypad2, Keypad3, Keypad0, KeypadPeriod, Key{}, Key{}, Key{}, F11,
        // 0x58
        F12};

    /// The same while a shift key is held: a key that types a character
    /// types its shifted one, and every other key is itself.
    constexpr auto SET1_SHIFTED = std::array<Key, 128>{
        // 0x00
        Key{}, Escape, '!', '@', '#', '$', '%', '^',
        // 0x08
        '&', '*', '(', ')', '_', '+', Backspace, Tab,
        // 0x10
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
        // 0x18
        'O', 'P', '{', '}', Enter, LeftControl, 'A', 'S',
        // 0x20
        'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
        // 0x28
        '"', '~', LeftShift, '|', 'Z', 'X', 'C', 'V',
        // 0x30
        'B', 'N', 'M', '<', '>', '?', RightShift, '*',
        // 0x38
        LeftAlt, ' ', CapsLock, F1, F2, F3, F4, F5,
        // 0x40
        F6, F7, F8, F9, F10, NumLock, ScrollLock, Keypad7,
        // 0x48
        Keypad8, Keypad9, KeypadMinus, Keypad4, Keypad5, Keypad6, KeypadPlus, Keypad1,
        // 0x50
        Keypad2, Keypad3, Keypad0, KeypadPeriod, Key{}, Key{}, Key{}, F11,
        // 0x58
        F12};

    /// What each code after the 0xE0 prefix presses: the extended keys, set
    /// by number, since most numbers press nothing here. A keyboard also
    /// sends 0xE0 0x2A and 0xE0 0xAA around some of them, a shift it fakes
    /// for the first keyboards' sake; neither presses a key here, so neither
    /// touches the shift state.
    constexpr auto SET1_EXTENDED = [] {
        auto table = std::array<Key, 128>{};
        table[0x1C] = KeypadEnter;
        table[0x1D] = RightControl;
        table[0x35] = KeypadDivide;
        table[0x37] = PrintScreen;
        table[0x38] = RightAlt;
        table[0x47] = Home;
        table[0x48] = Up;
        table[0x49] = PageUp;
        table[0x4B] = Left;
        table[0x4D] = Right;
        table[0x4F] = End;
        table[0x50] = Down;
        table[0x51] = PageDown;
        table[0x52] = Insert;
        table[0x53] = Delete;
        table[0x5B] = LeftWindows;
        table[0x5C] = RightWindows;
        table[0x5D] = Menu;
        return table;
    }();

    /// The name of |key|, in lower case with its words joined by hyphens.
    constexpr auto named_key_name(NamedKey const key) -> std::string_view {
        switch (key) {
            case Enter:
                return "enter";
            case Escape:
                return "escape";
            case Backspace:
                return "backspace";
            case Tab:
                return "tab";
            case LeftShift:
                return "left-shift";
            case RightShift:
                return "right-shift";
            case LeftControl:
                return "left-control";
            case RightControl:
                return "right-control";
            case LeftAlt:
                return "left-alt";
            case RightAlt:
                return "right-alt";
            case LeftWindows:
                return "left-windows";
            case RightWindows:
                return "right-windows";
            case Menu:
                return "menu";
            case CapsLock:
                return "caps-lock";
            case NumLock:
                return "num-lock";
            case ScrollLock:
                return "scroll-lock";
            case F1:
                return "f1";
            case F2:
                return "f2";
            case F3:
                return "f3";
            case F4:
                return "f4";
            case F5:
                return "f5";
            case F6:
                return "f6";
            case F7:
                return "f7";
            case F8:
                return "f8";
            case F9:
                return "f9";
            case F10:
                return "f10";
            case F11:
                return "f11";
            case F12:
                return "f12";
            case PrintScreen:
                return "print-screen";
            case Insert:
                return "insert";
            case Delete:
                return "delete";
            case Home:
                return "home";
            case End:
                return "end";
            case PageUp:
                return "page-up";
            case PageDown:
                return "page-down";
            case Up:
                return "up";
            case Down:
                return "down";
            case Left:
                return "left";
            case Right:
                return "right";
            case KeypadEnter:
                return "keypad-enter";
            case KeypadDivide:
                return "keypad-divide";
            case KeypadMinus:
                return "keypad-minus";
            case KeypadPlus:
                return "keypad-plus";
            case KeypadPeriod:
                return "keypad-period";
            case Keypad0:
                return "keypad-0";
            case Keypad1:
                return "keypad-1";
            case Keypad2:
                return "keypad-2";
            case Keypad3:
                return "keypad-3";
            case Keypad4:
                return "keypad-4";
            case Keypad5:
                return "keypad-5";
            case Keypad6:
                return "keypad-6";
            case Keypad7:
                return "keypad-7";
            case Keypad8:
                return "keypad-8";
            case Keypad9:
                return "keypad-9";
        }
        return "unknown";
    }

    /// Every printable ASCII character in order, from the space on: a
    /// character key's name is the one-character view of it here.
    constexpr auto PRINTABLE = std::string_view{
        " !\"#$%&'()*+,-./"
        "0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"};
} // namespace

auto Key::name() const -> std::string_view {
    if (m_code >= std::to_underlying(Enter)) {
        return named_key_name(static_cast<NamedKey>(m_code));
    }
    if (m_code == ' ') {
        return "space";
    }
    if (m_code > ' ' && m_code < ' ' + PRINTABLE.size()) {
        return PRINTABLE.substr(m_code - ' ', 1);
    }
    return "unknown";
}

auto Decoder::feed(u8 const code) -> ustl::Option<Key> {
    if (code == EXTENDED_PREFIX) {
        m_state |= State::Extended;
        return ustl::NONE;
    }
    auto const extended = !!(m_state & State::Extended);
    m_state &= ~State::Extended;

    auto const released = (code & BREAK) != 0;
    auto const number = static_cast<u8>(code & ~BREAK);
    auto key = Key{};
    if (extended) {
        key = SET1_EXTENDED[number];
    } else if (!!(m_state & (State::LeftShift | State::RightShift))) {
        key = SET1_SHIFTED[number];
    } else {
        key = SET1[number];
    }

    // Each shift key is held from its make code to its break code.
    auto const track = [this, released](State const shift) {
        if (released) {
            m_state &= ~shift;
        } else {
            m_state |= shift;
        }
    };
    if (key == LeftShift) {
        track(State::LeftShift);
    } else if (key == RightShift) {
        track(State::RightShift);
    }

    if (released || key.is_none()) {
        return ustl::NONE;
    }
    return ustl::some(key);
}
} // namespace pc_ps2
