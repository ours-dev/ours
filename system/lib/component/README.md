# component

The compiled component manifest: its format, a decoder that checks its structure, and the router that walks a use to its source.

A component's manifest is a `component_manifest()` GN target (`//system/component_manifest.gni`). `//system/component_manifest.py` checks it with cmc's per-manifest rules and writes `meta/<name>.cm` into the BootFS; this library is what the root reads it back with. The design is `docs/design/261004/component-framework.md` ("The manifest", D1-D4, D26).

- `component/format.hpp`: the layout, a header, fixed-size record tables (Program, Arg, Child, Capability, Use, Offer, Expose) and one string table, with the enums and limits both halves share. Format version 3 gives a Capability a `path` and `rights` and an Expose a `subdir` and `rights`, the fields an Offer has, so that a directory can be declared, narrowed at an expose and routed to a provider (see "Directory capabilities" below); version 2 gave a Capability and a Use a `type` string, the protocol they speak, and an empty one means the entry's own name, so the decoded views always hold the name in that case.
- `component/decl.hpp`: `Decl::decode()`, which checks bounds, counts, string references, enum values, flags, rights, reserved fields and child indices, and nothing semantic (it refuses any version but 3), and the decoded records, whose strings are views of the manifest's bytes.
- `component/route.hpp`: `route()`, which walks one use of an instance through its parent's offers and its providers' exposes to a built-in or a providing instance, checking rights and joining subdirs at each hop ("The routing walk" in the design, D9). The use, an offer, an expose, the declaration of a directory and a built-in are the hops; the answer carries the declared path of a directory. It is a template over the `Instance` concept, which the root's instance record and the test's both satisfy; the lookups and the hop rules it is built from are plain functions in `src/route.cpp`. The comment on its declaration is the whole spec. P3 of `.agents/tasks/component-framework/PLAN.md` reserves it for the user to write by hand; the AI wrote it for the MVP on the user's instruction of 2026-10-04, and the exercise stays open.

## Protocol types

A name does not say which protocol is behind it, because an `offer` may rename a capability with `as`. A protocol capability, a protocol use and each of the root's protocol built-ins therefore carry a `type`; an `offer` and an `expose` pass a capability on and carry none. The rule is that a route must end at a capability of the type its use states, and it is checked twice: when the image is built, by `//system/component_manifest.py` over the whole tree of a manifest that has children (a route that does not resolve is left to run time), and on every Open, by `route()`, which answers `BadType` with the two types in `RouteError::want` and `got`. The design is `docs/design/261029/protocol-identity.md`.

## Directory capabilities

The format and the router carry a directory that a component provides; the rest of the system does not yet (`.agents/tasks/directory-capability/DESIGN.md`). A directory capability is declared with the `path` in its outgoing directory where it sits and the `rights` it allows at most, exposed with `rights` and a `subdir` that narrow it, offered and used as a built-in directory is. `route()` holds the rights of each hop against the hop before it, the declaration included, so a provider that allows less than an offer asks is `AccessDenied` at the provider (verb `Declare`) and an expose that passes on less than the hop before it asked is `AccessDenied` at the child (verb `Expose`). The route's `path` is the declared path, kept apart from the `subdir` the hops joined; the root will open the two together, which `route()` checks fit one Open. The decoder does not check that a directory declaration states a path and rights; the compiler will, and until it can write one every manifest leaves the new fields empty. A declaration or an expose that states no rights narrows nothing.

## Layout

```text
include/component/   public headers, reached as <component/...>
src/                 sources
test/                host-side tests, built with googletest
test/fixtures/       component_manifest() targets the tests decode and route through
```

## Building and testing

The library is `//system/lib/component`; its headers alone are `//system/lib/component:headers`. It makes no system call, so it builds for the host and for user programs alike.

The decoder test is `//system/lib/component/test:decl`, a host executable (`out/<dir>/host/obj/system/lib/component/test/decl`). It decodes the manifests the template built from `test/fixtures/`, one in the default toolchain and its children in the user toolchain, so the Python writer and the C++ reader are checked against each other. No fixture declares a directory yet, so the records that only a directory uses (a Capability's path and rights, an Expose's subdir and rights) are patched into a copy of `provider.cm`. The compiler's own tests are `python3 system/test_component_manifest.py`.

The router test is `//system/lib/component/test:route` (`out/<dir>/host/obj/system/lib/component/test/route`). It routes uses through three trees of fixture manifests, `realm.cm` and `top.cm`, and `mismatch.cm`, whose two routes end at the wrong protocol on purpose (built with `type_check = false`), with an instance tree of its own that resolves children on demand from a fixed pool of 16 instances, and breaks a few manifests on purpose for the rules no compiled manifest can show, among them a manifest that names itself as a child, whose walk ends when the pool is full. Its directory routes are the protocol route of leaf's `echo` in `top.cm`'s tree with each of its six records patched into a directory's and made to state what the test says.
