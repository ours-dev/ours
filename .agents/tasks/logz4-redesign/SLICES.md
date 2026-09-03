# logz4 重设计实施切片

按 PLAN.md 的决策执行。调用点零改动。

1. system/lib/logz4 核心可编译、hosted 测试通过
2. GN 依赖改指向；删除 kernel/lib/logz4 旧实现与死头文件
3. 内核胶水：串口 Logger（irq-save TAS）、hooks、emergency_write、kshell `log`
4. panic 转储 ring；phys `log.level=` 引导选项
5. format/tidy，内核与测试构建
