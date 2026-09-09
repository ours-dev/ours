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
