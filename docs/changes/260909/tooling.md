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

## 调试配置从 launch 改成 attach：装着的 lldb-dap 根本没有那两个键（2026-09-09）

**文件**
- `ours.code-workspace` — 三个配置 `"request"` 由 `launch` 改为 `attach`，
  `targetCreateCommands` + `processCreateCommands` 合并为 `attachCommands`，
  去掉 `stopOnEntry`
- `docs/development/debugging.md` — 更正"自动放行"的说法，新增「为什么是 attach」

**症状**
在真实 VS Code 里按下启动按钮，弹出模态框：

```
executable doesn't exist: '(empty)'
[Open 'launch.json']  [Cancel]
```

调试会话根本起不来。`kernel: build` 和 `kernel: qemu debug (dbg.1)` 两个任务都正常跑完
（QEMU 起来了、gdbstub 也在 1234 上听着），死在适配器这一步。

**原因**
`targetCreateCommands` / `processCreateCommands` 这两个键，**这台机器上装的 lldb-dap
里不存在**。查扩展自己的 schema：

```
$ python3 -c '...package.json...'
launch required: ['program']
   props: [..., 'launchCommands', ..., 'program', ..., 'stopOnEntry', ...]
attach required: None
   props: ['attachCommands', ..., 'gdb-remote-port', 'program', 'postRunCommands', ...]
```

`launch` 的必填项只有一个 `program`，而且整份属性表里没有 `targetCreateCommands`。
适配器读不到 `program`，于是拿空串去 `stat`，报 `'(empty)'`。扩展是
`llvm-vs-code-extensions.lldb-dap-0.4.1`，底下是
`/Library/Developer/CommandLineTools/usr/bin/lldb-dap`（LLVM 21）。

这个错**上一次没被发现**，原因值得记下来：上一次的验证是

```bash
xcrun lldb --batch -o 'target create ...' -o 'target modules load --slide ...' \
                   -o 'gdb-remote localhost:1234' -o 'breakpoint set --name start_kernel' ...
```

——它直接驱动 lldb，**把 DAP 适配器整个绕过去了**。那几条命令本身没问题（现在也还是
这三条），有问题的是"VS Code 用什么键把它们交给适配器"。验证走的路径和用户走的路径
不是同一条，于是验证通过而功能不通。上一节的改动记录里写了"没有在真实 VS Code 里按过
F5"，这一条就正好落在没验的那块。

**改法**
改用 `attach`。扩展对 `attachCommands` 的原话是：

> Custom commands that are executed instead of attaching to a process ID or to a
> process by name. These commands **may optionally create a new target** and must
> perform an attach.

正好就是这里要的——没有 pid 也没有进程名，"进程"是一个 TCP 端口上的 gdbstub；建 target
这件事也得自己干。三条命令原封不动搬进去：

改前：

```jsonc
{
    "name": "Kernel: debug (dbg.1, asserts on)",
    "type": "lldb-dap",
    "request": "launch",
    "targetCreateCommands": [
        "target create ${workspaceFolder}/out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours"
    ],
    "processCreateCommands": [
        "target modules load --file vmours --slide 0xffffff8840000000",
        "gdb-remote localhost:1234"
    ],
    "stopOnEntry": false
}
```

改后：

```jsonc
{
    "name": "Kernel: debug (dbg.1, asserts on)",
    "type": "lldb-dap",
    "request": "attach",
    "attachCommands": [
        "target create ${workspaceFolder}/out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours",
        "target modules load --file vmours --slide 0xffffff8840000000",
        "gdb-remote localhost:1234"
    ]
}
```

`stopOnEntry` 一并去掉——它只在 `launch` 的属性表里，`attach` 没有这个键。行为上也不需要：
QEMU 带 `-S`，`gdb-remote` 连上时机器就停在复位向量 `0xfff0`，会话天然是"停着"开始的。

被否掉的方案：`"postRunCommands": ["continue"]`，为了保持上一版文档里"自动放行到第一个
断点"的手感。否掉的理由是时序——扩展对 `postRunCommands` 的定义是"attach 成功、尚未自动
继续之前"执行，也就是**跑在 DAP 的 attach 请求里，早于编辑器把断点下发给适配器**。加了它，
早期断点会被直接跑过去，而这套东西存在的全部理由就是能断在早期启动代码上。所以改的是
文档而不是配置：会话停在 `0xfff0`，打断点，然后按继续。

