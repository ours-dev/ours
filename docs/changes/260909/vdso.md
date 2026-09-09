# vdso 改动记录

## 打包出 vDSO 本体（2026-09-09）

**文件**
- `system/public/ours/vdso.hpp` — 新文件，vDSO 的 ABI（入口表布局、magic、version）
- `kernel/lib/userabi/vdso/vdso.S` — 新文件，入口表 + 系统调用桩
- `kernel/lib/userabi/vdso/BUILD.gn` — 新文件
- `kernel/products/image/BUILD.gn` — 挂进 `common_package_items`
- `system/public/public_headers.gni` — 登记

**症状**
不是 bug。owner 的要求：**系统调用只能通过 vDSO**。今天 userboot 自己内联了一条
`syscall`（`userboot/userboot.cpp` 的 `syscall2()`），这一整条线就是要把它拿掉。

**原因与三处取舍**

**1. 为什么它不是动态共享对象。** Zircon 的 vDSO 是 `loadable_module("libzircon")`，
因为 Zircon 的用户态**真的会动态链接它**——它的 userboot 用 elfldltl 做自重定位，从
vDSO 的 `DT_GNU_HASH` 里按名字查符号。ours 没有动态链接器，userboot 是个零重定位的
static PIE。把 vDSO 建成 `-shared` 意味着：

- 要和 `//system:user.config` 打架——它强制 `-static` 和 `-Wl,--pie`，而 ld.lld 对
  `-shared` 与两者都是**硬错误**（"-shared and -pie may not be used together"，
  "attempted static link of dynamic object"，都实测过）；
- 换来一张**没有读者**的符号表。

所以它就是一个普通的用户态 ELF，形状和旁边的 userboot 一模一样。等到有东西真的去
**链接** vDSO 而不是被内核递给它的那天，这里改成 `loadable_module`，userboot 长出重定位
过程——`kernel/lib/eftl` 已经有 `GnuHashTableView::lookup()` 那一套了。

**2. 没有动态符号表，程序怎么找到入口。** 用 **`e_entry`**：

```gn
ldflags = [ "-Wl,-e,OURS_VDSO_TABLE" ]
```

vDSO 从来不会被跳转进去，所以对它而言「入口」唯一有用的含义就是「导出表在哪」。内核
本来就要读 `e_entry` 才能加载一个镜像，于是两侧都不需要任何新机制。

**这不是我编的**：Zircon 自己也这么干，`-Wl,-e,_zx_process_exit`，注释写着让 e_entry
指向已知函数「从而不读 .dynsym 也能找到它」（`vdso/BUILD.gn:113`）。

**3. 表里存偏移，不存指针。** 没有任何东西重定位 vDSO——内核把它映射到自己挑的地址、
不做重定位过程（userboot 现在能跑正是靠零重定位）。绝对地址会是链接器猜的那个，指向别处。
偏移在任何加载地址上都正确。

这也是**表必须写在汇编里**的原因：`.long entry - OURS_VDSO_TABLE` 是链接期常量，而 C++
构不出这样的常量初始化式——vDSO 里没有启动代码可以在运行期填它。

**改法**
```asm
OURS_VDSO_TABLE:
    .long OURS_VDSO_MAGIC
    .short OURS_VDSO_VERSION
    .short OURS_VDSO_NR_ENTRIES
    .long ours_vdso_debug_write - OURS_VDSO_TABLE
```
```asm
ours_vdso_debug_write:
    mov $OURS_SYS_DEBUG_WRITE, %eax
    syscall
OURS_VDSO_SYSRET_DEBUG_WRITE:      // 被批准的返回地址
    ret
```

`OURS_VDSO_SYSRET_*` 标号对标 Zircon 的 `CODE_SYSRET_<name>_VIA_<caller>`——那里注释写着
"identifies this as an approved call site"。本轮还没有人读它（强制先用整段范围），但它
是将来收紧到逐点校验的锚。

