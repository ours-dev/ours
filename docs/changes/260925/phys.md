# kernel/phys 改动记录（2026-09-25）

## 一个地址只映射进翻译它的那棵根表（2026-09-25）

**文件**
- `kernel/phys/aspace.cpp` — `Aspace::map()` 在双根架构下按 va 选一棵表，只映射进去；跨半区/回绕的范围直接拒绝
- `kernel/phys/include/ours/phys/aspace.hpp` — 新增 `Aspace::is_upper()` 和约束两套 paging 方案的 `static_assert`；顺手修掉 `init()` 里断言写错变量的复制粘贴

**症状**
潜伏，x86 上当前触发不到。x86 的 `ArchLowerPaging` 和 `ArchUpperPaging` 是同一个类型
（`kernel/arch/x86/phys/include/ours/phys/arch_paging.hpp` 里两个 `PagingDispatcher<PageMapLevel4>::Type`），
所以 `DUAL_ASPACE == false`，`if constexpr` 把出错的那一支丢掉了。arm64 的 paging 方案还没写
（`kernel/arch/arm/` 下只有空的 BUILD.gn），所以这条路径还没有任何镜像跑过。等 arm64 的
`ArchLowerPaging`/`ArchUpperPaging` 一落地，phys 的恒等映射和内核高地址映射会同时落到错误的表里。

另一件当场可见的：`init()` 里分配 upper 根表之后断言的是 `m_lower_pgd`，所以 upper 根表分配失败
永远断不出来，后面 `UpperPaging::map()` 会拿 0 当根表地址往里写。

**原因**
这一段是这条改动的全部价值所在，因为它是"两种同名但语义不同的分页布局"被混成一种的结果。

x86-64 和 RISC-V 是**单根**：CR3/SATP 指向一棵表，这棵表的低半边条目翻译低半区、高半边条目翻译
高半区。所以一个地址无论在哪个半区，查的都是同一棵表，`VirtAddrExt::Canonical` 描述的就是这件事——
不可寻址位是符号扩展，扩展成 0 是低半区，扩展成 1 是高半区，两者共用一棵表。

arm64 是**双根**：TTBR0 翻译低半区，TTBR1 翻译高半区，硬件按地址的高位在两个寄存器之间选一个，
一个地址只归其中一棵表管。`VirtAddrExt::AlwaysZero` / `AlwaysOne` 描述的就是这种"上下半区分开配置"
的方案（见 `kernel/lib/arch/include/arch/paging/paging_dispatcher.hpp` 里那三个枚举项的注释）。

原来的 `map()` 把同一个 (va, pa, n) 依次喂给两棵表，而 `Paging::map()` 只按 va 的低位算下标——
它不知道自己是哪一棵根表，也不会因为 va 在低半区就拒绝写 TTBR1 那棵表。于是两个方向同时错：

- 低端 RAM 的恒等映射（`map_usable_ram()` 在物理 0 附近映出来的那批）也被写进了 TTBR1 那棵表。
  高半区凭空多出一份低端物理内存的别名，而内核以为高半区只有自己布下的那些映射。
- 内核的高地址映射也被写进了 TTBR0 那棵表。低半区是用户半区，等于把整个内核映射进了用户地址空间——
  同样的表项、同样的权限位，只差一个 User 位就能被用户态读到。

还有一个次要的：先映射 upper，失败就提前 return；如果 upper 成功而 lower 失败，upper 那份留在表里
没人清，调用者拿到 Err 却已经有一半映射生效了。改成只碰一棵表之后，这个不一致状态不存在了。

`is_upper()` 用第 63 位判断。arm64 硬件实际是按第 55 位在 TTBR0/TTBR1 之间选的，但只要地址的
top-byte tag 是 0，第 55 位和第 63 位就一致，而 phys 不映射任何带 tag 的地址。加的
`static_assert` 把这个前提钉在编译期：双根时两套方案必须分别是 `AlwaysZero` 和 `AlwaysOne`，
否则 `is_upper()` 会把地址送进错的表。

**改法**
`if constexpr (!DUAL_ASPACE)` 一支原样只走 `LowerPaging::map`，另一支按 va 选表。跨两个半区的范围
直接回 `MapError::InvArgs`，不拆成两段分别映射：拆开就等于替调用者决定"这个范围横跨用户半区和内核
半区是合理的"，而在 phys 里这只可能是调用者算错了地址。回绕（`last < va`）和 `n * PAGE_SIZE` 溢出
一并在这里挡掉。

`static_assert` 放在类定义之后的命名空间作用域，不放在 `map()` 里面：`Aspace::map()` 不是模板，
`if constexpr` 丢弃的分支照样要通过语义检查，`static_assert` 也照样会触发，写在函数里 x86
（`VirtAddrExt::Canonical`）会直接编译失败。

改动前：

