# syscall 改动记录（2026-09-24）

## 系统调用有了唯一的描述：`syscalls.gni` 与生成的私有调用号头（2026-09-24）

**文件**
- `system/public/ours/syscalls.gni`（新建）— 8 个调用的描述，条目位置即调用号
- `build/syscall/gen_syscalls.py`（新建）— 读描述的 JSON，全部校验，写出 `syscall_abi/numbers.hpp`
- `system/public/BUILD.gn` — 默认工具链里的 `generated_file` + `action`，以及各工具链共用的 `group("syscalls.private")`
- `kernel/lib/syscall/BUILD.gn` — 依赖 `syscalls.private`
- `kernel/lib/syscall/src/table.cpp` — 生成的号与手写的 `OURS_SYS_*` 逐个 `static_assert`

**症状**
没有运行时症状，是结构问题：一个调用的形状写在四处——`syscall.hpp` 的调用号宏、
`vdso.S` 的桩、`vdso.hpp` 的 extern "C" 声明、`handlers.hpp` 的 `sys_*` 声明加
`table.cpp` 的 shim——彼此之间没有任何编译期关联。改错一处，结果是一个调用的参数被送进
另一个调用的处理函数，或者寄存器错位，而构建照常通过。

**原因**
这是 P0 为了先跑通而手写的代价（设计 4.7 当时就写明"P0 的 6 个桩手写，生成器是 P1-A 的
第一件事"）。四处手工同步能维持多久取决于改的人记不记得全部四处；每加一个调用，漏改的
机会就多一次。Zircon 用 kazoo 从 FIDL 生成全部这些东西，出发点相同。

**改法**
一个描述文件，一个生成器，本条先只接出第一样产物（调用号头），其余在第 5–10 条逐个接上。

描述是 GN scope 列表而不是另起一种格式：GN 是这棵树已有的唯一解析器，`generated_file`
加 `output_conversion = "json"` 把它变成 JSON，生成器只用标准库读 JSON（宿主 python 是
3.9，没有 tomllib）。条目示例：

```gn
  {
    name = "channel_create"
    brief = "Makes a channel and writes a handle to each end into |out0| and |out1|."
    doc = [ ... ]
    args = [
      { name = "options"  type = "ChannelCreateOptions" },
      { name = "out0"     type = "HandleValue"  dir = "out" },
      { name = "out1"     type = "HandleValue"  dir = "out" },
    ]
    attrs = []
    ret = "Status"
  },
```

生成器在每次运行时做全部校验，不论要求写哪些产物——坏条目应该被第一个看到它的构建拒绝，
而不是等到某个产物恰好绊倒：参数超过 8 个；恰好 7 个（vDSO 还没有 7 参数桩，第一个需要的
是 P1-C 的 `vmar_map`）；调用重名或参数重名；`count` 指向不存在的参数或指向一个指针
（长度若经指针读，读和拷贝之间可以被改）；未知的 attr、dir、键；`release` 用在按值传的
`HandleValue` 以外。

产物只在默认工具链里生成一次。它不依赖工具链，而默认工具链本来就只描述 action 和元数据图、
不编译任何东西。其他工具链通过 `group("syscalls.private")` 拿到：`public_deps` 跨工具链指向
生成动作，`public_configs` 给出生成目录的 include 路径。这个 group 的 `visibility` 只放行
`//kernel/lib/syscall/*` 和 `//kernel/lib/userabi/vdso/*`——调用号是内核与 vDSO 之间的私有
约定（D8），用户程序依赖它就是依赖了 vDSO 存在的意义所要隐藏的那样东西。

生成器只在内容变化时重写输出，配合 GN 给 action 的 `restat = 1`，描述里只改了注释不会
触发重编。没有写 depfile：生成器只读 GN 已知的那一个 JSON，没有隐藏输入。

生成的头：

```cpp
namespace ours::syscall_abi {
inline constexpr auto SYSCALL_COUNT = usize{8};

inline constexpr auto SYS_DEBUG_WRITE = usize{0};
...
inline constexpr auto SYS_PROCESS_SPAWN_BOOT = usize{7};

inline constexpr std::string_view SYSCALL_NAMES[SYSCALL_COUNT] = { "debug_write", ... };
}
```

`table.cpp` 在过渡期逐个断言新旧两套号相等：

```cpp
static_assert(syscall_abi::SYS_DEBUG_WRITE == OURS_SYS_DEBUG_WRITE);
...
```

下一步（第 6 条）表由生成的 shim 填，这些断言随手写的宏一起删除。

**参照**
Zircon 的 kazoo（`zircon/tools/kazoo`）从 `//zircon/vdso/*.fidl` 生成 `zx-syscall-numbers.h`、
vDSO 桩、内核 wrapper 与分发；设计 4.7。

**验证**
- 增量构建、干净构建（新建 `out/clean.x86`，复制 args.gn，全部目标）都通过。
- 干净构建的 ninja 图里，`table.cpp` 的编译对 `phony/system/public/syscalls.gen` 有 order-only
  依赖，生成头一定先于使用者。
- 本地对调描述里前两条的名字（未提交）：
  `error: static assertion failed due to requirement 'syscall_abi::SYS_DEBUG_WRITE == 0'`。
- 逐条喂坏输入给生成器，全部以非零退出并说明原因：9 个参数、7 个参数、调用重名、
  悬空 count、未知 attr、u32 上的 release、out 指针上的 release、未知 dir。
- 两个镜像启动正常，init↔shell 对话照常。

**故意没做**
- 生成器暂不校验 `type` 是否真有其类型：类型名写错会在编译生成的声明时报错（第 6、9 条），
  比在生成器里维护一份类型表更不容易与代码脱节。
- 描述里的 `doc` 这一步还没有被任何产物用到；第 9 条生成 `vdso.hpp` 的声明时用。