参数不需要搬运：syscall ABI 的前六个参数就选成和 SysV 一致，正是为了让桩只有一条 trap
指令。`%rcx`/`%r11` 永远不可能是参数寄存器——SYSCALL 指令自己会毁掉它们。

**参照**
- `Zircon zircon-syscall-x86.S` 的 `zircon_syscall` 宏：`mov $num,%eax; syscall;` + 返回
  地址标号，逐条对得上。
- `Zircon vdso/BUILD.gn:113` 的 `-Wl,-e,...`：`e_entry` 当免符号表把手的先例。

**验证**
```
$ llvm-readelf -hl vdso
  Type: DYN    Entry point address: 0x270
  LOAD 0x000000 ... R     ← 入口表
  LOAD 0x000280 ... R E   ← 代码
$ llvm-readelf -r vdso
There are no relocations in this file.
$ llvm-objdump -d vdso
0000000000001280 <ours_vdso_debug_write>:
    1280: b8 00 00 00 00   movl $0x0, %eax
    1285: 0f 05            syscall
0000000000001287 <OURS_VDSO_SYSRET_DEBUG_WRITE>:
    1287: c3               retq
```
`e_entry` 处 16 字节解出来：`magic=0x4F564453  version=1  nr_entries=1
debug_write=+0x1010`，`0x270 + 0x1010 = 0x1280`，正是桩的地址。

`obit tree` 确认它在启动镜像里：
```
└── [KSTR] kernel — BootFS, 5 files
    ├── userboot (824 bytes)
    ├── vdso (792 bytes)
    ...
```
QEMU 实测无回归。

**故意没做**
- **还没有人加载它**。这一步只把它造出来并打进镜像；映射、传址、改造 userboot、以及
  强制，是接下来的三步。
- **RW 段还在**（`FileSiz 0x90 / MemSiz 0xd78`）。那是 static PIE 链接留下的
  `.dynamic` 和 bss，vDSO 用不到。Zircon 的链接脚本对此有
  `ASSERT(SIZEOF(.nodata) == 0)` 硬断言；这里要同样的保证得先有用户态的链接脚本，
  记为待办而不是现在造一个文件类。

## 内核映射 vDSO，userboot 改走它（2026-09-09）

**文件**
- `kernel/lib/userabi/userboot.cpp` — `load_image` 泛化成 `map_elf_image`，新增 `load_vdso`
- `kernel/lib/userabi/userboot/userboot.cpp` — **内联 `syscall` 删除**，改走 vDSO 入口表
- `kernel/object/include/ours/object/process_dispatcher.hpp` / `.cpp` — 记录 vDSO 代码范围

**症状**
无回归。这一步把 userboot 自己那条 `syscall` 拿掉。

**原因**

**加载器泛化而不是复制一份。** 两个镜像都是零重定位的 static PIE，差别只有三样：放在哪、
什么权限、叫什么名字。所以 `map_elf_image(bytes, root_vma, where, flags, name, out)` 一个
函数管两个，`load_userboot` 和 `load_vdso` 只是它的两个薄封装。

**vDSO 绝不可写**：`User | Readable | Executable`，没有 `Writable`。它是地址空间里唯一被
允许进内核的代码，一个能改写它的程序就能在内核唯一信任的那段范围里放下一条 trap。
（userboot 自己的镜像目前仍是 RWX 一整段映射——但那**不会**破坏这条规则：程序把
`0f 05` 写进自己的正文再执行，返回地址仍在 vDSO 之外，正是内核要拒绝的。）

**入口表要校验，不能只信 `e_entry`。** `e_entry` 只是一个承诺。来自另一棵树的 vDSO、
或者表挪了位置的 vDSO，会变成一次朝着「那里恰好是什么」的间接调用——在 ring 3，且没有
任何办法说出哪里错了。所以内核查 magic 和 version，userboot 在调用前**再查一遍**：

```cpp
if (vdso == nullptr || vdso->magic != OURS_VDSO_MAGIC ||
    vdso->version != OURS_VDSO_VERSION || vdso->nr_entries < OURS_VDSO_NR_ENTRIES) {
    for (;;) { __asm__ volatile("pause"); }
}
```

