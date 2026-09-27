# build 改动记录（2026-09-26）

## 链接前拦住 `.init.rodata` 里忘了 `const` 的数据（2026-09-26）

**文件**
- `build/toolchain/ours/elf_link.py` — 新增 `check_init_rodata()`、`_sections()`、`_objects()`、`_link_inputs()`；`main()` 在 `os.execv` 之前跑检查，有违规就返回 1

**症状**
潜伏。`.init.rodata` 这个段名承诺只读，而没有任何东西强制它。`INIT_RODATA`（原
`INIT_CONST`）只是个链接段属性，`const` 要由人手写，所以会忘——`kernel/mm/init_vmm.cpp`
那张预设 VMA 表就忘了，而且只有从别的方向（查段权限为什么是 `WA`）才发现。

**原因**
先说为什么不在 `kernel.ld` 里断言——问过，试过，这个 lld 做不到：

- linker script 的表达式里没有任何读取段标志的东西（能用的只有 `ADDR`/`LOADADDR`/
  `SIZEOF`/`ALIGNOF`/`DEFINED`/`CONSTANT` 这一类）。
- GNU ld 的 `INPUT_SECTION_FLAGS(SHF_WRITE)` 可以把可写的输入段单独收走再断言长度为 0，
  lld 不认：`ld.lld: error: section pattern is expected`。
- GNU ld 的输出段属性 `(READONLY)` 也不认：`ld.lld: error: symbol not found: READONLY`。

判据本身也不能是「段不可写」，因为那**今天就不成立**：段里的条目含指针，内核是 DYN 镜像，
这些是 `R_X86_64_64` 重定位，工具链必须把段标成可写。能同时做到「今天成立」和「拦住真问题」
的判据是：

> 某个目标文件里的 `.init.rodata` **可写**、**且没有任何重定位指向它** → 忘了 `const`。

**改法**
放在 `elf_link.py` 里，`os.execv` 之前——检查的对象是**输入目标文件**，不是链接产物：
链接之后 `.init.rodata` 是一个合并段，来源信息没了。`execv` 的写法因此不用改。

ELF 段表用 Python 直接解，不调 `llvm-readelf`：这段代码在每次链接的每个输入上都要跑，
而 readelf 的输出是会变的文本格式。只读段表，每个目标文件几次 `struct.unpack`。
归档（`.a`）也解开逐个成员看，否则 `libclang_rt.builtins.a` 那类输入会被静默跳过。

**覆盖到什么，说清楚免得误以为万无一失**：

- 一个 TU 的 `.init.rodata` 里只有可写、无重定位的数据 → **本检查拦住**。
- 一个 TU 里把可写对象和只读对象放进同一个显式命名的段 → **根本到不了链接器**，
  clang 自己就报 `'PRESET_VMAS' causes a section type conflict with 'CHECK_PROBE'`，
  因为两者要求同一个段有不同的标志。
- 一个 TU 的 `.init.rodata` 里只有可写、**带指针**的数据 → **任何链接期检查都看不见**：
  实测加不加 `const`，目标文件里那个段的标志和重定位表**完全一致**。那种情形靠编译器在
  写入点报错。

**参照**
无，本项目自有设计。思路上和 Zircon 的 `//zircon/kernel/scripts` 下那些 ELF 后处理检查
（例如镜像里不许出现某些重定位类型）同类：链接器表达不了的不变量，交给一段读 ELF 的脚本，
挂在链接这一步上。

**验证**
- `out/bringup.x86`、`out/numa.x86` 两个配置构建通过（检查在两个镜像的链接上都跑过，
  没有误报），两个镜像启动到 shell 全部检查通过，numa 7 个 AP 全 online，无 fault / panic。
- 先在真实目标文件上单测过解析器：扫 206 个 `.o`，看到 1 个 `.init.rodata` 段
  （`mm.init_vmm.cpp.o`，有重定位），违规数 0。
- **端到端验证检查真的会拦**：在 `kernel/mm/pmm.cpp`（本来没有 `.init.rodata`）里插一行
  `INIT_RODATA usize CHECK_PROBE[] = {1, 2, 3};`，链接被拒：

```
FAILED: kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests
error: writable data with no relocations in .init.rodata -- the
       declaration is missing `const` (INIT_RODATA only names the
       section; see kernel/include/ours/init.hpp):
       kernel_x86.dbg.0/obj/kernel/mm/mm.pmm.cpp.o: .init.rodata is writable
```

  探针已移除。第一次把探针插在 `init_vmm.cpp` 里是无效实验——clang 先报了段类型冲突，
  这本身就是上面第二条覆盖情形的证据。

**故意没做**
- 没有在 GN 里把这个脚本登记成链接规则的输入依赖。改了检查不会触发重链接，代价是改它之后
  要碰一下源文件；换取的是不动 toolchain 的 GN 定义。
- 没有让检查可开关。它在没有 `.init.rodata` 的镜像上是空操作（phys、host 测试都不受影响），
  一个开关只会多一个要解释的旋钮。
- 没有处理 `SHN_XINDEX`（段数超过 0xFF00 时记在第 0 个段头里）。没有编译单元会产生六万多个段，
  代码里写明了这一点。
- 没有顺手检查别的段。`.init.text`/`.init.data` 没有类似的命名承诺可以被违背。

## 加 `ours = {}` 参数包：dev 镜像的用户程序和 userboot 目标可按构建目录替换（2026-09-26）

**文件**
- `build/product/ours_args.gni` — 新增，声明构建参数 `ours`（一个 scope，默认空）
- `kernel/products/image/dev/BUILD.gn` — 用户程序 deps 和 `kernel.select.userboot=` 改为从 `ours` 取，缺省字段回落到镜像自己的默认值

