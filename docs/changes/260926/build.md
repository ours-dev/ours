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

## `tools/set.py` 加 `--with`/`--userboot`，并在 gn gen 后检查 userboot 目标确实在镜像里（2026-09-26）

**文件**
- `tools/set.py` — 新增 `--with`（可重复）、`--userboot` 两个开关，拼成 `ours = {...}` 写进 args.gn；gn gen 之后检查 userboot 的目标是否在镜像的程序里，不在就报错并撤销这次配置

**症状**
上一条立了 `ours` 参数包，但只能手写进 args.gn；而 `tools/set.py` 每次运行都整份重写 args.gn，
手写的内容下次一跑就没了，所以参数实际上选不了。另外 `with` 和 `userboot` 可以互相矛盾：
`--with //examples/shell` 换掉了默认列表但 userboot 仍默认启动 init，这样的镜像能编出来，
进 QEMU 才发现第一个进程找不到文件。

**原因**
args.gn 归 set.py 所有（它的文档里写了这一点，`--args` 就是为此存在的），所以任何要"按目录选择"的构建参数
都得能从 set.py 说出来。矛盾检查则必须在 gn gen 之后做：两个问题的答案都是构建给的——
userboot 会被交给哪个名字（`--userboot` 或镜像默认值），镜像实际带了哪些用户程序（deps 的
`bootfs_user_visible` metadata 汇出的白名单，`--with` 决定了它）。在 set.py 里从 label 猜 BootFS 文件名
是另起一份真相，label 的最后一段不一定是产物名。

改前：

```python
def set_build(product: str, arch: str, variants: list[str], out_dir: Path,
              tool_dir: str | None, extra_args: list[str]) -> int:
    ...
    result = subprocess.run(cmd, cwd=str(ROOT))
    if result.returncode == 0:
        _refresh_debug_menu()
```

**改法**
改后：

```python
def check_userboot(gn: Path, out_dir: Path) -> str | None:
    result = subprocess.run(
        [str(gn), "outputs", str(out_dir), OURS_IMAGE_RSP, OURS_IMAGE_WHITELIST], ...)
    rsp_path, whitelist_path = (out_dir / p for p in result.stdout.split())
    # rsp 里的 --entry=kernel.select.userboot=<name> 是镜像生效的 userboot 目标，
    # 白名单是镜像实际带的用户程序；前者必须在后者里。
    ...

    result = subprocess.run(cmd, cwd=str(ROOT))
    if result.returncode == 0 and ours is not None:
        error = check_userboot(gn, out_dir)
        if error is not None:
            print(f"error: {error}", file=sys.stderr)
            _restore(gn, out_dir, created_out_dir, previous_args)
            return 1
```

取舍：

- **只写用户给了的字段**：args.gn 里的 scope 整个替换默认值，镜像对缺省字段用自己的默认值兜底。
  set.py 若替用户把 `userboot = "init"` 也写上，就是悄悄钉死了一个不该由它决定的值。
- **文件路径问 GN 要**（`gn outputs <dir> <label>`），不在 set.py 里写死 `gen/...` 路径；
  set.py 只认两个 label：`dev:_obi.dev.rsp` 和 `dev:ours.user_visible`。
- **失败就撤销**：这次运行新建的目录整个删掉；已存在的目录恢复原 args.gn 并重新 gn gen——
  build.ninja 已经按被拒的参数生成，不重新 gen 的话 ninja 会一直按它构建，直到有人再动 args.gn。
  否掉的方案是"报错但留着 args.gn"：那会留下一个配置好了、却注定起不来的构建目录。
- 输入先做两条便宜的检查：`--with` 必须是绝对 label（相对 label 会相对镜像的 BUILD.gn 解析，
  不是用户以为的位置）；`--userboot` 必须是裸 BootFS 名（空白会把它切成第二个 cmdline 选项）。
  和 `--args ours=...` 同时出现直接报错。

