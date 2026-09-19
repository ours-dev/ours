# vDSO 改动记录（2026-09-19）

## vDSO 改成共享对象，`.dynsym` 就是它的导出表（2026-09-19）

**文件**
- `kernel/lib/userabi/vdso/BUILD.gn` —— 目标类型改成 `loadable_module`，链接策略
  从「静态 PIE」换成「共享对象」。
- `kernel/lib/userabi/vdso/vdso.S` —— 两个 stub 去掉 `.hidden`，改名为
  `ours_debug_write` / `ours_command_list`（原先是 `ours_vdso_*`）。
- `system/BUILD.gn` —— 把静态 PIE 的链接策略从 `user.standalone` 拆出去，新增
  `user.executable`（`-static --pie -fpie`）和 `user.shared`（`-fPIC -fno-plt
  -z now --no-dynamic-linker -Bsymbolic`）。
- `build/toolchain/ours/BUILD.gn`、`build/toolchain/ours/ours_toolchain.gni`
  —— user 工具链新增 `executable_configs`，模板转发这个字段。

**症状**
潜伏，当前能跑。问题是加一个系统调用要动的地方：vdso.S 里加 stub、
`system/public/ours/vdso.hpp` 里加偏移宏和结构体字段、版本号加一、然后**每个用户
程序**在自己的 `Vdso` 类里加一个访问器，再把返回的偏移加回基址。程序调一个系统
调用之前，得先自己做一遍「动态链接」。

**原因**
把「怎么找到入口」这件事放在了程序身上。vDSO 原先是静态 PIE，没有 `.dynsym`，
所以内核把入口表的地址塞进 `%rdi`，程序照着 `u32` 偏移自己算指针——这就是一个手写
的、只有一张表那么大的动态链接器，而且每个程序各写一份。

ELF 早就有解决这件事的机制：共享对象的 `.dynsym` 加上重定位，加载器按名字绑定。
physload 那边已经是这么做的（`kernel/phys/physload_module.gni` +
`ModuleLoader::load()` 里的 `SymbolBinder`），用户态却在另起炉灶。

**改法**
这一步只动 vDSO 本身，不动加载器，也不动程序——所以行为完全不变，只是从此有了一张
可以按名字查的符号表。

改前（`BUILD.gn` 的注释自己就写着该怎么改）：
```gn
# An ordinary user-mode ELF, the same static-PIE shape as userboot beside it,
# rather than the loadable_module Zircon uses for libzircon.so.
# ...
# The day something links against the vDSO rather than being handed it, this
# becomes a loadable_module and userboot grows a relocation pass.
kernel_elf_binary("vdso") {
  visibility = [ "//kernel/products/image/*" ]
  has_patches = false
  sources = [ "vdso.S" ]
  ldflags = [ "-Wl,-e,OURS_VDSO_TABLE" ]
}
```

改后：
```gn
kernel_elf_binary("vdso") {
  visibility = [ "//kernel/products/image/*" ]
  target_type = "loadable_module"
  configs -= [ "//system:user.executable" ]
  configs += [ "//system:user.shared" ]
  has_patches = false
  sources = [ "vdso.S" ]
  ldflags = [ "-Wl,-e,OURS_VDSO_TABLE" ]
}
```

拦路的是链接策略：`//system:user.config` 是 user 工具链的全局 config，里面带着
`-static` 和 `--pie`，而 ld.lld 拒绝它们与 `-shared` 共存。所以把这两项从
`user.standalone` 拆进新的 `user.executable`，由工具链的 `executable_configs`
下发——BUILDCONFIG.gn 本来就把 `default_executable_configs` 和
`default_shared_library_configs` 分开，只是 `ours_toolchain()` 没转发那个字段，补上
即可。这样 config 落在可执行文件上，共享对象天生就没有它，不需要每个共享目标再减
一次。

stub 去掉 `.hidden`：隐藏可见性的符号不进 `.dynsym`，而 `.dynsym` 正是导出机制
本身。内部标号（`OURS_VDSO_TABLE`、`OURS_VDSO_SYSRET_*`）继续 `.hidden`——导出一个
符号等于对外承诺，这些不是。

顺手改名 `ours_vdso_debug_write` → `ours_debug_write`：`system/public/ours/syscall.hpp`
的注释里写的一直是后者，而且从现在起它是程序直接写在代码里的名字，不该带实现细节
前缀。

**参照**
`Zircon //zircon/kernel/lib/userabi/vdso`：libzircon.so 是货真价实的
`loadable_module`，程序链接它、调用 `zx_*`，userboot 用 elfldltl 在加载时绑定。
本项目内部的对照物是 `kernel/phys/physload_module.gni` 和
`ours::phys::ModuleLoader::load()`：phys 模块就是共享对象，未定义符号在加载时绑到
physload 的导出表上，`config("user.shared")` 的每一条 flag 都是照
`config("phys-module")` 抄的（`-fPIC`／`-fno-plt`／`-z now`／`--no-dynamic-linker`／
`-Bsymbolic`），理由逐条相同。

