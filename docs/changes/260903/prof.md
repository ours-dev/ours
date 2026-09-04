# kernel/lib/prof 与 unwind — 改动记录

格式见 `docs/changes/TEMPLATE.md`。待修项在 `docs/repairs/260903/ktl.md`（分配器那一侧）。

范围：`kernel/commands/prof.cpp`（profiler 的 init hook 与控制台命令）、
`kernel/lib/unwind`（`.eh_frame` 解析与 FDE 索引）。`kernel/lib/prof` 目录里另有一段
未提交的采样器重构，不属于这里的任何一条。

---

## 1. `prof_init()` 挂死：FDE 索引超过 kmalloc 上限，`ktl::Vec` 在空指针上构造（2026-09-03）

**文件**
- `kernel/commands/prof.cpp` — FDE 索引数组改从页分配器拿整页，不再用 `ktl::Vec`；
  两遍扫描的返回值都检查
- `kernel/lib/unwind/unwind/fde_index.cpp` — 记录声明长度越过节尾时拒绝整节

**症状**
每次启动都停在 `InitLevel::Platform` 的第二个 hook。日志最后三行是上一个 hook 的
`Free frame(D000/E000/F000)`，之后再无输出，QEMU 活着；接 gdbstub 则变成三重错误。
kshell 的控制台线程和 `labour_routine()` 后面的两个 gibberish 线程都到不了。

**原因**
交接时的假设是 `unwind::FdeIndex::build()` 遇到不前进的 CFI 记录死循环。**这个假设不成立**：
把同一份 vmours 的 `.eh_frame`（0x6ff4 字节）用 `llvm-objcopy --dump-section` 抽出来，在宿主机
上链接 `cfi.cpp`/`cursor.cpp`/`fde_index.cpp` 走一遍，792 条记录（1 个 CIE，791 个 FDE）
在 0x6ff0 的零长度终结符处正常停下；核内加标记也确认计数遍返回了 `ok=true count=791`。

真正停住的是下一行 `g_fde_entries.reserve(791)`。`ktl::Vec` 的分配器是
`ktl::Allocator::allocate(n)`，它做的是 `new (GAF_KERNEL) Element[n]()`，最终落到
`ktl::kmalloc(size)`。而 kmalloc 只有 8 B 到 4 KiB 十个 size class，超过 `MAX_MALLOC_SIZE`
直接 `return nullptr`（注释写着"Now we do not support to allocate frames from kmalloc"）。
791 × 32 B = 25 312 B，六倍于上限。

nullptr 本身不致命，`Vec::reserve()` 检查了 `!buffer`。致命的是它检查不到：这个
placement `operator new[]` 没有声明 `noexcept`，按语言规则编译器可以假定它不返回空，于是
`new (gaf) Element[n]()` 紧接着在返回值上做值初始化——往地址 0 起的 25 KiB 写零。
内核当前的缺页处理在这里没有出路，所以表现是安静挂死；gdbstub 下则是三重错误。
两种现象同一个根。

**改法**
索引数组改用 `mm::get_frames_n<Entry>(GAF_KERNEL, nr_frames)`：按字节数向上取整到页，
从伙伴分配器拿一块 2^order 页，经 physmap 直接可见，永不释放（它和它描述的内核代码活得
一样久）。`ktl::Vec` 换成一个三字段的 `FdeTable{entries, capacity, count}`，填充 sink 带
容量检查，两遍扫描的 `build()` 返回值都看，失败就走已有的 "unwinding disabled" 分支。

否掉的方案：
- 只在分配失败时降级（"unwinding disabled"）。能过启动，但 `.eh_frame` 只会更大，等于
  永久禁用回溯；而且不解决 `ktl::Allocator` 在空指针上构造这件事，只是绕开它。
- 用 VMO + 映射（内核栈那条路）。表是只读、一次性、几十 KiB，物理连续页走 physmap 就够，
  没有理由多一层映射。

顺手在 `FdeIndex::build()` 里加了节尾检查：记录的声明长度 `4 + length` 超过
`section.size - offset` 就拒绝整节。Cursor 的每次读都有界，所以这不是防越界读，而是防
把一个指令流被截断的 FDE 收进索引——`offset + length` 会成为 `instructions_end`，
unwinder 之后按它求值。

**参照**
无，本项目自有设计。Linux 的对应物 ORC 表由 objtool 在链接期生成，不在运行时建；
Zircon 内核不做 `.eh_frame` 索引。分配器一侧可对照 Linux `kmalloc()` 超过
`KMALLOC_MAX_CACHE_SIZE` 时转 `kmalloc_large()` 走页分配器——这是待修项，见 `ktl.md`。

