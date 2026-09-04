# userboot 改动记录

## 打包出第一个用户态二进制：新增 user 工具链与 userboot 程序（2026-09-04）

**文件**
- `build/toolchain/ours/BUILD.gn` — 新增 `ours_toolchain("user-x86")`，environment = `"user"`
- `system/BUILD.gn` — 新增 `config("user.config")` / `config("user.standalone")`，用户态的编译链接策略
- `kernel/lib/userabi/userboot/userboot.cpp` — 新文件，`_start` 自旋
- `kernel/lib/userabi/userboot/BUILD.gn` — 新文件，用 `kernel_elf_binary()` 打包
- `kernel/products/image/BUILD.gn` — `common_package_items` 挂上 userboot

**症状**
不是 bug，是缺口。树里没有任何一个用户态程序：`kernel/lib/userabi/userboot.cpp`
里有 `userboot_init()` 的桩，`kernel/top/main.cpp:60` 有一行注释写着
"next is to load userboot"，但镜像里根本没有可以被加载的东西。
打包出来的 BootFS 只有三个文件：

```
└── [KSTR] kernel — BootFS, 3 files
    ├── vmours/
    │   ├── code-patches (0 bytes)
    │   └── image.elf (479512 bytes)
    └── physboot (82760 bytes)
```

对象层、系统调用层、地址空间层因此一直没有一个具体的需求去定形。

**原因**
这里真正要想清楚的不是"写个 hello world"，而是**为什么不能用现成的内核工具链去编它**。

内核工具链 `kernel-x86` 通过 `//kernel/products:kernel.config` 带上了
`//kernel/arch/x86:abi`，其中三条对用户态程序都是错的：

```gn
config("abi.thread-pointer") {
  cflags = [ "-mcmodel=kernel" ]     # 假设所有符号都在地址空间最高 2 GiB
}
config("abi.stack-pointer") {
  cflags = [ "-mno-red-zone" ]       # 为中断栈帧让路，用户态不需要
}
config("abi.integer-register") {
  cflags = [ "-mno-sse", ... ]       # 内核不保存 FPU 状态才这么编
}
```

`-mcmodel=kernel` 是致命的那条：它让编译器对每一次符号引用都发
`R_X86_64_32S` 式的绝对寻址，前提是符号落在 `0xffffffff80000000` 以上。
一个映射在低地址运行在 ring 3 的程序，链接期就会因为重定位截断而失败，
即便侥幸链出来，运行时取到的也是内核地址——立刻 #GP。

好在 `build/config/x86/BUILD.gn` 早就为这件事做过切分，注释里明写着
"split by concern so future userspace toolchains can take the ISA baseline
without the kernel-mode constraints"：`cpu_baseline` 只给 ISA 基线
（`-march=x86-64-v2`），ABI 约束留在内核工具链里。所以新工具链要做的事很少，
只是不去继承内核那套。

另一半原因在 `build/toolchain/toolchain_environment.gni`：`"user"` 这个
environment 名字本来就是文件里写好的兜底分支，并且 BUILDCONFIG.gn 的
`is_kernel` 只认 `"kernel"` 和 `"kernel.phys"`——也就是说 `"user"` 工具链
天然拿不到内核的 freestanding 链接策略、libc++ 头串接和 `//system/public`
包含路径。这是想要的效果，但也意味着凡是内核靠 `is_kernel` 免费拿到的东西，
用户态都得自己声明一遍。`//build/config:build_id` 就是踩到的那一条：
`kernel_elf_binary()` 会跑 `build/elf/extract-build-id.py` 从镜像里抽 GNU
build ID，抽不到就报错退出。

**改法**
1. `system/BUILD.gn` 里定义用户态策略（内核侧 `//kernel/products:kernel.config`
   的对位物）。链接形态和内核镜像一致：static PIE。

   改前——文件里只有一个 SDK availability 的 group，没有任何编译策略。

   改后：

   ```gn
   config("user.config") {
     configs = [
       ":user.standalone",
       "//build/config:no_exceptions",
       "//build/config:no_rtti",
       "//build/config:no_stack_protector",
       "//build/config:build_id",
     ]
     cflags = [ "-fpie" ]
   }

   config("user.standalone") {
     configs = [
       "//build/config:freestanding",
       "//build/config:static-link",
       "//build/config:pie-link",
     ]
   }
   ```

   `-static` + `--pie` 一起给的是 static PIE：ET_DYN 带 `R_X86_64_RELATIVE`
   让加载器自己选基址，`-static` 保证不产生 `PT_INTERP`——树里没有任何东西
   能响应一个动态链接器请求。

2. 新工具链。改前 `build/toolchain/ours/BUILD.gn` 里只有 host / kernel-\* /
   kernel-phys-\* 三类；改后追加：

   ```gn
   ours_toolchain("user-x86") {
     cpu = "x86"
     os = target_os
     environment = "user"
     elf_linker = true
     tags = [ "user" ]
     tool_dir = clang_tool_dir
     configs = [ "//system:user.config" ]
   }
   ```

   `elf_linker = true` 的理由和其它非 host 工具链一样：clang driver 会把
   ELF 目标的链接交给宿主链接器，macOS 宿主上根本产不出 ELF。

