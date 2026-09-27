# kernel/lib/user_copy — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 通用层的 fault capture 接口，以及第一个真正跑缺页的测试（2026-09-27）

**文件**
- `kernel/lib/user_copy/include/user_copy/user_copy.hpp` — `copy_{from,to}_user_capture_faults()`，`UserPtr` 的四个 capture 方法；改写调用方规则
- `kernel/lib/user_copy/src/user_copy.cpp` — 两个自由函数的实现
- `kernel/lib/user_copy/BUILD.gn` — `:headers` 在内核工具链下补上 arch 与 mm 的依赖
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — 纠正上一条引用错的 Zircon 路径
- `kernel/commands/user_copy_test.cpp`、`kernel/commands/BUILD.gn` — 新增 `usercopy.test` 控制台命令

**症状**

两件事。其一，capture 只在 arch 层有（`x86-kernel.md` 第 1 条），上层代码只能直接调 `ours::arch_*`，
拿不到 `UserPtr` 那一套类型检查。其二，这个库的头文件注释把中转页写成了规则：

```cpp
///  - Copy into a kernel buffer first, then take the object's lock; dequeue
///    under the lock, drop it, then copy out.
```

中转页已被否决（每页多一次 memcpy，存在的唯一理由是避免持锁碰用户内存，而 capture 已经让这件事安全了），
这句注释等于在教人重犯。

**原因**

`UserCopyCaptureResult` 该放在哪一层，照 Zircon 定：它把 `UserCopyCaptureFaultsResult` 定义在
`kernel/include/arch/user_copy.h`，也就是 arch 接口里，`lib/user_copy` 的 `user_ptr.h` 直接 include 它。
所以这里 `user_copy.hpp` include `<ours/arch/user_copy.hpp>`，原样使用 `ours::UserCopyCaptureResult`，
不另造一个类型再来回转换。

**改法**

自由函数：先查范围、再判空，然后才交给 arch 层。两种"没尝试"的情况都**不带缺页信息**，
对应 Zircon 注释里的第三种结果（"The operation fails without ever trying"）：

```cpp
auto copy_to_user_capture_faults(ours::VirtAddr const dst, void const *const src, usize const size)
    -> ours::UserCopyCaptureResult {
    if (!is_user_range(dst, size)) {
        return ours::UserCopyCaptureResult{.status = ours::Status::InvArgs, .fault = {}};
    }
    if (size == 0) {
        return ours::UserCopyCaptureResult{.status = ours::Status::Ok, .fault = {}};
    }
    return ours::arch_copy_to_user_capture_faults(dst, src, size);
}
```

`UserPtr` 上加 `copy_from_user_capture_faults()`、`copy_array_from_user_capture_faults()`、
`copy_to_user_capture_faults()`、`copy_array_to_user_capture_faults()`，方向约束（`requires(D != ...)`）
和非 capture 版一致；元素个数乘出来溢出时同样拒绝且不带缺页。

调用方规则改为：普通拷贝就地解析缺页，所以不能在自旋锁、地址空间锁或 VMO 锁下用；要在这些锁下拷，
用 capture 版，拿到缺页后放锁、`soft_fault()`、重新拿锁、重查、重试；**不要**改走中转页。

`:headers` 的依赖：头文件现在 include arch 接口，而它又用到 `mm::VmfCause`。只在内核工具链下补
`//kernel/arch/$ours_cpu:headers` 与 `//kernel/mm:headers`——宿主测试只 include `range.hpp`，
而 `//kernel/mm:headers` 在宿主工具链下求值不了。

**参照**

`Zircon kernel/lib/user_copy/include/lib/user_copy/user_ptr.h` 的
`copy_to_user_capture_faults()` / `copy_array_to_user_capture_faults()` 等；
`kernel/include/arch/user_copy.h` 的 `UserCopyCaptureFaultsResult` 及其三种结果的说明。

**测试**

仓库还没有内核态单测运行时（`//kernel/products/bin:vmours.with-tests` 只是结构性依赖），
而要测的东西——MMU 在用户映射上抛出的缺页——宿主测试造不出来。所以照 `mutex.stress` 的先例，
写成控制台命令 `usercopy.test`，在 QEMU 里经串口敲进 kshell 执行。

它**自己建一个用户地址空间**，把调用线程挂上去跑完再挂回来。原因是 kshell 在自己的内核线程上执行命令，
没有用户地址空间可借，而往某个进程的地址空间里映射测试页等于碰不属于测试的内存。内核线程在用户地址上
缺页时用的是自己的 `m_aspace`（`Thread::Current::fault()`），所以挂上之后，缺页路径、`soft_fault()`
和拷贝看到的都只是测试的地址空间。挂接与解挂在关抢占下完成：调度器每次切换都按
`thread->aspace()` 装载 CR3，两步之间被抢占会让调度器和这里对"正在离开哪个地址空间"各执一词。

