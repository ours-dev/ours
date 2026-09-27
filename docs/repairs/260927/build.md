# build 待修项（2026-09-27）

## 1. `init_startup()` 的 `job` 能力给的是根 job，不是每个条目一个子 job

**现象**：潜伏的权限过大，当前没有条目用到 `job`。一旦有条目声明 `job`，它拿到的是 init 自己手里的
job——也就是根 job——可以在根 job 下起任何进程、杀任何进程。

**原因**：Fuchsia 的 component_manager 给每个组件单独建一个子 job，组件只能管自己那一棵。
ours 的用户态库（`system/lib/ou`）目前没有 `job_create`，init 只能复制它手里的句柄。

**修法方向**：`ou` 里补 `job_create`（对照 Zircon `zx_job_create`，内核侧看 `kernel/object` 的 JobDispatcher 是否已有对应的 syscall），
init 发 `job` 能力时为该条目新建一个子 job 并交出去；`system/service/init/init_startup.gni` 里 `job` 的说明同步改掉。
