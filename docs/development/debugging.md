# 调试内核

## 一键

用 `ours.code-workspace` 打开这个仓库（`File > Open Workspace from File...`，或者
在 Finder 里双击它），按 **F5**，在弹出的下拉框里挑一个内核。就这两步。

> 必须从这个文件打开：调试配置和它依赖的那几个任务都写在 `ours.code-workspace`
> 里，直接 `Open Folder` 打开目录的话 VS Code 读不到它们，调试面板会是空的。

它会：编译 → 用 QEMU 启动镜像并**停在第一条指令之前** → 挂上 lldb。会话开始时机器
停在复位向量 `0xfff0`（固件都还没跑），这时候把断点打上，再按继续（F5）——**最早期的
启动代码也能断下来**，这正是要 `-S` 的原因。

结束调试会话时 QEMU 会被一并杀掉。

按 F5 会先问一句 **Which kernel?**：

| 选项 | 是什么 |
|---|---|
| `kernel.dev.dbg_1` | 默认。`KERNEL_DEBUG_LEVEL=1`，`DEBUG_ASSERT` 和 trace/debug 日志都在 |
| `kernel.dev` | 断言和 trace 都被编译掉、并且**链进了单测**的那个镜像。一个 bug 在一个镜像里断言、在另一个里静默，所以两个都要能调 |

名字就是 `kernel_image()` 产出的镜像名，不是另起的一套别名。

`fake`（`//kernel/tests/fake-kernel` 那条 phys 链）构建里有、脚本也认，但**暂时没放进下拉框**：
`fake.obi` 现在起不来，physboot 报 `Invalid kernel package: 4`，见
`docs/repairs/260909/products.md`。修好之后把菜单项加回去就行。

调试面板的下拉框里另有一个 `Kernel: attach to a QEMU I started myself`，用于终端里已经
跑着 `./scripts/qemu-debug.sh` 的场合，它不重启 QEMU。

## 名单从哪来

**从构建里来，没人手写。**

`kernel_image()` 每产出一个镜像，就往 `out/bringup.x86/debug_images.json` 里写一行：

```json
{
  "name": "kernel.dev",
  "obi": "kernel.dev.obi",
  "symbols": "gen/kernel/products/image/dev/kernel.dev.debug.json",
  "label": "//kernel/products/image/dev:dev(//build/toolchain/ours:kernel-x86)"
}
```

`symbols` 指向的那份是一次 GN metadata walk 的结果，同时收了两个 key，缺一不可：

- `images` —— 谁被打进了镜像、打在哪。内核是 `type="kernel-elf"` 且 `destination` 为
  `"<名字>/image.elf"` 的那条；和它并列的顶层条目是 userboot 和 vDSO。
- `debug_symbol_files` —— 未 strip 的路径。`images` 给的是真正打包进去的
  `.image.elf`（已 strip），而调试信息在它那个没后缀的兄弟里。

两者**按 GN label 精确 join**，不靠去后缀猜。

`scripts/qemu-debug.sh` 读这份清单，启对应镜像，并把解析出来的 ELF 链到
`out/bringup.x86/.debug/kernel.elf`；调试器永远只打开这一个路径。于是"符号和启动的内核
对不上"这件事**没法表达**。

这么做的理由是它已经错过一次。之前每个内核一份配置，要在两处分别写下镜像和 ELF，而这
两者**不是机械对应的**：`kernel.dev.obi` 装的是 `vmours.with-tests` 而不是 `vmours`，
`fake.obi` 装的干脆不是 vmours。那份 `dev` 配置指着 `vmours`，两个二进制里 `start_kernel`
差 `0xE000`（56 KiB）——断点全落在不相干的代码上；更糟的是 dbg.0 的 `vmours` 已经不是
构建目标了（`ninja -t query` 报 unknown），是个不会再被重建的陈旧文件。

下拉框那两行还是手写的——VS Code 没法动态生成 `pickString` 的选项。但它只写名字，
**写错了脚本会把构建里真有的名字列出来**：

```
$ ./scripts/qemu-debug.sh bogus
qemu-debug: unknown image 'bogus' (the build offers: fake, kernel.dev, kernel.dev.dbg_1)
```

