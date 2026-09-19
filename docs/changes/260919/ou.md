# ou 改动记录（2026-09-19）

## `//system/lib/ou`：每个系统调用在用户态的具名入口（2026-09-19）

**文件**
- `system/lib/ou/` —— 新建库（copier 生成骨架）。`include/ou/str.hpp`（`ou::Str`）、
  `include/ou/debug.hpp` + `src/debug.cpp`（`ou::debug_write` / `ou::debug_line`）、
  `include/ou/command.hpp` + `src/command.cpp`（`ou::command_list` / `ou::command_text`）。
- `examples/shell/shell.cpp`、`examples/shell/BUILD.gn` —— 改用 ou，删掉自己手写的
  `Str`、`length_of`、分块写的 `Console`。
- `system/public/ours/syscall.hpp`、`kernel/lib/syscall/src/debug.cpp` —— 单次
  `debug_write` 的上限 `MAX_DEBUG_WRITE` 从内核私有常量挪进两边共用的 ABI 头。
  **注意：这两处改动被同一工作区里另一个会话的提交 `074b325`（*Control → *Options
  重命名）一并带走了**，内容就是这里描述的，只是挂在了那条不相关的提交信息下。

**症状**
潜伏。shell 自己写了一个借用字符串类型、一个有界 `strlen`、一个按 256 字节切分的输出
循环，而那个 256 是照着 `kernel/lib/syscall/src/debug.cpp` 里的私有常量手抄的——两边
没有任何编译期联系，内核改了上限，shell 只会在运行时收到 `OutOfRange`。第二个程序出现
时，这三样东西会被再抄一遍。

**原因**
缺一层。「怎么调用一个系统调用」目前散在每个程序里：知道原始符号名、知道参数怎么配对、
知道内核的限制。这些是**系统调用本身的属性**，不是某个程序的属性，应该只有一处知道。

Zircon 的做法是两层：`zx_*`（C，libzircon.so 导出）是原始 ABI，`zx::` C++ 库在其上给
每个对象和调用一个具名、有类型的入口。ours 现在有了前一层（`<ours/vdso.hpp>` +
加载时绑定），缺的是后一层。

**改法**
新建 `ou`，规则只有一条：**每个系统调用在这里有且只有一个名字**。哪怕不需要任何包装
（`ou::command_list` 就是纯转发），也要有这个名字——程序应该到这里找入口，而不是绕过它
直接调 `ours_*`。

shell 改前：
```cpp
class Console {
  public:
    static auto write(Str const text) -> void {
        auto const *at = text.data();
        auto left = text.size();
        while (left != 0) {
            auto const chunk = left < CHUNK ? left : CHUNK;
            static_cast<void>(ours_debug_write(at, chunk));
            ...
        }
    }
  private:
    /// Matches MAX_DEBUG_WRITE in kernel/lib/syscall/src/debug.cpp. ...
    static constexpr usize CHUNK = 256;
};
...
auto fetch() -> ours::Status { return ours_command_list(m_text, CAPACITY); }
```

改后：
```cpp
static_cast<void>(ou::debug_line(BANNER));
...
auto fetch() -> ours::Status { return ou::command_list(m_text); }   // 容量由数组类型推出
```

`MAX_DEBUG_WRITE` 进了 `<ours/syscall.hpp>`，内核与 ou 读同一个常量：
```cpp
// system/public/ours/syscall.hpp
inline constexpr auto MAX_DEBUG_WRITE = usize{256};
```

`ou::Str` 保留了手写实现而没有用 `std::string_view`，原因写在头文件里：用户程序
`-nostdlib`，`std::string_view` 在运行时调用 `strlen`/`memcmp`，链不上。这不是风格
选择，是等 ou 提供那几个 libc 函数之后就会变成别名的过渡。

`BUILD.gn` 断言只能在用户工具链里构建：这些入口会陷入内核，ring 0 里调用它们不是
系统调用。

**参照**
`Zircon //zircon/system/ulib/zx`（`zx::thread`、`zx::vmo` 等）在 `zx_*` 之上的那一层；
Fuchsia 程序不直接调 `zx_*` 的地方，正是经由它。

**验证**
- 构建通过；clang-format 干净。
- QEMU 实测两个镜像，shell 完整输出（横幅、命令表、`--- end ---`）。这一轮途中撞出的
  两个问题各自单独提交：`ba6d2de`（`-fpie`/`-fno-plt` 必须作用于每个 ring-3 目标）、
  `a51f4cd`（eftl 读不出没有 `DT_RELAENT` 的 `.rela.plt`，绑定器又把它当成没有）。

**故意没做**
- **线程生命周期**。内核现在只有 `debug_write` 与 `command_list` 两个系统调用，
  `HandleTable` 还是空壳（没有存储、没有实现文件），所以「创建线程并拿到句柄」这条路
  还不存在。能先做且不需要句柄的是 `thread_exit`、`thread_yield`、`nanosleep` 一类——
  下一步从 `ou::Thread::exit()` 开始，它同时让 shell 不必在末尾空转 `pause`。
- 没有宿主单测：`ou` 的逻辑（分块、有界长度）可以在宿主上测，但宿主测试目前没有挂进
  构建图的地方（见 `docs/repairs/260919/elfload.md` 第 2 条），一起做。
