# kernel/task（线程）— 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/task.md`。调度器那一层在 `scheduler.md`。

---

## 1. `Thread::spawn` 绑定的参数一多就会撞 `operator new` panic 桩

`kernel/include/ours/task/thread.hpp`，两个变参 `spawn` 模板：

```cpp
StartRoutine entry_point = ustl::function::bind(entry, ustl::forward<Args>(args)...);
```

`StartRoutine` 是 `std::function<int()>`，小缓冲 32 字节（宿主 libc++ 实测；内核 libc++
配置应一致）。`std::bind` 的结果 = 可调用对象 + 所有绑定参数的 tuple。裸函数指针不带参数
是 16 字节，塞得下；再多绑两个指针大小的参数就超了。超了 `std::function` 就去堆上要空间，
而这个内核的全局 `operator new` 是 `kernel/lib/libc/cpp_dependencies.cpp` 里的 panic 桩。

`dpc.cpp:19` 的 `Thread::spawn(name, prio, &Queue::do_work, this)` 已经是「成员函数指针
（16 字节）+ this（8 字节）」= 24 字节，离上限只差一个字。

严重性：目前没炸，但离炸只差一个参数，而且炸出来的信息只有 "operator new"，
和 `changes/260903/task.md` 第 1 条一模一样，会误导人去查重载决议。

方向：要么给 `StartRoutine` 换成不分配的 `ustl` 函数视图 / 固定容量 function，
要么让 `Thread` 内嵌一块参数存储。Zircon 的 `Thread::Create` 只收 `thread_start_routine`
+ 一个 `void *arg`，根本不绑定——这是最省事的形状。

## 2. `Thread::spawn` 两个重载家族的参数顺序不一致

非模板是 `(name, routine, priority)`，模板是 `(name, priority, entry, args...)`。
`changes/260903/task.md` 第 1 条修的那个无限递归就是这个不一致直接造成的：模板体按自己的
顺序回传，就一定选中自己。现在回传顺序已改对，但只要两套顺序还不一样，下一个在模板体里
写 `Self::spawn(name, priority, ...)` 的人还会掉同一个坑。

改法是统一成一种顺序。动的是接口，所有 `Thread::spawn` 调用点都要跟；单独一次改动。
