# component-framework: the steps of PLAN.md as they land, one section per step

**Date**: 2026-10-04

## Delivered
- Step 7, "Manifest template, compiler and decoder": `system/component_manifest.gni`, `system/component_manifest.py` with `system/test_component_manifest.py`, and `system/lib/component` (format, decoder, gtest, fixtures). Change record: `docs/changes/261004/component.md`.

## Verification
Step 7:
- `python3 system/test_component_manifest.py`: 48 tests, OK.
- `out/bringup.x86/host/obj/system/lib/component/test/decl`: 15 tests, PASSED.
- `boot_check.py`: dbg PASS (24 passed, 1 skipped), rel PASS (22 passed, 3 skipped).
- The manual gn gen checks the plan asks for, each made on `system/lib/component/test/fixtures/BUILD.gn` and reverted:
  - a dangling child, `manifest = ":ghost.cm"` with no such target, fails gen:

    ```text
    ERROR Unresolved dependencies.
    //system/lib/component/test/fixtures:realm.cm(//build/toolchain/ours:kernel-x86)
      needs //system/lib/component/test/fixtures:ghost.cm(//build/toolchain/ours:user-x86)
    ```

  - a child label spelled with a toolchain fails the template's assert: "component_manifest(realm.cm): a child's `manifest` takes a plain label without a toolchain suffix";
  - a program label spelled with a toolchain fails the same way: "component_manifest(provider.cm): `program` takes a plain label without a toolchain suffix".
- Packaging, checked by hand and reverted: with `fixtures:realm.cm` in dev's `user_bootfs` deps, `bootfs.fini` held `meta/realm.cm`, `meta/provider.cm`, `meta/consumer.cm`, `meta/hollow.cm` and `bin/fixture_program`.

## Deviations from the design
Step 7: None. The compiler also refuses a label that names a toolchain in the JSON it reads, behind the template's assert, so that the Python unit test can cover the rule the design lists for it.

## Found along the way
Step 7: every `*_dist` action declares a depfile it never writes (`ninja -d explain`: "depfile ... is missing"), so ninja re-runs them and the image packs on every build. Not recorded in `docs/repairs/`: it belongs to the distribution templates, outside this task.

## Not done
Step 7: the router (step 8). No image packages a manifest until step 13.
