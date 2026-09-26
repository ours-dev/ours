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
