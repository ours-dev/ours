# user-copy 改动记录（2026-09-24）

## 拷贝改为一段汇编，分 SMAP 与普通两个变体（2026-09-24）

**文件**
- `kernel/arch/x86/kernel/user_copy.S`（新建）— 一个 `.macro` 展开出 `x86_user_copy` 与 `x86_user_copy_smap`
- `kernel/arch/x86/kernel/user_copy.cpp` — 删掉 `UserAccessWindow` 和 `memcpy`，按 `feature_has_smap` 选变体
- `kernel/arch/x86/kernel/include/ours/arch/user_copy.hpp` — 注释指向新实现；改正 Zircon 文件路径
- `kernel/arch/x86/kernel/BUILD.gn` — 加 `user_copy.S`

**症状**
无运行时症状。这是故障恢复（下一条）的前置重构。

**原因**
原来的拷贝是 `stac; memcpy; clac`，由 C++ 对象的构造和析构包住。它是对的——`"memory"`
clobber 让拷贝自己的访存留在窗口内——但窗口里放的是对内核通用 `memcpy` 的一次调用，
和其他所有调用者跑的是同一个函数。

故障恢复（设计 4.6）要做两件事，`memcpy` 都给不了：

1. 认出"这是拷贝在访问用户内存时出的错"——凭出错指令的地址。`memcpy` 被整个内核共用，
   它的地址说明不了任何事；
2. 把出错的拷贝送到一个已知地址、以已知的寄存器状态继续——编译器生成的代码不保证
   任何一点的寄存器和栈是什么样。

**改法**
改前：

```cpp
    class UserAccessWindow final {
      public:
        UserAccessWindow() {
            if (feature_has_smap) {
                asm volatile("stac" ::: "memory");
            }
        }
        ~UserAccessWindow() { ... clac ... }
    };

auto arch_copy_from_user(void *const dst, VirtAddr const src, usize const size) -> void {
    UserAccessWindow const window;
    std::memcpy(dst, reinterpret_cast<void const *>(src), size);
}
```

改后（`user_copy.S`）：

```asm
.macro user_copy name, smap
.function \name, global, nosection=nosection
    movq %rdx, %rcx
    shrq $3, %rcx
.if \smap
    stac
.endif
    rep movsq
    movl %edx, %ecx
    andl $7, %ecx
    rep movsb
.if \smap
    clac
.endif
    ret
.end_function
.endm

.section .text.x86_user_copy, "ax", @progbits
user_copy x86_user_copy, 0
user_copy x86_user_copy_smap, 1
```

两个变体放在同一节里前后相接，下一条用它们合起来的地址区间认出拷贝的故障。普通变体的
任何路径上都没有 `stac`/`clac`：在没有 SMAP 的 CPU 上它们是 #UD（Nehalem，这里 QEMU 的
默认型号，就是这样的 CPU）。C++ 一侧只剩按 `feature_has_smap` 二选一。原来 `UserAccessWindow`
上的说明（中断如何不继承 AC、拷贝自己的缺页如何恢复）搬到了 `.S` 的头注释。

**参照**
- Zircon `kernel/arch/x86/user-copy/user-copy.S`：同一个文件按 `-DSMAP` 汇编两次；
  `user-copy.cc` 用代码补丁在启动时选定变体。本项目不带 `.code-patches` 构建，所以是缓存的标志位。
- Linux `arch/x86/lib/copy_user_64.S`。

**验证**
- 构建通过；反汇编：`x86_user_copy` 只有 `rep movsq`/`rep movsb`，`stac`/`clac` 只出现在
  `x86_user_copy_smap` 里。
- 两个镜像在默认的 Nehalem 与 `CPU=max`（有 SMAP）下都启动正常，init↔shell 对话与通道检查照常。
- `CPU=max` 下在 `sys_debug_write` 里临时加一次对用户缓冲区的裸解引用（未提交），仍然报
  `potential SMAP violation: ring 0 touched user address 0x11ffe10 outside user_copy`。

**故意没做**
故障恢复本身在下一条。
