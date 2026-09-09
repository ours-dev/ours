# tooling 改动记录

## VS Code 一键调试（2026-09-09）

**文件**
- `.vscode/launch.json` — 新文件，三个调试配置
- `.vscode/tasks.json` — 新增 build / qemu-debug / stop 三个任务
- `scripts/qemu-debug.sh` — 新文件，带 gdbstub 的启动
- `docs/development/debugging.md` — 新文件

**症状**
不是 bug。此前调试内核要手工串起四步：编译、用 `-s -S` 起 QEMU、盯着它起来、再用一份
手写的 lldb 脚本挂上去，而且每次都要记住那个 slide 常量。

**原因与几处取舍**

**为什么要一个脚本，而不是把命令塞进 tasks.json。** 有一件事 tasks.json 表达不了：
**调试器会在任务报告「就绪」的那一瞬间去连端口**，而连一个还没人监听的端口会让整次启动
失败。所以必须真的**轮询到端口接受连接**才报就绪——不能 sleep 一个猜的秒数，因为 QEMU
被 exec 之前还要重建 ESP（grub-mkimage、mformat、mcopy），那段时间不是常数。

脚本还得先清掉上一次残留的进程。这条是踩过的：**两个进程抢同一个 gdb 端口会互相打死，
而活下来的不一定是新启动的那个**。只杀真正占着端口的进程（`lsof -ti tcp:$PORT`），
旁边跑着的普通 `qemu-boot.sh` 不受影响。

**为什么用 `-S`（停在第一条指令之前）。** 不是保守：等到一个**正在运行**的内核可以被挂上
时，`start_kernel` 早已过去，早期启动代码根本没法下断点。代价是会先停在 UEFI 固件里，
所以 `stopOnEntry` 设成 `false`，让它自己跑到用户的断点——这才是「一键」。

**为什么是 lldb-dap 而不是 CodeLLDB。** `llvm-vs-code-extensions.lldb-dap` 已经装在这台
机器上，且经 `xcrun` 用系统的 `lldb-dap`——也就是本会话一路验证下来能驱动 QEMU gdbstub
的那个 lldb。选 CodeLLDB 要装扩展，并且它自带一份可能不同的 lldb。

**为什么三个配置而不是一个。** `dbg.1` 与 `dev` 的符号在不同的 switchset 目录下，而
**一个 bug 在一个镜像里断言、在另一个里静默**，两个都得能调。第三个是给「QEMU 已经在
终端里跑着」的情形，不去重启它。

**slide 是必须的**：内核链接在 0，physboot 把它加载到 `KERNEL_LOAD_BASE`，不滑动的话每个
地址差 64 GiB。注释里写明了它**不是常量**——启用 KASLR 后镜像会带最多 GB(1) 的偏移，
真实值在启动日志的 `Reserve area k:head` 那一行。

**验证**
逐段实测，不是照着文档写完就算：

1. `./scripts/qemu-debug.sh` 按顺序打出两行标记：
   ```
   qemu-debug: booting .../kernel.dev.dbg_1.obi, gdbstub on localhost:1234
   qemu-debug: gdbstub ready on localhost:1234
   ```
   `lsof -ti tcp:1234` 确认端口在监听。
2. 用 launch.json 里**逐字相同**的那几条命令挂 lldb，断点解析到源码行并命中：
   ```
   Breakpoint 1: where = vmours`::start_kernel(ours::PhysAddr) + 9
                 [inlined] ours::(anonymous namespace)::com1_put(char) at main.cpp:105:9,
                 address = 0xffffff884008a0b9
   * thread #1, stop reason = breakpoint 1.1
       frame #0: ... com1_put(c='C') at main.cpp:105:9 [inlined]
   ```
   连内联帧和实参都在，说明 slide 与调试信息都对。
3. 另一处断点（`userboot_init`）命中后 `frame variable` 能读出局部量。
4. stop 任务那条命令执行后 `lsof -ti tcp:1234` 为空；脚本被 kill 时的 trap 同样清理干净。
5. 两个 json 都用 `json.loads`（去掉注释后）验证过可解析。

**故意没做**
- **没有动 tasks.json 里既有的那些 GN 任务**。它们指向 `target/default` 和裸 `gn`，与
  当前的 `out/bringup.x86` 和 `prebuilt/gn/gn` 已经对不上了——那是另一件事，混进来会让
  这次改动没法单独回退。
- **没有 userboot / vDSO 的调试配置**。它们是用户态 ELF，被内核映射到自己挑的地址，
  要调需要另一个 slide 和一次单独的 `target modules load`；等真要调它们时再加。
- **没有把 lldb-dap 的路径写死进 settings.json**。`xcrun` 找得到，写死一台机器的路径进
  版本库是负债。
