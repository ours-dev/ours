# kernel/task（线程）— 待修

按严重程度排。已修的从这里删掉，改动记录在 `docs/changes/260903/task.md`。调度器那一层在 `scheduler.md`。

---


## 1. `Thread::spawn` 两个重载家族的参数顺序不一致

非模板是 `(name, routine, priority)`，模板是 `(name, priority, entry, args...)`。
`changes/260903/task.md` 第 1 条修的那个无限递归就是这个不一致直接造成的：模板体按自己的
顺序回传，就一定选中自己。现在回传顺序已改对，但只要两套顺序还不一样，下一个在模板体里
写 `Self::spawn(name, priority, ...)` 的人还会掉同一个坑。

改法是统一成一种顺序。动的是接口，所有 `Thread::spawn` 调用点都要跟；单独一次改动。
