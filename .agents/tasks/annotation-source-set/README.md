# Annotation Source Set

Status: design approved; implementation pending.

## Objective

Expose annotation processing as one GN target rule. A caller declares an
`annotation_source_set()` and receives a normal `source_set` whose generated
sources are produced before compilation.

## Subtasks

1. Define the GN template interface and target expansion.
2. Add focused GN generation and dependency tests.
3. Update the third-party manual with the new interface.
4. Run direct CLI and GN verification where the toolchain is available.
