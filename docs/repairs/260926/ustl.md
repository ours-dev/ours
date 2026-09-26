# ustl 待修项（2026-09-26）

第 2 条是修 `Option<T &>` → `Option<T const &>` 时看到的，不是那次改动引入的（见 `docs/changes/260926/ustl.md`）。
第 1、3、4、5、6 条已修。编号不重排。

---

## 2. 树容器里 12 处 `static_cast<T const &>` 已经多余（清理）

`bstree.hpp`、`rbtree.hpp`、`avltree.hpp`、`splaytree.hpp` 各 3 处，都在 `find`/`lower_bound`/`upper_bound` 的 const 版本里：

```cpp
: ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
```

这个 cast 当初是为了让 `some_ref` 推导出 `Option<T const &>`，因为 `Option<T &>` 不能转 `Option<T const &>`。现在能转了，
去掉 cast 写成和 list/sgtree 一样的 `ustl::some_ref(*ValueTraits::to_value_ptr(node))`，返回类型不变。
不影响正确性，只是让 intrusive 容器只剩一种写法。改完用 `intrusive-const-accessors-test` 的做法给这四棵树也钉一下返回类型。
