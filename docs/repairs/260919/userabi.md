# userabi 待修项（2026-09-19）

## 1. 加一个系统调用仍要手工同步三处

加一个系统调用今天要动：

1. `system/public/ours/syscall.hpp` —— `#define OURS_SYS_<NAME> <n>` 和
   `enum class SyscallNumber` 的对应项；
2. `kernel/lib/userabi/vdso/vdso.S` —— 一个 stub（`mov $号, %eax; syscall; ret`）
   加一个 `OURS_VDSO_SYSRET_<NAME>` 返回地址标号；
3. `system/public/ours/vdso.hpp` —— 一行 `extern "C"` 声明；

外加 `kernel/lib/syscall/src/table.cpp` 里的处理函数登记。这四处彼此没有任何编译期
关联：号写错了是运行时跑到别的处理函数上，stub 漏了是链接期未定义符号，声明的签名
和处理函数不一致则没有任何东西会发现。

改法：照 Zircon 的 abigen／`.fidl → syscalls.inc` 做法，把系统调用表写成一份声明式
描述（每条：号、名字、参数类型、返回类型），由它生成 stub 汇编、用户侧声明、内核侧
调度表和号的枚举。生成物进 `$target_gen_dir`，不要签进树里——本仓库的规矩是描述构建
产物的列表由构建生成，不手工维护。
