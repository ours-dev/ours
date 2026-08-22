#include <ours/phys/build-id-note.hpp>
#include <ours/phys/build-id.hpp>

#include <ours/types.hpp>

#include <span>

namespace ours::phys {
namespace {
    extern "C" char const __build_id_note_start[];
    extern "C" char const __build_id_note_end[];
} // namespace

auto own_build_id() -> std::string_view {
    // Plain zero-initialized storage: constant-initialized (no dynamic-init
    // guard machinery), and safe to refill on every call since physload is
    // single-threaded at every point this can be reached from.
    static char buffer[41]{}; // sha1 = 20 bytes = 40 hex chars + NUL

    auto const notes = std::span(reinterpret_cast<u8 const *>(__build_id_note_start),
                                 static_cast<usize>(__build_id_note_end - __build_id_note_start));
    auto const length = hex_encode_build_id(notes, buffer);
    return {buffer, length};
}

} // namespace ours::phys
