# userabi / userboot — 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260905/userabi.md`。

---

## 1. userboot 的映射同时可写可执行

`load_image()` 把三个 PT_LOAD（R / RX / RW）当成一整段用 `MmuFlags::User | R | W | X` 映射。
这正是 W^X 要禁止的形状。应当逐段映射，各按自己的 `p_flags` 给权限
（段之间已经是页对齐的，`llvm-readelf -l` 显示 Align 0x1000）。

## 2. 解压 BootFS 的那块帧永不释放

`materialize_kernel_package()` 按页分配整个解压后的内核包（当前约 0.5 MB，主要是 vmours
的 ELF），而 userboot 只需要其中 776 字节。等 ELF 的字节被拷进自己的帧之后，这块缓冲就该还
回去。现在它活到系统结束。

## 3. `USER_ASPACE_RESTRICTED_SIZE` 没有使用者

`changes/260905` 第 2 条把它从 0 改成了 `(1 << 46) - USER_ASPACE_BASE`，但
`ProcessDispatcher` 里的 `PRIVATE_ASPACE_SIZE` / `SHARED_ASPACE_BASE` 目前都还是
`[[maybe_unused]]`。等 `ProcessDispatcher::create` 有实现时要一起核对。