**参照**
Fuchsia `fx set --with <label>`（`//tools/devshell/set`，把 label 写入 args.gn 的
`legacy_universe_package_labels` 等参数）。`kernel.select.userboot` 对标 Zircon 同名选项；
检查的依据是 `kernel/kernel_package.gni` 从 `bootfs_user_visible` metadata 生成的白名单，
这也是内核给用户态的 BootFS 的准入名单。

**验证**
- 不带新开关重跑 `set.py bringup.x86 --out out/ours-args`：args.gn 与改前逐字一致。
- `--with //examples/shell --userboot shell`：args.gn 写出 `ours = { with = [...] userboot = "shell" }`。
- `--with //examples/shell`（userboot 仍默认 init）写到新目录：报
  `userboot would start 'init', but the image only carries userboot, vdso, shell.`，目录被删除。
  同一组参数写到已有目录：报同样的错，args.gn 恢复且与原文件 diff 为空。
- 相对 label、与 `--args ours=` 冲突：建目录之前就报错，不留目录。
- 端到端：`out/ours-shell`（只带 shell、userboot=shell）完整构建后 `user-x86/obj/system/service/init`
  **不存在**（默认目录里存在），init 确实没被编译。两个目录的 `kernel.dev.obi` 同时在 QEMU 里跑 40 秒
  （各自的 WORK 目录）：默认镜像 kernel package 8 个文件、日志有 `init: started with the root job`，
  shell 是 koid 40；定制镜像 7 个文件、没有 init，shell 是 koid 26（第一个进程）。两者都跑完
  `shell: libc checks passed`，零 panic。

**故意没做**
- 没有给 set.py 写单元测试：仓库里还没有 set.py 的测试框架，上面的场景都是对真实 GN 跑的。
- 检查写死了 dev 镜像的两个 label：`ours` 目前只有 dev 读（见上一条）。以后如果别的镜像也读 `ours`，
  这里应改成从产品的镜像列表里找，而不是再加一对 label。
- 不改 `scripts/qemu_boot.py`：并行跑本来就只需要各设一个 `WORK`，它已经支持。

## `ours.userboot`/`--userboot` 改名为 `ours.next`/`--next`，检查改读 `userboot.next=`（2026-09-26）

**文件**
- `build/product/ours_args.gni` — 字段 `userboot` 改名 `next`，说明改为"userboot 接下来启动谁"
- `kernel/products/image/dev/BUILD.gn` — `ours.userboot`/`_userboot` 改为 `ours.next`/`_next`
- `tools/set.py` — `--userboot` 改为 `--next`；检查从找 `--entry=kernel.select.userboot=` 改为找 `--entry=userboot.next=`

**症状**
userabi 会话落地 `userboot.next`（a53c5ec5..52bfc80f）之后，dev 镜像的 cmdline 不再写
`kernel.select.userboot=`，但 set.py 的检查还在 rsp 里找它：一用 `--userboot` 就报
`dev.obi.rsp has no --entry=kernel.select.userboot=<name> entry`，开关不可用。

名字本身也是错的：这个字段的值是 userboot **接下来**启动的程序（init、shell），从来不是 userboot 自己。
用户的原话是「userboot 是 userboot，init 是 init，userboot 启动 init」。

**原因**
前两条改动沿用了当时镜像的写法 `kernel.select.userboot=init`——那时内核把这个选项指名的程序
直接放进第一个进程，init 顶替了 userboot，于是"选第一个进程"和"选 userboot 启动谁"被混成一件事，
参数也就顺势叫了 `userboot`。Zircon 把它们分成两个选项：`kernel.select.userboot` 选第一个进程跑哪个
userboot 程序（正常是 `userboot`，eng 镜像是 `userboot-test-rust`），`userboot.next` 由 userboot
程序自己读、决定它接下来启动谁。userboot.next 落地后 ours 也是这个模型，参数应当跟着它命名。

改前：

```gn
if (defined(ours.userboot)) {
  _userboot = ours.userboot
} ...
    "userboot.next=$_userboot",
```

```python
USERBOOT_ENTRY = "--entry=kernel.select.userboot="
    parser.add_argument("--userboot", default="", metavar="NAME", ...)
```

