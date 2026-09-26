# libc 改动记录（2026-09-25）

## `memcmp` 按 unsigned char 比较（2026-09-25）

**文件**
- `kernel/lib/libc/string.cpp` — `memcmp()` 改按 `u8` 读字节，并写明原因

**症状**
潜伏，当前触发不到。凡是涉及 0x80 及以上字节的比较，返回值的**符号**都是反的：`memcmp({0x80}, {0x01}, 1)` 回 -129，
应为正数；`memcmp({0xff}, {0x00}, 1)` 也是负数。编译产物里读字节用的是 `movsbl`（符号扩展）。

**原因**
C17 7.24.4 规定 `memcmp` 的结果"由第一对不同字节按 unsigned char 解释的差的符号决定"。旧实现把两边都读成 `char`，而 x86-64 的
`char` 是有符号的：0x80 读出来是 -128，比 0x01 小。

- **为什么一直没暴露**：全树调用方都只问"相等吗"（ELF 魔数、符号名比较，例如 `eftl/elf/gnu_hash.hpp:226` 的 `== 0`）。
  差值非零这一点没错，所以相等判断永远是对的，只有"谁大谁小"是错的。
- **谁会问顺序**：libc++。`std::string_view` 的 `compare()` 与 `operator<` 走 `char_traits<char>::compare`，运行期落到这个函数；
  而同样的比较在编译期求值时，libc++ 按 unsigned char 比较（`char_traits.h` 的 `lt()`）。两者会在含非 ASCII 字节的名字上给出相反的顺序：
  一张按一种顺序排好、按另一种顺序二分查找的表，会查不到东西，而且不报错。
- **为什么现在修**：U1 马上要把这份实现交给所有用户程序（`ou::Str` 计划改成 `std::string_view`，那之后排序就会走到这里）。
  修在搬移之前，这条修复留在它原来的路径的历史里，搬移提交也保持纯搬移。

**改法**
改前：

```cpp
    auto p1 = static_cast<char const *>(x);
    auto p2 = static_cast<char const *>(y);
    ...
            return (p1[i] - p2[i]);
```

改后：

```cpp
    auto p1 = static_cast<u8 const *>(x);
    auto p2 = static_cast<u8 const *>(y);
    ...
            return p1[i] - p2[i];
```

两个 `u8` 相减先整型提升为 `int`，差在 [-255, 255]，不会溢出。

**参照**
Linux `lib/string.c` 的 `memcmp()`、Zircon `kernel/lib/libc/string/memcmp.c`（`const unsigned char *su1, *su2`，`res = *su1 - *su2`），
都按 unsigned char 读。

**验证**
- 编译产物：`memcmp` 里读字节从 `movsbl` 变为 `movzbl`（`llvm-objdump` 看 `kernel_x86.dbg.1` 的 `string.string.cpp.o`）。
- 宿主上用同一段实现（`-fsigned-char` 模拟 x86-64，`[[clang::no_builtin]]` 防止被换成宿主 libc）跑：
  `{0x80}` 对 `{0x01}` 得 127，`{0xff}` 对 `{0x00}` 得 255，反过来得 -255，`"abc"` 对 `"abd"` 得 -1，前两字节比较得 0，长度 0 得 0。
- 两个镜像构建、启动：dev `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`，shell 的各组检查照常通过。

**故意没做**
- `memchr` 同样按 `char` 比较，但它只判断相等，而 `char` 与 `unsigned char` 之间的转换是一一对应的，所以结果正确，不改。
- 这份实现的宿主单测：等它搬到 `system/lib/libc`（U1）之后在那里加，免得测试跟着搬一次。

## 内核与用户程序共用一份 mem*/strlen：`string.cpp` 搬到 `system/lib/libc`，`user_executable()` 默认链接（2026-09-25）

