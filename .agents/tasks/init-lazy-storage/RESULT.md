# init-lazy-storage: the root's server storages are built at run time, not written into init's image

**Date**: 2026-10-30

## Delivered
- Commit 1, `81e87dcb` `refactor(init): the resource servers' storage is built at run time, as the other servers' is`: the five `ResourceServer::Storage` globals of `system/service/init/init.cpp` are `constinit ustl::LazyInit<...>`, built by `main()` with `*X.init()`. Its change record is the first of the two sections appended to `docs/changes/261029/init.md`; item 1 of `docs/repairs/261029/init.md` is deleted.
- Commit 2, `refactor(init): the Launcher's storage is built at run time`: `LAUNCHER_STORAGE` the same way, and its `[[clang::no_destroy]]` gone. The second section of `docs/changes/261029/init.md`; item 9 of `docs/repairs/261004/init.md` rewritten for the pool of four, which remains.

## Verification
- `prebuilt/ninja/ninja -C out/init-lazy` succeeds; `boot_check.py --out out/init-lazy` after each commit: dbg PASS, rel PASS (6/6 required, `routing_checks: passed`), 148 host tests passed.
- `init` at `a8489d18`, after commit 1 and after commit 2 (`llvm-size -A`, stripped with `llvm-objcopy --strip-all`): `.data` 51,360, 28,960, none; `.bss` 55,296, 77,696, 106,656; `.text` 108,878, 109,166, 109,502; stripped file 177,776, 155,184, 126,208 bytes. All six storages are type `d` at `a8489d18` and `b` after their commit. `.data` plus `.bss` is 106,656 in all three: the RAM the root keeps does not change.
- Breakage, each followed by `cmp` against the saved copy: `DEBUG_RESOURCE_STORAGE.init()` left out makes the rel image fail (30 FAILED lines, 1/6 required, `init` terminated by a page fault at address 0); `LAUNCHER_STORAGE.init()` left out the same (11 FAILED lines, 4/6 required). The dbg image passes either way, since its boot uses neither the resource protocols nor the Launcher service.
- clang-tidy on `init.cpp` against `out/init-lazy`: no diagnostic, after each commit. `scripts/format_cpp.py --check`: clean. `scripts/lint_docs.py --changed` on each document: no violations.

## Deviations from the design
Item 9 of `docs/repairs/261004/init.md` was rewritten, not deleted as the brief said: it also records the capacity of four connections, which this does not change. The user can delete what is left.

## Found along the way
- The comments in `system/service/init/resource_server.hpp` (on `ResourceConnection` and on `Storage`) and in `launcher_server.hpp` (on `LauncherConnection` and on `Storage`) still say the storage is a `constinit` global, and the Launcher's also says `[[clang::no_destroy]]` and that the constructor is `constexpr` so that the pool can be wired at compile time. They are stale in those words only. The files are another agent's to change; not edited here.
- A storage left unbuilt fails only the rel image: the dbg boot does not reach the resource protocols or the Launcher service. The rel image is the one that checks a change here.

## Not done
- Sharing one set of message buffers among the servers, resizing the Launcher's pool of four, or moving the buffers into globals of their own: not needed once the storages are zeros in `.bss`, and the first changes who owns the buffers. The pool of four stays in `docs/repairs/261004/init.md` item 9.
