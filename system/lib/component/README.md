# component

The compiled component manifest: its format and a decoder that checks its structure.

A component's manifest is a `component_manifest()` GN target (`//system/component_manifest.gni`). `//system/component_manifest.py` checks it with cmc's per-manifest rules and writes `meta/<name>.cm` into the BootFS; this library is what the root reads it back with. The design is `docs/design/261004/component-framework.md` ("The manifest", D1-D4, D26).

- `component/format.hpp`: the layout, a header, fixed-size record tables (Program, Arg, Child, Capability, Use, Offer, Expose) and one string table, with the enums and limits both halves share.
- `component/decl.hpp`: `Decl::decode()`, which checks bounds, counts, string references, enum values, reserved fields and child indices, and nothing semantic, and the decoded records, whose strings are views of the manifest's bytes.

## Layout

```text
include/component/   public headers, reached as <component/...>
src/                 sources
test/                host-side tests, built with googletest
test/fixtures/       component_manifest() targets the tests decode
```

## Building and testing

The library is `//system/lib/component`; its headers alone are `//system/lib/component:headers`. It makes no system call, so it builds for the host and for user programs alike.

The decoder test is `//system/lib/component/test:decl`, a host executable (`out/<dir>/host/obj/system/lib/component/test/decl`). It decodes the manifests the template built from `test/fixtures/`, one in the default toolchain and its children in the user toolchain, so the Python writer and the C++ reader are checked against each other. The compiler's own tests are `python3 system/test_component_manifest.py`.
