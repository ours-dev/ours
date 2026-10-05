# build-dist outstanding repairs

## 1. The ported dist templates set a metadata key nothing collects and point at documents this tree does not have

**Severity**
Low: no wrong output; dead metadata and comments that send a reader to templates and documents that do not exist here.

**Symptom**
Latent, cannot be triggered today. `resource()`, `resource_group()` and `resource_tree()` (`build/obi/resource.gni`), `generated_resource()` (`build/dist/generated_resource.gni`) and `renamed_binary()` (`build/dist/renamed_binary.gni`) each set `zbi_input_barrier` in their metadata, under the comments "Used by the zbi() template" and "Stop *_manifest() and zbi_test() from picking up files or zbi_input() items from the deps". No template in the tree has `zbi_input_barrier` among its `data_keys` or `walk_keys`, and there is no `zbi()`, `zbi_test()` or `zbi_input()`: the image is assembled by `obi()` and `obi_input()`, which walk `obi_input_args`. Separately, `build/dist/distribution_manifest.gni`, `build/dist/fini_manifest.gni`, `build/dist/renamed_binary.gni` (spelled `.mg` there) and `build/python/modules/distribution_manifest/distribution_manifest.py` refer the reader to `//docs/development/build/gn_concepts/internals/manifest_formats.md`, which does not exist in this tree.

**Cause**
The templates were ported close to verbatim, and the port kept the metadata of the image template they were written for. The comment sweep under the 2026-10-04 comment rule left these alone because they do not compare this build with another one; they claim a consumer and a document that are simply absent. Fixing them is a decision about code, not comment text: either the key goes, together with its comments, or `obi_input()` is taught to honour a barrier on `data_deps` and the key is renamed to one it walks. In `renamed_binary()` the second comment is also wrong on its own terms: that block sets no `distribution_entries_barrier`, so it stops no `*_manifest()` walk.

**Change**
Delete `zbi_input_barrier` and the two comments from all five metadata blocks, unless `obi()` should stop at a resource's `data_deps`, in which case add that barrier to `obi_input_args_barrier` handling instead and say so in the comment. Point the four documentation references at `//build/dist/distribution_schema.json`, which is the local description of the manifest format, and say in prose what a renaming entry is where `renamed_binary.gni` relied on the missing document.

**Reference**
The keys and documents come from Fuchsia's `//build/dist/resource.gni`, `//build/dist/generated_resource.gni`, `//build/dist/renamed_binary.gni` and `//build/zbi/zbi.gni` (`zbi()`, `zbi_input()`, `zbi_test()`), and from `//docs/development/build/gn_concepts/internals/manifest_formats.md` in the Fuchsia tree.

**Acceptance**
`grep -rn zbi build tests` finds nothing; `python3 build/dist/distribution_manifest_unittests.py` passes; `boot_check.py` builds and both images PASS; `tests/dist-smoke` still builds when generated.
