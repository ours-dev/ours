# ustl 待修项（2026-09-26）

第 2 条是修 `Option<T &>` → `Option<T const &>` 时看到的；第 5、6 条是修线性 `Slist` 的 swap 时看到的，都不是那几次改动
引入的（各次改动都见 `docs/changes/260926/ustl.md`）。第 1、3、4 条已修。按严重程度排，编号不重排。

---

## 5. `CacheLast` 名不副实：每次插入、删除都从表头走一遍

`CACHE_LAST` 的用处是让 `push_back()` 变成 O(1)。但 `insert_after()`、`erase_after_and_dispose()`、`splice_after()`、`swap()`、
`reverse()` 最后都调 `refresh_last()`，而它是 `m_last = Algorithms::get_previous_node(header(), end_node())`，从表头走到末尾，
O(n)。于是开了 `CacheLast` 之后，`push_back()` 读 `m_last` 是 O(1)，紧接着的 `refresh_last()` 又走一遍整表；而且每次修改
都多走一遍。结果是对的，只是这个选项白开。

Boost 按操作就地维护缓存：`insert_after()` 里 `if (get_last_node() == prev_n) set_last_node(n);`
（`system/lib/boost.intrusive/include/boost/intrusive/slist.hpp:832`），`swap()` 走 `priv_swap_cache_last()`，只在确实需要时
才重新找末尾。照这个把各操作里的 `refresh_last()` 换成就地更新。目前没有使用者。

## 2. 树容器里 12 处 `static_cast<T const &>` 已经多余（清理）

`bstree.hpp`、`rbtree.hpp`、`avltree.hpp`、`splaytree.hpp` 各 3 处，都在 `find`/`lower_bound`/`upper_bound` 的 const 版本里：

```cpp
: ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
```

这个 cast 当初是为了让 `some_ref` 推导出 `Option<T const &>`，因为 `Option<T &>` 不能转 `Option<T const &>`。现在能转了，
去掉 cast 写成和 list/sgtree 一样的 `ustl::some_ref(*ValueTraits::to_value_ptr(node))`，返回类型不变。
不影响正确性，只是让 intrusive 容器只剩一种写法。改完用 `intrusive-const-accessors-test` 的做法给这四棵树也钉一下返回类型。

## 6. `slist.hpp` 里 `NoSize`/`NoLast` 的注释引用了已经不存在的用法（文档）

`Slist` 私有区 `NoSize`/`NoLast` 上的注释说 Slab 在页帧描述符里嵌了一个 `Slist<Object, ConstantTimeSize<false>>`，并
static_assert 它只有一个指针宽（"see kernel/mm/slab.hpp"）。现在 `kernel/mm/slab.hpp` 用的是裸的 free-list 指针，全仓库
只剩这段注释提到这个用法。`[[no_unique_address]]` 本身的理由（选项关掉时不占空间）仍然成立，删掉 Slab 那一段即可。
