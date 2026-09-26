# libc 改动记录（2026-09-26）

## libc 的宿主单测：实现挪到 `ours::libc` 名下，C 名字只转发（2026-09-26）

**文件**
- `system/lib/libc/src/string_impl.hpp`、`string_impl.cpp` — 新增：逐字节循环挪到这里，名字是 `ours::libc::memcpy` 等
- `system/lib/libc/src/string.cpp` — 只剩 `extern "C" WEAK_LINK` 的 `memcpy` 等六个 C 名字，各自转发到 `ours::libc`
- `system/lib/libc/BUILD.gn` — 新 `:impl`（带 `-fno-builtin`，宿主上另加 `-fsigned-char`）；`:libc` 只转发、依赖 `:impl`，宿主工具链里不定义；`group("tests")`
- `system/lib/libc/test/BUILD.gn`、`test/string_test.cpp` — 新增：12 个 googletest
- `BUILD.gn` — 默认构建带上 `//system/lib/libc:tests`

**症状**
无，补测试。`system/lib/libc` 按库目录的约定缺 `test/`；这份实现修过两个"答案错但不报错"的 bug（memchr 返回缓冲区起点、memcmp 符号反了），
都是靠别处出问题才发现的。

**原因**
这一节的学习价值在"为什么不能直接测 `memcpy`"。

- **编译器认识这些名字。**在宿主上写 `memcmp(a, b, 1)`，clang 知道 memcmp 该算出什么，可以不调用、直接给出答案。实测：把一个永远返回 -1 的
  错误 `memcmp` 放在单独的文件里链进测试程序，`memcmp({0x80}, {0x01}, 1)` 打印出来的是 1——链进来的函数根本没被调用。
  这样写的测试，被测代码错了也照样通过。内核和用户程序没有这个问题，是因为它们都带 `-ffreestanding`；测试程序是宿主程序，不能这么编。
- **一个程序里只能有一个 `memcpy`。**把我们的 C 名字链进测试程序，会顶掉宿主 libc 的同名函数，googletest 自己、以及测试拿来当参照的
  `std::memmove`/`std::memcmp` 都会跑到我们的实现上——拿自己和自己比。
- 所以给实现一个编译器不认识、也不会撞名的名字：`ours::libc::memcpy`。同一个错误实现改成这个名字，测试打印出 -1，错误被抓住。

**改法**
改前：`string.cpp` 里每个 C 名字的函数体就是循环本身。

```cpp
extern "C" WEAK_LINK
auto memcpy(void *d, void const *s, size_t n) -> void * {
    auto md = static_cast<char *>(d);
    ...
}
```

改后：循环在 `string_impl.cpp`，C 名字只转发。

```cpp
// string_impl.cpp
namespace ours::libc {
auto memcpy(void *const dst, void const *const src, usize const n) -> void * {
    ... // the same byte loop
}
}

// string.cpp
extern "C" WEAK_LINK
auto memcpy(void *d, void const *s, size_t n) -> void * {
    return ours::libc::memcpy(d, s, n);
}
```

- **两个编译单元，多一次 `jmp`**：内核和 shell 里的 `memcpy` 现在就是一条 `jmp` 跳到 `ours::libc::memcpy`。没有 LTO，跨编译单元内联不了；
  代价可以忽略。架构以后提供的强定义替换的正是这一层 C 名字（`WEAK_LINK` 的用意，见上一节）。
- **`:impl` 必须带 `-fno-builtin`**：否则 LLVM 的循环识别会把 memset 的填充循环改写成 `call memset`、把 strlen 的计数循环改写成 `call strlen`
  （拷贝循环它不改，只因为源和目标可能重叠）。LLVM 会放过**名字本身**就叫 memset/strlen 的函数，但不会放过 `ours::libc::memset`：
  在目标构建里，这个调用经 C 名字绕回循环自己，无限递归；在宿主测试里，它调到宿主 libc，测试测的就成了宿主的实现。实测：去掉
  `-fno-builtin` 编宿主版，目标文件里出现对 `_memset` 和 `_strlen` 的调用；加上后一个都没有。
