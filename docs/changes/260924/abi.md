# abi 改动记录（2026-09-24）

## `KoId` 改为 u64（2026-09-24）

**文件**
- `system/public/ours/types.hpp` — `using KoId = u64;`，并注释为什么
- `kernel/include/ours/koid.hpp` — `KoIdGenerator::generate()` 的 count 形参改为 `usize`
- `kernel/object/handle.cpp` — `static_assert(sizeof(Handle) == 40)`

**症状**
潜伏。koid 是 32 位，用户程序循环 `channel_create` 就能把它耗尽并回绕。

**原因**
koid 的契约只有一条：在系统的整个生命周期里唯一，永不复用。句柄表的第五项检查
（句柄上记的 table koid 等于本表的 koid）、将来的 `object_get_child(koid)`、调试器
按 koid 找对象，都建立在这条契约上。

32 位守不住它。每个 dispatcher、每张句柄表各取一个，一次 `channel_create` 取两个；
按每微秒一个算（单核上紧循环创建），2^32 在 72 分钟内用完。回绕之后：

- 先发出 0，也就是 `KOID_INVALID`，同时也是 `Handle::table_koid()` 表示"不在任何表里"
  的值——一个对象的 koid 和"无"撞在一起；
- 再往后发出仍在使用的 id：一个句柄被移进另一张表，而那张表恰好与原表同 koid，
  本该失败的表检查会通过。

这个值会进 ABI（P1-B 冻结 `InfoHandleBasic` 时 koid 是其中的字段），所以必须在那之前改。
按同样的速率，2^64 能用五十多万年。

**改法**
改前：

```cpp
using KoId = u32;
...
    static auto generate(KoId count = 1) -> KoId {
```

改后：

```cpp
using KoId = u64;
...
    static auto generate(usize count = 1) -> KoId {
```

`generate` 的形参是"要几个"，是数量而不是 id，所以用 `usize`，与 KoId 的宽度脱钩。

`Handle` 的大小没有变：原来 `m_table_koid`（4 字节）之后有 4 字节填充，为的是让
`m_dispatcher` 的指针按 8 对齐；现在 8 字节的 koid 正好占满。仍是 8 + 8 + 4 + 4 + 16 = 40。
句柄 arena 是静态的 `HANDLE_ARENA_SLOTS × sizeof(Handle)`，给 Handle 加一个成员就是给每个
槽加，所以用 `static_assert` 把 40 钉住，让以后的增长是一个明确的决定而不是副作用。

**参照**
`Zircon zx_koid_t`（`uint64_t`），以及 `KernelObjectId::Generate()`。

**验证**
- 构建通过；触碰 `types.hpp` 和 `koid.hpp` 后全量重编，没有新增的转换或窄化告警。
- 两个镜像启动正常，init↔shell 对话与 shell 的句柄检查照常。

**故意没做**
- `prof` 的 `current_tid()` 仍返回 u32，`prof_pmu.cpp` 里是显式 `static_cast<u32>`：
  采样记录的格式是 prof 自己的，改宽属于 prof 的事。线程 id 什么时候改用 dispatcher
  的 koid 是 P1-B 的事。
- `Thread::spawn()` 里 `KOID_INVALID == tid` 的检查保留：u64 下实际不会触发，但它表达的是
  "生成器不发 0"这条约定。
