## process_start，句柄从调用者转进新进程；内核 launch 也走同一条路（K23a-1 第 3 步，2026-09-29）

**文件**
- `kernel/object/include/ours/object/process_dispatcher.hpp`、`process_dispatcher.cpp`：新增 `ProcessDispatcher::start()`；`attach_thread()` 多一个参数 `first_thread`，不再靠“线程表是不是空的”推断
- `kernel/object/include/ours/object/thread_dispatcher.hpp`、`thread_dispatcher.cpp`：`start()` 多一个参数 `first_thread`
- `kernel/lib/syscall/src/process.cpp`：`syscall::process_start()`
- `system/public/ours/syscalls.gni`：`process_start` 条目（`arg1` 标 `release`）
- `system/lib/ou/include/ou/process.hpp`、`src/process.cpp`：`ou::process_start()`
- `kernel/lib/userabi/src/userboot.cpp`：`launch()` 改用 `ProcessDispatcher::start()`
- `examples/shell/shell.cpp`：`check_process_start()`
- `docs/design/260924/userspace-filesystem.md`：K23 行

**症状**
用户态没法启动一个进程；而内核自己的 `launch()` 是手工把句柄加进新进程的表、再直接调 `ThreadDispatcher::start()`，
和将来 launcher 走的路径不是同一条，前者的失败路径也没有把句柄收拾干净。

**原因**
“把一个句柄从调用者的表转进新进程的表，再启动线程”是一个整体，里面有三个不能弄错的顺序：
消耗、放入、启动。

**改法**

```cpp
auto process_start(process, thread, entry, stack, arg1, arg2) -> Status {
    // 1. 进程和线程先取出（各需要 Write），持有引用
    // 2. arg1 再从调用者的表里摘下：从这里起成败都被消耗；先取再摘，所以 arg1 可以就是 thread 的句柄
    // 3. 线程必须属于这个进程；arg1 需要 Transfer
    return target->start(*thread, entry, stack, move(arg1), arg2);
}

auto ProcessDispatcher::start(thread, entry, stack, Box<Handle> arg1, arg2) -> Status {
    if (m_vdso_code_size == 0) { return BadState; }                 // 进程一个系统调用也发不了，不启动
    arg1_value = m_handle_table->add(move(arg1));                    // 先放进新进程的表
    status = thread.start({entry, stack, arg1_value, arg2}, /*first*/ true);  // 再启动
    if (status != Ok && arg1_value != HANDLE_INVALID) { m_handle_table->remove(arg1_value); }  // 失败：按值取出并关闭
    return status;
}
```

- **为什么先放句柄、后启动线程**：线程一旦可运行，就可能在另一个 CPU 上立刻运行，它做的第一件事可能就是用 rdi 里的句柄值。
  先启动、后放入，那个值在表里对应的东西就是“还没有”，程序能不能正常运行取决于两边谁快：这样的顺序本身就是错的，
  不管这个窗口平时多短。放在前面，新进程还没有线程，没有人能通过这张表访问到它，所以表里什么都不缺，也没有人能提前看到这个句柄。
  （值本身并不是“未定义”：它是句柄和目标表的纯函数，放进去之前就能算出来；缺的是表里的那一项。）
- **失败时谁关掉这个句柄**：`ProcessDispatcher::start()` 自己。句柄在调用者那里已经被消耗，调用者不可能再关它；
  如果不在这里收拾，它就会一直留在新进程的表里，直到那个进程死掉。而失败后的进程还是 Initial，调用者完全可以带着新的句柄重试，
  旧的那个就成了泄漏。所以失败路径要**按值**取出并关闭，而且放在进程锁之外（`attach_thread()` 已经放了锁），
  因为关闭一个句柄可能触发对象的 `on_zero_handles()`。
- **并发地转 Dead 怎么办**：进程被并发地转成 Dead 时，`finish_dead_transition()` 会清空这张表，
  失败路径的 `remove` 找不到那个句柄，返回空，什么也不做。表的锁决定谁取到句柄，只会有一方关闭它。