**验证**
- `ninja -C out/bringup.x86` 通过；`clang-format`/`clang-tidy -p out/bringup.x86` 在两个文件上无新告警。
- 宿主 gtest：`host/obj/kernel/lib/unwind/tests/cfi` 9 例、`tests/unwind` 5 例全过。
- QEMU 实测：

| 镜像 | prof 日志 | 之后 |
|---|---|---|
| `kernel.dev.dbg_1.obi` | `indexed 791 unwind frames (28660 bytes)` | `I'm Thread 1` ×417、`I'm Thread 2` ×370，`laborer` 退出 |
| `kernel.dev.obi` | `indexed 705 unwind frames (24688 bytes)` | 同上（debug 日志编译掉了，看不到线程行） |

两个镜像在 `laborer` 退出后都停在 `[error] operator new`——全局 `operator new` 的 panic
桩被调到了。那是 laborer 退出路径上的下一个问题，不在 prof 里，已交接给 kernel/task。

**故意没做**
- 没改 `ktl::Allocator`（在空指针上构造）和 kmalloc 的 4 KiB 上限，都是别的 lane，
  写进 `docs/repairs/260903/ktl.md` 并已交接。
- 没动 `kernel/lib/prof` 里未提交的采样器重构。
- `cfi.hpp` 里 `DW_EH_PE_SLEB128 = 0x05`…`SDATA8 = 0x08` 与规范（0x09…0x0C）不符，
  现在靠 `cursor.cpp` 里对 0x0B 的特判撑着，README 把它记成 psABI 的"怪癖"，其实是常量
  错了。内核自己的 `.eh_frame` 只用 0x1B，所以此刻不触发；单独一条改动，见下面第 2 条。

## 2. `DW_EH_PE_*` 有符号格式常量改回规范值，删掉对 0x0B 的特判（2026-09-03）

**文件**
- `kernel/lib/unwind/include/unwind/cfi.hpp` — `SLEB128/SDATA2/SDATA4/SDATA8` 从 0x05–0x08 改为
  `无符号 | DW_EH_PE_SIGNED(0x08)`，即 0x09–0x0C
- `kernel/lib/unwind/unwind/cursor.cpp` — `encoded_pointer()` 里 `case 0x0B` 的特判删掉
- `kernel/lib/unwind/README.md` — "psABI 怪癖"一段改成对格式位域的说明

**症状**
潜伏，当前触发不到。内核自己的 `.eh_frame` 只用 0x1B（pcrel|sdata4），而 0x0B 被特判成
4 字节有符号读，所以碰巧能走通。任何 `sleb128`（0x09）、`sdata2`（0x0A）、`sdata8`（0x0C）
编码的节都会落到 `default: m_ok = false`，被判成畸形而拒绝；反过来 0x05–0x08 这四个值在规范里
根本不存在，却被当作合法格式接受。

**原因**
DW_EH_PE 的格式半字节不是顺序编号的枚举，是位域：bit 0–2 选宽度（uleb128、2、4、8 字节），
bit 3（0x08）是"有符号"标志。所以有符号一组在 0x09–0x0C，和对应的无符号只差一位
（LSB 4.1 "DWARF Extensions"、GCC `dwarf2.h`）。原来的常量把它当作 0x05 起顺延，于是
0x1B 的低半字节 0x0B 在表里找不到，作者没有回头查常量，而是加了一个 `case 0x0B` 并在
README 里记成 psABI 的怪癖。特判的存在本身就是常量错了的证据。

**改法**
常量按位域写成 `DW_EH_PE_UDATA4 | DW_EH_PE_SIGNED` 这种形式，让"差一位"的关系在代码里
可见；特判随之没有存在的理由，删掉。

**参照**
GCC `include/dwarf2.h` 的 `DW_EH_PE_sdata4 = 0x0b` 等；LLVM `llvm/BinaryFormat/Dwarf.h`
的 `DW_EH_PE_sdata4 = 0x0b`。

**验证**
构建通过；`clang-format`/`clang-tidy` 无新告警；宿主 gtest `cfi` 9 例、`unwind` 5 例全过
（`unwind_test.cpp` 用的是字面量 0x1B，不随头文件变，所以是真实检验）；QEMU dbg 镜像
仍然 `indexed 791 unwind frames`，两个 gibberish 线程照常运行。

**故意没做**
没给 `encoded_pointer()` 补 `TEXTREL`/`DATAREL`/`FUNCREL`/`ALIGNED` 的解析——内核
`.eh_frame` 用不到，unwinder 的设计是"不支持就拒绝而不是猜"。
