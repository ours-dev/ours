# Annotation Action Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement an independent pre-compile C++ attribute scanner that dispatches normalized annotations to built-in or configured external action plugins.

**Architecture:** Add a Python 3 standard-library tool under `tools/annotation`. The core owns registry validation, Clang AST JSON discovery, deterministic scheduling, JSON protocol validation, output materialization, diagnostics, and the CLI. In-process Python callbacks and external commands use the same request/result model. GN integration is a thin pre-compile action and does not expose annotation semantics to the build graph.

**Tech Stack:** Python 3, Clang AST JSON, `unittest`, JSON over stdin/stdout for external actions, GN `action()`.

---

### Task 1: Define the framework data model and registry

**Files:**
- Create: `tools/annotation/__init__.py`
- Create: `tools/annotation/model.py`
- Create: `tools/annotation/registry.py`
- Test: `tools/annotation/tests/test_registry.py`

- [ ] **Step 1: Write the failing tests**

Cover these behaviors with `unittest`:

```python
def test_registers_action_and_matches_canonical_attribute():
    registry = Registry()
    registry.register(ActionSpec("ours::generate", ("record",), 1, callback))
    self.assertIs(registry.resolve("ours::generate").callback, callback)

def test_rejects_duplicate_attribute_names():
    registry = Registry()
    registry.register(spec)
    with self.assertRaises(RegistryError):
        registry.register(spec)

def test_rejects_invalid_external_command_and_schema_version():
    with self.assertRaises(RegistryError):
        ActionSpec("ours::bad", ("record",), 0, command=[])
```

Define `ActionSpec`, `AnnotationInfo`, `ActionRequest`, `GeneratedFile`,
`Diagnostic`, and `ActionResult` as frozen dataclasses. `ActionSpec` must have
exactly one of `callback` and `command`, a positive schema version, a
non-empty canonical name containing `::`, and at least one declaration kind.

- [ ] **Step 2: Run the focused tests and verify the expected failure**

Run:

```sh
python3 -m unittest tools.annotation.tests.test_registry -v
```

Expected: import or symbol failures because the package is absent.

- [ ] **Step 3: Implement the model and registry**

Implement immutable records and `Registry.register()` / `Registry.resolve()`.
Normalize declaration kinds to tuples, reject duplicate names, and expose a
read-only `items()` method sorted by canonical name. Do not add plugin loading,
AST logic, or build integration in this task.

- [ ] **Step 4: Run focused and package tests**

Run:

```sh
python3 -m unittest tools.annotation.tests.test_registry -v
```

Expected: all registry tests pass.

- [ ] **Step 5: Commit the registry slice**

```sh
git add tools/annotation docs/superpowers/plans/2026-07-20-annotation-action-framework.md
git commit -m "feat: add annotation action registry contracts"
```

### Task 2: Add Clang AST discovery and normalization

**Files:**
- Create: `tools/annotation/discovery.py`
- Test: `tools/annotation/tests/test_discovery.py`
- Create: `tools/annotation/tests/fixtures/ast_valid.json`
- Create: `tools/annotation/tests/fixtures/ast_malformed.json`

- [ ] **Step 1: Write failing AST fixture tests**

Use structured JSON fixtures to test:

```python
def test_discovers_registered_attribute_on_function():
    records = discover_from_ast(valid_ast, source_path, registry)
    self.assertEqual(records[0].canonical_name, "ours::generate")
    self.assertEqual(records[0].qualified_name, "sample::make")

def test_ignores_attributes_outside_registered_namespaces():
    self.assertEqual(discover_from_ast(ast_with_clang_attribute, path, registry), [])

def test_rejects_unknown_attribute_in_registered_namespace():
    with self.assertRaises(DiscoveryError):
        discover_from_ast(ast_with_unknown_ours_attribute, path, registry)

def test_rejects_unsupported_declaration_kind():
    with self.assertRaises(DiscoveryError):
        discover_from_ast(ast_with_namespace_declaration, path, registry)

def test_orders_records_by_path_line_column_and_name():
    self.assertEqual([item.line for item in discover_from_ast(ast, path, registry)], [3, 8])
```

