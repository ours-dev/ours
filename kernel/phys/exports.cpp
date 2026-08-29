#include <ours/phys/exports.hpp>

#include <cstring>

namespace ours::phys {
namespace {
    // Bounds of the .phys.exports section, provided by phys.ld around the
    // table gen-phys-exports.py produced from the first link pass.  An image
    // linked without the table (the first pass itself) sees an empty range.
    extern "C" PhysExport const __phys_exports_start[];
    extern "C" PhysExport const __phys_exports_end[];
} // namespace

auto exports() -> std::span<PhysExport const> {
    return {__phys_exports_start, static_cast<usize>(__phys_exports_end - __phys_exports_start)};
}

auto resolve_export(std::string_view const name) -> void const * {
    // Linear scan.  The table has a few hundred rows and a module binds each
    // of its imports once, at load time; a hash table would cost more to
    // build than the scans it saves.
    for (auto const &entry: exports()) {
        if (name == entry.name) {
            return entry.address;
        }
    }
    return nullptr;
}

} // namespace ours::phys
