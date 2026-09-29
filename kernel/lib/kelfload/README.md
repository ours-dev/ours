# kelfload

`//system/lib/elfload` 的内核特化：把一个 ELF 的 PT_LOAD 段放进用户地址空间。
内核只能经对象层做这件事——一个 VMO 装它填好的页，一个 VMA 把它放进进程——
所以它依赖 `//kernel/object`，不能放在 `system/lib`。

`userabi` 用它启动内核亲自启动的进程；之后把镜像绑到 vDSO 上，用的是通用那半的
`elfload::Binder`。命名照 `system/lib/concurrent` 与 `kernel/lib/kconcurrent`。
