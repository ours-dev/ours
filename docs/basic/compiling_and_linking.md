# Concepts of Compiling and Linking

Compiling and linking are not just build-system implementation details. They
define the boundary between source code, object code, binary layout, runtime
loading, debugging, exception handling, and ABI compatibility. If a program
fails only in release mode, only after being split into a shared library, only
on one architecture, or only after enabling LTO, the cause is often in this
boundary.

The useful mental model is:

```text
source files
  -> translation units
  -> object files
  -> archives or shared objects
  -> executable image
  -> loaded process
```

Each step preserves some information, discards some information, and creates
new obligations for the next step.

## Compiling

Compiling turns one translation unit into one relocatable object file. A
translation unit is usually one `.c`, `.cc`, or `.cpp` file after all `#include`
and macro expansion has happened. The compiler does not see "the whole
program"; it sees one expanded file at a time, plus declarations imported
through headers or modules.

That is why declarations and definitions matter:

- a declaration tells the compiler that a name exists and how to call or access
  it;
- a definition allocates storage or emits code;
- a mismatch between declaration and definition can compile cleanly but fail at
  link time, or worse, run with undefined behavior because the ABI contract is
  wrong.

The object file produced by the compiler usually contains:

- machine code in sections such as `.text`;
- constants and read-only data in `.rodata`;
- initialized writable data in `.data`;
- zero-initialized data in `.bss`;
- a symbol table describing definitions and unresolved references;
- relocation records saying "this instruction or data slot must be fixed when
  the final address is known";
- optional debug information such as DWARF;
- optional unwind information used by stack walking and exceptions.

The compiler must emit enough information for the linker to finish the job, but
it should not decide things that belong to whole-program layout. For example,
when a function calls another function from a different object file, the
compiler can emit a call instruction plus a relocation record; the linker later
chooses the final target address.

### ABI Is the Real Contract

The application binary interface, or ABI, is the low-level contract that lets
separately compiled code work together. It covers details such as:

- how arguments and return values are passed;
- which registers a caller or callee must preserve;
- how structures are laid out and aligned;
- how names are mangled in C++;
- how virtual tables, RTTI, exceptions, TLS, and atomics are represented;
- how stack frames can be walked.

Why this matters: source compatibility does not imply binary compatibility. Two
libraries may compile from compatible headers but still be ABI-incompatible if
they use different compilers, different standard library ABIs, different
packing rules, different exception settings, or different C++ ABI versions.

How to work with it:

- keep ABI-facing types simple across binary boundaries;
- prefer C interfaces or explicitly versioned C++ interfaces for plugin and
  shared-library boundaries;
- avoid exposing STL containers, inline-heavy templates, or compiler-specific
  types across long-lived ABI boundaries;
- rebuild all dependent code after changing structure layout, virtual
  functions, enum size, calling convention, or exception mode.

### Visibility

Visibility controls whether a symbol is available outside the object or shared
library that defines it. Developers often learn `public` and `private` at the
source-language level, but binary visibility is different. A function can be
private in design and still exported from a shared object unless the compiler
and linker are told otherwise.

Common ELF visibility values:

- `default`: the symbol can be exported and may be interposed by another shared
  object;
- `hidden`: the symbol is not exported for normal dynamic lookup outside the
  shared object;
- `protected`: the symbol is exported, but references inside the defining
  object bind locally;
- `internal`: a stronger toolchain-specific form of hidden visibility.

Why this matters:

- smaller exported symbol tables make dynamic loading faster;
- fewer exported names reduce accidental ABI commitments;
- hidden symbols allow better optimization because the compiler and linker can
  assume they cannot be replaced by another shared object;
- symbol interposition can surprise you: a call that looks local in source may
  legally bind to another definition at runtime when default visibility is used.

How to do it:

```c
// GCC and Clang
__attribute__((visibility("default"))) void public_api();
__attribute__((visibility("hidden"))) void internal_helper();
```

For CMake or direct compiler flags, a common baseline is:

```sh
-fvisibility=hidden -fvisibility-inlines-hidden
```

Then explicitly export the small API surface:

```c
#if defined(_WIN32)
#  define API_EXPORT __declspec(dllexport)
#else
#  define API_EXPORT __attribute__((visibility("default")))
#endif

extern "C" API_EXPORT int library_init(void);
```

