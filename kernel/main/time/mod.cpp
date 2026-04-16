namespace ours::time {
    /// m = t << s / f (1)
    template <typename Target, typename Integer>
    auto calculate_mult_shift(Integer const f, Integer const t, Integer maxsec) -> void {
        // Assumps delta always lesser than |maxsec|.
        //
        // Make sure that the expression "|m| * delta" capable to storage in a |Target|.

        // Calculate the shift factr which is limiting the conversion range:
        Integer m = (maxsec * f) >> 32, mbits = 32;
        while (m) {
            m >>= 1;
            mbits -= 1;
        }
    
        // Find the conversion shift/mult pair which has the best
        // accuracy and fits the maxsec conversion range:
        Integer s = 32;
        for (; s > 0; --s) {
            m = ((t << s) + (f >> 1)) / f; // See (1)
            if ((m >> mbits) == 0) {
                break;
            }
        }

        return {m, s};
    }

} // namespace ours::time