# object 改动记录

## `Dispatcher` 的 koid 从未被初始化（2026-09-08）

**文件**
- `kernel/object/dispatcher.cpp` — 构造函数补上 `m_koid` 与 `m_signals`

**症状**
潜伏。每个 dispatcher 的 `koid()` 返回的是分配器留下的任意内容。至今没人读它，所以没暴露。

**原因**
```cpp
Dispatcher::Dispatcher(u32 magic) : m_canary(magic), m_lock(), m_handle_count() {}
```
初始化列表里没有 `m_koid`，也没有 `m_signals`。`KoIdGenerator::generate()` 早就存在
（`task::Thread` 用它发 tid），只是这里没调。

它为什么现在要紧：句柄表设计（`docs/architecture/object/handle-lookup.md`）的第 5 道关
是"`handle->handle_table_id() == 本表 koid`"，这是**进程间隔离**的那一道。表 koid 是
垃圾，这道关就能被碰巧通过。

**改法**
```cpp
Dispatcher::Dispatcher(u32 magic)
    : m_canary(magic)
    , m_koid(KoIdGenerator::generate())
    , m_lock()
    , m_handle_count()
    , m_signals(Signals::None) {}
```
和 `task::Thread` 共用同一个生成器，线程与包着它的 dispatcher 永不撞号。

**参照**
`Zircon Dispatcher::Dispatcher(zx_signals_t)`：`koid_(KernelObjectId::Generate())`。

**验证**
`ninja` 通过；QEMU 实测 `kernel.dev.dbg_1.obi` 无回归（`hello from ring 3` + 904 次
用户态中断）。

**故意没做**
`m_koid` 仍是 `KoId`（u32）。Zircon 是 64 位，本树 `KoId` 定义在 `types.hpp`（ABI），
改宽度是另一件事。
