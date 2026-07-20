# Annotation Action Framework V1 Design

Date: 2026-07-20
Status: Draft for user review

## Goal

Build an independent build-time annotation framework. The framework scans C++
source files before their normal compilation, discovers attributes written in
the standard attribute form, and dispatches structured annotation records to
registered actions. Annotation actions are plugins: the framework owns
discovery, validation, scheduling, diagnostics, and build integration; plugins
own annotation-specific behavior.

Example input:

```cpp
[[ours::generate_serializer]]
struct Message {};
```

The framework must be useful without depending on the semantics of any one
plugin or subproject.

## V1 Decisions

- The source syntax is `[[namespace::attribute]]`.
- Discovery happens before the normal compile action for the affected source.
- The first discovery backend is Clang AST JSON. Source-text regular
  expressions are not used for semantic discovery.
- The framework exposes a registration API for in-process actions.
- External actions can be registered by configuration and run as child
  processes.
- Both action kinds consume and produce the same structured protocol.
- An action may emit generated source, metadata, dependency files, and build
  diagnostics.
- The framework fails closed on malformed registrations, invalid protocol
  messages, unknown attributes in registered namespaces, and action failures.
  Attributes outside registered namespaces are ignored so unrelated compiler
  extensions can coexist in the same source.
- Generated outputs are written below the build output directory, never beside
  user source files by default.
- The framework does not perform runtime reflection or execute generated code.

## Architecture

```text
Build rule
    |
    v
Annotation CLI
    |
    +-- Clang AST discovery backend
    |
    +-- annotation registry
    |       +-- built-in/in-process actions
    |       +-- configured external actions
    |
    +-- action scheduler
    |
    v
ActionResult set and dependency manifest
    |
    v
Normal compilation
```

The framework is divided into five stable units:

1. **Discovery** invokes Clang with the same source, include paths, defines,
   language mode, and relevant compiler flags as the real compile, then walks
   the AST and returns source-located declarations and attributes.
2. **Registry** maps a canonical attribute name to one action descriptor. It
   rejects duplicate registrations and records the accepted declaration kinds
   and schema version.
3. **Scheduler** groups records by action, orders them deterministically, and
   invokes actions. Independent action groups may be parallelized later; V1
   executes them deterministically in one process.
4. **Action bridge** invokes either an in-process callback or an external
   command using the common request/response schema.
5. **Build adapter** materializes outputs and dependencies for GN or another
   build system. The core does not hard-code a single build generator.

## Structured Contracts

Every discovered attribute is normalized to an `AnnotationInfo` record:

```text
AnnotationInfo {
  schema_version
  canonical_name       // e.g. "ours::generate_serializer"
  source_file           // normalized path
  line
  column
  declaration_kind      // record, function, enum, namespace, ...
  declaration_name
  qualified_name
  declaration_usr       // stable identity when Clang provides one
  arguments             // structured attribute arguments
}
```

The action request contains the action name, tool invocation context, and an
ordered list of `AnnotationInfo` records. An action returns:

```text
ActionResult {
  generated_files[]     // path, contents or producer description
  dependencies[]        // files that affect the result
  diagnostics[]         // severity, message, source location
  exit_status
}
```

The wire representation is JSON. JSON is an interchange format only; the
framework validates all required fields before dispatch and rejects unknown
schema versions. Generated file paths must remain inside the configured build
output root.

## Registration

The in-process API is conceptually:

```text
register_action(
  canonical_attribute_name,
  accepted_declaration_kinds,
  action_schema_version,
  action_callback
)
```

The callback receives an immutable request and returns an `ActionResult`.
Callbacks must not mutate source files or global registry state during
execution. Registration occurs during tool startup, before discovery.

External actions use configuration with equivalent fields:

```json
{
  "actions": [
    {
      "attribute": "ours::generate_serializer",
      "declaration_kinds": ["record"],
      "schema_version": 1,
      "command": ["tools/generate_serializer"]
    }
  ]
}
```

Configuration paths are resolved relative to the configuration file or an
explicit workspace root. The registry applies the same duplicate and schema
validation to both registration sources.

## Lifecycle and Failure Semantics

1. Load built-in registrations and configuration registrations.
2. Validate registrations before invoking Clang.
3. Run discovery for each source and normalize paths and declaration data.
4. Reject unsupported or malformed registered attributes with source-located
   diagnostics; ignore attributes outside registered namespaces.
5. Sort records by normalized source path, line, column, and canonical name.
6. Dispatch each action with a deterministic request.
7. Validate returned results and write generated outputs atomically.
8. Emit a build manifest containing outputs, dependencies, and diagnostics.
9. Return a non-zero status if discovery, validation, an action, or output
   materialization fails; normal compilation must not start in that case.

An action failure is isolated to the current framework invocation. V1 does not
silently skip an annotated declaration, retry actions, or partially continue
to compilation.

## Build Integration

The initial build adapter is a pre-compile action that receives the real compile
arguments and source list. It writes generated sources and a dependency
manifest under the build directory. The build graph declares generated files
as inputs to the normal compile target, ensuring that annotation processing
completes first and changes invalidate the correct targets.

The framework CLI remains usable outside GN. A standalone invocation accepts
the compiler, compiler flags, source files, registrations, output root, and
manifest path. This makes the core testable without a full kernel build.

## Testing

V1 must include:

- AST fixtures for valid attributes, nested declarations, multiple attributes,
  and unsupported declaration kinds;
- registry tests for built-in and configuration registrations, duplicate names,
  and schema mismatches;
- protocol tests for valid results, malformed JSON, invalid paths, and failed
  actions;
- deterministic ordering tests;
- an end-to-end pre-compile test proving generated output is available to the
  subsequent compiler invocation;
- a failure test proving the compiler is not started after a discovery or
  action failure;
- a standalone freestanding-compatible smoke check for the framework's build
  integration code where applicable.

The tests must not use or depend on `hsien`.

## Explicit Non-Goals

V1 does not include runtime annotation reflection, C++ module integration,
parallel action execution, remote action caching, semantic source rewriting,
or a plugin marketplace/distribution mechanism.
