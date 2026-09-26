# kernel/object 待修项（2026-09-26）

## 1. `Handle::~Handle()` 先通知、后退还配额：被唤醒的一方可能还看到这个句柄记在创建者名下

**现象**：潜伏，今天碰不到。`kernel/object/handle.cpp` 的 `Handle::~Handle()` 先调 `m_dispatcher->on_zero_handles()`——对一个
channel 端来说，这一步在对端置 PEER_CLOSED 并唤醒等在上面的线程——然后才调 `m_account->refund()`，把这个句柄从创建者的配额里退掉
（K11 的每创建者配额）。于是一个被 PEER_CLOSED 唤醒的线程，如果恰好就是创建者、并且马上去数自己的配额，在另一个 CPU 上可能先于
`refund()` 跑到，看到的配额少一个。

**触发条件**：句柄是 A 进程建的、交给了 B，B 关掉（或死掉）时 A 正等在对端上，A 醒来后立刻用配额做判断。dbg_1 的 shell 就是这样：
它给 fault_probe 建回答 channel，把远端交过去，等 PEER_CLOSED，之后跑 `check_handle_quota()`。今天两者之间还隔着三组检查，窗口碰不到
（`docs/changes/260926/syscall.md`，fault_probe 迁到 runtime 那一节）。

**原因**：通知和退还是两件事，而"对方已经放手"这件事被发出去的时刻早于它真正完成的时刻。

**改法**：在 `Handle::~Handle()` 里把 `m_account->refund()` 挪到 `on_zero_handles()` 之前。被唤醒的一方经过 holder 锁与等待队列，
与 `refund()` 之间就有了先后关系，醒来时一定看得到已经退还的配额。改之前核对 Zircon `Handle::Delete()` / `HandleTableArena` 的顺序，
以及 refund 提前后会不会让"配额已退、对象还没被告知"成为另一种可观察的中间态（例如创建者在这段时间里用退回的额度再建一个句柄——
这本身无害）。

**验收**：`shell.cpp` 的 `run_probe()` 里去掉"只是缩小窗口"那段注释的限定；在一个等到 PEER_CLOSED 后**立刻**数配额的测试里（例如把
配额检查挪到探针之后紧接着跑），多次启动 dbg_1 都打出 `512 channels made before the quota`。