另一个可用但没选的形状是 `"gdb-remote-port": 1234` + `"initCommands"` 放 slide。没选是因为
它把"连哪儿"和"怎么摆符号"拆成了两个机制，而 `attachCommands` 三条命令就是命令行里验证过
的那三条，一一对应，出问题时好比对。

**参照**
无，本项目自有设计。依据是 `llvm-vs-code-extensions.lldb-dap` 扩展 `package.json` 里
`configurationAttributes.attach` 的 schema 与 `attachCommands` 的描述文本。

**验证**
这次是**在真实 VS Code 里点的按钮**（computer-use 背景控制，VS Code 只给到 click 层级）：

1. `code ours.code-workspace` 打开，标题栏 `ours.code-workspace — ours (Workspace)`，
   状态栏显示 `Kernel: debug (dbg.1, asserts on) (workspace)`——**括号里的 `workspace`
   是 VS Code 自己标的配置来源**，上一节"配置搬进工作区文件"到此才算真的验过。
2. 点 Run and Debug 的启动按钮，改前：弹 `executable doesn't exist: '(empty)'`。
3. 改后再点，Debug Console：

   ```
   (lldb) target create /Users/.../kernel_x86.dbg.1/obj/kernel/products/bin/vmours
   (lldb) target modules load --file vmours --slide 0xffffff8840000000
   (lldb) gdb-remote localhost:1234
   Process 1 stopped
   * thread #1, stop reason = signal SIGTRAP
       frame #0: 0x000000000000fff0
   Target 1: (vmours) stopped.
   ```

   调试工具条出现，状态栏转成调试配色，`kernel: build` 与 `kernel: qemu debug (dbg.1)`
   两个任务都在终端面板里跑过。
4. 断点命中本身仍然是上一节 `lldb --batch` 那次的证据（`start_kernel` 命中、
   `c = 'C'` 可读）。**没能在 UI 里打上断点**：VS Code 只给到 click 层级，编辑器沟槽
   的点击是 canvas 命中，背景 AXPress 打不进去；`Run > Toggle Breakpoint` 菜单同样被
   click 层级挡掉。DAP 这一侧的 `setBreakpoints` 因此**没有实测**。

**故意没做**
- **没有加 `postRunCommands: ["continue"]`**，理由见上，改的是文档。
- **没有把 `stopOnEntry` 换成 attach 侧的等价物**——没有等价物，`-S` 已经给了同样的效果。
- **没有去掉 `program` 之外再加 `program` 字段**。target 由 `attachCommands` 第一条建，
  再给一个 `program` 就是同一件事的第二个真相来源。

## 调试配置参数化：一个配置 + 内核下拉框，镜像与符号的配对收归脚本（2026-09-09）

**文件**
- `scripts/qemu-debug.sh` — 接受变体名，`case` 表同时给出镜像和 ELF，把 ELF 发布到
  `out/bringup.x86/.debug/kernel.elf`
- `ours.code-workspace` — 三个 launch 配置收成一个 + `${input:kernel}` 下拉框；
  两个 qemu 任务收成一个
- `docs/development/debugging.md` — 改用下拉框的说法，命令行小节改用 `.debug/kernel.elf`
- `docs/repairs/260909/products.md` — 新建，记 `fake.obi` 起不来

**症状**
表面上是"每加一个内核就要手写一份配置"。底下是一个已经发生的错：**那份 `dev` 配置指着
一个不存在于构建图里的陈旧二进制**。

```
$ ninja -C out/bringup.x86 -t query kernel_x86.dbg.0/obj/kernel/products/bin/vmours
ninja: error: unknown target 'kernel_x86.dbg.0/obj/kernel/products/bin/vmours',
       did you mean 'kernel_x86.dbg.1/obj/kernel/products/bin/vmours'?

$ ls -la out/bringup.x86/kernel_x86.dbg.0/obj/kernel/products/bin/vmours*
... Sep  2 20:28  vmours              <- 一周前的残留，不会再被重建
... Sep  9 07:28  vmours.with-tests   <- 镜像里真正装的那个
```

打包器自己的清单说得很清楚：

```
$ grep image.elf out/bringup.x86/gen/kernel/products/image/dev/ours.fini
vmours/image.elf=kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests.image.elf
```

后果可以量出来。同一个 `start_kernel`，两个二进制里差 `0xE000`（56 KiB），而且行号都对不上：

```
# 配置指向的 vmours（陈旧）
Breakpoint 1: where = vmours`::start_kernel(...) + 12 ... at main.cpp:69:9,
              address = 0xffffff88400680bc
