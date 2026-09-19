# x86/kernel 待修项（2026-09-19）

## 1. ISR 的 `has_error` 名单漏了 21/29/30 号

`kernel/arch/x86/kernel/idt.S:23-25`：

```asm
        isr.current.has_error = isr.current == 8 || \
                                (isr.current >= 10 && isr.current <= 14) || \
                                isr.current == 17
```

硬件会压错误码的向量还有三个不在名单里：21（#CP，CET 的 control protection）、
29（#VC，SEV-ES 的 VMM communication）、30（#SX，security exception）。名单漏掉
一个向量的后果是这个 stub 会自己再补压一个假的错误码，于是栈上多出 8 字节：
`iretq` 会从错位的位置取 RIP/CS/RFLAGS，异常帧里的 `error_code` 和其后的每个字段
也全部错位。CFI 那行 `(8 * (5 + isr.current.has_error))` 同样会算错，展开器跟着走偏。

这份名单是从 Zircon 逐字抄来的（`zircon/kernel/arch/x86/exceptions.S:36` 一模一样），
所以它是继承来的欠账而不是本仓库写错。今天也确实打不到：#CP 要开 CET，#VC 要跑在
SEV-ES 客户机里，#SX 要 AMD 的特定配置，这三样本内核都还没有。

但它是那种「开启某个特性的那一天才爆、且爆成三重错误」的欠账，补上去只是在条件里
多写三个数，值得现在就补，并在旁边写清楚这三个向量各自的触发前提。