- **`first_thread` 显式传入**：原来 `attach_thread()` 用“线程表是空的”推断这是第一个线程。这样两个人同时以“第一个线程”启动时，
  输的那个会被当成普通的后续线程，在已经 Running 的进程上启动成功。现在“第一个”由调用者（只有 `ProcessDispatcher::start()`）明确声明，
  状态必须与之一致，`DEBUG_ASSERT` 里再确认“线程表为空”等价于“Initial”。
- **错误顺序**：先是被点名的句柄（BadHandle、BadType、缺 Write 是 AccessDenied），再是线程属于该进程（AccessDenied），
  然后是 `arg1`（BadHandle、缺 Transfer 是 AccessDenied），最后才是 `start()` 的结果。缺 vDSO 的 BadState 排在入口地址的 InvArgs 之前。
- **内核的 `launch()` 走同一条路**：`process.dispatcher()->start(...)`，参数和用户态 launcher 一样，
  以前手工 `handle_table().add(...)` 再直接调 `ThreadDispatcher::start()` 的做法删掉了。

**参照**
Zircon `sys_process_start()`：进程、线程各要 `ZX_RIGHT_WRITE`，线程必须属于该进程（否则 `ZX_ERR_ACCESS_DENIED`），
句柄需要 `ZX_RIGHT_TRANSFER`；`ProcessDispatcher::Start()`：状态必须是 INITIAL，把句柄加进新进程的表，再启动线程。

**验证**
- 两个镜像均构建通过（含 vDSO ABI 检查）；`boot_check.py` 两个镜像都 PASS（dbg 17 passed；rel 13 passed、8 skipped）；
  两个镜像的 `usercopy.test` 都是 18/18、`pmnode.test` 都是 20/20。
  内核启动 userboot、init、shell 和各探针都是走新的 `ProcessDispatcher::start()`，说明成功路径是通的。
- dbg_1 的 shell 打出 `process_start checks passed`。它启动的进程都没有映射 vDSO（`vma_map` 还不登记它，K23a-2），
  所以所有格式正确的调用都回 BadState，正好可以专门测失败路径：
  - 句柄无效回 BadHandle；进程的位置传 VMA、线程的位置传进程回 BadType；进程句柄没有 Write、线程属于另一个进程，回 AccessDenied；
  - `arg1` 指向不存在的句柄回 BadHandle；`arg1` 没有 Transfer 回 AccessDenied，而且句柄已经不在调用者的表里（再关它回 BadHandle）；
  - 把 channel 的一端作为 `arg1` 传入，启动失败（BadState）之后，另一端读到 `PEER_CLOSED`：说明那个句柄被关掉了，
    没有留在新进程的表里。连做两次，结果相同：进程与原来一样；
  - `arg1` 传线程自己的句柄：照常回 BadState，句柄同样被消耗。
- 最初有一处测试写错：channel 的句柄没有 Duplicate 权限，复制不出“没有 Transfer”的副本，`arg1` 变成了空。改用 VMO 造这个句柄。

**故意没做**
- 没有测成功路径里“先放句柄、后启动”的顺序和“启动与最后一个句柄关闭并发”：前者要 launcher 真的启动一个带句柄的进程（K23b），
  后者要第二个用户线程（等 `thread_start`）。
- `stack` 没有检查是不是规范的用户地址，只检查了 `entry`。
- 用户传入的进程在 job 之下的层级、`process_start` 之后进程的 job 记账，都还没有。

**怎么发现的**
无。

**用户的预测**
- 题 1（能不能先启动线程、再放句柄）：用户答“不能，因为线程被调度后可能立刻运行，读取句柄的时间我们没法估计，参数值会 UNDEFINED”。
  结论对，理由的方向也对：任何“正确性取决于时机”的顺序都是错的。补一处措辞：值不是未定义，它可以事先算出来，
  缺的是表里那一项（第一次使用会回 BadHandle）。
