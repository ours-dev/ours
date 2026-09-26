# ustl 待修项（2026-09-26）

第 2 条是修 `Option<T &>` → `Option<T const &>` 时看到的，第 3 条是修 `Slist` 的 swap 时看到的，都不是那两次改动引入的
（两次改动都见 `docs/changes/260926/ustl.md`）。第 1 条（`Slist` 的 swap 在空表上把表头接错）已修。按严重程度排，编号不重排。

---

## 3. 线性 `Slist`（`Linear<true>`）不能移动、swap、sort：`LinearSlistAlgorithms` 没有 `swap_nodes`

`Slist::swap()` 无条件调用 `Algorithms::swap_nodes(header(), other.header())`，但 `Linear<true>` 时 `Algorithms` 是
`LinearSlistAlgorithms`，它只有 `swap_trailing_nodes`。移动构造、移动赋值、`sort()` 都经过 `swap()`，所以一旦实例化就编不过。
用一个 `Slist<Value, Linear<true>>` 做移动构造的小程序复现：

```
slist.hpp:295:21: error: no member named 'swap_nodes' in
  'ustl::collections::intrusive::LinearSlistAlgorithms<ustl::collections::intrusive::SlistNodeTraits<void *>>'
```

潜伏：类模板的成员不被调用就不实例化，目前没有代码移动或 swap 线性 `Slist`。

Boost.Intrusive 的 `slist_impl::swap()`（`system/lib/boost.intrusive/include/boost/intrusive/slist.hpp:721`）按选项分派：
`cache_last` 走 `priv_swap_cache_last()`，否则 `priv_swap_lists(..., bool_<LINEAR>())`——环形表调 `swap_nodes()`，线性表调
`swap_trailing_nodes()`。照这个在 `Slist::swap()` 里用 `if constexpr (PackedOptions::LINEAR)` 分开；`CACHE_LAST` 那一支对照
Boost 看 ustl 现在的 `refresh_last()` 做法够不够。修完给 `slist_test` 补一个线性 `Slist` 的移动和 `swap()` 用例。

## 2. 树容器里 12 处 `static_cast<T const &>` 已经多余（清理）

`bstree.hpp`、`rbtree.hpp`、`avltree.hpp`、`splaytree.hpp` 各 3 处，都在 `find`/`lower_bound`/`upper_bound` 的 const 版本里：

```cpp
: ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
```

这个 cast 当初是为了让 `some_ref` 推导出 `Option<T const &>`，因为 `Option<T &>` 不能转 `Option<T const &>`。现在能转了，
去掉 cast 写成和 list/sgtree 一样的 `ustl::some_ref(*ValueTraits::to_value_ptr(node))`，返回类型不变。
不影响正确性，只是让 intrusive 容器只剩一种写法。改完用 `intrusive-const-accessors-test` 的做法给这四棵树也钉一下返回类型。