The rule is simple: default to hidden, export intentionally.

### Unwind Information

Unwind information describes how to recover caller stack frames from the current
machine state. It is commonly stored in sections such as `.eh_frame`,
`.eh_frame_hdr`, `.debug_frame`, `.ARM.exidx`, or platform-specific
equivalents.

Why this matters:

- C++ exceptions need unwind metadata to destroy objects and find handlers;
- profilers and crash reporters need it to produce stack traces;
- sanitizers and debuggers use it to explain failures;
- kernels, runtimes, and embedded systems often need stack traces even when C++
  exceptions are disabled.

Unwind metadata is related to, but not the same as, debug information. Debug
information answers "what source line and variable is this?" Unwind information
answers "how do I move from this stack frame to its caller?"

How to choose:

- use `-fno-exceptions` only when you also have a clear error-handling model;
- keep unwind tables with `-funwind-tables` or `-fasynchronous-unwind-tables`
  when reliable stack traces matter;
- keep frame pointers with `-fno-omit-frame-pointer` if simple profiling and
  postmortem stack walking are more important than the extra register;
- strip debug information from release artifacts only after producing a separate
  symbol/debug file.

### Debug Info and Optimization

Debug info is not executable behavior, but it is part of the developer
experience of a binary. Optimized code can inline functions, remove variables,
merge identical code, and reorder instructions. The debugger then has to map a
changed machine program back to source code.

Why this matters: `-g` does not mean "debuggable in the way source code looks."
At `-O2`, a variable may not exist as a stable memory location. A function may
not have a call frame. Several source lines may map to the same instruction.

How to work with it:

- use `-Og -g` when interactive debugging matters most;
- use `-O2 -g` for production builds that still need symbolized crash reports;
- split debug info from shipped binaries with tools such as `objcopy`,
  `llvm-objcopy`, or platform-specific symbol servers;
- preserve build IDs so crash reports can locate the correct debug file.

## Linking

Linking combines object files and libraries into an executable or shared
object. The linker resolves symbols, applies relocations, lays out sections,
builds program headers, chooses exported symbols, and emits metadata that the
loader or debugger will need later.

A linker is not just a file concatenator. It is a whole-program layout engine.

### Symbols, Bindings, and Relocations

A symbol has a name, a binding, a type, a section, and sometimes visibility.
Important bindings include:

- local: visible only inside the object file;
- global: visible to other objects;
- weak: can be overridden by a strong definition and may resolve to zero or a
  fallback when absent.

A relocation is a delayed fixup. It says: when the final address or offset of a
symbol is known, patch this instruction or data field according to this
relocation type.

Why this matters:

- "undefined reference" means the linker could not find a required definition;
- "multiple definition" means more than one strong definition survived;
- architecture-specific relocation errors often mean the code model, PIC mode,
  or address range is wrong;
- weak symbols are useful for optional hooks, but they can hide configuration
  mistakes if overused.

How to inspect:

```sh
nm -C file.o
readelf -Ws file.o
readelf -r file.o
objdump -dr file.o
```

Use `nm -C` for a quick C++ demangled symbol view. Use `readelf` or
`llvm-readelf` when you need exact ELF fields.

### Sections and Segments

Object files contain sections. Loaded programs contain segments. The linker maps
input sections into output sections, then groups output sections into loadable
segments.

Common sections:

- `.text`: executable code;
- `.rodata`: read-only constants;
- `.data`: initialized writable data;
- `.bss`: zero-initialized writable data;
- `.init_array`: constructors or initialization callbacks;
- `.got` and `.plt`: dynamic linking support;
- `.eh_frame`: unwind metadata;
- `.debug_*`: debug metadata.

Why this matters:

- memory protection is segment-based: code should be executable but not
  writable, constants should be readable but not writable, data should be
  writable but not executable;
- embedded systems and kernels often need exact placement of boot headers,
  vectors, page-aligned regions, or memory-mapped data;
- linker garbage collection works at section granularity.

How to do it:

- compile with `-ffunction-sections -fdata-sections`;
- link with `-Wl,--gc-sections`;
- protect required metadata from garbage collection with `KEEP(...)` in a
  linker script;