**症状**
没有运行时症状，是工作流问题。dev 镜像打进哪些用户程序、userboot 启动哪一个，
都写死在 BUILD.gn 里。多个会话并行推进内核时，想验证自己的用户程序只能改这个共享文件，
大家又共用一个 `out/bringup.x86`，结果是互相把对方改了一半的东西带进自己的验证。

**原因**
"镜像里装什么"本应是一次构建的配置，却被写成了源码。GN 里能按构建目录变化的只有
构建参数（`declare_args`，写在 `out/<dir>/args.gn`），所以要让每个会话各有一套程序，
这份列表必须变成参数。

还有一点决定了语义必须是"替换"而不是"追加"：ninja 只构建默认目标顺着 deps 能走到的东西。
如果参数只是在写死的列表后面追加，那写死的程序永远可达、永远被编译，
"只想构建 A 却连 B 也编了"就改不掉。替换默认列表，B 不在 deps 里，自然不被构建。

改前：

```gn
  deps = [
    "..:common_package_items",
    "//kernel/products/bin:vmours.with-tests(//build/switch/set/dbg.0:kernel_${target_cpu}.dbg.0)",
    "//examples/shell(//build/toolchain/ours:user-${target_cpu})",
    "//system/service/init(//build/toolchain/ours:user-${target_cpu})",
  ]
...
  args = [
    "kernel.select.userboot=init",
    "kernel.shell=true",
  ]
```

**改法**
新增一个 scope 型构建参数 `ours`，字段都可选：`with`（普通 label 列表）、`userboot`（BootFS 文件名）。
dev 镜像读它，字段缺省时用镜像自己的默认值，默认值与改前完全相同。

改后：

```gn
if (defined(ours.with)) {
  _user_programs = ours.with
} else {
  _user_programs = [ "//examples/shell", "//system/service/init" ]
}
if (defined(ours.userboot)) {
  _userboot = ours.userboot
} else {
  _userboot = "init"
}
...
  foreach(program, _user_programs) {
    assert(get_label_info(program, "toolchain") == current_toolchain,
           "ours.with takes plain labels without a toolchain suffix: $program")
    deps += [ "$program(//build/toolchain/ours:user-${target_cpu})" ]
  }
...
    "kernel.select.userboot=$_userboot",
```

取舍：

- **一个 scope 而不是 `ours_with`、`ours_userboot` 两个平铺参数**：以后加新选项只是加字段，
  不扩散参数命名空间。代价是 GN 对 scope 参数的语义：args.gn 里写 `ours = {...}` 会整个替换默认 scope，
  不按字段合并，所以默认值只能放在消费方用 `defined()` 兜底。这恰好也是默认值该在的地方——
  "镜像默认带哪些程序"是镜像的属性，不是构建的属性。
- **工具链后缀由镜像补，不让调用者写**：用户程序必须在 user 工具链里编，否则会在内核工具链里编出
  `-mcmodel=kernel` 的"用户程序"。让调用者每次写后缀既啰嗦又容易写错，所以 `with` 只收普通 label，
  带了后缀的直接 assert 拒绝，而不是再拼一次后缀拼成 GN 解析不了的东西。
- **userboot 与 init 是两回事**：userboot 是内核侧启动第一个用户进程的机制，`userboot` 字段只指名
  它启动哪个程序（BootFS 文件名），不改变 userboot 本身。

**参照**
Fuchsia `fx set --with`：它把 label 写进 `legacy_universe_package_labels` 等构建参数，
由产品装配把这些 label 并入构建图；本条是同一思路，参数落在 `ours.with`。
`kernel.select.userboot` 对标 Zircon 的同名启动选项（ours 侧见
`kernel/lib/userabi/src/userboot.cpp` 的 `USERBOOT_OPTION`）。
scope 型构建参数的用法参照本仓库 `build/config/BUILDCONFIG.gn` 的 `toolchain_variant`。

**验证**
- 独立构建目录 `out/ours-args`，改前改后各构建一次：`kernel.dev.obi`、`kernel.dev.dbg_1.obi`
  的 sha256 **逐字节一致**，确认默认行为没变。
- 探针目录写入 `ours = { with = [ "//examples/shell" ] userboot = "shell" }`：`gn desc` 显示
  dev 的 `ours` 包 deps 只剩 shell（init 不再可达），生成的 `dev.obi.rsp` 里是
  `--entry=kernel.select.userboot=shell`。
- 探针写入带工具链后缀的 label：`gn gen` 在新加的 assert 处失败，符合预期。
- QEMU 同时启动两个镜像 60 秒：dev 由 init 起 shell、跑完自检后正常退出；dbg_1 的 fault_probe
  被逐个 kill，零 panic。

**故意没做**
- `tools/set.py` 的 `--with`/`--userboot` 命令行开关放在下一步，这一步只立参数本身。
- 不影响 `dev.dbg_1`：它的程序集合（shell + fault_probe，userboot 选 shell）就是这个诊断镜像的用途，
  不该被按目录替换。注意这也意味着构建整个 bringup 产品时，dbg_1 仍会把 shell 拉进构建图。
- 不做"userboot 指名的程序必须在 with 里"的 GN 端校验：它只能比对 BootFS 文件名和 label，
  两者不是同一命名空间，放在 set.py 里对用户的输入做检查更直接。
- 没有照 Zircon 给 dbg_1 加 `kernel.pmm.alloc-random-should-wait` 这类压力测试开关：
  那些开关要内核侧先有读它们的代码，ours 目前只认 `kernel.select.userboot`、`kernel.shell`、`kernel.name`，
  已记入 `docs/repairs/260926/build.md`。
