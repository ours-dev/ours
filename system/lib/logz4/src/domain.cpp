/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#include <logz4/domain.hpp>

#include <ours/compiler.hpp>

namespace logz4 {
namespace {
    // The bounds of LOGZ4_DOMAIN_SECTION. On ELF the linker script of every
    // freestanding image defines them (kernel.ld, phys.ld, physload.ld), and
    // a default GNU ld/LLD layout defines them itself because the section
    // name is a C identifier. Mach-O's linkers synthesize section$start$ and
    // section$end$ for any section named that way, present or not.
    //
    // A default ELF layout defines __start_/__stop_ only when the section
    // exists, so a hosted ELF program linking this file must declare at least
    // one Domain. Not weak to paper over that: a weak undefined reference
    // resolves to address zero, and an empty registry that silently answers
    // "no such domain" is worse than a link error that says why.
#if defined(__MACH__)
    extern "C" Domain section_start[] LINK_NAME("section$start$__DATA$__logz4_domains");
    extern "C" Domain section_end[] LINK_NAME("section$end$__DATA$__logz4_domains");
#else
    extern "C" Domain section_start[] LINK_NAME("__start_logz4_domains");
    extern "C" Domain section_end[] LINK_NAME("__stop_logz4_domains");
#endif
} // namespace

auto Domain::all() -> std::span<Domain> {
    return {section_start, section_end};
}

auto Domain::find(ustl::views::StringView name) -> Domain * {
    for (auto &domain: all()) {
        if (name == domain.name()) {
            return &domain;
        }
    }
    return nullptr;
}
} // namespace logz4
