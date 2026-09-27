## capture_sample_context() 的返回值由 std::optional 改为 ustl::Option（2026-09-27）

**文件**
- `kernel/lib/prof/include/prof/arch.hpp`：接口的返回类型
- `kernel/arch/x86/kernel/prof_pmu.cpp`：x86 实现，`std::nullopt` 改为 `ustl::NONE`
- `kernel/lib/prof/src/sampler.cpp`：调用方改用 `is_none()` 判断
- `kernel/lib/prof/test/fake_arch.hpp`、`fake_arch.cpp`：测试替身跟着接口改

**症状**
潜伏，当前触发不到。`Sampler::on_tick()` 在解引用之前已经检查过 `has_value()`。

**原因**
本构建把 libc++ 的 hardening 设成了 NONE。对空的 `std::optional` 做 `*` 或 `->`
不会有任何检查：会读到未初始化的 `SampleContext`，然后把一条垃圾样本写进环形缓冲区，
事后根本看不出这条样本是假的。`ustl::Option` 做同样的访问时会 `__builtin_trap()`。
这个函数在时钟中断里调用，漏掉一次检查的后果恰恰是最难查的那种。

这和同一天 `UserCopyCaptureResult::fault` 的改动是同一个理由，见
`260927/x86-kernel.md`。

**改法**

之前：

```cpp
auto capture_sample_context() -> std::optional<SampleContext>;
...
if (!context.has_value()) {
```

之后：

```cpp
auto capture_sample_context() -> ustl::Option<SampleContext>;
...
if (context.is_none()) {
```

x86 实现里的 `return prof::SampleContext{...};` 不用改，它会隐式转换成 `Option`。
测试里 `set_next_sample_context(prof::SampleContext{...})` 也一样不用改。

**参照**
无对应物。Linux 的 perf 用 `struct pt_regs *` 加空指针表示"没有上下文"。

**验证**
- 主机测试 `prof/test:sampler` 和 `prof/test:format` 通过。
- `kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 均构建通过。
- QEMU 实测：两个镜像都无 panic，`usercopy.test` 都是 18/18。启动时没有专门
  开启 tick 回退采样，所以这条路径在内核里没有被实测。

**故意没做**
`prof/cli/main.cpp` 里的 `std::optional` 没换。那是主机上跑的工具，本来就在用
`std::vector`，而且主机的 libc++ 不受内核 hardening 设置的影响。
