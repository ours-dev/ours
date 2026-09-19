# kernel/products/image 改动记录（2026-09-19）

## 示例 shell 从共享内核包零件里挪走，由镜像自己声明（2026-09-10 起，2026-09-19 改）

**文件**
- `kernel/products/image/BUILD.gn` —— `common_package_items` 去掉
  `//examples/shell`，只留 userboot 与 vDSO。
- `kernel/products/image/dev/BUILD.gn`、`kernel/products/image/dev.dbg_1/BUILD.gn`
  —— 各自的 `kernel_package("ours")` 加上 `//examples/shell`，各自的
  `kernel_cmdline("cmdline")` 写 `kernel.select.userboot=shell`。

**症状**
潜伏。`common_package_items` 是所有产品、所有镜像、连 `//tests/*` 的冒烟镜像都要
引用的共享零件，示例程序挂在那里，等于每个镜像都被迫带上一份 `//examples` 的产物；
而「到底启动哪个程序」还得靠内核在包里挑（见同日
`docs/changes/260919/userabi.md`）。

**原因**
放错了层。这个 group 的定义就是「每个镜像都要的那几样」：userboot 是内核不指定
`kernel.select.userboot` 时默认启动的程序，vDSO 是用户态唯一能进内核的那段代码——
两个都是内核自己的东西，缺了内核就没法把第一个进程跑起来。示例 shell 不是：它是
某个镜像想跑的程序，换个镜像就该换掉。

把它挂进共享零件还有个连带后果：`//examples` 成了内核包的隐性依赖，一个只想验证
打包拓扑的冒烟镜像也要先把示例程序编出来。

**改法**
共享零件恢复成只有 userboot + vDSO；想跑 shell 的镜像自己在 `kernel_package` 里
声明它，并在自己的 `kernel_cmdline` 里指名：

```gn
# kernel/products/image/BUILD.gn （改前）
deps = [
  ".../userboot(user 工具链)",
  ".../vdso(user 工具链)",
  # The example shell, which the loader starts instead of userboot.
  "//examples/shell(user 工具链)",
]

# kernel/products/image/dev/BUILD.gn （改前）
kernel_cmdline("cmdline") {
  args = []
}
```

```gn
# kernel/products/image/BUILD.gn （改后）
deps = [
  ".../userboot(user 工具链)",
  ".../vdso(user 工具链)",
]

# kernel/products/image/dev/BUILD.gn （改后）
kernel_package("ours") {
  deps = [
    "..:common_package_items",
    ".../vmours.with-tests(dbg.0 工具链)",
    "//examples/shell(//build/toolchain/ours:user-${target_cpu})",
  ]
}

kernel_cmdline("cmdline") {
  args = [ "kernel.select.userboot=shell" ]
}
```

工具链后缀不能省：`deps` 是在内核工具链上下文里求值的，不写就会用 kernel-x86 编出
一个 `-mcmodel=kernel` 的「用户程序」，要到第一条指令 #GP 才发现。

被否掉的方案：走产品级 `bootfs`（`product()` 支持 `bootfs = [ resource 目标 ]`，
对标 Fuchsia 产品装配的 `bootfs_files`）。两个原因否掉：一是 `product()` 的产品级
bootfs/cmdline 注入只作用于内联镜像条目，bringup 用的是预定义镜像
（`{ image = "//kernel/products/image/dev" }`），注入根本到不了；二是内核现在只会
读 kernel package，OBI 顶层那块 BOOTFS item 还没有消费者。等内核能读它了，再把
示例程序整体搬过去，那才是 Fuchsia 的真实分层。

**参照**
`Fuchsia //zircon/kernel/image/BUILD.gn` 的 `group("common-package-items")`：里面只有
userboot、vDSO 和 version-string.txt；镜像要换第一个用户程序，是在自己的
`kernel_package("zircon")` 里加（`zircon/kernel/image/eng/BUILD.gn` 就加了
`userboot:userboot-test-rust`），或者像
`zircon/kernel/lib/userabi/trivial-test-userboot/BUILD.gn` 那样，自建一个
`kernel_package` 放进自己的程序，再用 `kernel.select.userboot=$long_name` 指名。
那边甚至还分出了 `common-package-items.no-userboot`，专门给要换掉 userboot 的镜像。
本次没有跟进这一层：ours 这两个镜像是在 userboot 之外**追加**一个程序，不是替换它，
没有 group 会用到 no_userboot，建一个空着反而是噪音。

**验证**
- 构建：`prebuilt/ninja/ninja -C out/bringup.x86` 全过。
- `obit list out/bringup.x86/kernel.dev.obi` → `0 physload / 1 cmdline (28 字节) /
  2 kernel`，cmdline item 的 payload 正是 `kernel.select.userboot=shell`。
- QEMU 实测两个镜像，都启动到 shell：

  ```
  [info] userboot: found 'shell', 1368 bytes of ELF
  ours shell: the kernel's command registry, seen from ring 3
  ```

  `kernel.dev.obi` 与 `kernel.dev.dbg_1.obi` 表现一致。注意这条要和同日的
  `libc.md` 一起看：`memchr()` 修好之前，选项写了也读不出来。

**故意没做**
- 没动 `//kernel/products/image/fake` 和 `//tests/*` 的镜像：它们本来就不需要示例
  程序，现在共享零件里没有它了，自然也就不带了。
- 没把示例程序改成产品级 BOOTFS 资源，理由见上。
