# libc 改动记录（2026-09-19）

## `memchr()` 返回的是缓冲区首地址，不是命中的那个字节（2026-09-19）

**文件**
- `kernel/lib/libc/string.cpp` —— `memchr()` 命中时 `return (void *)p` 改为
  `return p + i`。

**症状**
内核里任何一次「在字符串里找某个字符」都返回 0。直接的表现是启动选项解析全错：
OBI 命令行里明明写着 `kernel.select.userboot=shell`，
`obi::CommandLineView::value("kernel.select.userboot")` 却返回 none，内核于是用
默认值启动了 userboot 而不是 shell。加临时日志抓到的就是这一行：

```
DIAG: text size=28 first=6B 65 72 at22=3D      ← text[22] 确实是 '='
DIAG: equals_pos=0 ...                          ← 但 find('=') 说是 0
```

**原因**
```cpp
for (size_t i = 0; i < n; i++) {
    if (p[i] == (char)c) {
        return (void *)p;     // 命中在 i，返回的却是 p
    }
}
```
返回值少加了一个 `i`。C 的 `memchr()` 约定返回**命中的那个字节的地址**，调用方拿
`result - base` 当下标；返回 `p` 等于永远宣称「命中在第 0 个字节」。

这个错之所以能一直藏着，是因为内核里没有一处代码按名字调用 `memchr()`——
调用它的是 libc++：`std::string_view::find(char)` 落到
`std::char_traits<char>::find`，后者落到这里。于是「找不到」是对的（返回
`nullptr` → npos），「找到了」永远说在 0。调用方检查的是 `== npos`，拿到的是一个
看起来合理的下标，不是一个错误码，所以没人会怀疑。

具体到 `CommandLineView::value()`：
```cpp
auto const equals = token.find('=');                       // 22 → 实际得到 0
if (equals != npos && token.substr(0, equals) == key) {    // substr(0,0) 是空串
```
键变成空串，于是任何 `key=value` 选项都匹配不上任何键。所有走这条路的启动选项都
是坏的，不止 `kernel.select.userboot` —— `contains()` 走的 `find_token()` 同理。

**改法**
```cpp
auto const *p = static_cast<char const *>(str);
for (size_t i = 0; i < n; i++) {
    if (p[i] == static_cast<char>(c)) {
        return p + i;
    }
}
return nullptr;
```
顺手把 `(char)c`、`(void *)`、`NULL` 换成 `static_cast` 与 `nullptr`，其余不动。

被否掉的方案：把签名改成 C 标准的 `void *memchr(void const *, int, size_t)` 并加
`extern "C"`。试了，编不过：本树自带的 freestanding 头
`system/lib/libc/include/string.h` 声明的是 `const void *memchr(...)`，改签名就得
一起改那个共享头（phys 侧也吃它），那是另一件事。记在
`docs/repairs/260919/libc.md` 里。

**参照**
`Linux lib/string.c 的 memchr()`：
```c
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n-- != 0) {
        if ((unsigned char)c == *p++) {
            return (void *)(p - 1);   // 返回命中处
        }
    }
    return NULL;
}
```
注意它 `return (void *)(p - 1)` 那一步——正是本函数漏掉的东西。

**验证**
- 构建：`prebuilt/ninja/ninja -C out/bringup.x86` 全过。
- clang-format 已跑；clang-tidy 见下条改动（同一轮）。
- QEMU 实测 `kernel.dev.obi`：修之前 `userboot: found 'userboot'`，修之后
  `userboot: found 'shell', 1368 bytes of ELF`，并且 ring 3 打出
  `ours shell: the kernel's command registry, seen from ring 3`。命令行里只有
  `kernel.select.userboot=shell` 这一项，所以这条日志直接就是 `find('=')` 修好了的
  证据。

**故意没做**
- 没给 libc 加单元测试。这个库现在没有测试目录，建一套是独立的一件事；本次的回归
  证据是启动日志。
- 没碰 `system/lib/libc/include/string.h` 里那一串返回 const 指针的声明，也没补
  `strchr`/`strstr` 等「只声明没定义」的函数，见 repairs。
