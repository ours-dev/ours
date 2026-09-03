# logz4 重设计（2026-09-01）

## 决策记录

- 分层：核心框架下沉 `system/lib/logz4`（freestanding，内核/phys/宿主/将来的用户空间
  通用，仅依赖 ustl:headers 与 system/public）；`kernel/lib/logz4` 只留内核实现
  （串口 Logger、hooks、emergency_write、kshell 命令），命名空间 `logz4::kernel`。
- 修的病根：
  1. `log.cpp` 的 `static char format_buffer[512]` 全 CPUs 共享无同步 → 栈上装配整行。
  2. 前缀/正文两次 `write()` → 单次 write，sink 侧锁保证行原子。
  3. emergency 丢 `[error]` 前缀 → 前缀进同一缓冲统一路由。
  4. 非原子全局 logger 指针 → `std::atomic<Logger*>`。
  5. 死代码：record.hpp / ring_buffer.hpp / types.hpp / register_logger /
     per-Logger 等级重载 / Logger::flush/kill / Level::Status。
  6. 两套矛盾的编译期等级门 → 运行时 `set_min_level` + 保留 debug/trace 编译剔除。
  7. 元数据是 Time（何时发生）+ cpu。外部 `set_clock` 接上钟，单位由钟
    自己定，核心不换算。未接钟/cpu 时字段省略。
  8. 零测试 → system 侧 hosted 测试（ustl 风格 assert 可执行）。
- 新增：MPSC 票号记录环（256 槽 × 544B ≈ 136 KiB .bss，覆盖最旧，COMMITTED 状态机），
  panic 与 kshell `log` 命令转储；记录与输出解耦（环是历史记录器，不是异步队列）。

## 外部兼容（170 个调用点零改动）

`log::debug/trace/info/warn/error(fmt, args…)`、`log::vlog(get_global_logger(),
Level::Error, …)`、`Logger` 子类 `write(char const*, usize)`、`init`/`get/set_global_logger`、
强 `emergency_write`、`namespace log = logz4`、`#include <logz4/log.hpp>` 路径不变、
`vlog_dispatch`（PHYS_EXPORT）ABI 不变。

## 关键接点

- `build/switch/kernel_switchset.gni` 前缀配置改 `//system/lib/logz4:headers.config`；
  其余 14 处 dep 改指向（kernel/top 保留 kernel 胶水 + 新增 system 核心；
  kernel/phys 两处整库指向 system 核心）。
- 钟与 CPU 分开接：bootstrap 时 `set_clock`（内核可读 TSC，那是钟的实现）；
  `set_cpu` 经 `GKTL_INIT_HOOK` 在 `InitLevel::CpuLocal` 补装
  （`CpuLocal::current_cpu()` 在 `init_early()` 之前不可用）。
- phys `log.level=trace|debug|info|warn|error` 引导选项（仅作用 phys 镜像自身的
  filter 副本；内核侧等内核 cmdline 管道落地再接）。