# 镜像实装的 vmours.with-tests
Breakpoint 1: where = kernel.elf`::start_kernel(...) + 9 ... at main.cpp:105:9,
              address = 0xffffff88400760b9
```

也就是说：用旧的 `dev` 配置调试，每一个断点都落在不相干的代码上，而且一切看起来都正常
——lldb 不会抱怨，它只是在按一份错的地图工作。

**原因**
一份"每个内核一个配置"的方案，必须在**两个地方分别**写下镜像和 ELF。而这两者的对应
**不是机械的**：

| 镜像 | 内核 ELF |
|---|---|
| `kernel.dev.dbg_1.obi` | `kernel_x86.dbg.1/.../vmours` |
| `kernel.dev.obi` | `kernel_x86.dbg.0/.../vmours.**with-tests**` |
| `fake.obi` | `kernel_x86.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel` |

`dev` 那一行是从 `//kernel/products/image/dev/BUILD.gn` 的
`vmours.with-tests(//build/switch/set/dbg.0:...)` 来的——换句话说，从目录名和镜像名都
推不出来，只能去读 BUILD.gn 或者读 `.fini`。凡是要人把两件事分别写下来、而它们之间又
没有机械关系的地方，早晚会脱节；这次只是脱节得比较早。

**改法**
把"选哪个内核"这件事收成一次决策，放在启动镜像的那个脚本里。

`scripts/qemu-debug.sh` 改前只认一个 `IMAGE` 环境变量，符号是调用者的事：

```sh
OUT=${OUT:-$ROOT/out/bringup.x86}
IMAGE=${IMAGE:-$OUT/kernel.dev.dbg_1.obi}
PORT=${PORT:-1234}
```

改后，变体名同时决定镜像和 ELF，两者一起校验、一起生效：

```sh
VARIANT=${1:-${VARIANT:-dbg.1}}
case "$VARIANT" in
    dbg.1) IMAGE=$OUT/kernel.dev.dbg_1.obi
           ELF=$OUT/kernel_x86.dbg.1/obj/kernel/products/bin/vmours ;;
    dev)   IMAGE=$OUT/kernel.dev.obi
           ELF=$OUT/kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests ;;
    fake)  IMAGE=$OUT/fake.obi
           ELF=$OUT/kernel_x86.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel ;;
    *)     echo "qemu-debug: unknown variant '$VARIANT' ..." >&2; exit 2 ;;
esac
[ -f "$IMAGE" ] || { echo "... build it first" >&2; exit 1; }
[ -f "$ELF" ]   || { echo "... build it first" >&2; exit 1; }
mkdir -p "$OUT/.debug"
ln -sfn "$ELF" "$OUT/.debug/kernel.elf"
```

于是调试器那边不需要知道启的是哪个内核，永远开同一个路径：

```jsonc
// 改前——三份配置，每份自己写死一条路径
"attachCommands": [
    "target create ${workspaceFolder}/out/bringup.x86/kernel_x86.dbg.1/obj/kernel/products/bin/vmours",
    "target modules load --file vmours --slide 0xffffff8840000000",
    "gdb-remote localhost:1234"
]

// 改后——一份配置
"attachCommands": [
    "target create ${workspaceFolder}/out/bringup.x86/.debug/kernel.elf",
    "target modules load --file kernel.elf --slide 0xffffff8840000000",
    "gdb-remote localhost:1234"
]
```

任务那边同样从两个收成一个，变体由下拉框给：

```jsonc
{
    "label": "kernel: qemu debug",
    "command": "./scripts/qemu-debug.sh ${input:kernel}",
    ...
},
// 与 "tasks" 顶层的：
"inputs": [{ "id": "kernel", "type": "pickString", "description": "Which kernel?",
             "default": "dbg.1",
             "options": [ {"label": "asserts and trace on", "value": "dbg.1"},
                          {"label": "asserts compiled out, unit tests linked in", "value": "dev"} ] }]
```

几处不明显但都是查过 VS Code 1.136.2 自身实现（`workbench.desktop.main.js`）后定的：

- **`inputs` 放在 `tasks` 段里而不是 `launch` 段里**。`resolveInputs()` 按 section 取
  `launch.inputs` / `tasks.inputs`，两份互不相通；写在工作区根一级会被静默忽略。这里
  只有任务的 `command` 引用它，所以归 `tasks`。
- **一次只弹一次**。`DebugService.createSession` 先解析 launch config 的变量、再跑
  preLaunchTask，两次独立的 `resolveWithInteraction`，section 不同、答案不共享——**同一个
  `${input:}` 被 launch 配置和它的 preLaunchTask 同时引用就会问两遍**。把选择收进脚本，
  顺带也就只剩一次询问。
