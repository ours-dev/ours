# init-lazy-storage: the root's server storages are built at run time, not written into init's image

**Date**: 2026-10-30

## Delivered
- Commit 1, `refactor(init): the resource servers' storage is built at run time, as the other servers' is`: the five `ResourceServer::Storage` globals of `system/service/init/init.cpp` are `constinit ustl::LazyInit<...>`, built by `main()` with `*X.init()`. The change record is the last section of `docs/changes/261029/init.md`; item 1 of `docs/repairs/261029/init.md` is deleted.
- Commit 2: the Launcher's pool, not yet done (this note is written in commit 1).

## Verification
- `prebuilt/ninja/ninja -C out/init-lazy` succeeds; `boot_check.py --out out/init-lazy`: dbg PASS, rel PASS (6/6 required, `routing_checks: passed`), 148 host tests passed.
- `init` before and after (`llvm-size -A`, stripped with `llvm-objcopy --strip-all`): `.data` 51,360 to 28,960, `.bss` 55,296 to 77,696, `.text` 108,878 to 109,166, stripped file 177,776 to 155,184 bytes (22,592 fewer). The five storages are type `d` before and `b` after. `.data` plus `.bss` is 106,656 in both: the RAM the root keeps does not change.
- Breakage: `DEBUG_RESOURCE_STORAGE.init()` left out makes the rel image fail (30 FAILED lines, 1/6 required, `init` terminated by a page fault at address 0); restored and `cmp`-ed against the saved copy.
- clang-tidy on `init.cpp` against `out/init-lazy`: no diagnostic. `scripts/format_cpp.py --check`: clean. `scripts/lint_docs.py --changed` on each document: no violations.

## Deviations from the design
None. There was no design document; the brief was the decision.

## Found along the way
- The comments in `system/service/init/resource_server.hpp` (on `ResourceConnection` and on `Storage`) still say the storage is "a constinit global". They are stale in that word only, and the file is another agent's to change; not edited here.

## Not done
- Sharing one set of message buffers among the five servers, or moving the buffers into their own globals: not needed once the storage is zeros in `.bss`, and the first changes who owns the buffers.