- 题 2（启动失败，那个已放进表里的句柄谁关）：用户答“失败了要么重试，成功后无需销毁；不允许重试就 kill 进程，句柄随之死亡”。
  漏了一点：`arg1` 已经被调用者消耗，重试时调用者会带一个新的句柄，旧的那个如果留在新进程的表里，就永久泄漏到进程死亡。
  所以失败路径要主动按值取出并关闭，不能指望“进程以后被 kill”。
  另外，“进程恰好并发转 Dead、表已被清空时，按值关闭会出什么问题”没有答：答案是找不到，什么也不做，表的锁只让一方取到它。

**你应该能回答的三个问题**
- 为什么句柄必须在线程可运行之前就放进新进程的表？把顺序反过来，会出现什么现象？
- 启动失败后，那个被消耗的句柄由谁关闭？进程恰好并发地转成 Dead 时，会不会关两次？
- `attach_thread()` 为什么不能靠“线程表是空的”来判断这是第一个线程？

## The launcher maps the vDSO, and mapping it registers the range (2026-09-29)

**Files**
- `kernel/mm/include/ours/mm/vm_aspace.hpp` — `VmAspace` now holds the vDSO range: `claim_vdso()`, `release_vdso()`, `set_vdso_code_range()`, `has_vdso()`, `vdso_contains()`
- `kernel/object/include/ours/object/process_dispatcher.hpp`, `kernel/object/process_dispatcher.cpp` — `set_vdso_code_range()` and `m_vdso_code_*` are gone; `vdso_contains()` asks the address space; `start()` uses `has_vdso()`
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `mark_vdso()`, `is_vdso()`
- `kernel/object/vma_dispatcher.cpp` — `VmaDispatcher::map()` recognises the vDSO's VMO by identity, restricts the shape of its mapping and registers it
- `kernel/lib/userabi/src/userboot.cpp` — `lay_out_vdso()` marks the VMO; `write_bootstrap()` hands a `VmoVdso` handle to the first process; `launch()` no longer registers the range by hand
- `system/public/ours/process_args.hpp` — `HandleType::VmoVdso = 0x11`
- `kernel/lib/userabi/userboot/userboot.cpp`, `system/service/init/init.cpp` — userboot forwards the handle, init holds it
- `examples/shell/shell.cpp` — `check_vdso_map()`
- `docs/design/260923/userspace-interface.md` (D2 now chooses B), `docs/design/260924/userspace-filesystem.md`