**改法**
改后：

```gn
if (defined(ours.next)) {
  _next = ours.next
} ...
    "userboot.next=$_next",
```

```python
NEXT_ENTRY = "--entry=userboot.next="
    parser.add_argument("--next", dest="next_program", default="", metavar="NAME", ...)
```

检查逻辑不变：`gn outputs` 取 dev 的 rsp 和白名单，rsp 里 `userboot.next=` 的值必须在白名单里，
否则报错并撤销这次配置。旧开关 `--userboot` 直接删掉，不留兼容别名——它刚加进来一天，
留着只会让"userboot 可以被选成别的程序"这个错误说法继续存在。
`dest="next_program"` 是因为 `next` 是 Python 内建名，不拿它当变量。

**参照**
Zircon `userboot.next`（`zircon/kernel/lib/userabi/userboot/main.rs` 读取它）与
`kernel.select.userboot`（`zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc`）。
ours 侧的 userboot.next 实现见 `docs/changes/260926/userabi.md` 最后五节。

**验证**
- 改名后 `build/product`、`kernel/products/image`、`tools/set.py` 里没有残留的 `ours.userboot`/`_userboot`/`--userboot`；`gn format --dry-run` 通过。
- 默认配置：rsp 为 `--entry=userboot.next=init`。
- `--with //examples/shell --next shell`：args.gn 写出 `next = "shell"`，rsp 为 `--entry=userboot.next=shell`；构建后 `init` 目录不存在。
- `--with //examples/shell`（next 默认 init）：报 `userboot would start 'init' next, but the image only carries userboot, vdso, shell.`，新目录被删除。`--next nosuch` 同样被拒。`--userboot` 被 argparse 拒绝。
- QEMU 同时跑 60 秒三个镜像，零 panic：
  默认 dev：`passing userboot.next=init to userboot` → `userboot: started init` → `init: started with the root job`，userboot 随后退出，shell 跑完 `libc checks passed`；
  定制 dev：`userboot: started shell`，日志里没有任何 init，shell 跑完 `libc checks passed`；
  dbg_1：`userboot: started shell`，fault_probe 被 kill 11 次。

**故意没做**
- 没加 `kernel.select.userboot` 对应的开关：镜像都用默认的 `userboot`，要换 userboot 变体（像 Zircon 的
  `userboot-test-rust`）时再加，字段可以叫 `userboot`——那时它的值才真的是一个 userboot。

## 改用 Fuchsia 的模型：删掉 `--next`，`--with` 改为追加（2026-09-26）

**文件**
- `build/product/ours_args.gni` — 删掉 `next` 字段；`with` 改为"在镜像自己的程序之外追加"
- `kernel/products/image/dev/BUILD.gn` — 开机流程固定为 `userboot.next=init`；`ours.with` 追加进程序列表，已在列表里的跳过
- `tools/set.py` — 删掉 `--next`，以及只为它存在的 gn gen 后检查和失败撤销逻辑

**症状**
无运行时症状，是模型问题。用户问"都 `--with` 了为啥还要 `--next`"，追下去发现前两条的模型和 Fuchsia 不一样：
ours 让一次构建同时改"包里有什么"（`--with` 替换列表）和"开机跑什么"（`--next`），
而 Fuchsia 里 `--with` 从来不碰开机流程。

**原因**
Fuchsia 的分工是：开机流程属于产品，构建只让更多程序"可用"。
- 开机固定是 userboot → component_manager（`zircon/kernel/lib/userabi/userboot/main.rs` 的
  `DEFAULT_NEXT_BOOT = "bin/component_manager+--boot"`，只有 eng 的开发配置能改，
  见 `src/lib/assembly/platform_configuration/src/subsystems/development.rs`）；
  component_manager 再按 `root.cml` → `bootstrap` 起系统服务，其中 console-launcher 在串口上起 shell。
- `fx set --with` 只把包加进 universe（`tools/devshell/set` 的说明），开机后再按需运行。
- 要"开机后自动跑某个程序"，用的是 `zircon.autorun.boot=<command>`，由 console-launcher 在 shell 起来后执行
  （`docs/reference/kernel/kernel_cmdline.md`），开机流程本身仍不变。

