# x86-mm changes

## Title: largepage.test destroys an address space with a 2 MiB leaf still mapped, and checks the table set aside for it comes back (2026-10-27)

**Files**
- `kernel/commands/large_page_test.cpp` — before the first address space is destroyed, a 2 MiB leaf is mapped and left there; the last check now says its table is back too
- `docs/repairs/261026/x86-mm.md` — removed: its one item is this change

**Symptom**
Latent, cannot be triggered today: no kernel test reached the code that gives back the table set aside for a 2 MiB leaf still mapped when an address space's page tables go. With `derived().discard_all_tables(child)` taken out of `X86PageTableImpl::free_below()` (2026-10-26, step 5 of `.agents/tasks/x86-page-table-crtp/PLAN.md`), both images booted and `largepage.test` passed; only the host test `FreeTables.GivesEveryTableBackAndCountsTheLeaves` failed. A teardown that leaked one table per leaf would have passed every kernel test.

**Cause**
An address space goes in two steps, and the leaves are almost always gone after the first:

- `VmAspace::destroy()` destroys the root VMA, which unmaps every mapping it holds. A 2 MiB leaf unmapped whole gives its table back right there, through the unmap's `discard_table()`.
- The page tables themselves go later, from `~VmAspace`, through `ArchVmAspace::destroy()`, `PageTable::destroy()` and `free_tables()`. Only then does `free_below()` meet whatever leaves are left, and only there does `discard_all_tables()` give back their tables.

So a leaf reaches `free_below()` only if no mapping holds it: one mapped straight through the arch address space. `largepage.test` did map its first leaves that way, but unmapped them all before destroying the address space, and `check_mappings()` leaves leaves mapped, but through VmMappings, which the first step unmaps. Neither ever had a leaf left for the second step.

A table set aside is chained from the frame descriptor of the 2 MiB-level table that holds the leaf (`MmuBody::deposits`). Freeing that table without `discard_all_tables()` frees only the table: the chain goes with the descriptor's role, and the tables on it are never freed.

**Change**
Before, the first address space is destroyed with nothing mapped:

```cpp
                    "a leaf unmapped whole gives its table back");

            verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");
        }
        verdict(tally, mm::PageTableDeposits::nr_held() == held_before,
                "with the address space gone, every table set aside is back");
```

After, a leaf is mapped through the arch address space, so that no VmMapping unmaps it, and left for the teardown:

```cpp
            auto const left =
                arch.map(next, pa, LARGE / PAGE_SIZE, rw,
                         mm::ArchMapOptions::ErrorIfExisting | mm::ArchMapOptions::UndoOnFailure,
                         nullptr) == Status::Ok;
            auto const left_leaf = translate(*aspace, next);
            verdict(tally,
                    left && left_leaf.present && left_leaf.size == LARGE &&
                        mm::PageTableDeposits::nr_held() == held_before + 1,
                    "a leaf is left mapped for the teardown, with its table set aside");

            verdict(tally, aspace->destroy() == Status::Ok, "the address space is destroyed");
        }
        verdict(tally, mm::PageTableDeposits::nr_held() == held_before,
                "with the address space gone, every table set aside is back, the one of the "
                "leaf left mapped too");
```

`~VmAspace` reports the leaf it found, as it should for any leaf left behind: "mm: aspace 'largepage.test' still had 1 pages mapped when its tables went". In this test that line is expected.

Rejected: a second address space of its own for the leaf. The first one is already at the right point (every earlier leaf unmapped, the count back to `held_before`), and the existing last check is the one that has to see the table come back.

**Reference**
The deposits follow Linux's THP page-table deposit (decision D1 of `.agents/tasks/large-pages`). Linux gives a huge PMD's deposited table back in `zap_huge_pmd()` through `zap_deposited_table()` (`mm/huge_memory.c`), and checks at the end that no page-table bytes are left in `check_mm()` (`kernel/fork.c`, "non-zero pgtables_bytes on freeing mm"), which is what the `nr_held()` check here does for one test. In Linux every huge PMD belongs to a VMA, so exit always reaches it through the zap; here the teardown of the tables has to give it back itself, because it frees the tables apart from the mappings.

**Verification**
- `boot_check.py`: dbg PASS (44 passed, 4 skipped), rel PASS (41 passed, 8 skipped, 6/6 required); host tests 147 passed.
- `kshell_run.py largepage.test` on the dbg image: ok, 28 checks (27 before).
- Control: with `discard_all_tables(child)` taken out of `free_below()`, the dbg kernel panics in `free_page_table()` on `DEBUG_ASSERT` "freeing the page table 0x324000 with tables still set aside for it" (`kernel/mm/pmm.cpp`), right after "the address space is destroyed", and `largepage.test` fails. Restored, it passes. In a build without that assertion the `nr_held()` check would be what fails; not run, since only the dbg image has a kernel shell and it has the assertion.
- clang-tidy clean on `large_page_test.cpp`; clang-format clean on the changed lines.

**Deliberately not done**
- The warning of `~VmAspace` counts leaves and says pages: a 2 MiB leaf is reported as "1 pages". `kernel/mm/vm_aspace.cpp` holds another session's uncommitted change, so it was not touched here.
- No test reaches a 1 GiB leaf at teardown: user address spaces never map one (D3 of `.agents/tasks/large-pages`), so there is no table set aside for one to lose.

**How it was found**
The control of step 5 of `.agents/tasks/x86-page-table-crtp/PLAN.md` (2026-10-26) took `discard_all_tables()` out and found no kernel test failing. Reading `VmAspace::destroy()` (the root VMA's `destroy()` unmaps every mapping) and `~VmAspace` (only then `ArchVmAspace::destroy()`) showed why no leaf survived to the teardown.

**The user's prediction**
None (homework paused).

**Three questions you should be able to answer**
- Why does destroying an address space whose 2 MiB leaves are all mapped through VmMappings never reach `discard_all_tables()`?
- What catches a leaked table set aside in the dbg image before the test's own check does, and why does the test keep the `nr_held()` check anyway?
- Why must a table set aside be freed before the table that holds the leaf, and not after?
