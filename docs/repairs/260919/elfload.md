# elfload 待修项（2026-09-19）

## 2. eftl 的宿主测试没有挂进任何构建图

`system/lib/eftl/test/BUILD.gn` 定义了二十多个测试可执行文件，但没有任何目标依赖
它们——`gn ls out/bringup.x86 "//system/lib/eftl/test/*"` 和 `out/phys-host-tests`
里都是空的。所以这些测试写了但不会被构建、更不会被运行；2026-09-19 修的 PLT 表
`DT_RELAENT` 问题，本该被一条「只有 DT_JMPREL」的用例挡住，现在只能靠启动日志发现。

改法：在宿主测试的汇总组（`out/phys-host-tests` 用的那个 group，或新建一个）里挂上
`//system/lib/eftl/test:*`，然后补上 DT_JMPREL-only 的用例。elfload 的宿主单测
（拆成 Binder/Loader 的动机之一）也等这一步。
