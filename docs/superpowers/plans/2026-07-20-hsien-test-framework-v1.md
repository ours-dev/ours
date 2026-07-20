# Hsien Test Framework V1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the first usable `hsien` test path: C++26 annotation discovery through a Python Clang-AST scanner, generated `.hsien.tests` records, and a no-exception runner that records failures and continues.

**Architecture:** Keep the annotation-free runtime behind a small `TestRecord`/`Context`/`Reporter` interface. A GN action invokes a P2996-capable Clang to dump AST JSON, the Python scanner validates `[[=hsien::test]]`, and emits a wrapper source that includes each test source once and places internal records into `.hsien.tests`. Hosted and kernel runners consume the same section range; users never write a registration API or test entry.

**Tech Stack:** C++26/P2996 Clang for annotated test sources, C++ freestanding-compatible runtime, Python 3 standard library JSON/process APIs, GN `action()`/`source_set()`, and the existing kernel linker script/init-hook conventions.

---

### Task 1: Define the annotation-free runtime contract

**Files:**
- Create: `system/lib/hsien/include/hsien/test.hpp`
- Create: `system/lib/hsien/include/hsien/runner.hpp`
- Create: `system/lib/hsien/runner.cpp`
- Test: `system/lib/hsien/tests/runtime_test.cpp`

- [x] **Step 1: Write the failing runtime test**

Create a hosted-only test translation unit that constructs two `TestRecord`
values backed by ordinary functions, runs the section-independent runner over a
provided range, makes the first function record one failed check, and verifies
the second function still runs. The test must not use exceptions and must assert
that the summary contains two executed records and one failure.

- [x] **Step 2: Run the runtime test to verify it fails**

Run the P2996 container's ordinary C++ compiler against the test and runtime
sources:

```sh
docker run --rm -v "$PWD:/src" -w /src vsavkov/clang-p2996:arm64 \
  clang++ -std=c++26 -fno-exceptions -Wall -Wextra \
  system/lib/hsien/runner.cpp system/lib/hsien/tests/runtime_test.cpp \
  -o /tmp/hsien-runtime-test && /tmp/hsien-runtime-test
```

Expected result: compilation fails because the runtime headers and runner do
not exist yet.

- [x] **Step 3: Implement the minimal contract**

Define:

```cpp
namespace hsien {
struct Context;
struct Failure {
    char const* test_name;
    std::source_location location;
    char const* message;
};
using ReportFn = auto (*)(Failure const&) -> void;
struct Reporter {
    ReportFn report;
};
struct TestRecord {
    char const* name;
    void (*invoke)(Context&);
    std::source_location location;
};
struct Summary {
    std::size_t executed;
    std::size_t failed;
};
auto run(TestRecord const* begin, TestRecord const* end, Reporter&) -> Summary;
}
```

`Context::check` records failures and returns without changing control flow.
The runner invokes every record, never catches or throws, and reports only
through the injected reporter. Use `std::source_location`; do not add a
custom file/line type or assertion macro in this task.

- [x] **Step 4: Run the runtime test to verify it passes**

Run the same Docker command. Expected result: exit code 0, with the failing
check reported and both records executed.

- [x] **Step 5: Commit the runtime slice**

```sh
git add system/lib/hsien/include/hsien/test.hpp \
  system/lib/hsien/include/hsien/runner.hpp \
  system/lib/hsien/runner.cpp \
  system/lib/hsien/tests/runtime_test.cpp
git commit -m "feat: add hsien no-exception runtime contract"
```

### Task 2: Implement the Python annotation scanner

**Files:**
- Create: `system/lib/hsien/tools/scan_annotations.py`
- Create: `system/lib/hsien/tools/tests/test_scan_annotations.py`
- Create: `system/lib/hsien/tools/tests/fixtures/valid_test.cpp`
- Create: `system/lib/hsien/tools/tests/fixtures/invalid_test.cpp`

- [x] **Step 1: Write failing scanner tests**

Use Python's `unittest` and a fake AST JSON fixture to verify that the scanner:

1. emits one record for a `CXX26AnnotationAttr` whose nested
   `DeclRefExpr.referencedDecl.name` is `test` and whose type is
   `hsien::test_t`;
2. rejects an annotation of another type;
3. rejects a function whose parameter list is not exactly
   `hsien::Context&`;
4. sorts records deterministically by normalized source path and function name;
5. escapes names and paths as C++ string literals.

- [x] **Step 2: Run the scanner tests to verify they fail**

```sh
python3 -m unittest discover -s system/lib/hsien/tools/tests -v
```

Expected result: import/command failures because the scanner module is absent.

- [x] **Step 3: Implement the scanner**