- **选项标签不重复 value**。`showUserInput` 里 `label: v ? \`${v}: ${f}\` : f`，即渲染成
  `"<label>: <value>"`；写成 `"dbg.1 -- asserts on"` 会显示成 `dbg.1 -- asserts on: dbg.1`。
- **`default` 只是预选**，不跳过弹窗；三种 input 都没有非交互开关。
- **选项列表没法动态生成**——`pickString.options` 是静态数组，`"type":"command"` 只能返回
  字符串，不装第三方扩展做不到。所以这是一份手写菜单，而它只写变体名、不写路径，路径归表。

symlink 的位置也是有讲究的：**发布在两个存在性检查之后、起 QEMU 之前**。缺 ELF 时如果放行，
QEMU 会正常启动（镜像里装的是 strip 过的 `*.image.elf`，不需要这个未 strip 的兄弟），
失败推迟到 attach，表现成"断点解析不了"——看起来像调试器坏了而不是像没构建，而且那时端口
已占、QEMU 已起，收拾更贵。反过来，**失败时不删旧 link**：终端里可能正跑着上一次的 QEMU，
那条 link 对它仍然是对的，`Kernel: attach to a QEMU I started myself` 靠的就是它。

被否掉的方案：**两个 `${input:}`，一个给任务选镜像、一个给 launch 配置选 ELF**。否掉是因为
它把出错的那一列（`dev → vmours.with-tests` 这个非机械映射）原样搬回 JSON 里，而且要在
`launch.inputs` 和 `tasks.inputs` 里各写一遍选项、问两遍；还救不了那个没有 preLaunchTask
的 attach 配置——它无从知道终端里跑的是哪个变体。symlink 让它自动正确。

**参照**
无，本项目自有设计。VS Code 侧依据是 1.136.2 的 `configurationResolverSchema` /
`resolveInputs` / `showUserInput` 实现，以及
https://code.visualstudio.com/docs/reference/variables-reference#_input-variables 。

**验证**
1. 两个菜单项各完整启动一次，符号链接指向和断点地址都对上：

   ```
   dbg.1  -> kernel_x86.dbg.1/obj/kernel/products/bin/vmours              address = 0xffffff884008a0a9
   dev    -> kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests   address = 0xffffff88400760a9
   ```
2. `fake` 也跑通了脚本这一侧（link 指向 `fake-kernel`，`start_kernel` 正确地报
   `no locations (pending)`——那个二进制里本来就没有这个符号，说明拿到的确实是它的符号
   而不是 vmours 的）。但镜像本身起不来，见下。
3. 未知变体：`./scripts/qemu-debug.sh bogus` → `unknown variant 'bogus'`，退出码 2。
4. 半个覆盖：`IMAGE=... ./scripts/qemu-debug.sh dev` → `IMAGE and ELF override the table
   together, or neither`，不启动。完整覆盖 `IMAGE=... ELF=... ` 正常生效。
5. `sh -n scripts/qemu-debug.sh` 通过；工作区文件去注释后 `json.loads` 通过。
6. **下拉框本身没有在 VS Code 里点过**：VS Code 窗口此刻在另一个全屏 Space 上，
   background 控制送不进点击。上面第 1 条覆盖了它按下去之后会执行的全部命令，没覆盖的是
   VS Code 弹出 quick pick 并把选中值代入 `${input:kernel}` 这一步。

**故意没做**
- **没有把 `fake` 放进下拉框**。`fake.obi` 现在根本起不来（physboot:
  `Invalid kernel package: 4`），见 `docs/repairs/260909/products.md`。脚本里的映射留着——
  它是对的，也只有一行；菜单里不放，是因为一个点下去黑屏的选项比没有这个选项更糟。
- **没有加"拿 `.fini` 校验这张表"的断言**。形状是现成的：

  ```sh
  grep -qF "/image.elf=${ELF#$OUT/}.image.elf" "$FINI" || exit 1
  ```

  它恰好能检查引发这次改动的那一类 bug。没加是因为它把一处硬编码换成了另一处（`gen/`
  下的清单路径同样可能被 GN 挪），而表现在只有三行。等表长到第四个变体、或者再被咬一次，
  就该加了。
- **没有按 `PORT` 区分 symlink**。`PORT=1235` 起第二个变体会覆盖同一个
  `.debug/kernel.elf`。旧的三配置方案有完全相同的缺陷（三份都写死 `localhost:1234`），
  不是这次引入的回归。
