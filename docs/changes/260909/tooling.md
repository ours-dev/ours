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

## 调试配置搬进 `ours.code-workspace`（2026-09-09）

**文件**
- `ours.code-workspace` — 新增顶层 `launch` 和 `tasks` 两节，原来的 `folders` /
  `settings` 原样保留
- `.vscode/launch.json` — 删除
- `.vscode/tasks.json` — 退回到上一节改动之前的样子，只剩既有的那几个 GN 任务
- `docs/development/debugging.md` — 「一键」一节改成先从 `ours.code-workspace` 打开

**症状**
不是 bug，是位置不对。上一节把配置放进了 `.vscode/`，但这个仓库的编辑器配置一直归
`ours.code-workspace` 管——字体、`files.exclude`、`clangd.path`、语义高亮全在那里。
两处配置并存的后果是具体的：从 `ours.code-workspace` 打开时 VS Code **不会**再去读
`.vscode/launch.json` 之外的工作区级设置合并逻辑之外的东西，而按这个项目的用法（工作
区文件里已经列了 `folders`），大家打开的就是工作区，于是"编辑器设置在这边、调试配置
在那边"是纯粹的分裂，找起来要猜。

**原因**
`.vscode/` 和 `.code-workspace` 是同一套配置的两种作用域，不是两类东西：
`.code-workspace` 支持顶层 `launch` 和 `tasks` 两节，语义与 `.vscode/launch.json`、
`.vscode/tasks.json` 完全相同，只是随工作区文件走。仓库里已经有一个被跟踪的
`ours.code-workspace`，那就是这个项目认定的"共享编辑器配置"落点；再开一个
`.vscode/launch.json` 等于凭空多一个真相来源。

**改法**
三个 `lldb-dap` 配置和四个任务整体搬到 `ours.code-workspace`，内容一字未改。

搬之前（`.vscode/launch.json` 与 `.vscode/tasks.json` 两个文件）：

```jsonc
// .vscode/launch.json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Kernel: debug (dbg.1, asserts on)",
            "type": "lldb-dap",
            ...
        },
        ...
    ]
}
```

```jsonc
// .vscode/tasks.json —— 四个新任务被塞在既有 GN 任务前面
{
    "version": "2.0.0",
    "tasks": [
        { "label": "kernel: build", ... },
        { "label": "kernel: qemu debug (dbg.1)", ... },
        { "label": "kernel: qemu debug (dev)", ... },
        { "label": "kernel: stop qemu", ... },
        { "label": "gn gen", ... },        // 既有
        ...
    ]
}
```

搬之后（`ours.code-workspace` 一个文件）：

```jsonc
{
    "folders": [ { "path": "." } ],

    "launch": {
        "version": "0.2.0",
        "configurations": [
            {
                "name": "Kernel: debug (dbg.1, asserts on)",
                "type": "lldb-dap",
                "preLaunchTask": "kernel: qemu debug (dbg.1)",
                "postDebugTask": "kernel: stop qemu",
                "targetCreateCommands": [
                    "target create ${workspaceFolder}/out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours"
                ],
                "processCreateCommands": [
                    "target modules load --file vmours --slide 0xffffff8840000000",
                    "gdb-remote localhost:1234"
                ],
                "stopOnEntry": false
            },
            ...
        ]
    },

    "tasks": {
        "version": "2.0.0",
        "tasks": [
            { "label": "kernel: build", ... },
            { "label": "kernel: qemu debug (dbg.1)", ... },
            { "label": "kernel: qemu debug (dev)", ... },
            { "label": "kernel: stop qemu", ... }
        ]
    },

    "settings": { ... 原样 ... }
}
```

`${workspaceFolder}` 在工作区文件里是**有条件**可用的：多文件夹工作区里它有歧义，
要写成 `${workspaceFolder:<name>}`。这里 `folders` 只有一项 `"."`，所以无歧义，
路径原样搬过来即可；将来真加了第二个文件夹，这几处要一起改成带名字的形式。

`.vscode/tasks.json` 是**退回**而不是重写：用 `git show <上一次提交>^:.vscode/tasks.json`
取回原文覆盖，因此 diff 是纯删除的 64 行，一行既有的 GN 任务都没碰。这一步值得说，
因为手工"把我加的删掉"很容易顺手改掉旁边的东西。

**参照**
无，本项目自有设计。VS Code 侧的依据是 workspace 文件的 `launch` / `tasks` 两节，
与 `.vscode/` 下同名文件语义一致。

**验证**
1. 两个文件去掉注释和尾逗号后 `json.loads` 均可解析；`ours.code-workspace` 里
   3 个 launch 配置、4 个任务、5 个 settings 键全在。
2. `git diff HEAD -- .vscode/tasks.json`：删除 64 行、新增 0 行。
3. 端到端跑了一遍搬家后的配置所执行的原始命令：`./scripts/qemu-debug.sh` 起来后
   `lldb` 依次执行 launch 配置里的 `target create` / `target modules load --slide` /
   `gdb-remote`，`start_kernel` 断点命中：

   ```
   Breakpoint 1: where = vmours`::start_kernel(ours::PhysAddr) + 9
                 [inlined] ours::(anonymous namespace)::com1_put(char) at main.cpp:105:9
   * thread #1, stop reason = breakpoint 1.1
   (const char) c = 'C'
   ```
4. stop 任务那条命令跑完 `lsof -ti tcp:1234` 为空。
5. **没有在真实 VS Code 里按过 F5**——这台机器上跑不了 GUI 验证。按下去会执行的每一条
   命令都在上面单独验过了，没验的是 VS Code 自己读工作区文件那一步。

**故意没做**
- **没有保留 `.vscode/launch.json` 作为"直接 Open Folder 时的兜底"**。留着就是两份会
  各自漂移的配置，而漂移的那天没人会发现——恰恰是调试器行为诡异、最不该怀疑配置的时候。
  代价是必须从工作区文件打开，这一条写进了 `docs/development/debugging.md`。
- **没有顺手修 `.vscode/tasks.json` 里那些过期的 GN 任务**（指向 `target/default` 和裸
  `gn`）。理由和上一节一样：那是另一件事。