**验证**
- 构建通过。`llvm-readelf` 确认产物形状：
  ```
  Type: DYN (Shared object file)
  Symbol table '.dynsym' contains 3 entries:
     1: 00000000000012f0  8 FUNC GLOBAL DEFAULT 7 ours_debug_write
     2: 0000000000001300  8 FUNC GLOBAL DEFAULT 7 ours_command_list
  Dynamic section: SYMTAB / STRTAB / GNU_HASH / HASH，无 RELA
  ```
  没有 RELA 说明这一步还不需要加载器做重定位；`GNU_HASH`/`HASH` 都在 `PT_DYNAMIC`
  里，所以打包时的 `--strip-sections` 不影响运行期查找（节头没了，段还在）。
- QEMU 实测 `kernel.dev.obi`：vdso 从 808 字节变成 920 字节，入口表仍在 e_entry
  （日志 `vdso mapped [A00000, A03000), table at A002D8, 2 entries`），shell 照常
  打印命令表。行为零变化，正是这一步要的。

**故意没做**
- 没动入口表，也没动 `<ours/vdso.hpp>` 里的 `VdsoEntryTable`。它还在被 shell 用，
  等加载器会按名字绑定、程序改成直接调用之后再拆。
- 没给 vDSO 加 `DT_SONAME`。现在没有链接期消费者（程序还不 link 它），等到有了
  再说。
- 没碰系统调用号与 stub 的「一处描述、处处生成」问题：现在加一个系统调用仍要手写
  `syscall.hpp` 的宏和 vdso.S 的 stub。那是独立的一件事。

## 删掉 vDSO 的入口表，userboot 也改成直接调用（2026-09-19）

**文件**
- `kernel/lib/userabi/vdso/vdso.S` —— 删掉 `OURS_VDSO_TABLE` 整节。
- `kernel/lib/userabi/vdso/BUILD.gn` —— 去掉 `-Wl,-e,OURS_VDSO_TABLE`。
- `system/public/ours/vdso.hpp` —— 删掉 magic／version／偏移宏／`VdsoEntryTable`
  与那一串 `static_assert`，只留两个函数声明。
- `kernel/lib/userabi/userboot/BUILD.gn`、`userboot/userboot.cpp` —— 改用
  `user_executable()`，`_start()` 不再收参数，直接调用 `ours_debug_write()`。
- `kernel/lib/userabi/src/userboot.cpp` —— `load_vdso()` 不再校验入口表，
  `thread.start()` 不再传 `arg1`。

**症状**
无。这是把上一条换掉的旧机制清理干净——入口表已经没有读者。

**原因**
留着不删有实际代价，不只是「多了点代码」：这张表是一份**需要手工保持同步**的三处
契约（vdso.S 的 `.long` 顺序、vdso.hpp 的偏移宏、同文件的结构体字段），而
`static_assert` 只能证明后两处彼此一致，证明不了第一处。加一个系统调用时，最容易
错的恰恰是这里，而且错了要到 ring 3 跳进一个野地址才发现。删掉它，契约就只剩
`<ours/vdso.hpp>` 里的函数声明——链接器和加载器负责让它成立。

`_start` 的参数同理。它原来接的是表地址（内核放在 `%rdi`），现在程序进来时导入已经
绑好，没有什么需要在入口告诉它。`ThreadStartArgs` 的 `arg1/arg2` 保留着：下一个要
放进去的是 bootstrap channel 的 handle，Zircon 的 userboot 收的就是那个。

**改法**
改前（vdso.S）：
```asm
.section .rodata.vdso.table, "a", @progbits
.balign 8
.globl OURS_VDSO_TABLE
.hidden OURS_VDSO_TABLE
OURS_VDSO_TABLE:
    .long OURS_VDSO_MAGIC
    .short OURS_VDSO_VERSION
    .short OURS_VDSO_NR_ENTRIES
    .long ours_debug_write - OURS_VDSO_TABLE
    .long ours_command_list - OURS_VDSO_TABLE
```
改后：整节删除，`ldflags` 里的 `-Wl,-e,OURS_VDSO_TABLE` 一起删——没有东西会跳进
vDSO，它的 e_entry 从此不表示任何东西。

改前（加载器）：
```cpp
VdsoEntryTable table{};
std::memcpy(&table, bytes.data() + table_offset, sizeof(table));
if (table.magic != OURS_VDSO_MAGIC) { ... return Status::BadType; }
if (table.version != OURS_VDSO_VERSION) { ... return Status::Unsupported; }
```
改后：这段校验没有了，换成的检查更强——`bind_imports()` 发现 vDSO 没导出程序要的
名字时会直接报名字失败。版本号只能说「两个镜像对不上」，这个能说「缺的是哪个调用」。

