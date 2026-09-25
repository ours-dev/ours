# kernel/lib/ktl — 改动记录

## KernelMemoryResource 改走内核的 operator new，大于 4 KiB 的请求不再必然失败（2026-09-24）

**文件**
- `kernel/lib/ktl/src/memory_resource.cpp` — 分配改用 `::operator new(bytes, gaf)`，释放改用 `::operator delete(pointer)`
- `kernel/lib/ktl/include/ktl/memory_resource.hpp` — 类注释写明路由规则和大请求继承的 vmalloc 调用约束

**症状**
接在 `gktl.md` 同日那条之后：命令树改用 ustl 分配器后 panic 消失了，但 `kshell> ` 下
输入 `log level bogus` 什么也不打——应有的 `log: unknown level 'bogus'` 不出现，
`log` 也不 dump。`gktl::run_command()` 找不到命令时是静默返回的，所以看上去只是"没反应"。

**原因**
命令树一次性 `reserve()` 整个节点数组。`RadixTree::Node` 有 66 个子指针，552 字节；
镜像里 6 条命令的键长加起来约 50，于是一次要 ≈27 KiB 的连续内存。

`ktl::KernelMemoryResource` 直接调 `heap::kmalloc()`：

```cpp
// memory_resource.cpp（改前）
auto *pointer = heap::kmalloc(bytes, m_gaf);
...
heap::kfree(pointer);
```

kmalloc 的尺寸阶梯到 `heap::KMALLOC_MAX_SIZE`（4096）为止，超过就返回空。内核里决定
"小的走 kmalloc、大的走 vmalloc"的地方只有一个——`heap/new.hpp` 的
`operator new(size, gaf)`，它的注释原话是 callers do not choose and should not have to。
这个 resource 绕过了它，自己替调用方选了 kmalloc，于是所有经 ustl 容器发出的大于 4 KiB
的请求都拿到 OutOfMemory，哪怕系统里有几百 MiB 空闲。`reserve()` 失败 → 树为空 →
每条命令都"不存在"。

之前没暴露，是因为 ustl 容器在内核里的主要用户是 BTree（VmaSet 等），节点 256 字节，
从来碰不到这条线（`docs/repairs/260908/heap.md` 里也记过"kmalloc 不会因尺寸拒绝"）。
命令树是第一个要一大块的。

释放一侧如果只改分配不改释放，会更糟：vmalloc 出来的地址交给 `heap::kfree()`，
它会把这个地址当 physmap 地址去找所属 slab 页——`new.cpp` 的 `deallocate()` 注释里
专门写了在 `KERNEL_DEBUG_LEVEL=0` 下这会把 frame 0 的内容当 MemCache 指针读。
所以两侧必须一起改。

**改法**

```cpp
// memory_resource.cpp（改后）
auto *pointer = ::operator new(bytes, m_gaf);
...
::operator delete(pointer);
```

分配走内核 `new`，由它按大小在 kmalloc/vmalloc 之间选；释放走内核 `delete`，由它按地址
（`heap::is_vmalloc_addr()`）路由回去。resource 不再需要记住某块来自哪个堆。
对齐检查（`alignment > MAX_ALIGNMENT` 拒绝）保留不动。

代价写进了类注释：大于 4 KiB 的请求现在继承 vmalloc 的约束——可能阻塞，不能在中断
处理里、也不能持自旋锁时发出。这不是新增的风险：同样的请求以前是必然失败，现在是
可能阻塞；而对 ≤ 4 KiB 的请求行为完全不变，仍是 kmalloc。

被否掉的方案：

- **在 resource 里自己写 `if (bytes > KMALLOC_MAX_SIZE) vmalloc else kmalloc`**。
  那就是第二份阈值判断，和 `new.cpp` 的 `allocate()` 可以各自漂移——`kmalloc.hpp`
  把 `KMALLOC_MAX_SIZE` 放进头文件时就说过要避免这种重复定义。
- **把命令树改成逐节点分配以绕开 4 KiB**。能让命令跑起来，但 resource 对所有大请求
  都错，只修一个调用者等于把坑留给下一个。

**参照**
`Linux kvmalloc_node()` / `kvfree()`：先试 slab，放不下再走
`__vmalloc_node()`；释放时用 `is_vmalloc_addr()` 分流。本项目在 `heap/new.cpp` 的
`allocate()` / `deallocate()` 里按大小直接选、不先试，理由见那里的注释。
Zircon 的 `fbl::AllocChecker` + `new` 同样统一走一个全局分配入口，不让容器自选堆。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过；clang-tidy 对两个文件无告警。
- QEMU dev 镜像（`kernel.dev.obi`）：`kshell> ` 下
  - `log level bogus` → `[... cpu0 warn] log: unknown level 'bogus'`
  - `log` → 从 `Vector[8] was bind to IST[0]` 起完整 dump 出 record ring
- QEMU `kernel.dev.dbg_1.obi`：调试线程的 `I'm Thread 1/2` 输出刷屏，但
  `log level bogus` 同样打出 `log: unknown level 'bogus'`；无 panic，无 `operator new`。

**故意没做**
- 没有处理镜像里剩下的 7 处 `operator new(size_t)` 调用（libc++ 的 `std::format`
  `__allocating_buffer`、浮点格式化、`basic_string`）。它们同样会落到 panic 桩，
  但属于 libc++ 的使用面，是另一件事。
- 没有给 `KernelMemoryResource` 加 `MAX_ALIGNMENT` 与 kmalloc 实际对齐保证的核对；
  那是 `docs/repairs/260908/heap.md` 里已有的对齐条目。
