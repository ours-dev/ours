// libc-free translation unit for the hermetic_source_set smoke test.
// No includes and no external calls: the hermetic partial link must end up
// with zero undefined symbols (verified by verify-undefined-symbols.py).
extern "C" int hermetic_smoke_symbol(int x) {
    return x * 2;
}