- inspect layout with `readelf -S`, `readelf -l`, and linker map files.

### Archives Are Searched Lazily

A static archive (`.a`) is a collection of object files plus an index. Linkers
usually pull members from an archive only when they satisfy an unresolved symbol
seen so far.

Why this matters: library order can change the result.

```sh
# Often works: main.o needs symbols from libmath_helpers.a
cc main.o libmath_helpers.a -o app

# Often fails: the archive was scanned before anything needed it
cc libmath_helpers.a main.o -o app
```

How to do it:

- put libraries after the objects that use them;
- use linker groups for circular static-library dependencies:
  `-Wl,--start-group ... -Wl,--end-group`;
- avoid circular archive dependencies when possible; they usually signal a
  design problem.

### COMDAT, Weak ODR, and Template Code

C++ templates, inline functions, and vtables often produce duplicate candidate
definitions in multiple object files. Linkers handle many of these through
COMDAT groups or weak ODR definitions, keeping one copy and discarding the
others.

Why this matters:

- duplicate template instantiations are normal;
- duplicate non-inline global definitions are usually a bug;
- violating the One Definition Rule can appear to work until LTO, optimization,
  or link order changes the chosen definition.

How to work with it:

- put function templates and inline definitions in headers intentionally;
- put non-template global definitions in exactly one source file;
- use `inline` variables in C++17+ when a header-defined variable is intended;
- treat ODR sanitizer or LTO ODR diagnostics seriously.

### LTO Changes the Boundary

Link-time optimization keeps compiler IR until link time so the optimizer can
see across translation units.

Why this matters:

- the compiler can inline across object-file boundaries;
- unused code can be removed more aggressively;
- ODR and ABI mistakes may surface only with LTO;
- build systems must use compatible compiler, linker, and archive tools because
  the "object file" may contain IR rather than only final machine code.

How to use it:

- use the compiler driver for linking, not raw `ld`, so the LTO plugin is passed
  correctly;
- use `llvm-ar` with Clang LTO or the matching GCC binutils plugin with GCC LTO;
- test non-LTO and LTO builds if you ship both modes;
- do not use LTO to compensate for unclear module boundaries.

## Statically Looking Up

Static lookup means symbol resolution happens before the program starts. This is
the normal model for ordinary function calls inside one executable and for code
pulled from static libraries.

The static path is:

1. a compiler emits an undefined reference into an object file;
2. the linker sees that reference;
3. it finds a matching definition in another object file or archive member;
4. it applies relocations;
5. the final binary contains a direct address, relative offset, or resolved data
   reference.

Why static lookup is useful:

- startup is simpler because less work is deferred to the loader;
- missing symbols are caught at build time;
- calls can often be direct and optimizable;
- deployment is easier when fewer runtime library files must be matched.

What can go wrong:

- missing object or library: `undefined reference`;
- wrong library order: archive member is never pulled;
- duplicate strong definitions: `multiple definition`;
- C++ name mismatch: missing `extern "C"` or incompatible declaration;
- ABI mismatch: link succeeds but runtime behavior is wrong.

How to debug it:

```sh
nm -C libthing.a | grep symbol_name
readelf -Ws app.o | grep symbol_name
cc -Wl,-Map=app.map ...
```

Use a linker map when you need to answer "which object file provided this
symbol?" or "why is this section in the final binary?"

### Static Registration

Some systems avoid explicit registration calls by placing records in custom
sections and asking the linker to provide start and end symbols. Test
frameworks, kernels, plugin registries, tracing systems, and initialization
tables often use this pattern.

Why this matters: it allows independently compiled modules to contribute to one
registry without central code edits.

How it works:

```c
struct Command {
  const char *name;
  void (*run)(void);
};

__attribute__((used, section("commands")))
static const struct Command help_command = {
  .name = "help",
  .run = run_help,
};
```

A linker script or platform convention then preserves the section and exposes
its bounds.

What to watch:

- mark records `used` so the compiler does not discard them;
- protect the section with `KEEP(...)` if linker garbage collection is enabled;
- define alignment explicitly;
- do not assume section order unless the linker script specifies it.

## Dynamically Looking Up