The fixture must represent `AnnotateAttr`/`CXX11Attribute` AST nodes through the
actual node shape accepted by the implementation; keep the fixture small and
document the node fields used by the walker.

- [ ] **Step 2: Run discovery tests and verify they fail for missing code**

Run:

```sh
python3 -m unittest tools.annotation.tests.test_discovery -v
```

Expected: import or symbol failures.

- [ ] **Step 3: Implement the AST walker**

Implement recursive traversal of `inner` arrays. Normalize each supported
attribute to `AnnotationInfo` with source path, line, column, declaration kind,
name, qualified name, and structured arguments. Match registered namespaces
without treating unrelated compiler attributes as framework attributes. Raise
`DiscoveryError` with `path:line:column` for malformed registered attributes,
unsupported declaration kinds, and missing required declaration identity.

- [ ] **Step 4: Add compiler invocation**

Implement `run_clang_ast_dump(compiler, flags, source)` using
`subprocess.run(..., check=False, capture_output=True, text=True)`, adding
`-Xclang -ast-dump=json -fsyntax-only`. Convert compiler errors and invalid
JSON into source-located `DiscoveryError` values. Preserve the exact compile
flags supplied by the caller.

- [ ] **Step 5: Run discovery tests and commit**

Run:

```sh
python3 -m unittest tools.annotation.tests.test_discovery -v
```

Expected: all fixture and fake-compiler tests pass.

```sh
git add tools/annotation/discovery.py tools/annotation/tests
git commit -m "feat: discover registered C++ attributes from Clang AST"
```

### Task 3: Implement action dispatch and JSON protocol

**Files:**
- Create: `tools/annotation/actions.py`
- Create: `tools/annotation/protocol.py`
- Test: `tools/annotation/tests/test_actions.py`
- Test: `tools/annotation/tests/fixtures/external_action.py`

- [ ] **Step 1: Write failing dispatch tests**

Test that an in-process callback receives one deterministic `ActionRequest`,
that an external command receives one JSON request on stdin and returns one
JSON `ActionResult` on stdout, and that malformed JSON, non-zero exit status,
and missing required result fields raise `ActionError`.

- [ ] **Step 2: Run the tests and verify the missing implementation failure**

```sh
python3 -m unittest tools.annotation.tests.test_actions -v
```

Expected: import or symbol failures.

- [ ] **Step 3: Implement JSON serialization and validation**

Implement `request_to_json()` and `result_from_json()` with explicit schema
version checks, required-field checks, diagnostic severity validation, and
relative output path validation. Reject absolute paths and `..` traversal
before any file is written. Keep wire JSON stable and deterministic with
sorted keys.

- [ ] **Step 4: Implement both action bridges**

Implement `dispatch(spec, request, output_root)`:

```python
if spec.callback is not None:
    result = spec.callback(request)
else:
    result = run_external(spec.command, request)
return validate_result(result, output_root)
```

External commands receive UTF-8 JSON on stdin, write exactly one JSON result
to stdout, and may write human-readable failure details to stderr. Do not
retry or continue after a failure.

- [ ] **Step 5: Run action tests and commit**

```sh
python3 -m unittest tools.annotation.tests.test_actions -v
git add tools/annotation/actions.py tools/annotation/protocol.py tools/annotation/tests
git commit -m "feat: dispatch annotation actions through a shared protocol"
```

### Task 4: Add the pre-compile CLI and manifest materialization

**Files:**
- Create: `tools/annotation/cli.py`
- Create: `tools/annotation/config.py`
- Test: `tools/annotation/tests/test_cli.py`
- Test: `tools/annotation/tests/fixtures/registrations.json`

- [ ] **Step 1: Write failing end-to-end tests**

