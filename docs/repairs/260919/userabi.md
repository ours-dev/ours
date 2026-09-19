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

## 2. 用户程序的三个段被当成一段映射

`map_elf_image()` 把一个 ELF 的全部 PT_LOAD 合并成一段连续区间，按同一套权限
（`User|Readable|Writable|Executable`）映射。也就是说每个用户程序的代码段是可写的，
数据段是可执行的——W^X 正是为了禁止这件事而存在。

目前不致命（ring 3 里还没有别人的输入能落进这块内存），但它同时也挡着 PT_GNU_RELRO：
绑定完成后本应把 GOT 所在的页改回只读，而现在整段本来就是可写的。

改法：按段映射，每段用自己的 `p_flags`；重定位写入走内核侧视图（`MappedImage::view`）
所以不受用户侧权限影响，绑定完成后再按 PT_GNU_RELRO 收权限。eftl 的
`LoadPolicy::make_plan()` 已经给出每段的 `LoadRegion` 和 `RelroInterval`。
