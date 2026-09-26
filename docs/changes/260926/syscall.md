# 系统调用改动记录（2026-09-26）

## fault_probe 迁到 runtime：探针名走 argv[1]，回答走另给的 channel（2026-09-26）

**文件**
- `kernel/lib/syscall/test/fault_probe/fault_probe.cpp` — 手写的 `_start` 换成 `main(argc, argv)`：名字取 `argv[1]`，回答写到启动句柄 User0，
  没被杀就从 `main` 返回，退出码区分"没死""不认识""没给名字"
- `kernel/lib/syscall/test/fault_probe/BUILD.gn` — 依赖 `//system/lib/runtime`
- `examples/shell/shell.cpp` — 新增 `run_probe()`：为每个探针写 processargs、另建回答 channel；`run_probes()` 用它，最后多跑一个对照探针 `none`
- `system/lib/runtime/BUILD.gn`、`test/runtime_smoke.cpp` — 注释改为"一对一对地迁移，fault_probe 先"
- `docs/design/260924/userspace-filesystem.md` — K14 拆为四个提交、U3 的搬移提前，写进这两行；总数那一段注明是计划数

**症状**
无，行为不变（探针照旧 11 个全被杀）。这是设计步骤 K14 拆出来的第一个提交：把"启动方与被启动方"这一对（dbg_1 的 shell → fault_probe）
先切到 processargs 与 runtime。

**原因**
这一节的学习价值在"为什么 fault_probe 可以先走、又为什么回答不能再走 bootstrap"。

- **为什么先走这一对**：K14 要把启动协议从"rdi 里一个句柄，双方私下约定是什么"换成 processargs。协议的两端必须同时换：写的一方开始写
  processargs 的那一刻，读的一方就得会读。fault_probe 只有一个启动方——dbg_1 的 shell——而 shell 作为写的一方只需要只有头文件的
  `encode_process_args()`，和 shell 自己是怎么被启动的无关。所以这一对可以独立切换。内核→init、内核→shell、init→shell 三条连成一体，
  留到后面一起切。
- **回答为什么不能再走 bootstrap**：runtime 读完 processargs 就关掉 bootstrap（U2，照 Zircon）。shell 如果还在 bootstrap 上等回答，
  探针还没进 `main`，它就先看到 PEER_CLOSED、把每个探针都记成"被杀"。所以 shell 另建一个 channel，把远端作为 User0 启动句柄放进
  processargs 交过去，在这一端上等。
- **"被杀"已经证明不了探针跑过**：runtime 启动失败时进程直接退出，回答 channel 同样是"对端关闭且没有数据"。所以验证不能只看
  `11 killed`，要逐个数到 `fault_probe: <名字>`，并核对每个探针的故障特征与以前一致。
- **对照探针**：11 个探针全都在回答之前就被杀，"`main` 返回 → runtime 调 `process_exit`"这条路一次都不会走到。shell 最后用一个
  不存在的名字 `none` 再起一次 fault_probe：它应当回答 "unknown"、从 `main` 返回 2，内核日志里出现 `retcode 2`。它不计入 11 个的统计。

**改法**
改前（fault_probe）：

```cpp
extern "C" [[noreturn]]
auto _start(ours::HandleValue const bootstrap, ours::VirtAddr const vdso_base) -> void {
    ... object_wait_one(bootstrap, ...);
    ... channel_read(bootstrap, buffer, sizeof(buffer), &actual) ...   // the probe's name
    ...
            probe.action(vdso_base);
            answer_and_stay(bootstrap, "survived");                     // write, then spin
```

改后：

```cpp
auto main(int const argc, char **const argv) -> int {
    auto const answer = runtime::take_startup_handle(ours::HandleType::User0, 0);
    ...
    auto const name = ou::Str::from_c_str(argv[1], 32);
    ...
            probe.action(runtime::vdso_base());
            reply(answer, "survived");
            return SURVIVED;
```

改后（shell 为每个探针做的事，`run_probe()`）：

```cpp
    u32 const infos[] = {ours::handle_info(ours::HandleType::User0, 0)};
    std::string_view const args[] = {"fault_probe", {probe.data(), probe.size()}};
    auto status = ours::encode_process_args(message, infos, args, {}, {}, &length);
    ...
        status = ou::channel_write(bootstrap, message, length, &answer_there, 1);
    static_cast<void>(ou::handle_close(bootstrap));
    ...
    if (status = ou::process_spawn_boot(job, "fault_probe", bootstrap_there); ...
```

- **先写后启动**：消息在进程启动之前就已经在队列里；runtime 反正会先等，但这样不依赖它等。
- **bootstrap 这一端写完就关**：它是 shell 建的句柄，按每创建者配额（K11）记在 shell 名下；留着既没用，还会让后面的配额检查少算。
- **回答之后再等对端关闭**：第一次启动时 dbg_1 的配额检查从 512 变成了 511。原因是对照探针正常返回、慢慢退出，而它手里那端回答
  channel 是 shell 建的、记在 shell 名下，shell 读到回答就往下走，到配额检查时子进程还没清理完（日志里晚了一秒多）。改为读到回答后
  再等 PEER_CLOSED，确认子进程放下了它那端再返回——这个数字是查句柄泄漏用的，不该随子进程死得快慢浮动。
  但这只是**缩小**了窗口：审查指出内核在 `Handle::~Handle()` 里先发 PEER_CLOSED（`on_zero_handles()`）、后退还配额（`refund()`），
  所以 shell 醒来时那个句柄可能还记在它名下几条指令的时间；被杀的探针也一样。今天配额检查之前还有三组检查，这个窗口碰不到；
  内核里把两步的顺序调过来是另一个改动，记为 `docs/repairs/260926/object.md` 第 1 条。
- **先编码、后建 channel**：审查指出第一版在编码失败时会漏关回答 channel 的远端（还没写出去，就没人消耗它）。编码不依赖任何句柄，
  挪到最前面，编码失败时手里还没有句柄。
- **退出码**：`SURVIVED = 1`、`UNKNOWN = 2`、`NO_NAME = 3`，调试内核打出的 retcode 就能说明一个没被杀的探针是怎么结束的。

**参照**
Zircon 的做法相同：子进程的参数与启动句柄都在 processargs 里（`zx_proc_args`），父进程与子进程之间另有专用 channel；bootstrap 读完即关。

**验证**
两个镜像构建、启动。dbg_1：
- 13 行 `fault_probe: ...`：11 个探针的名字，外加对照探针的 `fault_probe: none` 与 `fault_probe: unknown probe none`；
- 11 行 `shell: probe <p> killed`、11 行 `retcode -1028`，写 vDSO 的探针故障特征仍是 `error_code=0x7 ... cr2=0xa00000`（`runtime::vdso_base()` 给的地址对）；
- `shell: probes done (11 killed, 0 survived)`；`shell: control probe none answered "unknown"`，内核日志里这个进程是 `terminated, retcode 2`；
- 12 行 `dead, 1 handles closed`（每个探针死时只剩 User0 那一个句柄：bootstrap 已被 runtime 关掉）；
- 0 行 `runtime:`，0 个 `retcode -2147483649`（没有启动失败）；
- `shell: 512 channels made before the quota`（与以前相同）。
dev 与以前一致：`init: shell closed its channel`，`512 channels`。

**故意没做**
- init、shell 自己仍是手写的 `_start`：它们的启动方（内核、init）要一起切换，在 K14 的第 ③ 个提交。
- `same()` 仍是手写比较：可以换成 memcmp，与本步无关。
