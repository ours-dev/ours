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