3. userboot 本体。当前形态对标 Zircon 的 `trivial-test-userboot` 而不是完整的
   `userboot`：不带参数、不发系统调用、不返回。

   ```cpp
   extern "C" [[noreturn]]
   auto _start() -> void {
       for (;;) {
           __asm__ volatile("pause");
       }
   }
   ```

   `pause` 不是装饰。C++ 的 forward-progress 规则规定"无副作用、无 I/O、
   无同步"的循环是 UB，clang 在 -O2 下会直接把 `for (;;) {}` 删掉，让控制流
   掉出函数尾部执行后面的字节。`asm volatile` 是优化器必须保留的副作用，
   把循环钉住；而 `pause` 恰好也是这里该放的指令——它在用户态自旋，等着内核
   的时钟中断把它抢占掉。

4. 打包。用的是内核镜像自己那套 `kernel_elf_binary()`，`has_patches = false`
   （code patching 是 physboot 改写内核代码段的机制，用户程序没有这个 section；
   关掉之后 `kernel_elf_image()` 把 ELF 装成裸文件 `userboot` 而不是
   `userboot/` 子树）。挂点选 `common_package_items`：

   改前
   ```gn
   group("common_package_items") {
     visibility = [ "./*", "//products/*", "//tests/*" ]
   }
   ```
   改后
   ```gn
   group("common_package_items") {
     visibility = [ "./*", "//products/*", "//tests/*" ]
     deps = [ "//kernel/lib/userabi/userboot(//build/toolchain/ours:user-${target_cpu})" ]
   }
   ```

   工具链后缀必须显式写出来：`deps` 是在内核工具链上下文里求值的，不写就会用
   `kernel-x86` 编出一个 `-mcmodel=kernel` 的"用户程序"。
   `kernel/lib/userabi/userboot/BUILD.gn` 里另有一条 `assert(toolchain_environment
   == "user")` 做第二道闸——宁可在 gen 期报错，也不要到运行时才以 #GP 的形式发现。

**参照**
- `Zircon //zircon/kernel/lib/userabi/trivial-test-userboot`：同样是单个翻译单元、
  用 `kernel_elf_binary()` 打进 kernel_package、`has_patches = false`。本次连
  目录位置（`kernel/lib/userabi/userboot`）都对齐了完整版 userboot 的所在。
- `Zircon kernel_package("zircon")`：userboot 是 STORAGE_KERNEL BootFS 里的一个
  文件，不是独立的 ZBI item。本次的 OBI 结构与之一致。
- 工具链分环境这件事对应 Fuchsia 的 `user_basic_redirect()` / `user.basic`
  toolchain：Zircon 也不允许用内核工具链编用户态二进制。

**验证**
- `prebuilt/ninja/ninja -C out/bringup.x86` 通过，新增 `CXX`/`LINK`
  `user-x86/obj/kernel/lib/userabi/userboot/userboot` 两步。
- `obit tree out/bringup.x86/kernel.dev.dbg_1.obi`：

  ```
  └── [KSTR] kernel — BootFS, 4 files
      ├── userboot (776 bytes)
      ├── vmours/
      │   ├── code-patches (0 bytes)
      │   └── image.elf (479512 bytes)
      └── physboot (82760 bytes)
  ```

- `llvm-readelf -hl` 确认形态正确：`Type: DYN`、`Entry point 0x1270`、
  三个 `PT_LOAD`、无 `PT_INTERP`、有 `NOTE`（build ID）。
- QEMU 实测两个镜像均正常启动到原有终点（dbg_1 到线程调度循环，dev 到
  `prof: no architectural PMU`），无回归。

**故意没做**
- **没让内核去读它**。这一步只验证"镜像里有这个文件"。内核侧的 ELF 加载、
  地址空间、进入 ring 3 都还没有。
- **只做了 x86**。`user-riscv` / `user-arm` 等真有 riscv/arm 的启动路径再加；
  现在加只是三份没人编译的死代码。
- **没有 libc、没有 crt1.o、没有 vDSO**。`_start` 就是全部运行时。系统调用的
  ABI 还没定形（见 `kernel/lib/syscall/table.cpp` 自己的 TODO：没有任何类型
  描述 `syscall.S` 压出来的寄存器帧），定形之前写调用桩只会写错。
- **没给 userboot 配 `-Wl,-z,stack-size`**。Zircon 的 trivial-test-userboot 这么
  做是因为它的加载器读这个字段来决定栈大小；ours 的加载器还不存在，栈大小由谁
  决定要等第 3 步。
- **没有把 compiler-rt builtins 链进去**。内核那份
  （`//kernel/products:static-pic-link`）是因为 128 位定点数学会隐式调用
  `__udivti3`；userboot 目前没有任何这类调用，链接干净通过。真需要时按内核那条
  注释的写法加即可。
