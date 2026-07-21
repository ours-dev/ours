# Annotation Source Set Design

Date: 2026-07-21
Status: User-approved direction; implementation pending

## Goal

Provide one GN rule that combines pre-compile annotation processing with a
normal C++ `source_set`. Users should not declare an annotation action target
and then manually connect its generated files to another target.

## User Interface

```gn
annotation_source_set("schema") {
  sources = [
    "messages.cpp",
    "messages.hpp",
  ]

  annotation_config = "annotation-actions.json"
  generated_sources = [
    "generated/schema.cpp",
    "generated/schema.hpp",
  ]

  compiler = "clang++"
  flags = [ "-std=c++26" ]
  public_deps = [ "//some/dependency" ]
}
```

The public target is `schema`. The annotation processing target is an internal
implementation detail and has a stable generated-target name derived from the
public target.

## Expansion

The template creates two targets:

```text
annotation_source_set("schema")
    |
    +-- schema.annotation   (pre-compile action)
    |
    +-- schema              (ordinary source_set)
          +-- user sources
          +-- generated sources
          +-- dependency on schema.annotation
```

The action declares every generated source as an output. The source set names
the corresponding files below `$target_gen_dir` as sources and depends on the
action target. GN therefore schedules annotation processing before compiling
the source set and tracks generated files in the build graph.

## Interface Contract

Required template parameters:

- `sources`: source files passed to the scanner and compiled by the source set;
- `annotation_config`: JSON action registration file;
- `generated_sources`: relative generated source paths, declared both in GN
  and in the action configuration;
- `compiler`: compiler executable used for AST discovery;
- `flags`: compiler flags used by the discovery invocation.

Optional parameters mirror ordinary `source_set` properties where practical,
including `public_deps`, `deps`, `configs`, `public_configs`, `defines`, and
`include_dirs`. The template owns only the generated-source and ordering
behavior; it does not change global `source_set` semantics.

Generated paths must be relative, remain below the build output directory, and
not contain `..`. The template rejects a generated path that is not declared
by the action configuration.

## Failure Behavior

If discovery, configuration loading, action execution, generated output
validation, or manifest materialization fails, the action target fails and the
dependent source set is not compiled. No generated source is silently omitted.

The normal compiler flags and source list are passed to the discovery action so
that AST results match the following compilation as closely as the compiler
frontend allows.

## Testing

The implementation must verify:

- one `annotation_source_set()` declaration creates the public source-set
  target and hidden processing target;
- generated files are declared outputs and source-set inputs;
- the source set depends on the processing action;
- ordinary `source_set` properties are forwarded;
- action failure prevents generated-source compilation;
- undeclared or unsafe generated paths fail configuration;
- a minimal GN smoke target compiles a generated source.

## Non-Goals

This change does not redefine GN's built-in `source_set`, add runtime
reflection, or infer arbitrary generated outputs that are unknown to GN at
generation time.