照这个模型，`--next` 让每个构建目录改开机流程，是把产品的属性搬进了构建参数；
`--with` 替换列表更会让"开机流程需要的程序"（init、shell）被一个构建参数拿掉。

改前：

```gn
if (defined(ours.with)) {
  _user_programs = ours.with
} else {
  _user_programs = [ "//examples/shell", "//system/service/init" ]
}
if (defined(ours.next)) {
  _next = ours.next
} ...
    "userboot.next=$_next",
```

**改法**
改后：

```gn
_user_programs = [ "//examples/shell", "//system/service/init" ]
if (defined(ours.with)) {
  foreach(program, ours.with) {
    if (filter_include(_user_programs, [ program ]) == []) {
      _user_programs += [ program ]
    }
  }
}
...
    "userboot.next=init",
```

- 开机流程（userboot → init → shell）写死在镜像里，不再是构建参数。
- `ours.with` 追加：给的程序会被编译、打进 BootFS，但开机不会自动运行它。
  已经在开机流程里的程序（比如再写一次 `//examples/shell`）跳过，不会变成重复依赖。
- set.py 的 gn gen 后检查整段删掉：它唯一检查的是"`--next` 指名的程序在不在包里"，
  现在开机流程固定、`--with` 只增不减，不会再出现这种不一致。

代价：以前"只想构建 A 就不构建 B"的诉求，现在只对**额外**的程序成立；init、shell 是开机流程，
总会被编译——这和 Fuchsia 的 base 包集合是同一回事。

**参照**
Fuchsia `fx set --with`（`tools/devshell/set`）、userboot 的 `DEFAULT_NEXT_BOOT`（`userboot/main.rs`）、
`root.cml`/`bootstrap.cml`、console-launcher 的 `autorun_boot` 配置（`src/bringup/bin/console-launcher`）。

**验证**
- `gn format --dry-run` 通过；set.py 里不再有 next 相关代码，`--next` 被 argparse 拒绝，不建目录。
- 默认目录：rsp 为 `--entry=userboot.next=init`，白名单 `userboot vdso shell init`。
- `--with //kernel/lib/syscall/test/fault_probe --with //examples/shell`：白名单 `userboot vdso shell init fault_probe`，
  shell 只出现一次。
- QEMU 同时跑 60 秒，零 panic：默认 dev 包里 8 个文件；加了 fault_probe 的 dev 包里 9 个文件（日志列出
  `fault_probe (7952 bytes)`），开机同样是 `userboot: started init` → `init: started with the root job` →
  `shell: libc checks passed`，fault_probe 没有被运行（0 个进程）；dbg_1 照旧 userboot → shell，fault_probe 被 kill 59 次。

**故意没做**
- `--autorun`（对应 `zircon.autorun.boot`）还没加：要 init/shell 先能读这个选项并运行指名的程序，
  那是用户态的活，另交会话；落地后构建这边再加开关，并把刚删的"指名的程序必须在包里"检查按 autorun 重新加回来。
- 没动 dbg_1：它的程序集合和开机流程就是这个诊断镜像的用途，也不读 `ours`。

## `ours.autorun`/`--autorun`：让构建目录开机就跑自己的程序，对应 `zircon.autorun.boot`（2026-09-26）

**文件**
- `build/product/ours_args.gni` — 新增 `autorun` 字段
- `kernel/products/image/dev/BUILD.gn` — `ours.autorun` 存在时在 cmdline 末尾追加 `ours.autorun.boot=<command>`
- `tools/set.py` — 新增 `--autorun <BootFS 名>[+参数...]`；gn gen 后检查 autorun 指名的程序在镜像里，不在就报错并撤销本次配置

**症状**
上一条改成 Fuchsia 模型后，`--with` 只把程序打进包、不运行它，构建目录没有办法"开机就跑我的程序"。