不需要任何安装。调试适配器是 `llvm-vs-code-extensions.lldb-dap` 扩展（已装），它经
`xcrun` 找到系统的 `lldb-dap`；QEMU、GRUB、clang 全部来自 `//prebuilt`。

## 为什么是「停在第一条指令之前」

QEMU 用 `-S` 启动。这不是保守，是必要的：等到一个**正在运行**的内核可以被挂上时，
`start_kernel` 早就过去了，早期启动的一切都没法下断点。

代价是会话一开始停在固件之前，要自己按一下继续。**不要**用 `postRunCommands` 里加
`continue` 来省这一下：那串命令跑在 DAP 的 attach 请求里，早于编辑器把断点下发给适配器，
省掉的一下换来的是早期断点被跑过去。

## 为什么是 attach 而不是 launch

`lldb-dap` 的 `launch` 要求 `program`，并且要把它当本地进程拉起来——QEMU 里的内核不是
本地进程。只有 `attach` 提供 `attachCommands`：一串"代替按 pid 附加"执行、并且**允许自己
建 target** 的命令。配置里那三条（`target create` / `target modules load --slide` /
`gdb-remote`）就挂在这个钩子上。

## 命令行

同一套东西不进 VS Code 也能用：

```bash
./scripts/qemu-debug.sh                 # kernel.dev.dbg_1
./scripts/qemu-debug.sh kernel.dev
```

它清掉上一次会话残留的 QEMU/lldb（**两个进程抢同一个端口会互相打死**，而活下来的不一定
是新的那个），启动 QEMU，**轮询到端口真的接受连接**才打印 ready ——不是 sleep 一个猜的
秒数：QEMU 被 exec 之前还要重建 ESP（grub-mkimage、mformat、mcopy）。

另开一个终端：

```bash
lldb -o 'target create out/bringup.x86/.debug/kernel.elf' \
     -o 'target modules load --file kernel.elf --slide 0xffffff8840000000' \
     -o 'gdb-remote localhost:1234'
```

这里不用管刚才启的是哪个内核——脚本已经把配套的 ELF 链到 `.debug/kernel.elf` 了，它也
会把真实路径打在 `qemu-debug: symbols ...` 那一行上。

换端口：`PORT=1235`。启一个构建没描述的镜像：`IMAGE=... ELF=... ./scripts/qemu-debug.sh`
——**两个必须一起给**，只给镜像不给符号正是这一整套要防的那个错。

## 那个 slide 是什么

内核链接在地址 0，而 physboot 把它加载到 `KERNEL_LOAD_BASE`。不滑动符号的话每个地址都差
64 GiB，断点会打在虚空里。

`0xffffff8840000000` = `KERNEL_ASPACE_BASE + ARCH_PHYSMAP_SIZE + GB(1)`
（见 `kernel/arch/x86/kernel/include/ours/arch/aspace_layout.hpp`）。

**它不是永远不变的**：一旦启用 KASLR，镜像会带一个最多 GB(1) 的偏移。真实值在启动日志里：

```
Reserve area k:head: [FFFFFF8840000000, FFFFFF8840007000)
```

左边那个数就是 slide。

## 不用调试器往往更快

内核不退出，所以看日志是：

```bash
( IMAGE=$PWD/out/bringup.x86/kernel.dev.dbg_1.obi ./scripts/qemu-boot.sh > /tmp/boot.log 2>&1 & \
  pid=$!; sleep 30; kill $pid 2>/dev/null; wait $pid 2>/dev/null )
grep -a "..." /tmp/boot.log
```

两点值得记住：

- **`grep` 要带 `-a`**。启动日志里有终端转义和 NUL 字节，`grep` 会把它当二进制文件**静默
  跳过**——你会以为什么都没匹配上。
- **日志末尾如果没有 `terminating on signal 15`，说明 QEMU 是自己退的**，也就是三重故障，
  不是挂死。

追一处静默停止时，临时插几行标了 `// PROBE` 的 `log::trace` 再删掉，常常比 gdbstub 快
——gdbstub 下的启动要慢上不少。
