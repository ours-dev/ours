# ort outstanding repairs

## 1. The runtime's start hook has one slot, so a second library that needs to run before main cannot have it

**Severity**
Low: it fails at link time rather than at run time, and only fsio uses the hook today.

**Symptom**
Latent, cannot be triggered today. `ort_extensions_init()` is an empty weak definition in `system/lib/ort/src/start.cpp`, replaced by fsio's strong one (`system/lib/fsio/src/fsio.cpp`). A second library defining it in the same program is a duplicate-symbol link error, since fsio is a source_set and its object is always linked; were either built as an archive, the linker could pull one definition and silently skip the other.

**Cause**
A weak symbol is one replaceable slot, not a list. Behind it is a larger gap that the same file states: nothing walks `.init_array`, in ort or in the loader, so a library has no ordinary way to register code to run before main, and a global that needs a constructor silently starts out as zeros.

**Change**
Walk `.preinit_array` and `.init_array` in `ort_start()` after the processargs are read and before main; fsio registers its install as a constructor, and the weak hook goes. Ruled out: a fixed list of hooks in ort, which would make ort name every library built on it.

**Reference**
The ELF `DT_INIT_ARRAY` convention, which a libc walks in its start routine before main (musl's `__libc_start_init()`).

**Acceptance**
A program linking two libraries that each register a pre-main function sees both run before main, and a global with a constructor starts initialised; both images boot.
