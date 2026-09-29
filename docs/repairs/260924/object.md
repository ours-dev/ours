# object 待修项（2026-09-24）

## 3. AMD 上 `lfence` 不保证串行化派发

`kernel/arch/x86/kernel/user_copy.cpp` 在范围检查与拷贝之间用 `lfence` 做 Spectre V1/V1.1 缓解。Intel 上 `lfence`
是派发串行化的；AMD Fam10h–Zen1 上要先把 `MSR_AMD64_DE_CFG`（0xC0011029）第 1 位置上才是，内核从没设过。

改法照 Linux `init_amd()` 里的 `msr_set_bit(MSR_AMD64_DE_CFG, MSR_AMD64_DE_CFG_LFENCE_SERIALIZE_BIT)`，
或 Zircon 在 AMD 上的同类设置；需要 CPU 厂商识别。