Dynamic lookup means some symbol resolution is deferred until load time or call
time. Shared libraries make this possible. On ELF systems, the dynamic loader
uses metadata such as `.dynamic`, `.dynsym`, `.dynstr`, relocation sections,
GOT, PLT, `SONAME`, `RPATH`, and `RUNPATH`.

Why dynamic lookup is useful:

- multiple programs can share one library file;
- large components can be updated independently when ABI is stable;
- plugins can be loaded at runtime;
- optional features can be discovered with APIs such as `dlopen` and `dlsym`.

Why it is dangerous:

- missing libraries or incompatible versions fail at startup or first use;
- exported symbols become ABI commitments;
- symbol interposition can replace definitions unexpectedly;
- global constructors across shared objects have tricky ordering;
- unloading shared libraries is hard when threads, TLS, callbacks, or static
  state are involved.

### GOT, PLT, and PIC

Position-independent code can run no matter where the loader maps it. It avoids
hardcoding absolute addresses into instructions.

The global offset table, or GOT, stores addresses that the loader may need to
relocate. The procedure linkage table, or PLT, is a call stub mechanism commonly
used for external functions.

Why this matters:

- shared libraries normally need `-fPIC`;
- PIE executables use similar ideas for address space layout randomization;
- PLT indirection can have performance and security costs;
- hidden visibility can remove unnecessary PLT calls for internal functions.

How to choose:

- build shared libraries with `-fPIC`;
- build hardened executables with PIE when the platform supports it;
- use `-fno-plt` when appropriate on ELF targets;
- use hidden visibility for internal calls;
- use `-Wl,-z,now` when you prefer resolving calls at startup over lazy binding.

### Runtime Search Paths

The dynamic loader must find each shared library at runtime. Link-time `-L`
only helps the linker find libraries during the build. It does not guarantee
that the loader can find them after deployment.

How to reason about it:

- `SONAME` identifies the ABI name of a shared object;
- `DT_NEEDED` lists required shared objects;
- `RPATH` and `RUNPATH` encode runtime search hints;
- environment variables such as `LD_LIBRARY_PATH` can affect lookup, but should
  not be the deployment model for production software.

How to inspect:

```sh
readelf -d libthing.so
ldd ./app        # useful, but be careful with untrusted binaries
```

Prefer packaging, install names, or controlled rpaths over relying on developer
shell environment.

### Dynamic Symbol APIs

Runtime lookup APIs such as `dlopen` and `dlsym` are powerful because they let a
program decide what code to load after it has started.

How to use them safely:

- expose a small C ABI entry point, for example `plugin_init`;
- version the plugin ABI explicitly;
- pass function tables or opaque handles instead of exposing C++ class layouts;
- define ownership, threading, and unload rules;
- keep the host responsible for allocation if allocator mismatch is possible.

Avoid designing a plugin ABI around C++ exceptions, STL types, or inline class
methods unless every plugin is built with the same compiler, standard library,
flags, and release cadence.

### Visibility Revisited

Dynamic lookup makes visibility more important, not less. Every exported symbol
is part of the dynamic linker's search space and may become part of your ABI
whether you intended it or not.

Practical rule:

1. compile with hidden visibility by default;
2. export only the ABI surface;
3. verify exports with `nm -D`, `readelf --dyn-syms`, or `llvm-readelf`;
4. use a linker version script when a library has a stable public ABI.

Example version script:

```ld
LIBTHING_1.0 {
  global:
    thing_create;
    thing_destroy;
    thing_run;
  local:
    *;
};
```

This keeps accidental helper functions out of the dynamic symbol table.

## Practical Checklist

- Decide the ABI boundary before deciding the build flags.
- Keep internal symbols hidden and public symbols intentional.
- Use frame pointers or unwind tables if production stack traces matter.
- Use `-ffunction-sections -fdata-sections` plus linker garbage collection when
  size matters, but protect required metadata with `KEEP(...)`.
- Use the compiler driver for final linking unless you have a specific reason
  to call the linker directly.
- Inspect binaries with `nm`, `readelf`, `objdump`, and linker map files.
- Treat static and dynamic linking as design choices, not just build options.
- When a link problem is confusing, ask three questions: Who needs this symbol?
  Who defines it? At what stage should it be resolved?
