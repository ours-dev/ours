# build outstanding repairs

## 1. A second ninja run after a full build redoes the image packaging, because the `*_dist` actions' depfiles are missing

**Severity**
Low: nothing is wrong in the output, and the cost is build time (the packaging steps only, not compiles). It slows every edit-build-boot loop and hides real "nothing to do" answers.

**Symptom**
Right after a full build, `prebuilt/ninja/ninja -C out/bringup.x86 -n` still lists 38 steps, the image packaging actions (`_obi.dev.pack`, `_obi.dev.dbg_1.pack`, `_obi.fake.pack` and the dist steps before them). `-d explain` gives the reason for the first of them: `depfile 'gen/kernel/products/image/dev.dbg_1/bootfs_dist.depfile' is missing`, and the same for `cmdline_dist` and `ours_dist`. Seen on 2026-10-29 in `.worktrees/pi-runtime` at commit `ce7385e3`, which has none of the protocol-identity build checks, so they did not cause it; the agent that built the build-time check saw it too (36 steps).

**Cause**
Not investigated. The `*_dist` actions come from `//build/dist/distribution_manifest.gni`, which sets `depfile = "$target_gen_dir/${main_target}.depfile"` (line 135) and says the script reports its inputs through `--depfile`. Ninja is saying that file does not exist after the action ran, so either the script does not write it on some path, or it writes it somewhere else, or it is removed afterwards. Whichever it is, an action whose declared depfile is absent is always dirty.

**Change**
Find out why the file is absent, starting at the script that action runs and the `--depfile` argument it is given (compare the path in the args with `depfile =`). Fix the writer; do not drop the `depfile`, which is what makes the action rerun when a referenced file changes. Measure first: time a second `ninja` run before and after.

**Reference**
None, this project's own build.

**Acceptance**
After a full build, `prebuilt/ninja/ninja -C out/bringup.x86 -n` prints `ninja: no work to do.`, and changing one source file listed in a distribution manifest still reruns the packaging.