**Symptom**
A user-space launcher has to map the vDSO into the processes it starts (the user's decision: whatever can be done in user space is done there, since the kernel already offers enough memory primitives). But the kernel mapped the vDSO itself in `launch()`, registered its range by hand, and never handed the VMO out. A launcher had no vDSO to map, and the kernel had no other way to learn where a process's vDSO is.

**Cause**
"Only the vDSO can issue system calls" rests on one premise: what sits in the registered range really is the vDSO. While the kernel did the mapping the premise held by construction. Once a user-space launcher does it, the VMO's handle is in user space and the kernel can no longer take the mapping on trust. It has to recognise the vDSO's VMO in `vma_map`, register the range itself once the mapping succeeds, and impose rules on that one kind of mapping.

**Change**
Recognise, restrict, register, and read cheaply:

```cpp
// Recognise: by object identity, not by name, address or contents.
VmoDispatcher::mark_vdso();                       // once, when the kernel lays the vDSO out

// Restrict and register: VmaDispatcher::map(), for a VMO with is_vdso()
if (vmo_offset != 0 || length != vmo_size || perms != (Read | Execute)) return InvArgs;
if (!aspace->claim_vdso()) return AccessDenied;   // once per address space, claimed last
mapping = map_with_vmo(...);                      // on failure: release_vdso()
aspace->set_vdso_code_range(base, size);          // on success

// Read: asked on every system call, lock-free
vdso_contains(addr, len): size = load(acquire); return len <= size && addr - base <= size - len;
```

- **This flips decision D2 from A to B.** The old reason for A, "no privileged VMO has to be handed out", becomes the price of B: user space holds a VMO handle, so the kernel must constrain mappings made through it.
- **Each of the three rules has a job.** Whole VMO from offset 0: the recorded range is exactly the image, not a slice the caller picked. Read and execute only: the pages are code, and the rule does not lean on "no handle has Write", it holds for whoever holds the VMO. Once per address space: the range is fixed after it is set, so a second mapping cannot move it somewhere the first is not.
- **The claim is taken last and given back on failure.** A call that claimed and then returned an error for another reason would keep the address space from ever mapping its vDSO. So `claim_vdso()` sits right before `map_with_vmo()`, and `release_vdso()` undoes it if the mapping fails.
- **The range lives on `VmAspace`,** which is what D2 meant. Registration happens on a VMA (the launcher maps into the child's root VMA), and a VMA knows its address space but not its process; the process asks the address space.
- **Reads take no lock, the write happens once.** The syscall entry reads the range every time. The writer stores the base first and the size last with release semantics; the reader loads the size first with acquire, so a reader that sees a size also sees its base. Each word is written once, so no torn pair is possible. A process with no vDSO answers false for every range: it can make no system call at all.
- **A gap that is only recorded.** After registration nothing may be swapped into the range. Today user space has no `vma_unmap`, `vma_protect` or `vma_destroy`, so this holds by absence. When they land they must refuse any operation that meets the vDSO range, or a program can unmap the real vDSO and map its own code containing `syscall` at the same address. The design doc's list of deferred calls now says so.
- **The kernel's own launches take the same path.** `launch()` maps the vDSO's VMO through `Loader::map`, so `VmaDispatcher::map()` registers it; the hand-written `set_vdso_code_range()` call is deleted.

**Reference**
Zircon's `VDso` class: the kernel owns the vDSO's VMOs and recognises their mapping by object identity; `VmAspace::vdso_code_mapping_`, consulted by `VDso::ValidSyscallPC` on the system call path.

**Verification**
- Both images build and boot with `boot_check.py` (dbg 18 passed; rel 13 passed, 9 skipped); `usercopy.test` is 18/18 and `pmnode.test` 20/20 on both.
- Every process the kernel starts makes its system calls through the range registered by `VmaDispatcher::map()` (otherwise it would be killed as BadSyscall). The `trap_outside_vdso` probe is still refused and `pf_write_vdso` still killed, so the new registration path meets the old check correctly.
- The dbg_1 shell, which userboot hands the `VmoVdso` handle (in the dev image init holds it and prints `init: vdso vmo ok`), prints `vdso map checks passed`:
  - a writable mapping is AccessDenied (the handle has no Write); read-only without execute, and a mapping from an offset, are InvArgs;
  - lengths from one page up are tried: every one short of the whole is InvArgs and the first Ok is the whole, which is 3 pages;
  - a second mapping in the same address space is AccessDenied, while another process's first is Ok, so the claim belongs to the address space;
  - a slice of the vDSO's VMO has the same pages and maps fine, but registers nothing: `process_start` on a process that has only that mapping is still BadState. The kernel goes by object identity, not by what the pages contain.

**Deliberately not done**
- No test of "a hostile process unmaps the vDSO and maps its own code": `vma_unmap` and `vma_protect` do not exist yet (see above).
- The launcher does not yet use this handle to start a real process; that is K23b.
- Who may receive the vDSO's VMO is not restricted: the handle keeps its Transfer right. Since the rules are in the kernel, holding it does not let anyone forge a vDSO range.

**How it was found**
None.

**The user's prediction**
- Question 1 (which mappings must be refused): the user answered "read plus execute", and thought that a dedicated launcher maps it and afterwards the VMA is locked so nothing more can be mapped. The first part is right and is one of the three rules. The second needs correcting: the launcher is the honest user, but the rules cannot depend on it, because the child also holds its own root VMA (`VmaRoot`) and it is the one to guard against. And the whole VMA cannot be locked, since the child maps its heap and stack through it; what needs protecting is the vDSO's range, not the VMA. Missing were three rules: recognise by object identity, map the whole VMO from offset 0, and once per address space.
- Question 2 (which operations must be refused after registration): the user answered "Map, Unmap and Protect are all refused". The direction is right; the essential ones are Unmap and Protect (and Destroy), because they can replace the real vDSO in the range. Map needs no rule of its own: while the real vDSO is still there, its addresses cannot be covered by another mapping (AlreadyExists).

**Three questions you should be able to answer**
- Why do the rules live in the kernel's `vma_map`, instead of trusting the launcher to do it right?
- Why must unmap and protect of the range be refused once it is registered? How could a program otherwise get its own `syscall` instruction accepted?
- The syscall entry reads the vDSO range on every call and takes no lock. What order must the writer follow for that to be safe?

## Sealed mappings: the launcher asks, the kernel requires it for the vDSO (2026-09-29)

**Files**
- `system/public/ours/syscall.hpp` — `VmOptions::Sealed` (bit 20)
- `kernel/mm/include/ours/mm/vm_area_or_mapping.hpp`, `kernel/mm/vm_area.cpp`, `kernel/mm/include/ours/mm/vm_area.hpp` — `VmMapOption::Sealed`, `VmaFlags::Sealed`, `is_sealed()`; `VmArea::unmap()` and `protect()` refuse a range that meets a sealed mapping (`has_sealed_mapping_locked()`)
- `kernel/object/vma_dispatcher.cpp`, `kernel/lib/syscall/src/vma.cpp` — `Sealed` is an accepted option; a mapping of the vDSO's VMO without it is refused
- `kernel/lib/userabi/src/userboot.cpp`, `kernel/lib/kelfload/src/loader.cpp`, `kernel/lib/kelfload/include/kelfload/loader.hpp` — the kernel's own vDSO mapping is sealed
- `kernel/commands/vma_seal_test.cpp`, `kernel/commands/BUILD.gn` — the `vmaseal.test` console command
- `examples/shell/shell.cpp` — `check_vdso_map()` updated
- `docs/design/260923/userspace-interface.md`, `docs/design/260924/userspace-filesystem.md`

**Symptom**
The previous entry registered the vDSO's range from its mapping but left one gap open: once registered, nothing stopped a program from unmapping the real vDSO and mapping its own code, containing a `syscall`, at the same address. The gap was recorded as "must be refused when `vma_unmap` and `vma_protect` land".

**Cause**
"Only the vDSO can issue system calls" needs the range to hold real vDSO code and to go on holding it. Recognising the vDSO's VMO gave the first half; nothing gave the second, because a mapping had no way to say "I am not to be changed".

**Change**
A general immutability flag on mappings, requested by the launcher and required by the kernel for the vDSO. The user chose this shape after weighing three: an automatic refusal on the registered range (nothing to forget, no new interface), a general sealed flag applied automatically at registration, and a launcher request. The user wanted the launcher to ask, and proposed reusing `protect` to do so. It was rejected because `protect` changes permissions while sealing is a different property, the two would tangle (a `protect` that seals is itself refused by a seal), and it would make the vDSO's safety wait for a call that does not exist. The choice was `Sealed` as an option of `vma_map`, so the mapping is immutable from the moment it exists, with no gap between mapping and sealing, and no new system call.

```cpp
// mm: a bit set once, at creation, never cleared
create_mapping(...): if (option & VmMapOption::Sealed) vmaf |= VmaFlags::Sealed;

// mm: asked first, whole, before anything is torn down
VmArea::unmap(base, size):  if (has_sealed_mapping_locked(base, size)) return AccessDenied;
VmArea::protect(base, size): if (has_sealed_mapping_locked(base, size)) return AccessDenied;

// object: the vDSO's VMO must be mapped sealed
if (vmo.is_vdso() && !(options & VmOptions::Sealed)) return InvArgs;
```

- **Requested by the launcher, required by the kernel.** The launcher asks for `Sealed` as it would for any mapping it wants immutable, and the kernel refuses the vDSO's VMO without it. That keeps the user's choice (the launcher is explicit and the mechanism is general) and closes its one weakness: a launcher that forgot fails at its first start, loudly, instead of leaving a hole nobody sees.
- **Sealing does not replace registration.** A seal makes a range unchangeable; it does not say that this range is the vDSO. That still comes from recognising the VMO by identity in `vma_map`. The two work together.
- **Overlap counts, not coverage.** Trimming a sealed mapping changes it as surely as removing it, so any overlap refuses. A sub-area with a sealed mapping inside is refused too, since removing the area would remove what is inside. The walk is one `MappingEnumerator` however deep the areas nest, the same shape as `protect()`.
- **Refused whole, asked first.** An unmap that covers a plain mapping and reaches a sealed one refuses everything and leaves the plain mapping mapped, because half an unmap cannot be undone. `protect()` already worked this way.
- **The address space's own teardown is not refused.** `VmAspace::destroy()` goes through `VmArea::destroy()`, which does not look. Taking a space away is not a program changing its shape. A `vma_destroy` that a program can call must check the region for sealed mappings first; the doc comment on `unmap()` says so.
- **Concurrency.** The bit is written when the mapping is created, before it is linked, and only read after, under the address space lock the enumerator already runs under. It needs nothing of its own.
- **Kernel-internal callers are unaffected.** Only a mapping created with `Sealed` can refuse, and the kernel's own address space has none.

**Reference**
Linux `mseal()` (a sealed mapping refuses `munmap`, `mprotect`, `mremap`, `madvise(MADV_DONTNEED)` and the like) and OpenBSD `mimmutable(2)`. In Zircon the vDSO range is protected by the address space's `vdso_code_mapping_`, which unmap and protect consult; here the same protection is a property any mapping can carry.

**Verification**
- Both images build and boot with `boot_check.py` (dbg 18 passed; rel 13 passed, 9 skipped); `usercopy.test` is 18/18, `pmnode.test` 20/20 and the new `vmaseal.test` 13/13 on both.
- `vmaseal.test` checks directly, in the kernel, what no program can reach yet: unmapping a sealed mapping, trimming it, and changing its protection are AccessDenied; an unmap that covers a plain mapping and reaches a sealed one is refused whole and the plain mapping is still there; unmapping a sub-area with a sealed mapping inside is refused and the mapping survives; the plain mapping alone can be protected and unmapped; and the address space with all of them in it can be destroyed.
- The test was checked to fail without the code: with the two refusals removed, 7 of its 13 checks fail (every refusal, while the plain-mapping and teardown checks still pass), and it goes back to 13/13 with them restored.
- The dbg_1 shell: a mapping of the vDSO's VMO that is not sealed is InvArgs; sealed read-and-execute of the whole VMO is Ok and registers; a sealed mapping of an ordinary VMO is Ok, so the option is general. Every process the kernel starts maps its vDSO sealed (through `Loader::map`) and still makes system calls.

**Deliberately not done**
- `vma_unmap`, `vma_protect` and `vma_destroy` themselves. When they land they reach the refusals that exist; `vma_destroy` has to check the region first.
- A separate call to seal an existing mapping (like `mseal`). Nothing needs it yet.
- Sealing is not visible from user space: no program can tell a mapping is sealed except by being refused, and there is no query.

**How it was found**
None.

**The user's prediction**
The user first proposed that the launcher lock the vDSO's block of the address space so nothing could be mapped, unmapped or changed there, and then chose the launcher-requested form, suggesting `protect` as the way to ask. The idea is sound and close to `mseal`; the correction was the verb (`protect` changes permissions, sealing is another property) and one gap (a launcher that forgets), closed by the kernel requiring `Sealed` for the vDSO. An earlier reply of mine misread "lock the VMA" as locking the whole root VMA; the user meant the vDSO's block only.

**Three questions you should be able to answer**
- Why is a seal not enough on its own, and why is recognising the vDSO's VMO not enough on its own?
- Why does an unmap that reaches a sealed mapping refuse everything, including the plain mapping next to it?
- Why is the address space's own teardown allowed to remove sealed mappings, and what must a `vma_destroy` system call do differently?
