# build 改动记录

## user 工具链拿到公共头与 freestanding libc++，`kernel_libcxx_include` 改名（2026-09-08）

**文件**
- `system/BUILD.gn` — 新增 `config("user.headers")`，并入 `user.config`
- `build/config/BUILD.gn` — `kernel_libcxx_include` 改名为 `freestanding_libcxx_include`

**症状**
userboot 一旦 `#include <ours/syscall.hpp>`（它要拿系统调用编号），就编不过：

```
prebuilt/clang/llvm-23/bin/../include/c++/v1/__config:234:8: error: "No thread API"
```

**原因**
两层。

其一，`system/public/ours/types.hpp` 用的是不带限定的 `usize` / `u32`——那些别名来自
`kernel/include/ours/prelude.hpp`，内核工具链靠 `//kernel/products:headers` 强制
`-include` 它。user 工具链没有，所以公共头对它是编不了的。

其二，补上 prelude 之后 prelude 会 include `<cstddef>`，于是撞上 libc++ 的
`__config`：user 工具链没有 freestanding 的 libc++ 串接，clang 去找宿主的 libc++ 头，
而宿主那份没有线程 API 就直接 `#error`。

这套串接一直存在，名字叫 `kernel_libcxx_include`，并且在 BUILDCONFIG.gn 里被
`if (is_kernel)` 关着。可它的**内容从来不是内核特有的**——它做的事是"没有宿主 libc++
的时候，用 prebuilt 的头，并带上本项目的 libc++ 配置宏"。这是每一个 freestanding 环境
都需要的，而 kernel/kernel.phys 只是在此之前**恰好是仅有的两个** freestanding 环境。

**改法**
1. 改名，让非内核的工具链能理直气壮地引用它：

   改前
   ```gn
   # Freestanding libc++ wiring for kernel C++: ...
   config("kernel_libcxx_include") {
   ```
   改后
   ```gn
   # Freestanding libc++ wiring: ...
   #
   # It was called kernel_libcxx_include while the kernel and phys environments
   # were the only freestanding ones. They are not any more ...
   config("freestanding_libcxx_include") {
   ```

2. `system/BUILD.gn` 里把用户态需要的头串接成一条 config，并入 `user.config`：

   ```gn
   config("user.headers") {
     configs = [
       "//system/public:public.config",
       "//build/config:freestanding_libcxx_include",
     ]
     inputs = [ "//kernel/include/ours/prelude.hpp" ]
     cflags_cc = [
       "-include",
       rebase_path(inputs[0], root_build_dir),
     ]
   }
   ```

   prelude 放在 `//kernel/include` 下，从用户态这边引用读着别扭，但它确实是对的文件：
   凡是 include 公共头的工具链都需要那些别名。`//kernel/products:host-prelude` 出于
   一模一样的理由把同一个文件强制塞进 host 编译。

**参照**
`//kernel/products:host-prelude`（本树）：同一个文件、同一个理由，只是换成 host 工具链。

**验证**
`ninja` 通过；userboot 现在能 `#include <ours/syscall.hpp>` 并用 `OURS_SYS_DEBUG_WRITE`
编出来，实测能发出这次调用（见 `docs/changes/260908/userabi.md`）。

**故意没做**
prelude 没有搬家。把那些全局别名从 `//kernel/include` 移到 `//system/public` 才是它们
该在的地方（三个工具链都在强制包含一个名字里带 kernel 的路径），但那会动到每一个内核
目标，值得单独一条改动。