两侧都查，是因为两侧能表达的失败不一样：内核能打日志，userboot 除了它正要够到的那个
出口之外没有任何输出，只能停住。

**内核读表读的是自己那份，不是用户映射。** 那个映射属于**另一个**地址空间；从这里读一个
用户地址读到的是**当前**地址空间在那里的东西。

**范围记在进程上，不是全局。** 它是一个地址空间的属性；系统调用路径经由发生 trap 的那个
线程去够它。今天只有一个进程，但全局变量在有第二个进程的那一刻就是谎话，而且会以最难看的
方式被发现。

**改法（userboot 侧）**

改前
```cpp
auto syscall2(usize number, usize arg0, usize arg1) -> usize {
    usize result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(arg0), "S"(arg1)
                     : "rcx", "r11", "memory");
    return result;
}
...
syscall2(OURS_SYS_DEBUG_WRITE, reinterpret_cast<usize>(MESSAGE), sizeof(MESSAGE) - 1);
```
改后
```cpp
extern "C" [[noreturn]]
auto _start(ours::VdsoEntryTable const *const vdso) -> void {
    ...
    auto const debug_write = vdso_entry<DebugWriteFn>(vdso, vdso->debug_write);
    static_cast<void>(debug_write(MESSAGE, sizeof(MESSAGE) - 1));
```

`_start` 的参数不是装饰：内核把表地址放进第一个启动参数，`arch_build_uspace_irqframe`
把它放进 `%rdi`——正是 SysV 约定里第一个形参所在的位置，所以**声明它就足以收到它**。
没有动态链接器，没有符号查找，也没有把地址烘进二进制。

**参照**
`Zircon userboot` 的 `_start(zx_handle_t bootstrap, const void *vdso)`：vDSO 基址同样经
第二个启动参数寄存器到达。差别是 Zircon 接着跑 `StaticPieSetup` 做自重定位并按名字查符号；
这里表就是 ABI，所以那一步不存在。

**验证**
```
$ llvm-objdump -d userboot | grep -c syscall
0
$ llvm-objdump -d vdso | grep -c syscall
1
```
**userboot 里一条 `syscall` 都没有了，vDSO 里恰好一条。**

QEMU 实测：
```
userboot: vdso mapped [A00000, A03000), table at A00270, 1 entries
userboot: hello from ring 3
```
字符串照常打出，也就是说 user → vDSO → 内核 → 返回 这条路整个走通了。

**故意没做**
- **还没有强制**。内核目前记下了范围但还没检查；下一条改动才让「只能通过 vDSO」从约定
  变成内核执行的规则。
- **vDSO 地址是固定的**（`USER_ASPACE_BASE + MB(8)`）。它是地址空间里最值得随机化的映射
  ——唯一能进内核的代码——所以这个常量是「等有 ASLR 时按进程放置」的占位，不是结论。

## 强制：非 vDSO 发起的 syscall 一律拒绝（2026-09-09）

**文件**
- `kernel/lib/syscall/table.cpp` — 新增 `syscall_dispatch()`，唯一的门
- `kernel/arch/x86/kernel/syscall.S` — 改为 `call syscall_dispatch`，汇编里的钳制退役
- `kernel/include/ours/task/thread.hpp` — 新增只读 `user_thread()` 访问器
- `kernel/object/include/ours/object/thread_dispatcher.hpp` — 新增 `process() const`

**症状**
这一条之前，「系统调用只能通过 vDSO」只是**约定**：userboot 里没有 `syscall` 指令，
但没有任何东西阻止它有一条。现在是内核执行的规则。

**原因与取舍**

**「其它指令」这一半是天然成立的，而且是查证过的**，不是假设：

- IDT 的每一项都是 `type_attr = 0x8E`，即 **DPL=0**（`kernel/arch/x86/kernel/idt.cpp:21`），
  所以 ring 3 发 `int N` 只会得到 #GP，摸不到任何门；