The command-line interface must accept `--compiler`, repeated `--flag`, one or
more source paths, and `--output`. It must invoke:

```text
<compiler> <flags> -Xclang -ast-dump=json -fsyntax-only <source>
```

Parse JSON with Python's `json` module. Walk nested `inner` arrays recursively,
match `CXX26AnnotationAttr`, inspect its annotation expression, and validate
the referenced declaration and function type. Fail closed with a non-zero exit
status and source location when a test annotation cannot be proven valid.

Emit a wrapper containing the original source include and one internal record
per test. The generated wrapper must use a stable relative include path and
must not emit any user-visible registration function.

- [x] **Step 4: Run scanner tests to verify they pass**

```sh
python3 -m unittest discover -s system/lib/hsien/tools/tests -v
```

Expected result: all fixture tests pass, including rejection cases.

- [x] **Step 5: Run one real P2996 AST scan**

```sh
docker run --rm -v "$PWD:/src" -w /src vsavkov/clang-p2996:arm64 \
  python3 system/lib/hsien/tools/scan_annotations.py \
  --compiler clang++ --flag=-std=c++26 --flag=-freflection-latest \
  --output /tmp/hsien_generated.cpp \
  system/lib/hsien/tools/tests/fixtures/valid_test.cpp
```

Expected result: a generated wrapper containing the fixture include and one
`.hsien.tests` record.

- [x] **Step 6: Commit the scanner slice**

```sh
git add system/lib/hsien/tools
git commit -m "feat: discover hsien tests from Clang AST"
```

### Task 3: Integrate GN, ELF section, and standalone smoke runner

**Files:**
- Create: `system/lib/hsien/BUILD.gn`
- Create: `system/lib/hsien/tests/BUILD.gn`
- Create: `system/lib/hsien/tests/section_test.cpp`
- Create or modify: `system/lib/hsien/hsien-section.ld` only if a standalone linker script is needed by the smoke target
- Modify: `system/lib/BUILD.gn`
- Modify: `kernel/kernel.ld` to define and retain `.hsien.tests` only under the test-capable link

- [x] **Step 1: Write the failing section smoke test**

Create two annotated fixture functions, one recording a failure and one setting
a `second_ran` flag. Build a standalone hosted executable from generated
wrappers and the runtime. The test must assert that the linked executable has
`.hsien.tests`, that both records execute, and that the summary contains one
failure.

- [x] **Step 2: Run the smoke test to verify it fails**

Run the GN target or its direct Docker compilation command before adding the
GN target. Expected result: missing target/section symbols or no generated
records.

- [x] **Step 3: Add GN targets and generated action**

Add `hsien` headers/runtime, a test-only `scan` action, generated wrapper
outputs, and a hosted smoke executable. Wire `system/lib:tests` to
`hsien:tests`. Keep generated files under `$root_build_dir`; do not commit
generated output.

- [x] **Step 4: Add linker section symbols and retention**

Add `.hsien.tests` with `KEEP()` and start/stop symbols following the existing
`.gktl.init_hook*` pattern. Ensure the production path has no dependency on
the section or runner when `kernel_unit_tests_enabled` is false.

- [x] **Step 5: Run the smoke test to verify it passes**

Run the standalone hosted target, inspect the binary with `llvm-readelf -S`,
and verify output contains two executed records and one recorded failure.

- [ ] **Step 6: Run the disabled-build check**

Build the same target with tests disabled and verify `llvm-readelf -S` reports
no `.hsien.tests` section and no hsien runner object in the link response.

- [x] **Step 7: Run freestanding compile verification**

Compile the annotation-free runtime with:

```sh
clang++ -std=c++26 -ffreestanding -fno-exceptions -fno-rtti \
  -fsyntax-only system/lib/hsien/runner.cpp
```

Expected result: success without libc++ exception or RTTI dependencies.

- [x] **Step 8: Commit the integration slice**

```sh
git add system/lib/BUILD.gn system/lib/hsien/BUILD.gn \
  system/lib/hsien/tests/BUILD.gn system/lib/hsien/tests/section_test.cpp \
  kernel/kernel.ld
git commit -m "feat: collect hsien tests in a linker section"
```

### Task 4: Final verification

- [x] **Step 1: Run all Python scanner tests.**
- [x] **Step 2: Run hosted runtime and section smoke tests.**
- [x] **Step 3: Run freestanding compile verification with exceptions disabled.**
- [x] **Step 4: Inspect `git diff --check` and `git status --short`.**
- [x] **Step 5: Confirm no generated files or unrelated worktree changes are staged.**

The GN disabled-build check remains pending because this environment does not
provide a `gn` executable; the GN graph was reviewed statically instead.
