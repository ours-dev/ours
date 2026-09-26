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
