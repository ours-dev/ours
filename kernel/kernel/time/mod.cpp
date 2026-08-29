#include <ours/time/mod.hpp>

namespace ours::time {
/// Computes the (mult, shift) pair converting a count in |from| Hz into one
/// in |to| Hz as `(count * mult) >> shift`, picking the largest shift whose
/// mult still keeps `mult * delta` from overflowing across a |maxsec|-second
/// conversion range.
///
///     m = t << s / f      (1)
auto calculate_mult_shift(usize *mult, usize *shift, usize from, usize to, usize maxsec) -> void {
    auto const f = from;
    auto const t = to;

    // Assumps delta always lesser than |maxsec|.
    //
    // Make sure that the expression "|m| * delta" capable to storage in a |Target|.

    // Calculate the shift factr which is limiting the conversion range:
    usize m = (maxsec * f) >> 32, mbits = 32;
    while (m) {
        m >>= 1;
        mbits -= 1;
    }

    // Find the conversion shift/mult pair which has the best
    // accuracy and fits the maxsec conversion range:
    usize s = 32;
    for (; s > 0; --s) {
        m = ((t << s) + (f >> 1)) / f; // See (1)
        if ((m >> mbits) == 0) {
            break;
        }
    }

    *mult = m;
    *shift = s;
}

} // namespace ours::time
