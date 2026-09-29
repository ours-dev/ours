# elfload

把一个 ELF 的导入绑到别的镜像的导出上，并检查一个文件是不是能启动的程序。
内核（启动最初几个进程）和用户态 launcher（启动其余进程）共用这一份。

装载分成两半，是故意的，这个库只有其中一半：

- **binder**（`elfload::Binder`）——应用重定位，按名字把未定义符号解析到导出方
  镜像的 `.dynsym`。它只要一段内存和两个 ELF 文件，不碰地址空间、不碰内核对象，
  所以「算得最容易出错的那部分」可以在宿主上单测。
- **loader**——把 PT_LOAD 段放进地址空间。它不在这里：内核的在
  `//kernel/lib/kelfload`（走对象层，VmoDispatcher / VmaDispatcher），
  launcher 的走 `vma_map`。

日志只经 logz4 的接口打：链接它的一方负责装 logger——内核装串口的
（`//kernel/lib/logz4`），用户程序装 runtime 的。

两者合起来是 `//kernel/phys:elf_image`（`ours::phys::ElfImage` +
`SymbolicRelocationPass`）在用户态这一侧的对应物：physload 给它加载的模块做同样
两件事，导出方是生成的导出表，这里的导出方是另一个 ELF 的符号表。

`elfload::Image` 是两半之间唯一共享的东西：镜像落在哪、占多大、入口在哪、**加载者
自己那份可写视图**，以及链接期原点。那份视图是必需的——内核在内核上下文里修正一个
用户程序，它没法按用户虚拟地址去写。

## 为什么不是 eftl 的一部分

eftl 是通用的 ELF 工具箱（解析、段、符号、重定位表、哈希表），不知道地址空间也不
知道谁绑谁。elfload 是它的使用者：拿 eftl 读出来的东西，决定往哪写、从谁那里解析。
