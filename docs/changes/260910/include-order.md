# 全树 include 重排

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 按新的四类重排全树 include（2026-09-10）

**文件**
- 402 个 `.cpp`/`.hpp`，`clang-format -i` 机械重排。
- `kernel/mm/include/ours/mm/types.hpp` — 唯一一处手改，见下。

**症状**

无。这是 `cd77252` 把 include 分组收成四类之后的落地。

**原因（真正值得记的那部分）**

重排本身是机械的，但它**暴露了一个潜在缺陷**——`kernel/mm/include/ours/mm/types.hpp`：

```cpp
#include <arch/paging/controls.hpp>   // 原来排在最前
...
#include <ours/mm/cfg.hpp>

namespace ours::mm {
using arch::paging::HarvestControl;   // 未限定的 arch::
```

重排把 `<ours/...>` 提到最前之后立刻编不过：

```
error: no member named 'paging' in namespace 'ours::arch'; did you mean '::arch::paging'?
```

因为 `ours/mm/cfg.hpp` 里声明了 `ours::arch`，而在 `namespace ours::mm` 内部，
未限定的 `arch::` **先找到 `ours::arch`**。原来能编，只是因为 `<arch/paging/...>` 恰好排在
所有 `ours/` 头之前，那一刻 `ours::arch` 还没被声明——**语义正确性挂在 include 顺序上**，
这本身就是缺陷，重排只是把它掀出来了。

**改法**

从全局命名空间限定：

```cpp
using ::arch::paging::HarvestControl;
using ::arch::paging::MapControl;
using ::arch::paging::MmuFlags;
using ::arch::paging::UnmapControl;
```

**故意没做**

- 没有碰工作区里别人正在改的文件（重排前按 `git status` 排除），它们随下次改动自然收敛。
- `vendor/`、`kernel/lib/eftl/`、`.legacy/` 有各自的 `.clang-format` 或不在范围内，未纳入。

**验证**

构建通过；QEMU 两个镜像实测：dbg 无断言、16.9 万行线程输出；dev 正常存活。

---

## 2. `kernel/lib` / `system/lib` 下每个库先排自己的头（2026-09-10）

**文件**
- 24 个库根目录各新增一份 `.clang-format`（`kernel/lib/` 18 个，`system/lib/` 6 个）。
- 这些库里的 199 个源文件按新配置重排。

**症状**

无。第 1 条把全树统一成「`ours/` 在前」，但对库自身而言那是错的读序：`gktl` 的文件里
`ours/` 是**外部依赖**，`gktl/` 才是自己的头。

**原因**

分组按库归属，而「哪个前缀算自己」取决于**正在被格式化的文件属于哪个库**——一条全局正则
表达不了。根配置原来的注释就写着这一点，并指出解法是在库根放一份 `.clang-format`；
`kernel/lib/eftl` 一直是唯一照做的（但它还是旧的六组）。

**改法**

每个库根一份，用 `BasedOnStyle: InheritParentConfig` 继承根配置，只替换 `IncludeCategories`，
把第 1 组从 `^<ours/` 换成该库自己的前缀。分组数仍是四组：

```yaml
---
BasedOnStyle: InheritParentConfig
IncludeCategories:
  # 1. This library.
  - Regex: '^<gktl/'
    Priority: 1
  # 2. C++ standard library.
  - Regex: '^<[a-bd-z][a-z_0-9]*>$'
    Priority: 2
  ...
  # 4. Everything else, angle-bracketed or quoted.
  - Regex: '.*'
    Priority: 4
```

效果（`kernel/lib/gktl/` 下的文件）：

```cpp
#include <gktl/canary.hpp>

#include <atomic>

#include <cstdio>

#include "local.hpp"
#include <ours/types.hpp>
#include <ustl/rc.hpp>
```

注意 `IncludeCategories` 在 `InheritParentConfig` 下是**整体替换**而非追加，所以每份都要
写全四组——这也是为什么用模板生成而不是手写。

**故意没做**

- `system/lib/boost.intrusive` 不给（vendored，且本项目不依赖 Boost）；
  `system/lib/libc` 不给（它的 `include/` 直接放裸 C 头，没有属于自己的前缀）。
- `kernel/lib/eftl` 原有的六组配置一并换成四组，与根配置保持同一套分组。

**验证**

构建通过；QEMU 两个镜像实测：dbg 无断言、16.7 万行线程输出；dev 无异常且存活。

---

## 3. 补上 `kernel/lib` 之外的库（2026-09-10）

**文件**
- 6 个库根各新增一份 `.clang-format`：`kernel/arch/{arm,riscv,x86}/lib/arch`（前缀 `arch`）、
  `kernel/phys/lib/{bootmem,bootshim,multiboot}`。
- 这些库里的 18 个源文件重排。

**原因**

第 2 条只扫了 `kernel/lib/` 和 `system/lib/`，但「库」不只住在这两处：架构相关的 `arch` 库
在 `kernel/arch/<cpu>/lib/`，phys 的三个库在 `kernel/phys/lib/`。判据不是路径，是
**它有没有自己的 include 前缀**。

按这个判据把全仓库过了一遍，结论是只剩这 6 个。其余带 `include/` 的目录
（`kernel/`、`kernel/mm`、`kernel/object`、`kernel/phys`、`kernel/platform`、`kernel/driver`、
`kernel/arch/x86/{kernel,mm,phys}`）前缀都是 `ours`，本来就归根配置的第 1 组管，不需要
也不应该单独配。

**验证**

构建通过；QEMU 两个镜像：dbg 无断言、17.3 万行线程输出；dev 无异常且存活。