- SYSENTER 的 MSR **全树从未被写入**（`IA32Sysenter` 零引用），`IA32_SYSENTER_CS` 为 0 时
  ring 3 执行 `sysenter` 同样 #GP。

所以 `syscall` 是唯一的入口，强制只需覆盖 `x86_syscall` 一条路径。

**检查放在哪。** 三个位置：

| 位置 | 否掉的理由 |
|---|---|
| `syscall.S`，`call` 之前 | 汇编够不到当前线程——`Thread::Current` 是 `mm::CpuLocal::Current<task::Thread*>` 的模板静态量，名字被 mangle 过，汇编叫不出来；X86PerCpu 里也没有线程指针 |
| 每个 shim 开头各查一次 | 加一个 syscall 就要记得加一次检查。会被忘记的规则不是规则 |
| **一个 C++ 漏斗，表只能经它索引**（选中） | 新增一个 syscall 就是新增一个表项，而表项**除了经过这个函数无法抵达** |

于是 `syscall.S` 从「自己索引 `SYSCALL_TABLE`」变成 `call syscall_dispatch`。汇编里那条
钳制 `%rax` 的 `cmov` 也一并搬进去了——一扇门用两种语言各守一半，比两半都在同一处要难读，
而且汇编那半本来就解决不了 vDSO 这一半。

**`frame->ip` 是 `syscall` 指令之后的地址**（SYSCALL 把它停在 `%rcx` 里），所以接受范围
必须覆盖到最后一条指令**之后一字节**。这一点成立：映射按页粒度，镜像补齐到页边界，最后
一条指令之后总还有东西。

**没有用户线程的线程一律拒绝。** 内核线程不执行 `syscall`——它直接调函数。对一个认不出
调用者的情形，拒绝是安全的方向。

**拒绝而不是杀掉。** Zircon 抛 policy exception，那才是终点——一个伪造 trap 的程序，它的
下一条指令不该再被信任。但这里还没有异常机制，而一个调用方无法忽略的错误码，已经是
「规则」和「愿望」的分界。

**参照**
`Zircon VDso::ValidSyscallPC`，在 `do_syscall()` 的同一位置被查。Zircon 的更细：它比对的是
**被批准的返回地址集合**（那些 `CODE_SYSRET_*` 标号），所以连「跳进 vDSO 中段」都伪造不了
调用点。本树的 vDSO 已经放好了同名标号（`OURS_VDSO_SYSRET_*`），缺的是把它们的地址送到
内核的构建期通路。范围检查是诚实的子集：**任何来自 vDSO 之外的 trap 都被拒绝**，这正是
被要求的性质。

**验证——这一条是靠对照实验，不是靠阅读**

临时给 userboot 加一条裸 `syscall`（探针）：同一个调用号、同样的参数、同样的字符串，
唯一的差别是 trap 从它自己的正文发起而不是从 vDSO。

```
[info] userboot: hello from ring 3
[warn] syscall: refused a trap from 201303, which is outside the vDSO
```

- 经 vDSO 的那次：字符串打出来了；
- 直接发起的那次：**被拒绝**，`FORBIDDEN` 一个字都没出现。

`0x201303` 在 userboot 镜像（映射于 `0x200000`）内、在 vDSO（`[A00000, A03000)`）之外，
与设计一致。探针已删除，删除后 `llvm-objdump -d userboot | grep -c syscall` 回到 **0**，
两个镜像各打出一次 `hello from ring 3`、零次拒绝。

**故意没做**
- **没有把这个负向用例做成常驻测试**。要常驻就得让 userboot 永远带着一条 `syscall`
  指令，那与「userboot 里零条」自相矛盾。真要有常驻回归，需要一个专门的测试程序，
  以及一条能把结果报出来的通路。
- **没有收紧到逐点校验**（Zircon 的 `CODE_SYSRET_*` 集合）。标号已经在 vDSO 里了；
  差的是构建期把地址交给内核的机制。
- **没有 kill**。见上，等异常机制。
