# Naming

The reference order for choosing names: **Rust first, Zircon second**
(2026-11-04). When a concept needs a name, look at what Rust's standard
library and ecosystem call it before looking at Zircon. Examples that
followed it: the address object is `Vma` at every layer (Rust-idiomatic
short type name), not Zircon's `Vmar`; the two-way call pipeline is
`rpc::Channel::call`, not `channel_call`.

- Core types: short UpperCamelCase nouns (`Channel`, `Mount`, `Tree`).
- Methods on types, snake_case; conversions `as_`/`into_`/`to_`;
  constructors `new` or the type's own name; predicates `is_`.
- Wire-protocol names keep the platform family prefix (`ours.block.Block`)
  regardless of the C++ namespace — the binding namespace does not
  determine the wire name.
- Hardware and firmware nouns (pin, line, GSI, INTID, NSID) come from their
  specs, not application-architecture suffixes; `DescTable` is not a Store.
- The C ABI is exempt: vDSO symbols are `ours_<name>` and cannot follow
  Rust spelling.

A name earns its keep by the delete test: if the helper were deleted and
callers re-duplicated its checks, the abstraction earns its name.
