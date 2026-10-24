# large-pages: a large-page VMO's user mappings use 2 MiB leaves, split without allocating

**Date**: 2026-10-24

## Delivered
- `89d4a25f`, `18db9938` docs: the design and plan, and decisions D1 to D8, all as recommended.
- `8a80d0ec` feat(arch): the paging engine maps a 2 MiB leaf with a table set aside by its caller, splits it into that table when an unmap or protect covers part of it, and gives the table back on a whole-leaf unmap and at teardown (step 1). Record: `docs/changes/261024/arch-paging.md`.
- `d299416d` feat(mm): a user address space sets a table aside for each 2 MiB leaf, kept in the descriptor of the PD table that holds the leaf (`page::MmuBody`), and the kernel's is unchanged (step 2, D4, D5). Record: `docs/changes/261024/mm.md`.
- `904ab674` feat(mm): a VMO with `LargePages` fills a hole with the 2 MiB block it lies in, an order-9 allocation made order-0 frames at once (step 3, D2). Record: `docs/changes/261024/mm.md`.
- `8a7bff57` feat(mm): a fault or a populate maps a whole block as one 2 MiB leaf when the window is inside the mapping, has one protection, has no table under it, and is one contiguous block of the node that holds it; a clone's read maps it read-only and a write splits it and copies one page (step 4, D7). Record: `docs/changes/261024/mm.md`.
- `a0d1f8d8` feat(mm): a large-page object's mapping placed by the VMM starts where its object offset falls in a block (step 5, D6). Record: `docs/changes/261024/mm.md`.
- `6f4ae96a` feat(arch): `arch::machine_kind()` tells bare metal, a hypervisor and an emulator apart; `cpu status` prints it (step 6a). Record: `docs/changes/261024/arch.md`.
- `dab2feb3` feat(benches): `largepage.bench` and the `bench::Run` convention, under which a metric that needs the processor's TLB is skipped on an emulator (step 6b). Record: `docs/changes/261024/benches.md`.

## Verification
- `x86-paging-smoke`, host: 14 checks, 7 of them for leaves and tables set aside, against a pool that stands in for the allocator.
- `largepage.test`, dbg image: 25 checks, from the engine's leaf through blocks, faults, populate, protect, a clone's shared block and its copy-on-write split, to the VMM's placement.
- Control runs, each with one piece taken out, at every step: no `SplitsLargeLeaves` (5 of 8 fail), no block fill (3 of 13), frames left order 9 (2 of 13), no leaf (5), no owner check (exactly the clone's half-taken window), no empty-entry check (the split window), no placement phase (offset 0). Restored, all pass.
- `largepage.bench`, QEMU TCG, 64 MiB: faults 16384 against 32; first touch 184 to 294 ms against 37 to 40 ms; page tables 32 either way; one page touched per 2 MiB commits 128 KiB against 64 MiB; random reads 246 ns against 244 ns, which TCG cannot tell apart and so are skipped without `all`.
- `vmosnapshot.test` (223), `pagesource.test` (48), `usercopy.test`, `vmaseal.test`, `vmaundo.test`, `vmawx.test` pass throughout.
- `ours-verify` at the last step: dbg PASS (43 passed, 4 skipped), rel PASS (40 passed, 8 skipped, 6/6 required), 146 host tests pass.
- Not measured: the TLB benefit, the reason large pages exist. There is no bare-metal or KVM x86 machine here.
- Not exercised: a block fill falling back to one page when the allocator has no 2 MiB block, which a test cannot force.

## Deviations from the design
- Step 1's host tests went into `x86_paging_smoke.cpp`, not `paging_test.cpp`, which no build compiles.
- D4 named a FIFO, as Linux keeps it; the chain is LIFO. Any table set aside with an owner serves any of its leaves, so the order is immaterial, and a singly linked chain needs no tail.
- Step 3: nothing records the block. Its frames become order-0 frames as soon as it is allocated, each freed alone and merged back by the buddy. The leaf path proves a window is a block by its frames instead (`find_block_locked()`: one owner, contiguous, 2 MiB aligned). The user agreed on 2026-10-24.
- Step 6: the design's random and sequential reads over 256 MiB, with TLB misses, were not possible under TCG. The bench measures what any machine gives exactly, over 64 MiB, and keeps random reads as a hardware-only metric. D2 stays the flag until there are hardware numbers (the user's call, 2026-10-24).

## Found along the way
- `free_frames()` counts the buddy allocator only, not the per-CPU frame caches, so a difference of two readings moves in batches. That is fine for a diagnostic, but it is no measure of a few pages; the bench counts tables from the page table instead. Not a defect, so not in `docs/repairs/`.
- `kernel/lib/arch/test/paging_test.cpp` is stale and unbuilt, noted in `docs/changes/261024/arch-paging.md`.

## Not done
- Large pages for every eligible fault (D2), and offering the flag to programs through `vmo_create`: both wait for TLB numbers from bare metal or KVM.
- Putting split pages back into a leaf (D8), to be weighed against those numbers, with D7's one-page copy.
- 1 GiB user leaves (D3) and large leaves in the kernel address space beyond boot's (D5).
- Blocks in a clone, a node with a page source, or a hidden node: only a VMO's own root takes them.
- `take_pages()` of part of a block: `.agents/tasks/user-pager/`, which resumes at its step 4.2.