**参照**
`Zircon` 的 vDSO 也把 e_entry 指向 `_zx_process_exit`（「so it can be found without
reading the .dynsym」），那是给**不读 .dynsym** 的场合留的后门；ours 现在读
`.dynsym`，所以这个后门没有存在的理由。

**验证**
- 构建通过，clang-format/clang-tidy 干净。
- QEMU 实测两条路径。默认程序 userboot（临时把 dev.dbg_1 的
  `kernel.select.userboot` 清空后构建、启动，验证完即还原）：
  ```
  [info]  userboot: found 'userboot', 1024 bytes of ELF
  [debug] userboot: bound 'ours_debug_write' to A012F0
  userboot: hello from ring 3
  ```
  选中的程序 shell（`kernel.dev.obi`，未改动）：`vdso mapped [A00000, A03000)`
  之后照常打印完整命令表。两个程序现在走的是同一条机制。

**故意没做**
- 没动 `OURS_VDSO_SYSRET_*` 标号。它们是内核将来校验「合法 syscall 返回地址」用的
  （见 `kernel/lib/syscall/src/table.cpp` 的注释），和入口表无关。
- 系统调用号仍然是 `system/public/ours/syscall.hpp` 里的手写宏，stub 仍然是 vdso.S
  里手写的汇编。现在加一个系统调用要动三处（号、stub、声明），比原来的五处少，但
  仍然不是「一处描述、处处生成」。那是下一件事。

## 编译模型（`-fpie`、`-fno-plt`）放回每个 ring-3 目标，不只可执行文件（2026-09-19）

**文件**
- `system/BUILD.gn` —— `-fpie` 与 `-fno-plt` 移进 `config("user.standalone")`；
  `user.executable` 只剩 `-static --pie`，`user.program` 只剩链接期 flag，
  `user.shared` 只剩 `-fPIC`。

**症状**
第一个用户态库 `//system/lib/ou` 一链进 shell 就出两个问题：
```
ld.lld: error: relocation R_X86_64_32 cannot be used against local symbol; recompile with -fPIC
>>> defined in user-x86/obj/system/lib/ou/src/ou.debug.cpp.o
```
修掉这个之后，shell 的导入从 `.rela.dyn` 的 `R_X86_64_GLOB_DAT` 变成了 `.rela.plt` 的
`R_X86_64_JUMP_SLOT`（这又撞出了 eftl 的 DT_RELAENT 问题，见同日 `eftl.md`）。

**原因**
本文件前面「vDSO 改成共享对象」那一条拆 config 时，把 `-fpie` 跟着 `-static --pie`
一起搬进了 `user.executable`，把 `-fno-plt` 放在了 `user.program`。两者都只作用于
**可执行文件目标本身**。而一个程序由很多目标组成：它链进来的每个 `source_set`、
每个库，编译时都拿不到这两个 flag。

区分的判据是：**链接策略**因产物而异（可执行文件要 `-static --pie`，共享对象要
`-shared`），但**编译模型**对所有 ring-3 代码都一样——内核决定加载地址，所以谁都
不能写死地址；一个导入函数都该经 GOT 调用，所以谁都不该生成 PLT。把编译模型挂在
「可执行文件」上，等于说「只有程序自己的 .cpp 是位置无关的」，第一个库就会打破它。

之前没暴露，是因为 userboot 和 shell 都只有一个翻译单元，没有第二个目标。

**改法**
```gn
# 改前
config("user.executable") {
  configs = [ ":static-link", ":pie-link" ]
  cflags = [ "-fpie" ]
}
config("user.program") {
  cflags = [ "-fPIE", "-fno-plt" ]
  ...
}

# 改后
config("user.standalone") {      # 每个 ring-3 目标
  cflags = [ "-fpie", "-fno-plt" ]
  cflags += [ "-msoft-float", ... ]
}
config("user.executable") {      # 只有链接策略
  configs = [ ":static-link", ":pie-link" ]
}
```
`user.shared` 在后面加 `-fPIC`，后出现的 flag 覆盖前面的 `-fpie`，vDSO 仍然没有任何
重定位（`llvm-readelf -r` 确认）。

**参照**
`Zircon //build/config/zircon:user` 把 `-fPIE` 放在所有用户态代码共享的 config 上，
可执行文件与共享库只在链接侧分叉；本项目内核侧 `//kernel/products:kernel.config`
也是编译模型在全局、链接形状在产物上。

**验证**
- 构建通过。shell 的重定位回到预期形状：
  ```
  Relocation section '.rela.dyn' contains 2 entries:
    ... R_X86_64_GLOB_DAT  ours_debug_write + 0
    ... R_X86_64_GLOB_DAT  ours_command_list + 0
  ```
  vDSO：`There are no relocations in this file.`
- QEMU 实测两个镜像，shell 完整输出。

**故意没做**
- 没把 `-fpie` 升成 `-fPIE`。原来就是 `-fpie`（小模型，镜像在低 2 GiB），这一条只
  挪位置不改语义。
