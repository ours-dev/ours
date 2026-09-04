# kernel/lib/gktl — 改动记录

格式见 `docs/changes/TEMPLATE.md`。

---

## 1. 计数器的 per-CPU 槽被 --gc-sections 回收，导致读到并且改坏别的 per-CPU 变量（2026-09-03）

**文件**
- `kernel/kernel.ld` — `.init.data.cpu_local.gktl.counter.storage` 加 `KEEP()`。

**症状**

`SpinLock::num_held_cpu()` 返回 `0x0400b000_00000000`、`...001`、`...002`——低半个字是个
能正常自增的计数，高半个字是无关的垃圾。连带 `reschedule_common()` 里的
`DEBUG_ASSERT(SpinLock::num_held() > 0)` 形同虚设。

这只是从外面能看到的那一面。真实情况更糟：**每个 `GKTL_COUNTER` 都在读写别的 per-CPU 变量**，
`add()` 每调用一次就改坏一个不属于它的字。

**原因**

`GKTL_COUNTER_DEFINE` 展开出两个对象：描述符 `COUNTER_DESCRIPTOR_##VAR` 进
`.gktl.counter.descs`，存储 `COUNTER_STORAGE_##VAR` 进
`.init.data.cpu_local.gktl.counter.storage`。`Counter` 只持有 `&COUNTER_DESCRIPTOR_##VAR`，
槽是靠 `COUNTERS_START[descriptor 下标]` 算出来的——**没有任何东西按名字引用存储对象**。

内核开着 `--gc-sections`（`//build/config:default_linker_gc`）。链接脚本里描述符那一处是
`KEEP(*(SORT_BY_ALIGNMENT(.gktl.counter.descs)))`，存储这一处却是裸的
`*(.init.data.cpu_local.gktl.counter.storage)`。宏上的 `USED`
（`__attribute__((used))`）只拦得住**编译器**，拦不住链接器。描述符另有活的引用者，
所以活下来了；存储没有，于是**每一个都被回收**。

后果是连锁的。整个 section 变空，`__gktl_counter_start` 就和 `__cpu_local_start` 落在同一个
地址，`COUNTERS_START[i]` 于是索引进下面那条 catch-all
`*(SORT_BY_ALIGNMENT(.init.data.cpu_local*))` 摆在 per-CPU 区开头的东西里。
读到的是别人的字，写进去的也是。

证据（改动前最后一次成功链接的 `vmours.with-tests`）：

| 事实 | 值 |
|---|---|
| `COUNTER_DESCRIPTOR_*` 符号数 | 27 |
| `COUNTER_STORAGE_*` 符号数 | **0** |
| `__gktl_counter_start` | `0x77000` |
| `__cpu_local_start` | `0x77000`（同一个地址） |
| `sync.spin_lock.cpp.o` 里该 section | 存在，8 字节 |

目标文件里 section 是有的，链接后一个不剩——正是 GC 回收的形状。

**改法**

一个 `KEEP()`，和描述符那一处对齐。旁边补了一段注释说明为什么不能省：这个 section 天生
没有引用者，是设计使然（槽靠下标算，不靠符号），所以它必须被显式保住。

**参照**

Zircon 的 `kcounter` 用同一套「描述符 + 按下标寻址的 per-CPU 槽」结构，
`kernel.ld` 里对应的输入 section 同样是 `KEEP` 的。

**验证**

- 静态证据如上表，链接脚本两处的不对称也是直接可见的。
- **未链接验证**：当前树被另外两个会话的在途改动卡住编译
  （`kernel/mm/page_queues.cpp` 引用了尚不存在的 `Thread::sleep_until`；
  `kernel/mm/init_vmm.cpp`、`vm_area.cpp` 引用了正在改名中的 `VmMapOption::Fixed`），
  链接不出新镜像。树能编之后要补的确认是一条命令：
  `llvm-nm vmours.with-tests | grep -c COUNTER_STORAGE_` 应当是 27 而不是 0，
  且 `__gktl_counter_start` 不再等于 `__cpu_local_start`。

**故意没做**

- 没有动链接脚本里 `.gktl.counter.descs` 被放置两次（第 96-98 行无 KEEP、
  第 151-153 行带 KEEP）这件事。描述符 27 个都在，说明当前解析结果是对的，
  但两处 `PROVIDE_HIDDEN` 同名符号是个独立的隐患。
- 没有查 `__gktl_counter_end` / `__gktl_counter_desc_end` 在符号表里不存在这件事。
  `PROVIDE` 只在符号被引用时才定义，而 `Counter::COUNTERS_END` 没有用户，所以是正常的。

### 验证补记（2026-09-03，树恢复可编之后）

在本机独立跑了一遍，不是转述别人的结果：

```
COUNTER_STORAGE_    = 27
COUNTER_DESCRIPTOR_ = 27
```

改动前是 0 对 27。存储 section 现在扛过了 `--gc-sections`，27 个槽真实存在。

**上面写的第二条检查是错的，在此更正**：`__gktl_counter_start` 现在仍然等于
`__cpu_local_start`（都是 `0x75000`）。这不是缺陷，是链接脚本的结构使然——
`__cpu_local_start` 之后紧接着就是 `__gktl_counter_start`，中间没有任何东西，
两者本来就必定重合。真正有意义的差别在后面：以前这个 section 是空的，
catch-all glob 摆的 per-CPU 变量就从 `0x75000` 开始，和计数器槽重叠；
现在 27 个槽先占掉 216 字节，那些变量被推到后面去了。

所以能用来判断这个缺陷的检查只有一条：`COUNTER_STORAGE_` 的符号数，
而不是两个 start 符号是否相等。

debug 镜像可以启动，没有断言失败。
