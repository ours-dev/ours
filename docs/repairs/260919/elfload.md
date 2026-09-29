# elfload 待修项（2026-09-19）

## 1. 用户程序的三个段被当成一段映射

`elfload::Loader::map()` 把一个 ELF 的全部 PT_LOAD 合并成一段连续区间，按同一套权限
（`User|Readable|Writable|Executable`）映射。也就是说每个用户程序的代码段是可写的，
数据段是可执行的——W^X 正是为了禁止这件事而存在。

目前不致命（ring 3 里还没有别人的输入能落进这块内存），但它同时也挡着 PT_GNU_RELRO：
绑定完成后本应把 GOT 所在的页改回只读，而现在整段本来就是可写的。

改法：按段映射，每段用自己的 `p_flags`；重定位写入走内核侧视图（`elfload::Image::view`）
所以不受用户侧权限影响，绑定完成后再按 PT_GNU_RELRO 收权限。eftl 的
`LoadPolicy::make_plan()` 已经给出每段的 `LoadRegion` 和 `RelroInterval`。

## 2. eftl 的宿主测试没有挂进任何构建图

`system/lib/eftl/test/BUILD.gn` 定义了二十多个测试可执行文件，但没有任何目标依赖
它们——`gn ls out/bringup.x86 "//system/lib/eftl/test/*"` 和 `out/phys-host-tests`
里都是空的。所以这些测试写了但不会被构建、更不会被运行；2026-09-19 修的 PLT 表
`DT_RELAENT` 问题，本该被一条「只有 DT_JMPREL」的用例挡住，现在只能靠启动日志发现。

改法：在宿主测试的汇总组（`out/phys-host-tests` 用的那个 group，或新建一个）里挂上
`//system/lib/eftl/test:*`，然后补上 DT_JMPREL-only 的用例。elfload 的宿主单测
（拆成 Binder/Loader 的动机之一）也等这一步。
