# kernel/phys 改动记录（2026-09-26）

## 交接快照缓冲区按整页分配，它的保留区才描述得出来（2026-09-26）

**文件**
- `kernel/phys/physboot.cpp` — `snapshot_memory_map()` 的分配从 `alignof(bootmem::MemRange)`
  改成 `PAGE_SIZE`，尺寸向上取整到整页

**症状**
潜伏，但立刻可见：给保留区加类型、再按类型给帧打标之后（见
`docs/changes/260926/bootmem.md`），启动输出里每一类保留内存都有一行总数，唯独 `handoff`
一帧都没有——而 physboot 明明为交接快照分配并保留了一段内存。

查出来的形态是这个保留区在内核这边长成了**跨页的一小段尾巴**：

```
DBG handoff [0x3FE9DBE0, 0x3FE9E000) handoff
DBG   pfn 0x3FE9D uninitialized
```

那一页既不整个空闲也不整个属于交接数据，而且 `init_memmap()` 的主遍历根本没初始化它
（`pfn_valid()` 为真，但没有 `Flags::Initialized`），于是被打标那一遍的守卫正确地跳过了。

**原因**
`snapshot_memory_map()` 按 `alignof(bootmem::MemRange)`（8 字节）分配这块缓冲区。
对 phys 自己来说这是对的——它只要一块对齐的内存放数组。

但这块缓冲区**活过了跳转**：内核的 `mh.ranges` 是一个指进它的 span，
`handoff_early_pmm()` 从里面读整张内存图。而交接之后，下游一切东西的单位都是**帧**：
memmap 一个帧一个描述符，buddy 一个帧一个链表项，`Role` 和 `Flags` 都长在帧上。
一段要活到内核里的保留区如果不是整页的，就没有任何帧描述符能表达它——
这一页到底算空闲还是算交接数据，两个答案都不对。

这不是「打标那一遍有 bug」，而是打标把一个一直存在的错位照出来了：
phys 侧按自己的对齐要求分配，内核侧按帧管理，中间那段尾巴谁都说不清。

**改法**
按 `PAGE_SIZE` 对齐、尺寸向上取整到整页。`max_snapshot_entries(1)` 的估算不受影响——
多要的字节不会多出条目。

没有改成「内核侧把不整页的保留区向外扩到整页」：那会把一页空闲内存悄悄划给交接数据，
即把一个上游的对齐问题在下游用更大的谎言盖住。整页是分配方的责任。

**参照**
Zircon 的 `PhysVmo` / `PhysMapping`（`kernel/phys/include/phys/handoff.h`）全部以页为单位
（`PmmNode::InitReservedRange()` 开头就是
`DEBUG_ASSERT(IsPageRounded(range.addr)); DEBUG_ASSERT(IsPageRounded(range.size));`），
正是同一条约束：能跨交接边界的东西必须整页，因为对岸只认帧。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过。
- 两个镜像的启动输出里 `handoff` 出现了，一帧：

```
[info] Reserved 12 frames (48 KiB) of page table
[info] Reserved 1 frames (4 KiB) of handoff
[info] Reserved 2052 frames (8208 KiB) of kernel data
```

- `out/numa.x86`：7 个 AP 全 online，shell 六组检查全过；`out/bringup.x86` 六组全过。
  两者都无 fault / panic / 断言失败。

**故意没做**
- 没有管 `arch_handoff_to_kernel()` 里那个跳转蹦床页。它是在快照**之后**才分配的，
  所以根本没进内核拿到的内存图，内核会把它当空闲内存收下。这没有错：蹦床只在跳转那一刻执行，
  跳完就是死的，还给 buddy 正是该做的。但值得记一笔，因为「快照之后再分配的东西内核看不见」
  是一条容易被下一个人踩到的规则。
- 没有给 bootmem 加「保留区必须整页」的断言。`reserve()` 是按字节的，而 phys 内部有合理的
  非整页用法（它自己不按帧管理）。真正的约束是「要跨交接边界的保留区必须整页」，
  这条应该加在 `snapshot()` 上，而不是 `reserve()` 上——等有第二个犯同样错的分配点再加。
