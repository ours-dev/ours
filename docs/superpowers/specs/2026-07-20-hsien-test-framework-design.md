# Hsien Test Framework V1 Design

Date: 2026-07-20
Status: User-approved V1 direction; implementation pending

## Goal

Build `system/lib/hsien` as a small, freestanding-friendly test framework for
kernel subsystems such as memory management and scheduling. The user-facing
test registration interface is the C++26 annotation proposed by P3394:

```cpp
[[=hsien::test]]
void pmm_allocates_one_frame(hsien::Context&);
```

The same test source must be usable in hosted and freestanding builds. Users
do not write a test `main`, an explicit runner, a registration macro, or a
manual registry.

## Constraints and Decisions

- C++26 annotations are the only test discovery and registration interface.
- Runtime helpers such as `hsien::Context`, assertions, and reporters are
  allowed; they do not register tests.
- The framework never uses C++ exceptions. Builds use `-fno-exceptions` and
  the implementation has no exception-based control flow.
- Assertion failures are recorded only. They do not throw, panic, halt,
  terminate the current test, or alter kernel scheduling. Execution continues
  through the current test and all later tests.
- Tests are collected in a dedicated `.hsien.tests` ELF section so the whole
  feature can be removed through the build configuration.
- The first discovery backend is a Python Clang-AST scanner. It is a build-time
  adapter, not part of the kernel runtime.
- P2996 compile-time reflection remains useful for validating the language
  syntax, but V1 does not depend on the current experimental
  `std::meta::annotations_of()` implementation.

## Architecture

```text
GN test sources
      |
      v
Python annotation scanner
  clang++ -Xclang -ast-dump=json -fsyntax-only
      |
      v
Hidden generated wrapper source
  include original test source
  emit TestRecord objects
      |
      v
P2996-capable source compilation, then normal linking
      |
      v
.hsien.tests section
      |
      v
Hidden hsien runner scans section
      |
      v
Hosted or kernel reporter
```

The generated `TestRecord` is the internal seam between discovery and runtime:

```cpp
struct TestRecord {
    char const* name;
    void (*invoke)(Context&);
    std::source_location location;
};
```

The exact storage representation may be reduced for the kernel image, but the
runner must not depend on Python, Clang, libc++, or `std::meta`.

## Annotation Scanner

The scanner runs before normal compilation through a GN `action()` target. It
receives the same include paths, defines, target flags, and source list as the
test compile. It invokes Clang with AST JSON dumping and parses structured
JSON, never source-text regular expressions.

P2996 Clang represents the syntax as `CXX26AnnotationAttr` whose expression
contains a `DeclRefExpr` referring to `hsien::test`. The scanner therefore:

1. Finds annotations attached to declarations.
2. Accepts only the `hsien::test_t` annotation for V1.
3. Requires the function form `void test_name(hsien::Context&)`.
4. Rejects duplicate annotations, unsupported declaration kinds, overload
   ambiguity, and invalid signatures with a source-located build error.
5. Sorts records by normalized source path and function name for deterministic
   generated output.

For each source file containing tests, the scanner emits a hidden wrapper. The
wrapper includes the original source once and then emits section records that
refer to the discovered functions. The original test source is not compiled
as a second independent translation unit. This keeps V1 compatible with
anonymous namespaces and local helper definitions.

The scanner is an internal adapter. Replacing its implementation later must
not change test source syntax or the `TestRecord`/runner contract.

## Section and Linker Integration

Each generated record is placed in `.hsien.tests` with retention attributes.
The kernel linker script defines the section and exposes start/stop symbols in
the same style as existing init-hook sections:

```text
__start_hsien_tests
__stop_hsien_tests
```

The runner iterates the half-open range `[start, stop)` in link order. V1 does
not promise a semantic ordering beyond deterministic generation and linker
order.

When unit tests are disabled, GN does not compile the test wrappers or hsien
runner, and the production linker input contains no `.hsien.tests` section.
When enabled, the test source and generated wrappers are compiled with the
P2996-capable frontend so the annotation syntax is accepted. The annotation-
free hsien runtime and the final link remain usable with the selected hosted
or freestanding toolchain. The section and runner are linked as test-only
dependencies.

## Runtime

The runtime has three internal layers:

- `TestRecord`: immutable executable metadata in the section.
- `Context`: current test name, failure count, assertion checking, and
  reporter access.
- `Reporter`: hosted stdout or kernel console/serial output.

V1 uses `std::source_location` for assertion locations. The framework's
assertion helpers only update `Context` and call the reporter. They never use
exceptions or terminate execution.

The hidden runner automatically scans the section through the existing kernel
initialization seam. Hosted builds use a framework-provided executable entry;
kernel builds use an internal init hook. Neither is written by test authors.

## Failure Semantics

- Every failed check increments the current test's failure count.
- The failure is reported with the test name, source location, and message.
- Execution continues immediately after the check.
- The runner executes every record even after failures.
- The final summary is informational only. It does not return a failing
  process status, panic, halt, or otherwise change system behavior in V1.
- A separate kernel fault or explicit kernel panic remains kernel behavior and
  is outside hsien's assertion failure semantics.

## V1 Scope

V1 proves the complete path with a minimal test:

- annotation declaration and AST discovery;
- generated wrapper and `TestRecord` emission;
- `.hsien.tests` presence in the linked artifact;
- hosted section scanning and execution;
- freestanding compilation of the core runtime with exceptions disabled;
- removal of wrappers, runner, and section when tests are disabled;
- failure recording while subsequent tests continue.

V1 does not include fixtures, parameterized tests, timeouts, parallel tests,
test isolation, QEMU automation, or PMM/scheduler coverage beyond a minimal
smoke test. Those are later increments over the same seam.

## Risks and Verification

The P2996 Docker toolchain is experimental. V1 must pin the tested compiler
invocation and treat AST node-shape changes as scanner compatibility failures.
The scanner should fail closed when it cannot prove that an annotation is a
`hsien::test` annotation; silently missing tests is unacceptable.

The implementation plan must include hosted Docker smoke tests, AST fixture
tests for valid and invalid declarations, ELF section inspection, and a
freestanding compile check. It must also verify that unrelated working-tree
changes are not staged.
