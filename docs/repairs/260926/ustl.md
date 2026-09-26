# ustl 待修项（2026-09-26）

修 `Option<T &>` → `Option<T const &>`（见 `docs/changes/260926/ustl.md`）时看到的，不是那次改动引入的。按严重程度排。

---

## 1. `ustl/test/` 下大部分测试没有任何 target，从来不编

`system/lib/ustl/test/` 有 88 个测试文件（不算这次新增的），`system/lib/ustl/BUILD.gn` 只给其中 16 个建了 `executable`，
根 `BUILD.gn` 的 `default` 组只带了 bitfields、bitset、rc 三个（这次加了 option 和 intrusive-const-accessors）。
`//system/lib/ustl:tests` 和 `//system/lib:tests` 都没有人引用。

后果：其余测试不会随默认构建编译，头文件改坏了它们也不报错；这次的 const 访问器编译错误能潜伏下来，这是原因之一。

判断依据：

```
ls system/lib/ustl/test/*.cpp | wc -l          # 89（含新增的 1 个）
grep -c 'sources = \[ "test/' system/lib/ustl/BUILD.gn   # 17（含新增的 1 个）
```

已知情况：`list_test`、`slist_test`、`sgtree_test`、`treap_test`、`unordered_set_test`、`rbtree_test`、`set_test`
用宿主工具链的编译参数（`out/bringup.x86/compile_commands.json` 里 `rc_test.cpp` 那一条）单独编译运行，都能编过、退出码 0。
其余的没试。

要做的：逐个给它们建 `executable`，并入 `ustl:tests`，再在根 `BUILD.gn` 以 `($host_toolchain)` 挂进 `default`
（ustl 的测试都是宿主上的 `int main()` + `<cassert>` 程序）。编不过的单独记下来，不要为了接进来改测试的断言。

## 2. 树容器里 12 处 `static_cast<T const &>` 已经多余（清理）

`bstree.hpp`、`rbtree.hpp`、`avltree.hpp`、`splaytree.hpp` 各 3 处，都在 `find`/`lower_bound`/`upper_bound` 的 const 版本里：

```cpp
: ustl::some_ref(static_cast<T const &>(*ValueTraits::to_value_ptr(node)));
```

这个 cast 当初是为了让 `some_ref` 推导出 `Option<T const &>`，因为 `Option<T &>` 不能转 `Option<T const &>`。现在能转了，
去掉 cast 写成和 list/sgtree 一样的 `ustl::some_ref(*ValueTraits::to_value_ptr(node))`，返回类型不变。
不影响正确性，只是让 intrusive 容器只剩一种写法。改完用 `intrusive-const-accessors-test` 的做法给这四棵树也钉一下返回类型。