**原因**
Fuchsia 的答案不是改开机流程，而是 `zircon.autorun.boot=<command>`：开机流程照旧，console-launcher 起完 shell
后执行这条命令。ours 里对应 console-launcher 的是 init（它起 shell），用户态会话已经实现了
`ours.autorun.boot`（dc17bafd..68f9a0f4：内核按白名单转发 `ours.autorun.*` → userboot 转给 init →
init 起完 shell 后 `process_spawn_boot`）。构建这边缺的是把它变成按构建目录可选的参数。

检查要放回来：`--autorun` 可以指名一个包里没有的程序，这种镜像能编能启动，只是在日志里多一行
`init: starting the autorun command = NotFound`，很容易被当成"跑过了"。

改前：

```gn
  args = [
    "userboot.next=init",
    "kernel.shell=true",
  ]
}
```

**改法**
改后：

```gn
  args = [
    "userboot.next=init",
    "kernel.shell=true",
  ]
  if (defined(ours.autorun)) {
    _autorun = ours.autorun
    args += [ "ours.autorun.boot=$_autorun" ]
  }
}
```

```python
def autorun_program(command: str) -> str | None:
    for word in command.split("+"):
        if word:
            return word
    return None
...
    command = None
    for line in rsp_path.read_text().splitlines():
        if line.startswith(AUTORUN_ENTRY):
            command = line[len(AUTORUN_ENTRY):]      # init 取最后一个，这里也取最后一个
    ...
    if program not in programs:                      # programs = 镜像的用户可见白名单
        return f"--autorun would start '{program}', but the image only carries ..."
```

- 追加在 cmdline **末尾**：init 取最后一个 `ours.autorun.boot`，放最后保证构建参数覆盖镜像将来可能自带的值。
- 程序名的取法和 init 一致（`init::AutorunCommand::parse()`：按 `+` 切、跳过空词、第一个词是程序名），
  否则 `+foo` 这种写法会在这里和 init 里得到不同答案。
- 检查和失败撤销沿用上一版 `--next` 的做法（`gn outputs` 取 rsp 与白名单；新建目录删掉、已有目录恢复
  args.gn 并重新 gen），只是检查对象换成 autorun。
- `--autorun` 的字符集限制：不允许空白（会在 cmdline 上变成第二个选项）和引号（会破坏 GN 字符串）。

**参照**
Fuchsia `zircon.autorun.boot`（`docs/reference/kernel/kernel_cmdline.md`）与 console-launcher 的
`LaunchAutorun()`（`src/bringup/bin/console-launcher/main.cc`）；ours 侧实现见
`docs/changes/260926/` 下 userabi/init 相关条目。

**验证**
- `--with //kernel/lib/syscall/test/fault_probe --autorun fault_probe+none`：args.gn 写出 `autorun = "fault_probe+none"`，
  rsp 末尾是 `--entry=ours.autorun.boot=fault_probe+none`。
- `--autorun fault_probe`（没 `--with`）写新目录：报 `--autorun would start 'fault_probe', but the image only carries userboot, vdso, shell, init.`，目录删除。
  `--autorun nosuch` 写已有目录：报错，args.gn 恢复且 diff 为空，rsp 里的 autorun 项也随重新 gen 回来了。
- `--autorun "fault_probe none"`、`--autorun +++` 在建目录前被拒；只给 `--with` 时 rsp 里没有 autorun 项。
- QEMU 同时跑 60 秒，零 panic：dev 日志 `userabi: passing ours.autorun.boot=fault_probe+none to userboot` →
  `userboot: passing ... to init` → `init: spawned shell` → `init: autorun fault_probe+none` → `fault_probe: none`；
  dbg_1 不读 `ours`，日志里没有 autorun，自己的探针照常跑完。

**故意没做**
- 不在 set.py 里检查"最多 8 个词"：那是 init 的 `AutorunCommand::MAX_WORDS`，超出时 init 拒绝并报错、shell 不受影响；
  在这里再写一个 8 会多一份需要同步的真相。
- dbg_1 不支持 autorun：它的开机流程是 userboot 直接起 shell，没有 init，而这正是这个诊断镜像的用途。