- **宿主测试加 `-fsigned-char`**：让任何宿主上的 `char` 都像 x86-64 一样有符号。实现本身不依赖 char 的符号（memcmp 读 u8），
  但如果有人把 memcmp 改回读 `char`，只有 char 有符号的宿主才测得出来；在 char 无符号的宿主（Linux arm64）上，这个回归会照样通过。
- **宿主工具链里不定义 `:libc`**：谁在宿主上依赖它，就得到"找不到 target"的错误，而不是悄悄把 C 名字链进宿主程序。不用 `assert`，
  因为宿主工具链为了 `:impl` 和测试加载这个文件时，会把文件里每个 target 都求值一遍。
- **宿主的判断要用 `is_host_toolchain`**：第一版用的是 `toolchain_environment == "host"`，审查发现它在真正编测试的
  `//build/toolchain/ours:host` 里是 `"user"`，只有什么都不编的默认工具链才是 `"host"`。结果 `-fsigned-char` 没加到测试上
  （本机是 macOS arm64，char 本来就有符号，所以没暴露），`:libc` 也照样在宿主工具链里有定义。改用 BUILDCONFIG 已有的 `is_host_toolchain`。

测试覆盖（`test/string_test.cpp`）：
- `memcpy`：只拷 n 个字节、返回目标、n 为 0 时一个字节都不碰。
- `memmove`：32 字节缓冲里所有 (起点, 终点, 长度) 组合，与宿主 `std::memmove` 逐字节比较——两个方向的重叠、源等于目标、长度 0 全在里面。
- `memset`：只存低字节（0x1ab 存 0xab，-1 存 0xff），n 以外不碰，n 为 0 不碰。
- `memcmp`：0x80 对 0x01、0xff 对 0x00 两个方向的符号；第一个不同的字节决定结果、后面的不算；长度 0；再把全部 256×256 种字节对的
  符号与宿主 `std::memcmp` 比较（C 只规定符号，不规定大小）。
- `memchr`：返回命中的字节而不是起点（`"kernel.select.userboot=shell"` 里的 `=` 在 22）；第一个命中；n 以内最后一个字节能找到、
  刚过 n 的找不到；n 为 0；c 只取低字节（0x180 找 0x80，-1 找 0xff）。
- `strlen`：空串、普通串、中间有 NUL、4096 字节长串。

**参照**
Fuchsia 的 `sdk/lib/c/string/BUILD.gn` 为同样的原因给 mem* 的实现目标加 `-fno-builtin-<函数名>`，并注明不加会编成无限递归。
这里的做法（实现换名、C 名字转发）是本仓库自己的。

**验证**
- 宿主单测 12 个全部通过（`out/bringup.x86/host/obj/system/lib/libc/test/string`）。
- **变异检查**——测试要能抓到真 bug，而不是"永远是绿的"：临时把两个修过的 bug 放回实现里（memcmp 读 `char`、memchr 返回起点），
  5 个测试失败；在 `-fsigned-char` 真正生效之后单独放回 memcmp 那个，2 个测试失败；还原后 12 个全部通过。
- `gn desc`：宿主的 `:impl` 带 `-fno-builtin -fsigned-char`，用户的只带 `-fno-builtin`；`//system/lib/libc:libc(//build/toolchain/ours:host)`
  不存在。
- 宿主版 `:impl` 的目标文件里没有对任何 C 名字的调用或重定位；内核镜像和 shell 里的 `memcpy` 都是一条 `jmp` 到 `ours::libc::memcpy`，
  实现里没有调用自己。
- 两个镜像构建、启动：dev 与 dbg_1 的 shell 都打出 `shell: libc checks passed`；dev `init: shell closed its channel`，
  dbg_1 `shell: probes done (11 killed, 0 survived)`。
- 审查：三个方向（拆分的正确性、测试质量、构建与注释）并行审查、逐条反驳核实。成立的是宿主判断用错变量（两处）和 `-fno-builtin` 的注释举错了例子，
  都已在本提交修正。

**故意没做**
- 转发层的那次 `jmp` 不消除：要么 LTO，要么把实现放回同一个编译单元再用宏在测试里关掉 C 名字；前者这棵树没有，后者让产品代码里多一个只为测试存在的开关。
- `<string.h>` 的两条旧待修项仍在 `docs/repairs/260919/libc.md`。