**文件**
- `kernel/lib/libc/string.cpp` → `system/lib/libc/src/string.cpp` — 内容不变；换目录后按该目录的 `.clang-format` 重排了 include
- `system/lib/libc/BUILD.gn` — 新增：`config("headers.after")`（从 `kernel/lib/libc` 原样搬来）与 `source_set("libc")`
- `system/lib/libc/.gitignore`、`.clang-format` — 用 `tools/templates/cpp_library` 的 copier 模板生成后取用
- `kernel/lib/libc/BUILD.gn` — 删掉 `config` 与 `:string`；`:libc`、`:format-glue` 改依赖 `//system/lib/libc`
- `kernel/phys/BUILD.gn` — physboot 模块直接依赖的 `:string` 改为 `//system/lib/libc`
- `build/config/BUILD.gn`、`system/BUILD.gn` — `headers.after` 的标签改到新位置；`system/BUILD.gn` 两处"用户程序没有 libc"的注释改掉
- `system/user_executable.gni` — 默认依赖加上 `//system/lib/libc`
- `examples/shell/shell.cpp` — 新增 `check_libc()`；改掉三处"没有 libc"的注释
- `kernel/lib/userabi/userboot/userboot.cpp`、`system/lib/ou/include/ou/str.hpp`、`system/lib/ou/README.md` — 同上，改注释
- `docs/repairs/260919/libc.md` — 第 2 条里的文件路径改到新位置

**症状**
用户程序里写一个超过内联上限的零初始化数组（`char buf[136] = {}`）、一次大的结构体拷贝，或者用 `std::string_view`，都会**链接失败**：
编译器和 libc++ 会调用 `memset`/`memcpy`/`strlen`/`memcmp`/`memchr`，而用户程序以 `-nostdlib` 链接，背后什么也没有。
shell 手写 `spells()` 代替 `memcmp`、ou 另造 `ou::Str` 代替 `std::string_view`，都是这个原因。设计步骤 U1：之后的 runtime（U2）、
bootfs（U3）都要用这些函数。

**原因**
这几个函数的实现一直都有，只是放在 `kernel/lib/libc` 下，只有内核和 phys 的工具链编它。它本身并不依赖内核：只用 `u8`（强制包含的
prelude 给）、`WEAK_LINK`（`system/public/ours/compiler.hpp`，内核和用户看到的是同一个文件）和 `<cstring>`。缺的只是"用户工具链也编它、
用户程序也链它"这一步。

**改法**
改前：

```gn
# kernel/lib/libc/BUILD.gn
config("headers.after") { ... "-isystem", rebase_path("//system/lib/libc/include", root_build_dir) ... }
source_set("string") {
  sources = [ "string.cpp" ]
}
source_set("libc") {
  ...
  deps = [ ":string" ]
}
```

```gn
# system/user_executable.gni
    deps = [
      "//system/public",
      _vdso_label,
    ]
```

改后：

```gn
# system/lib/libc/BUILD.gn
config("headers.after") { ... }            # 原样搬来
source_set("libc") {
  assert(toolchain_environment != "host", ...)
  sources = [ "src/string.cpp" ]
}
```

```gn
# system/user_executable.gni
    deps = [
      "//system/lib/libc",
      "//system/public",
      _vdso_label,
    ]
```

- **一份源码，三种工具链**：内核（dbg.0、dbg.1）、phys、用户各编一次。一个 source_set 的编译参数由编它的工具链决定，与它在哪个目录无关，
  所以内核编出来的东西和搬之前一样，只是对象文件换了路径。`system/lib` 放通用框架、`kernel/lib` 放内核特化：`kernel/lib/libc` 剩下的
  （`libcxx.cpp`、`cpp_dependencies.cpp`、format 胶水）正是建在这份共用库之上的内核部分。
- **`headers.after` 必须是写死 `-isystem` 的 config**：libc++ 的 `<cstring>` 靠 `include_next` 找到这里的 `<string.h>`，这要求这组头文件
  排在 libc++ 自己的目录之后。`cpp_interface` 的 include_dirs 或 `public_configs` 都会变成 `-I`，排在所有 `-isystem` 之前，libc++ 的
  包装头就被绕过了。所以只是整体搬过来，写明原因，仍由工具链一侧（内核的 `default_include_dirs`、用户的 `user.headers`）加上。