Use a fake compiler executable that emits `ast_valid.json`, a configuration
file registering the external fixture action, and a temporary output root.
Assert that the CLI writes the generated file and manifest, sorts records,
and returns non-zero without writing outputs when discovery or the action
fails. The test must also create a sentinel compile command and prove it is
not invoked by the framework after failure.

- [ ] **Step 2: Run CLI tests and verify the expected failure**

```sh
python3 -m unittest tools.annotation.tests.test_cli -v
```

Expected: missing CLI/configuration symbols.

- [ ] **Step 3: Implement configuration loading**

Load the JSON `actions` array, resolve command paths relative to the config
file or explicit workspace root, and convert entries to `ActionSpec`. Load
built-in registrations through a `register_builtins(registry)` hook before
configuration entries. Reject duplicate names across both sources.

- [ ] **Step 4: Implement CLI lifecycle**

Add arguments for `--compiler`, repeated `--flag`, repeated `--source`,
`--config`, `--output-root`, and `--manifest`. Execute registration validation,
AST discovery, deterministic grouping, action dispatch, atomic output writes,
and manifest writing in that order. Print diagnostics to stderr and return 1
for any failure; return 0 only after all outputs and the manifest are complete.

- [ ] **Step 5: Run all Python tests and commit**

```sh
python3 -m unittest discover -s tools/annotation/tests -v
git diff --check
git add tools/annotation
git commit -m "feat: add annotation processing command"
```

### Task 5: Integrate the tool as a GN pre-compile action

**Files:**
- Create: `tools/annotation/BUILD.gn`
- Modify: `tools/BUILD.gn`
- Create: `tools/annotation/tests/BUILD.gn`
- Create: `tools/annotation/tests/annotation_input.cpp`
- Create: `tools/annotation/tests/annotation_action_config.json`
- Create: `tools/annotation/tests/annotation_action.py`

- [ ] **Step 1: Write the GN smoke target definition test**

Add a test fixture that expects a generated output under `$root_build_dir`,
not in the source tree, and a manifest dependency on the annotation source.
Keep the fixture action a small external Python command using the stable JSON
protocol.

- [ ] **Step 2: Run GN generation before integration**

```sh
gn gen out/annotation-framework-smoke
```

Expected: the new target is absent or fails because its `action()` and test
group are not defined.

- [ ] **Step 3: Add GN action and test groups**

Define an `action("process")` that passes the selected compiler, flags, source
list, configuration, output root, and manifest to `tools/annotation/cli.py`.
Declare generated files and the manifest as action outputs. Add a `tests`
group under `tools/annotation` and expose it through `tools:tests` without
changing unrelated tool groups.

- [ ] **Step 4: Verify GN and the end-to-end target**

Run:

```sh
gn gen out/annotation-framework-smoke
ninja -C out/annotation-framework-smoke annotation-framework-tests
```

Expected: the generated artifact exists only under `out/`, the manifest lists
the source dependency, and the external action output is available to the
following compile/check step.

- [ ] **Step 5: Commit the GN integration slice**

```sh
git add tools/BUILD.gn tools/annotation/BUILD.gn tools/annotation/tests/BUILD.gn tools/annotation/tests
git commit -m "build: run annotation actions before compilation"
```

### Task 6: Final verification and handoff

- [ ] **Step 1: Run the complete focused test suite**

```sh
python3 -m unittest discover -s tools/annotation/tests -v
```

- [ ] **Step 2: Verify static quality and generated-file hygiene**

```sh
git diff --check
git status --short
```

Confirm that only the new annotation framework files are staged by this work;
the pre-existing `hsien` changes and unrelated worktree changes remain
untouched.

- [ ] **Step 3: Verify failure gating**

Run the failing discovery/action fixtures and confirm no compiler sentinel is
created and no generated output is left behind.

- [ ] **Step 4: Commit any test-only corrections separately**

```sh
git add tools/annotation
git commit -m "test: harden annotation framework failure coverage"
```
