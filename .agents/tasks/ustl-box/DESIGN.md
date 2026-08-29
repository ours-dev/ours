# ustl::Box — Rust 式独占所有权智能指针

## 目标

在 `system/lib/ustl` 中实现仿 Rust `Box<T>` 的独占所有权智能指针，与
`rc.hpp` 同目录（`include/ustl/box.hpp`），补上本仓库所有权词汇表的缺口：
现有 `Rc`/`Arc`/`Weak` 覆盖共享所有权，裸指针覆盖借用，唯独缺少
`std::unique_ptr` 的替代品（内核代码禁用 `std::unique_ptr`）。

## 设计决策

1. **API 以 Rust `Box` 为蓝本**，按仓库习惯裁剪/扩展：
   - `Box<T>::make(args...)` ≈ `Box::new`，且原地构造（免移动临时对象）。
   - `from_raw` / `into_raw` / `leak` / `swap` 与 Rust 同名同语义。
   - `clone()` 对应 Rust 的 `#[derive(Clone)]`（要求 `T` 可拷贝构造）；
     拷贝构造/赋值本身 `= delete`。
   - 与 Rust 的差异：C++ 侧允许空（默认构造为空，同 `Rc` 的形态，
     便于成员声明）；多态销毁要求虚析构（Rust 由胖指针保证）。

2. **存储回收沿用 Rc 的 Disposer 契约**：析构 = `~T()` 之后
   `T::Disposer`（若有）或 `operator delete` 回收存储。为此把 rc.hpp 里的
   `detail::HasDisposer` / `detail::default_deallocate` 抽取到共享头
   `include/ustl/detail/deallocate.hpp`，rc.hpp 改为包含它（公共 API 不变）。
   对象缓存（slab）分配的类型由此获得与 Rc 一致的回收路径（`from_raw` 收养）。

3. **不做的事**：
   - 不支持 `Box<[T]>`/分配器参数（Rust 侧也是 unstable）。
   - 不提供 `adopt` 免费函数（`from_raw` 即收养；Rust 语义）。
   - 不进入 ktl 层（`new (gaf)` 变体以后按需加）。

## 交付物

- `include/ustl/detail/deallocate.hpp`（新，自 rc.hpp 抽取）
- `include/ustl/rc.hpp`（改为包含共享头，行为不变）
- `include/ustl/box.hpp`（新）
- `test/box_test.cpp`、`test/box_doc_examples_test.cpp`（新，宿主测试）
- `docs/box.md`（新，使用指南）
- `BUILD.gn`（headers 列表 + 测试 target）

## 验证

- `gn gen out/bringup.x86 && ninja` 构建 box 两个测试与全部 rc 回归测试，
  宿主直接运行。
- clang-format（llvm@22）+ clang-tidy（llvm@22，按 rc-test.ninja 的实际
  flags）过一遍所有改动的 C++ 文件。