- **没有动 `.vscode/tasks.json` 里那些过期的 GN 任务**。第三次说同一句话：那是另一件事。

## 调试名单改由 `kernel_image()` 生成，脚本不再自带映射表（2026-09-09）

**文件**
- `kernel/kernel_image.gni` —— 每个镜像旁边生成一份 `<镜像名>.debug.json`
- `scripts/qemu-debug.sh` —— 删掉 `case` 表，改读那份清单；`IMAGE`/`ELF` 覆盖提到最前
- `ours.code-workspace` —— 下拉框的值改成构建产出的镜像名
- `docs/development/debugging.md` —— 新增「名单从哪来」

**症状**
上一节把镜像↔ELF 的配对从三份调试配置收进了脚本里的一张 `case` 表。表还是手写的，
所以还是会漂——上一节记的那个错（`dev` 指向一个已经不是构建目标的 `vmours`）能再发生
一次，只是换个文件。新增一个 `kernel_image()` 也要记得回来加一个 arm。

**原因**
这张表**本来就不该由人写**：它的两列都是构建自己算出来的。镜像名来自
`kernel_image()` 的 `output_name`，内核 ELF 来自 `kernel_package` deps 里那个带
switchset 工具链的 label——两者都在 GN 的图里，只是从来没人把它们写到一个地方。

而且这份信息**已经以 metadata 的形式存在**了。`kernel/phys/kernel_elf_image.gni:126`
的 `binaries` 键每个 ELF 一条：

```gn
binaries += [
  {
    cpu = current_cpu
    debug = rebase_path(_debug_path, root_build_dir)   # 未 strip，带调试信息
    environment = toolchain_environment                # kernel / kernel.phys / user
    label = get_label_info(":$target_name", "label_with_toolchain")
    type = _binary_type
  },
]
```

`gn meta` 实测，一个镜像走出来正好四条，`environment` 把它们分得干干净净：

```
debug = "kernel.phys_x86.phys.dbg_0/obj/kernel/phys/physload"          environment = "kernel.phys"
debug = "user-x86/obj/kernel/lib/userabi/userboot/userboot"            environment = "user"
debug = "user-x86/obj/kernel/lib/userabi/vdso/vdso"                    environment = "user"
debug = "kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests"   environment = "kernel"
```

内核就是 `environment == "kernel"` 那条，一次筛选，不需要拼字符串也不需要 join。

**改法**
`kernel_image()` 里加一个 `generated_file`，把这次走图的结果落在 `.obi` 旁边：

```gn
  generated_file("$target_name.debug") {
    forward_variables_from(invoker, [ "testonly" ])
    visibility = [ ":*" ]
    deps = invoker.deps
    data_keys = [ "binaries" ]
    outputs = [ "$_output_dir/$_output_name.debug.json" ]
    output_conversion = "json"
  }
```

脚本那边，改前是一张手写表：

```sh
case "$VARIANT" in
    dbg.1) IMAGE=$OUT/kernel.dev.dbg_1.obi
           ELF=$OUT/kernel_x86.dbg.1/obj/kernel/products/bin/vmours ;;
    dev)   IMAGE=$OUT/kernel.dev.obi
           ELF=$OUT/kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests ;;
    fake)  IMAGE=$OUT/fake.obi
           ELF=$OUT/kernel_x86.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel ;;
    *)     echo "qemu-debug: unknown variant '$VARIANT' ..." >&2; exit 2 ;;
esac
```

改后没有表，只有一次查询：

```sh
IMAGE=$OUT/$VARIANT.obi
ELF=$(OUT="$OUT" VARIANT="$VARIANT" python3 <<'RESOLVE'
...
kernels = [e for e in json.load(open(path)) if e.get("environment") == "kernel"]
print(os.path.join(out, kernels[0]["debug"]))
RESOLVE
) || exit $?
```

「构建提供哪些镜像」也不再有第二份名单，就是一次 glob：

```
$ ./scripts/qemu-debug.sh bogus
qemu-debug: unknown image 'bogus' (the build offers: fake, kernel.dev.dbg_1, kernel.dev)
```

清单放在 `.obi` 旁边（`$_output_dir/$_output_name.debug.json`）而不是 `gen/` 深处，
是为了这一条：**glob 就能枚举，不需要索引**。