- **为什么依赖加在 `user_executable()` 里**：config 不能添加依赖；也不该让每个程序的 BUILD.gn 自己写，因为没有哪个程序是"选择"需要它的——
  是编译器替它调的。vDSO 不是 `user_executable`（它直接用 `kernel_elf_binary`），拿不到这个依赖：它只是系统调用桩，映射进每个进程。
- **永远不在宿主工具链里编**（断言）：这些定义用的是宿主 libc 的名字，宿主程序链进去就顶掉了宿主的 `memcpy`；而且宿主编译不带
  `-ffreestanding`，clang 会把字节循环认成 memcpy，把 `memcpy` 编成调用它自己。现在编它的三个工具链都是 freestanding 的。
- **代价**：用户工具链链接时不带 `--gc-sections`，每个用户程序（包括 userboot）都带上这六个函数，几百字节，用不用都在。
  Zircon 的 trivial-test-userboot 也链接了字符串库，可以接受。

**参照**
- 设计：`docs/design/260923/userspace-interface.md` 3.6（`system/lib/libc`：把 `kernel/lib/libc/string.cpp` 挪过来，内核与用户共用；
  `user_executable()` 默认链接 libc）。
- Zircon 把两边分开：内核用 `//zircon/kernel/lib/libc/string`，用户态用 `//sdk/lib/c/string`（其中 `static_library("hermetic")`
  供 userboot 一类程序链接，符号全部 hidden）。这里只有一份，因为区分内核与用户的只有工具链。

**验证**
- `gn ls --all-toolchains`：`//system/lib/libc:libc` 出现在 `kernel_x86.dbg.0`、`kernel_x86.dbg.1`、`kernel.phys_x86.phys.dbg_0`、`user-x86` 四个工具链里。
- `llvm-nm`：shell、init、userboot、fault_probe 里各有本地的 `t memcpy … t strlen`，`llvm-nm -u` 除了 `ours_*` 没有别的未定义符号；
  vDSO 里没有任何 mem*/strlen；两个内核镜像里六个函数各一份。
- shell 的 `check_libc()` 编出来的调用（`llvm-objdump`）：`memcpy` 两次——一次按名字、长度只在运行时才知道，一次是编译器为 256 字节的
  结构体拷贝生成的；`memset` 一次，是编译器为 256 字节的零初始化数组生成的；`memcmp` 三次、`strlen` 一次；`memmove`、`memchr` 经寄存器
  间接调用。结构体拷贝第一版被优化掉了（拷贝之后源不再变化，编译器直接拿源代替目标），改为拷贝后再改写源，目标必须是独立的一份。
  所有结果都用手写循环核对，不用被测的 memcmp。
- 两个镜像构建、启动：dev 和 dbg_1 的 shell 都打出 `shell: libc checks passed`，其余各组检查照常；dev `init: shell closed its channel`，
  dbg_1 `shell: probes done (11 killed, 0 survived)`。日志里没有 `elfload: bound 'memcpy'` 一类的行：这些函数在程序内部解析，
  不经 vDSO 绑定。

**故意没做**
- `WEAK_LINK` 原样保留。全树没有强定义去覆盖它，它的一个副作用是：同一镜像里若混进第二份 mem*，链接器会悄悄挑一个而不报重复定义。
  去不去是设计决定，另行讨论。
- 宿主单测：下一个提交 `test(libc)`。实现要以不冲突的名字编进测试，并带 `-ffreestanding`/`[[clang::no_builtin]]`（否则测到的是宿主 libc）
  与 `-fsigned-char`（在 char 无符号的宿主上也能测出 memcmp 那类错误）。
- 把 `ou::Str` 改成 `std::string_view` 的别名、shell 的 `spells()` 与 fault_probe 的 `same()` 改用 memcmp：各自是单独的一步，
  注释里已写明现在可以做了。
- `<string.h>` 的两条旧待修项（查找类函数返回 const 指针；9 个只声明没定义的 str*）不在这里处理，仍在 `docs/repairs/260919/libc.md`。
  U1 的标题说"mem* 和 str*"，实际只有 `strlen`：全树和 U2–U13 都没有别的 str* 调用方，不为标题去补。
