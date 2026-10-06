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
    extern "C" Domain SECTION_START[] LINK_NAME("section$start$__DATA$__logz4_domains");
    extern "C" Domain SECTION_END[] LINK_NAME("section$end$__DATA$__logz4_domains");
#else
    extern "C" Domain SECTION_START[] LINK_NAME("__start_logz4_domains");
    extern "C" Domain SECTION_END[] LINK_NAME("__stop_logz4_domains");
#endif

    /// The slot for an image with one thread of execution, and for the
    /// start of any image, before a per-context hook is installed.
    Domain const *IMAGE_DOMAIN = nullptr;

    auto image_domain_slot() -> Domain const *& {
        return IMAGE_DOMAIN;
    }

    /// Constant-initialized, so a LogDomain entered by the first line of an
    /// image already has a slot to write.
    Domain const *&(*REGISTERED_SLOT)() = image_domain_slot;
} // namespace

auto set_domain_slot(Domain const *&(*slot)()) -> void {
    REGISTERED_SLOT = slot;
}

auto current_domain_slot() -> Domain const *& {
    return REGISTERED_SLOT();
}

auto Domain::all() -> std::span<Domain> {
    return {SECTION_START, SECTION_END};
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