被否掉的方案 1：**`build_api_module("debug_images")` 汇总成一份顶层 `debug_images.json`**。
写出来了、也跑通了，然后否掉，两个理由。一是它要在 `//BUILD.gn` 里维护一张新的 deps 表
（"mirror group('default')'s image reach"）——一个既不在 `$ours_product` 下、也不是 `fake`
的新镜像会**静默**缺席，和 `case` 表漏一个 arm 是同一种失效，只是离脚本更远、更难发现；
真相来源从 1 个变成 2 个而不是 0 个。二是把 metadata 挂到 `obi()` 目标上会触发
`obi.gni` 的双重转发（那是个真 bug，已单独修掉，见 `260909/obi.md`），而放在 `.obi`
旁边根本不经过 `obi()`。

被否掉的方案 2：**读现成的 `.fini`**。不成立。`.fini` 给的是 strip 过的
`<x>.image.elf`，要砍后缀才拿到带符号的兄弟；而 `ours_strip_kernel_binaries` 一旦翻成
true，`link_output_suffix` 变成 `.debug`，**strip 产物的文件名不变**——砍后缀会在那一刻
静默指向一个没有符号的文件。再者 `.fini` 自己的路径也推不出来（`dev/ours.fini` vs
`fake/fake-kernel.fini`，basename 来自 `kernel_package` 的目标名），要读它先得有一张
variant→fini 的表，正是要删的那张。

被否掉的方案 3：**同时走 `images` + `debug_symbol_files` 两个键，按 label join**。第一版
就是这么写的，能用，但比需要的复杂：`images` 只是为了认出「哪个是内核」，而 `binaries`
的 `environment` 字段本来就回答了这个问题。少一个键、少一次 join、少十行注释。

**参照**
Fuchsia `//BUILD.gn` 的 `build_api_module("binaries")` / `("images")` 收的就是这几个键；
`environment` 字段对应 Fuchsia 的 `toolchain_environment`。本项目这次只做到「每个镜像
一份、放在镜像旁边」，没有做顶层索引，理由见上。

**验证**
1. `gn gen` 通过，`ninja -C out/bringup.x86` 32/32 全绿。
2. 三份清单生成在镜像旁边，筛 `environment == "kernel"` 各得一条，与之前手写表**逐字相同**：

   ```
   fake               kernel_x86.dbg.0/obj/kernel/tests/fake-kernel/fake-kernel
   kernel.dev.dbg_1   kernel_x86.dbg.1/obj/kernel/products/bin/vmours
   kernel.dev         kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests
   ```
3. 两个菜单项各完整启动 + attach 一次，断点落在各自 ELF 该在的地方：

   ```
   kernel.dev.dbg_1  Breakpoint 1: ... at main.cpp:106:9, address = 0xffffff88400890a9
   kernel.dev        Breakpoint 1: ... at main.cpp:106:9, address = 0xffffff88400760a9
   ```
4. 未知名字：列出构建里真有的三个，退出码 1。
5. 逃生口：`OUT=<空目录> IMAGE=... ELF=... ./scripts/qemu-debug.sh` 能跑——**覆盖分支
   移到了查清单之前**。上一版把它放在后面，于是「启一个构建不认识的镜像」在没有清单时
   反而走不通，正好堵死在最需要它的场合。半个覆盖仍然被拒。
6. 菜单值 ⊆ 构建产出的名字（脚本化对比 `ours.code-workspace` 与 `*.debug.json`）。
7. **下拉框仍然没在 VS Code 里点过**：窗口在另一个全屏 Space，后台控制送不进点击。

**故意没做**
- **没有让下拉框自动同步**。VS Code 生成不了 `pickString` 的选项（`"type":"command"`
  只能返回字符串，且没有能跑 shell 的内建命令），不装第三方扩展做不到。所以菜单还是两行
  手写的，但它**只写名字**，写错了脚本会把构建里真有的列出来——两边不会静悄悄地各说各话。
- **没有给 `kernel_image()` 加 `debug_name` 参数**把 `kernel.dev.dbg_1` 缩成 `dbg.1`。
  短名字好看，但那是又一个人写的名字，而菜单里显示的是**将要启动的那个 `.obi` 的真名**，
  这件事本身有价值。
- **没有收 `physload` / `physboot` / `userboot` / `vdso`**。清单里有它们（`environment`
  分别是 `kernel.phys` 和 `user`），但它们各自需要不同的 `--slide`，而 phys 那两个的
  加载地址是运行期由内存图选的、编译期不是常量（只能从串口日志的
  `boot-shim: physload at 0x...` / `Loaded phys module ... at 0x...` 读）。等真要调它们时
  再说。
