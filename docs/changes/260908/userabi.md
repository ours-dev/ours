# userabi 改动记录

## userboot 发出第一个系统调用（2026-09-08）

**文件**
- `kernel/lib/userabi/userboot/userboot.cpp` — 从纯自旋变成"发一次调试写，再自旋"

**症状**
无。这是这条切片的最后一步：打包 → 加载 → 进程与地址空间 → 映射 → 进入用户态 →
**一次系统调用**。

**原因**
在此之前 userboot 的全部内容是 `pause` 自旋。它证明了"进得去"，没有证明"回得来"。
而"回得来"是 `sysret` 那条路径，它在这一步之前从未被执行过——事实证明那条路径是错的
（见 `docs/changes/260908/syscall.md`：`sysret` 一直被汇编成 32 位形式）。

**改法**

改前
```cpp
extern "C" [[noreturn]]
auto _start() -> void {
    for (;;) {
        __asm__ volatile("pause");
    }
}
```

改后（节选）
```cpp
constexpr char MESSAGE[] = "userboot: hello from ring 3";

[[nodiscard]]
auto syscall2(usize const number, usize const arg0, usize const arg1) -> usize {
    usize result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(arg0), "S"(arg1)
                     : "rcx", "r11", "memory");
    return result;
}

extern "C" [[noreturn]]
auto _start() -> void {
    static_cast<void>(
        syscall2(OURS_SYS_DEBUG_WRITE, reinterpret_cast<usize>(MESSAGE), sizeof(MESSAGE) - 1));

    for (;;) {
        __asm__ volatile("pause");
    }
}
```

几处不是随手写的地方：

- **clobber 列表不是模板**。SYSCALL 会毁掉 `%rcx`（它把返回地址放进去）和 `%r11`
  （RFLAGS），两个都必须列出来，否则编译器会把活跃值留在里面跨过这条指令。`"memory"`
  是因为内核要读那个缓冲区——没有它，对 `MESSAGE` 的写入可以被合法地沉到调用之后。
- **`sizeof - 1`**：内核收的是长度，不找结束符；把 NUL 一起发过去会在控制台上多一个
  杂字节。
- **`MESSAGE` 必须是 RIP 相对寻址**，这一点比看上去要紧：镜像是个 static PIE，内核把它
  映射到自己挑的地址并且**不做重定位**（`userboot.cpp` 的 `load_image()` 没有重定位
  过程，因为这个二进制本来就没有重定位项）。如果这里烘进一个绝对地址，它会被原样读出
  并指向别处。反汇编确认了实际发出的是相对寻址、且文件里没有任何重定位项：

  ```
  0000000000001290 <_start>:
      1290: 48 8d 3d d9 ef ff ff   leaq -0x1027(%rip), %rdi   # 0x270 <MESSAGE>
      1297: be 1b 00 00 00         movl $0x1b, %esi
      129c: 31 c0                  xorl %eax, %eax
      129e: 0f 05                  syscall
      12a0: f3 90                  pause
      12a2: eb fc                  jmp  0x12a0
  ```
  ```
  There are no relocations in this file.
  ```

- **调用之后回到自旋，而不是退出**。回到这里本身就是这次测试的后一半：它意味着
  `sysretq` 还原了入口 stub 存下的用户 `%rip` 和 `%rsp`，ring 3 继续在跑。内核继续从
  用户态记到的时钟中断就是证据。

**参照**
`Zircon trivial-test-userboot`：同样是单个翻译单元、内联 `syscall`、用一次调试写来
证明整条路走通了。

**验证**
两个镜像各实测一次，都打出**恰好一次**：

```
[info] userboot: hello from ring 3
```

随后 30 秒内 912 次 `Exception[interrupt] from user space`，signal 15 收尾。

**故意没做**
- **没有第二个系统调用**。这条切片刻意只要一个。退出需要 `sys_thread_exit`，而它的
  签名收 `object::Handle*`，那要等句柄表。
- **没有检查返回值**。userboot 没有第二个可以用来报告结果的出口。

## 内核包缓冲区改用 `new`（2026-09-08）

**文件**
- `kernel/lib/userabi/userboot.cpp` — `materialize_kernel_package` 不再手写页分配

**症状**
无回归。和 `prof.cpp` 同一件事：把一处因为 `new` 用不了而绕的路收回来。

**原因**
原来的注释把理由写得很清楚：

```
/// This is deliberately not a `ktl::Vec<u8>`. ktl's allocator is
/// kmalloc, whose largest size class is 4 KiB, and an oversized request
/// there answers nullptr rather than falling back to pages; Vec then
/// constructs elements at that nullptr and page-faults inside reserve().
```

解压后的 BootFS 是**兆字节**级的（它装着整个内核 ELF）。这两个理由现在都没有了。

**改法**

```cpp
// 改前
out.nr_frames = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
out.base = mm::get_frames_n<u8>(mm::GAF_KERNEL, out.nr_frames);

// 改后
out = new (mm::GAF_KERNEL) u8[bytes];
```

`struct Package { u8 *base; usize nr_frames; }` 整个删掉了——`nr_frames` 存在只是为了
将来释放，而这块缓冲区**有意不释放**：包里每个文件的字节都是**指向它内部的视图**，放掉
它会让每一个都悬空。签名因此简化成 `materialize_kernel_package(u8 *&out)`，并把"故意不
释放"这件事从字段推进注释里说明。

`get_frames_n` 向上取整到 2 的幂在这里比在 prof 那边更浪费：一个 3 MiB 的包会拿 4 MiB。

**参照**
`Zircon userboot` 从 ZBI 解压 BOOTFS 也是普通分配。

**验证**
两个镜像实测：`userboot: kernel package holds 4 files` 照常，`hello from ring 3` 照常。

**故意没做**
`userboot.cpp:216` 的另一处 `get_frames_n(GAF_USER, ...)` **不迁**：那是 userboot 镜像的
backing，紧接着要 `PhysMap::virt_to_phys()` 交给 `VmObjectPhysical`（见同文件
`create_physical` 那处注释）——它要的是**物理**连续，而 vmalloc 给的正好是物理离散。