```cpp
    if constexpr (DUAL_ASPACE) {
        auto upper_result =
            UpperPaging::map(m_upper_pgd, PhysToTable(), allocator, va, pa, n, flags);
        if (!upper_result) {
            return ustl::err(upper_result.unwrap_err());
        }
    }

    auto result = LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    if (!result) {
        return ustl::err(result.unwrap_err());
    }
    DEBUG_ASSERT(result, "Failed to map");

    return ustl::ok();
```

改动后：

```cpp
    if constexpr (!DUAL_ASPACE) {
        return LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    } else {
        if (n == 0) {
            return ustl::ok();
        }
        // Guard the multiplication before computing the last byte's address.
        if (n > (~usize{0}) / PAGE_SIZE) {
            return ustl::err(MapError::InvArgs);
        }
        auto const last = va + (n * PAGE_SIZE - 1);
        if (last < va || is_upper(va) != is_upper(last)) {
            return ustl::err(MapError::InvArgs);
        }

        if (is_upper(va)) {
            return UpperPaging::map(m_upper_pgd, PhysToTable(), allocator, va, pa, n, flags);
        }
        return LowerPaging::map(m_lower_pgd, PhysToTable(), allocator, va, pa, n, flags);
    }
```

头文件里新增的判据：

```cpp
    [[nodiscard]]
    static constexpr auto is_upper(VirtAddr const va) -> bool {
        return (va >> 63) != 0;
    }
```

以及 `init()` 里改掉的那行：

```cpp
-        DEBUG_ASSERT(m_lower_pgd, "");   // 断言的是上一段分配的结果
+        DEBUG_ASSERT(m_upper_pgd, "");
```

**参照**
Linux 的 `arch/arm64/mm/mmu.c` 里 `create_pgd_mapping()` 的调用方从来不给一个地址同时建两份映射：
`map_kernel()` / `paging_init()` 只往 `swapper_pg_dir`（TTBR1）里建内核映射，用户映射走
`__pte_alloc()` 那条路填进 `mm->pgd`（TTBR0），两棵表在源码层面就不共用一个入口。
arm64 用 `TCR_EL1.T0SZ` / `T1SZ` 分别给两棵表定寻址宽度，这正是 `AlwaysZero`/`AlwaysOne`
这对枚举项在描述的东西。

Zircon 侧对应的是 `ArmArchVmAspace`：它的 `flags_` 区分 kernel/user/guest aspace，
`ArmArchVmAspace::Init()` 按类型选 `tt_virt_`，`MmuFlagsToPteAttrs()` 之后写的始终是那一棵表；
x86 侧 `X86PageTableMmu` 则是单根，两者不共用一个 map 实现。ours 这里用一个 `Aspace` 同时描述
两种方案，所以选表这件事必须自己做，`DUAL_ASPACE` 加 `is_upper()` 就是这个选择点。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置全量构建通过。
- `out/bringup.x86` 的 `kernel.dev.obi` 在 QEMU 下照常启动：phys 的 `Map identically`
  依旧从 `(0x0, 0xA0000)` 开始，一路到 shell 跑完 `vmo_read checks passed` 并正常销毁进程，
  日志里没有 panic / page fault。
- `out/numa.x86` 在 `Released 16 regions, 255672 frames (998 MiB) to the buddy allocator`
  之后有一个 `Unhandled page fault: vector=14, cr2=0xffffff8800016fff, rip=+0x24525`。
  **这是既有问题，与本改动无关**：把这两个文件 `git checkout` 回 HEAD 重新构建再启动，
  fault 的 vector、cr2、rip 完全一致（只有 rsp 差一个栈帧偏移）。
- **arm64 路径未实测**，也无法实测：`kernel/arch/arm/` 下还没有 paging 方案，
  没有任何镜像会让 `DUAL_ASPACE` 为真。这条改动在 x86 上是行为恒等的
  （`if constexpr` 仍然只走 `LowerPaging::map`），正确性靠上面那段推理和新增的
  `static_assert` 兜住。

**故意没做**
- 没有实现 `Aspace::query()`。它现在只有声明、没有定义、也没有调用者
  （`kernel/phys/include/ours/phys/aspace.hpp` 第 57 行），属于预留的脚手架。
  但实现它的时候要走同一条规矩：先用 `is_upper()` 选好表再查，不能两棵都查一遍。
  `unmap()` 同理，它现在直接回 `Status::NotSupported`。
- 没有把跨半区的范围拆成两段分别映射。见「改法」——在 phys 这一层，跨半区只可能是算错了地址，
  悄悄拆开会把一个调用者的 bug 变成两份看起来正常的映射。
- 没有动 `map_usable_ram()`。它映的全是低端物理地址的恒等映射，永远落在低半区，
  按新规则天然只进 lower 那棵表。
- 没有顺手修那个 numa 配置下的 page fault。那是另一件事，另一个提交。
