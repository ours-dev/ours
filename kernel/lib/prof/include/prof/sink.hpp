/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef PROF_SINK_HPP
#define PROF_SINK_HPP 1

#include <prof/record_buffer.hpp>

#include <array>

namespace prof {

/// Where exported lines go. A function pointer and the context it is called
/// with, the shape of `unwind::MemoryReader` and of `BufferLookup` below: the
/// library hands over bytes and the caller decides what a console is -- the
/// kernel's logger, a user buffer, a test's string. Each call gets one whole
/// line, so a writer that serialises its callers keeps lines intact.
struct LineWriter final {
    void *context;
    auto (*write)(void *context, char const *bytes, usize len) -> void;
};

/// Encodes one `<tag> <hex>\r\n` line on the stack and hands it to
/// |writer| in a single call. Line framing survives the texty serial console
/// without binary escapes, and the tag doubles as a resync point for the host
/// parser after a corrupted line.
///
/// `Bound` sizes the stack buffer; a caller passes the largest record it
/// will ever write.
template <usize Bound, usize TagSize>
auto write_hex_line(LineWriter const &writer, char const (&tag)[TagSize], void const *bytes,
                    usize len) -> void {
    constexpr usize TAG_LEN = TagSize - 1; // exclude the literal's NUL
    static constexpr char HEX_DIGITS[17] = "0123456789abcdef";

    std::array<char, TAG_LEN + 1 + 2 * Bound + 2> line{};
    usize pos = 0;
    for (usize i = 0; i < TAG_LEN; ++i) {
        line[pos++] = tag[i];
    }
    line[pos++] = ' ';

    auto const *raw = static_cast<u8 const *>(bytes);
    for (usize i = 0; i < len; ++i) {
        line[pos++] = HEX_DIGITS[raw[i] >> 4];
        line[pos++] = HEX_DIGITS[raw[i] & 0xF];
    }
    line[pos++] = '\r';
    line[pos++] = '\n';
    writer.write(writer.context, line.data(), pos);
}

/// Where records go, independent of what caused them.
///
/// This is the half of the profiler that a sampler and a tracer have in
/// common: per-CPU wait-free buffers, the storage for them injected by the
/// caller, aggregate statistics, and a hex-line export. What differs between
/// the two -- an interrupt versus reaching a patched code point -- lives in
/// the producer, not here, which is why nothing in this class knows about
/// PMUs, ticks, unwinding or trigger state.
///
/// Records are fixed-size. That covers a sampler's records and the ordinary
/// static tracepoint (an event id, a timestamp, a few arguments); an event
/// wanting a variable-length payload would need this to become a byte ring
/// instead, which is a change worth making only once such an event exists.
template <typename Record>
class RecordSink final {
  public:
    using Buffer = RecordBuffer<Record>;

    /// Looks up the buffer for `cpu`, or nullptr when there is none. The
    /// caller keeps owning the storage; on the kernel side that is
    /// `CpuLocal`, in a host test a plain array.
    using BufferLookup = Buffer *(*)(void *context, u32 cpu);

    struct Resources final {
        BufferLookup buffer_lookup = nullptr;
        void *buffer_lookup_context = nullptr;
        u32 num_cpus = 0;
    };

    /// Wires up the storage and clears whatever it reaches. Returns false
    /// when no buffer is reachable, which leaves the sink inert rather than
    /// unusable: `produce` simply drops.
    auto init(Resources const &resources) -> bool {
        m_buffer_lookup = resources.buffer_lookup;
        m_buffer_lookup_context = resources.buffer_lookup_context;
        m_num_cpus = resources.num_cpus;
        reset();
        return m_num_cpus != 0 && buffer_for(0) != nullptr;
    }

    auto reset() -> void {
        for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
            if (auto *buffer = buffer_for(cpu); buffer != nullptr) {
                buffer->reset();
            }
        }
    }

    [[nodiscard]]
    auto num_cpus() const -> u32 {
        return m_num_cpus;
    }

    [[nodiscard]]
    auto buffer_for(u32 cpu) const -> Buffer * {
        if (m_buffer_lookup == nullptr) {
            return nullptr;
        }
        return m_buffer_lookup(m_buffer_lookup_context, cpu);
    }

    /// Appends to `cpu`'s buffer. Wait-free; false means the buffer was
    /// full (and the drop has been counted) or there was no buffer.
    auto produce(u32 cpu, Record const &record) -> bool {
        auto *buffer = buffer_for(cpu);
        if (buffer == nullptr) {
            return false;
        }
        return buffer->try_produce(record);
    }

    [[nodiscard]]
    auto stats(u32 cpu) const -> BufferStats {
        auto const *buffer = buffer_for(cpu);
        if (buffer == nullptr) {
            return BufferStats{};
        }
        return buffer->stats();
    }

    /// Summed over every CPU.
    [[nodiscard]]
    auto totals() const -> BufferStats {
        BufferStats result{};
        for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
            auto const one = stats(cpu);
            result.retained += one.retained;
            result.dropped += one.dropped;
            result.total += one.total;
        }
        return result;
    }

    /// Writes every retained record as one `<tag> <hex>` line, in CPU order.
    /// `accept` decides which records to emit, so a producer can skip slots
    /// it never filled without this class knowing the record's layout.
    template <usize TagSize, typename Accept>
    auto export_records(LineWriter const &writer, char const (&tag)[TagSize], Accept accept) const
        -> void {
        for (u32 cpu = 0; cpu < m_num_cpus; ++cpu) {
            auto const *buffer = buffer_for(cpu);
            if (buffer == nullptr) {
                continue;
            }
            buffer->for_each_retained([&](Record const &record) {
                if (!accept(record)) {
                    return;
                }
                write_hex_line<sizeof(Record)>(writer, tag, &record, sizeof(record));
            });
        }
    }

  private:
    BufferLookup m_buffer_lookup = nullptr;
    void *m_buffer_lookup_context = nullptr;
    u32 m_num_cpus = 0;
};

} // namespace prof

#endif // #ifndef PROF_SINK_HPP
