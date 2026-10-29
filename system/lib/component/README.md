# component

The compiled component manifest: its format, a decoder that checks its structure, and the router that walks a use to its source.

A component's manifest is a `component_manifest()` GN target (`//system/component_manifest.gni`). `//system/component_manifest.py` checks it with cmc's per-manifest rules and writes `meta/<name>.cm` into the BootFS; this library is what the root reads it back with. The design is `docs/design/261004/component-framework.md` ("The manifest", D1-D4, D26).

- `component/format.hpp`: the layout, a header, fixed-size record tables (Program, Arg, Child, Capability, Use, Offer, Expose) and one string table, with the enums and limits both halves share.
- `component/decl.hpp`: `Decl::decode()`, which checks bounds, counts, string references, enum values, reserved fields and child indices, and nothing semantic, and the decoded records, whose strings are views of the manifest's bytes.
- `component/route.hpp`: `route()`, which walks one use of an instance through its parent's offers and its providers' exposes to a built-in or a providing instance, checking rights and joining subdirs at each hop ("The routing walk" in the design, D9). It is a template over the `Instance` concept, which the root's instance record and the test's both satisfy; the lookups and the hop rules it is built from are plain functions in `src/route.cpp`. The comment on its declaration is the whole spec. P3 of `.agents/tasks/component-framework/PLAN.md` reserves it for the user to write by hand; the AI wrote it for the MVP on the user's instruction of 2026-10-04, and the exercise stays open.

## Layout

```text
include/component/   public headers, reached as <component/...>
src/                 sources
test/                host-side tests, built with googletest
test/fixtures/       component_manifest() targets the tests decode and route through
```

## Building and testing

The library is `//system/lib/component`; its headers alone are `//system/lib/component:headers`. It makes no system call, so it builds for the host and for user programs alike.

The decoder test is `//system/lib/component/test:decl`, a host executable (`out/<dir>/host/obj/system/lib/component/test/decl`). It decodes the manifests the template built from `test/fixtures/`, one in the default toolchain and its children in the user toolchain, so the Python writer and the C++ reader are checked against each other. The compiler's own tests are `python3 system/test_component_manifest.py`.

The router test is `//system/lib/component/test:route` (`out/<dir>/host/obj/system/lib/component/test/route`). It routes uses through three trees of fixture manifests, `realm.cm` and `top.cm`, and `mismatch.cm`, whose two routes end at the wrong protocol on purpose (built with `type_check = false`), with an instance tree of its own that resolves children on demand from a fixed pool of 16 instances, and breaks a few manifests on purpose for the rules no compiled manifest can show, among them a manifest that names itself as a child, whose walk ends when the pool is full.
