// Tiny host shared library for the ifs_extract smoke test.
extern "C" int ifs_exported_symbol(int x) {
    return x + 1;
}
