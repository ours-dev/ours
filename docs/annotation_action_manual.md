# Annotation Action Framework Manual

This manual is for users who want to attach C++ attributes to declarations and
run a generator, validator, or other build-time action before normal
compilation.

The framework is independent of any runtime library. An annotation action is a
plugin: the framework discovers attributes, validates them, invokes the plugin,
and exposes generated files and dependencies to the build system.

## Requirements

- Python 3;
- Clang with an AST frontend that preserves the attributes you register;
- a build output directory writable by the current user;
- a build system that can depend on generated files.

The framework uses the standard C++ attribute spelling:

```cpp
[[acme::generate_schema]]
struct Message {
  int id;
};
```

Attributes outside registered namespaces are ignored. An unknown attribute in
a registered namespace is an error.

Important: an ordinary compiler may warn and discard an unknown attribute
before emitting its AST. The framework detects this and fails with
`compiler did not preserve registered annotation`; it never silently reports
success with missing actions. Use a compiler frontend that supports the
attribute syntax you register.

## Command-Line Usage

Run the framework before the compile command:

```sh
python3 tools/annotation/cli.py \
  --compiler clang++ \
  --flag -std=c++26 \
  --flag -Iinclude \
  --source src/messages.cpp \
  --source-root . \
  --config build/annotation-actions.json \
  --output-root build/generated/annotations \
  --build-root build \
  --manifest build/generated/annotations/manifest.json
```

All `--source` options are scanned independently. Each source must be inside
`--source-root`; this keeps source identities and dependencies unambiguous.
Both `--output-root` and `--manifest` must be inside `--build-root`.

The process returns `0` only after discovery, all actions, output validation,
and file materialization succeed. A non-zero result means the normal compiler
must not start.

## Registering an External Action

Create a JSON configuration file:

```json
{
  "actions": [
    {
      "attribute": "acme::generate_schema",
      "declaration_kinds": ["record"],
      "schema_version": 1,
      "outputs": ["generated/schema.cpp", "generated/schema.hpp"],
      "command": ["python3", "generate_schema.py"]
    }
  ]
}
```

The command path is resolved relative to the configuration file when the file
exists there. `outputs` declares the files that the action may generate. Every
generated path returned by the action must be listed in `outputs` when this
field is present.

The action receives one JSON request on stdin and must write one JSON result to
stdout. Human-readable failures may be written to stderr.

Request:

```json
{
  "schema_version": 1,
  "action_name": "acme::generate_schema",
  "source_root": "/workspace/project",
  "annotations": [
    {
      "schema_version": 1,
      "canonical_name": "acme::generate_schema",
      "source_file": "src/messages.cpp",
      "line": 1,
      "column": 1,
      "declaration_kind": "record",
      "declaration_name": "Message",
      "qualified_name": "Message",
      "declaration_usr": null,
      "arguments": {}
    }
  ]
}
```

Result:

```json
{
  "schema_version": 1,
  "generated_files": [
    {
      "path": "generated/schema.cpp",
      "contents": "// generated\n"
    }
  ],
  "dependencies": ["src/messages.cpp"],
  "diagnostics": [],
  "exit_status": 0
}
```

Generated paths and dependency paths are relative. Absolute paths and `..`
path components are rejected. Duplicate generated paths are rejected. A
non-zero `exit_status`, invalid JSON, a schema mismatch, or an undeclared
output stops processing.

## Writing an Action

A minimal Python action can look like this:

```python
#!/usr/bin/env python3
import json
import sys

request = json.load(sys.stdin)
annotations = request["annotations"]

json.dump(
    {
        "schema_version": request["schema_version"],
        "generated_files": [
            {
                "path": "generated/schema.cpp",
                "contents": "// generated schema\n",
            }
        ],
        "dependencies": [item["source_file"] for item in annotations],
        "diagnostics": [],
        "exit_status": 0,
    },
    sys.stdout,
    sort_keys=True,
)
```

Actions should be deterministic: sort input records before generating output,
avoid modifying source files, and report all inputs that affect generated
files in `dependencies`.

## In-Process Python Actions

Embedding code can register a Python callback directly:

```python
from pathlib import Path

from tools.annotation.actions import dispatch
from tools.annotation.model import ActionRequest, ActionResult, ActionSpec


def generate(request: ActionRequest) -> ActionResult:
    return ActionResult.empty(request.schema_version)


spec = ActionSpec(
    canonical_name="acme::generate_schema",
    declaration_kinds=("record",),
    schema_version=1,
    callback=generate,
)
# request is an ActionRequest produced by the discovery stage.
result = dispatch(spec, request, Path("build/generated"))
```

An action must provide exactly one of `callback` or `command`. The standalone
CLI configuration format is intended for external commands; embedded users
can use `Registry.register(ActionSpec(...))` and call the same discovery and
dispatch APIs.

## GN Integration

Use the reusable GN template:

```gn
import("//tools/annotation/annotation_action.gni")

annotation_action("generate_schema") {
  compiler = "clang++"
  flags = [ "-std=c++26" ]
  source_root = "//"
  sources = [ "//src/messages.cpp" ]
  config = "//build/annotation-actions.json"
  generated_outputs = [
    "generated/schema.cpp",
    "generated/schema.hpp",
  ]
  manifest = "annotation-manifest.json"
  inputs = [ "//tools/generate_schema.py" ]
}

source_set("generated_schema") {
  sources = [
    "$target_gen_dir/generated/schema.cpp",
  ]
  public_deps = [ ":generate_schema" ]
}
```

The configuration action must declare the same generated paths in its
`outputs` array. The downstream target depends on the annotation action, so
generation completes before the generated source is compiled.

`generated_outputs` are declared GN outputs. If a plugin can generate a
variable set of files, expose a stable generated manifest or a fixed output
set through the build adapter rather than relying on undeclared files.

## Diagnostics and Failure Handling

The framework fails before normal compilation when it encounters:

- malformed configuration or duplicate registration;
- an unknown attribute in a registered namespace;
- a registered attribute discarded by the compiler frontend;
- an unsupported declaration kind or missing declaration identity;
- invalid action JSON or schema version;
- an action failure or non-zero exit status;
- an absolute, traversing, duplicate, or undeclared generated path;
- an output or manifest path outside the build root.

Successful action diagnostics are copied into the manifest. Generated files
are written only after all discovered action groups have completed
successfully.

## Troubleshooting

### The action is never invoked

Check that the attribute spelling exactly matches the registered `attribute`
value and that the declaration kind is listed in `declaration_kinds`. Then
inspect the compiler used by `--compiler`; a compiler that discards unknown
attributes will cause a fail-closed error.

### The compiler starts before generation

Make the normal compile target depend on the GN action target or its generated
source target. The framework CLI cannot create a build dependency by itself.

### A generated file is rejected

Use a relative path without `..`, place it below `--output-root`, and list it
in both the action configuration `outputs` array and GN `generated_outputs`.

### An included header is reported as a source annotation

Pass the real source file in `--source` and keep it under `--source-root`. The
scanner filters AST declarations by their source location and does not treat
included declarations as declarations from the including source.
