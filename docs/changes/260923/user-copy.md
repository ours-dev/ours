## 用户内存只经 `user_copy` 读写，内核态误触用户页直接报 SMAP 违规（2026-09-23）

**文件**
- `kernel/lib/user_copy/`（新库，copier 模板生成）— `UserPtr<T, Direction>`、`copy_from_user`/`copy_to_user`、`is_user_range`，以及纯算术的 `is_within`
- `kernel/arch/x86/kernel/user_copy.cpp`、`include/ours/arch/user_copy.hpp`（新）— `arch_copy_from_user`/`arch_copy_to_user`：只做 `stac` → memcpy → `clac`
- `kernel/arch/x86/kernel/feature.cpp`、`include/ours/arch/x86/feature.hpp` — 缓存 `feature_has_smap`
- `kernel/arch/x86/lib/arch/include/arch/x86/interrupt.hpp` — `ArchCpuState` 加 `AC` 字段
- `kernel/arch/x86/mm/fault.cpp` — 内核态、SMAP 开、AC 清、用户地址 → 报 "potential SMAP violation"，不再交给 `page_fault()`
- `kernel/lib/syscall/` — 两个处理函数的指针参数改成 `UserInPtr`/`UserOutPtr`；`debug_write` 先拷进栈上缓冲区再写日志；`command_list` 逐段 `copy_to_user`

**症状**
在支持 SMAP 的 CPU 上（`CPU=max python3 scripts/qemu_boot.py`）shell 一个字都打不出来，
机器静默卡住，日志里什么都没有。默认的 Nehalem 没有 SMAP，所以一直没被发现。

**原因**
两个处理函数都是「先做范围检查，再直接解引用用户指针」。`mmu.cpp` 在 CPU 支持时会打开
CR4.SMAP（`debug.cpp` 里「SMAP 没开」的注释早就过时了），此后 ring 0 只有在 RFLAGS.AC=1
时才能碰用户页，否则缺页。

这个缺页被处理成了死循环：页确实在用户地址空间里映射着，`page_fault()` 看到 PTE 存在就回
Ok，指令重新执行，再次缺页，永远如此。

加上 SMAP 检测之后第一次启动**仍然**静默卡住，原因更值得记：`sys_debug_write` 第一次碰
用户内存发生在 `Logger::write()` 内部，已经拿着日志锁；缺页处理自己的 `log::error` 去等
这把锁，而持锁的正是本 CPU——死锁。所以规则不止是「经 user_copy 拷」，还有「先拷进内核
缓冲区，再拿任何锁」。

**改法**
三层，各管一件事：

1. `user_copy::is_within(addr, size, base, span)`：纯算术，不计算 `addr + size`（用户可控，
   会回绕），改用相对 `base` 的偏移；零长度的范围也必须**起点**在内（同 Zircon
   `is_user_accessible_range()`）。关键边界用 `static_assert` 在每次构建时检查。
   `is_user_range()` 用的是 `USER_ASPACE_BASE/SIZE`，而不是「不在内核半区」——两者差着
   空指针洞和 2^47 以上的非规范地址空洞，后者访问的结果是 #GP 而不是缺页。
2. `UserPtr<T, Direction>`：把地址存成数字而不是 `T *`，所以 `*p` 根本编译不过；方向是类型
   的一部分，往输入指针里写是编译错误。
3. `arch_copy_*`：`UserAccessWindow` 在构造/析构里 `stac`/`clac`（带 "memory" clobber，防止
   编译器把拷贝的读写挪出窗口）。内核没用 `.code-patches`，所以用 `feature_has_smap` 决定
   是否执行这两条指令——在没有 SMAP 的 CPU 上它们是 #UD。

改前（`debug.cpp`）：

```cpp
    auto const start = reinterpret_cast<VirtAddr>(buffer);
    if (mm::is_kernel_address(start) || length > std::numeric_limits<VirtAddr>::max() - start ||
        mm::is_kernel_address(start + length - 1)) {
        return Status::InvArgs;
    }
    ...
    log::get_global_logger().write(buffer, length);
```

改后：

```cpp
    char text[MAX_DEBUG_WRITE];
    if (auto const status = buffer.copy_array_from_user(text, length); status != Status::Ok) {
        return status;
    }
    log::get_global_logger().write(text, length);
```

缺页处理新增（`fault.cpp`）：

```cpp
    auto const supervisor = (error_code & PAGE_FAULT_USER) == 0;
    auto const access_open = frame->flags.get<arch::ArchCpuState::AC>() != 0;
    if (supervisor && feature_has_smap && !access_open && !mm::is_kernel_address(addr)) {
        log::error("potential SMAP violation: ring 0 touched user address {:#x} outside user_copy",
                   addr);
        return Status::AccessDenied;
    }
```

不要求「页存在」，与 Zircon 相同：SMAP 开着时，AC=0 的内核态用户地址访问不论页在不在都是 bug；
页不在的话，处理掉之后重试也会落回同一个死循环。

**参照**
Zircon `//zircon/kernel/lib/user_copy`（`user_ptr<T, Policy>`）与 `arch_copy_from_user()`
（`kernel/arch/x86/user_copy.cc`，靠代码补丁选 stac/clac 版本）；`x86_pfe_handler()`
（`kernel/arch/x86/exceptions_c.cc`）的 "potential SMAP failure" 检查。Linux
`__uaccess_begin()/__uaccess_end()`、`access_ok()`。

**验证**
- 两个镜像在默认 CPU 下都跑到 shell 打完命令表。
- `CPU=max`（带 SMAP）下，dev 和 dev.dbg_1 都跑到 `--- end ---`；改动前同样的启动静默卡死。
- 反向测试：在 `sys_debug_write` 里临时加一次裸解引用 `*reinterpret_cast<char const volatile *>(buffer.address())`，
  `CPU=max` 启动得到

  ```
  [cpu1 error] potential SMAP violation: ring 0 touched user address 0x2003b5 outside user_copy
  [cpu1 error] Unhandled page fault: vector=14, error_code=0x1
  [cpu1 error]   cr2=0x2003b5
  ```

  错误码 0x1 = 页存在、读、内核态；0x2003b5 正是 shell 那串字符串。探针已删除。
- `is_within` 的边界由 `static_assert` 保证；`test/user_copy_test.cpp` 的宿主测试和 eftl 的一样
  尚未挂进任何构建图（`docs/repairs/260919/elfload.md` #2）。

**故意没做**
- **故障恢复**（Zircon 的 `data_fault_resume`）：拷贝撞上未映射的用户地址现在是内核 panic。
  P0 只运行内核包里的受信程序，这是方案里明确的 P0 限制，排在 P1-A。
- **代码补丁**：用一个缓存标志 + 分支代替 Zircon 的补丁，一次可预测的分支对一次拷贝可以忽略。
- 把 `FEATURE_HAS_FSGSBASE` 同样改成小写，另起一个提交。