布局：映射三页匿名内存（不 commit），再把第三页 unmap 掉，得到 P0、P1 两个"已映射、未在内存"的页，
和一个"根本没映射"的 HOLE。13 项检查，每项先打印实际值再给判定：

| 检查 | 期望 |
|---|---|
| 内核地址 | InvArgs，无缺页 |
| 零长度 | Ok，无缺页 |
| 元素个数溢出 | InvArgs，无缺页 |
| capture 写 P0 | 缺页 va = P0，cause = Write\|Absent |
| 再 capture 读 P0 | 仍缺页，cause = Absent——capture 没有顺手把页补上 |
| **持自旋锁、关中断**时 capture 写 P1 | 缺页 va = P1——capture 存在的理由 |
| `soft_fault(P0)` | Ok |
| 重试写 P0、再读回 | 都 Ok，数据往返一致 |
| 从 P0 末尾跨进 P1 的 16 字节写 | 缺页 va = **P1 的首字节**，不是缓冲区起点 |
| capture 写 HOLE | 缺页 va = HOLE |
| `soft_fault(HOLE)` | 失败 |
| 普通拷贝写、读 P1 | 就地按需分页，往返一致 |
| 普通拷贝写 HOLE | InvArgs |

结束时解挂、销毁地址空间，也计入判定。

**验证**

构建通过；`clang-format` 跑过全部改动文件，`clang-tidy -p out/bringup.x86` 跑过
`user_copy_test.cpp` 与 `src/user_copy.cpp`，无告警。

`kernel.dev.obi` 在 `kshell>` 敲 `usercopy.test`：

```
usercopy.test: P0=0x200000 P1=0x201000 hole=0x202000
usercopy.test:   write to absent P0: status=10 fault va=0x200000 cause=0x3
usercopy.test:   read from P0 after capture: status=10 fault va=0x200000 cause=0x2
usercopy.test:   write to absent P1 under a spin lock, interrupts off: status=10 fault va=0x201000 cause=0x3
usercopy.test:   soft_fault(P0): status=0
usercopy.test:   P0 holds 0x5afec0de12345678
usercopy.test:   write straddling P0 into absent P1: status=10 fault va=0x201000 cause=0x3
usercopy.test:   write to the hole: status=10 fault va=0x202000 cause=0x3
usercopy.test:   soft_fault(hole): status=20
usercopy.test:   plain copies to/from absent P1: 0 0, P1 holds 0xbadf00d87654321
usercopy.test:   plain copy to the hole: 10
usercopy.test: ok -- 13 checks
```

`kernel.dev.dbg_1.obi`（`DEBUG_ASSERT` 开着，包括 capture 分支里新加的那条）同样 13/13，无断言。
同一次启动里连跑两遍，两遍都 13/13、地址相同，说明测试收拾干净了自己的地址空间。

值得单独记一笔：**"普通拷贝就地按需分页"这一项，是正常启动以来第一次看到缺页被成功解析。**
启动过程中的缺页全都落在解析不了的地址上（见 `260926/mm.md`），这条成功路径此前只有写
`VmMapping::fault()` 时的专项探针走过。

`soft_fault(HOLE)` 返回的是 20（`AccessDenied`）而不是 4（`NotFound`）：HOLE 是从一个映射里 unmap
出来的，映射对象还覆盖着它，只是那一段在 `ProtectionRanges` 里成了零权限区——这是 VMA 层"unmap 即零权限"
的表示方式。测试只要求它失败，不依赖是哪个错误码。

**故意没做**

- `kernel/lib/user_copy/test/` 下的宿主 gtest **从来没被编过**：它依赖 `//vendor/googletest:gtest`，
  而 `vendor/googletest` 只有 Bazel 和 CMake 构建、没有 `BUILD.gn`，也没有任何地方引用这个测试目标。
  给 googletest 写 GN 构建属于 vendor/构建那条线，不在这一步里做。
- 没有给通用层写"假 arch 后端"的宿主测试：范围拒绝、零长度、溢出这几条在 `usercopy.test` 里对着真实后端
  已经覆盖了。
- 发现 `gktl` 命令表不认下划线，命令名因此用 `usercopy.test`。bug 本身记在
  `docs/repairs/260927/kernel-lib-gktl.md`，没有顺手改。
- `VmoDispatcher::read_user()` / `write_user()` 仍在用中转页，下一步。
