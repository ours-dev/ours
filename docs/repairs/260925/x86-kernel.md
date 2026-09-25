# x86/kernel 待修项（2026-09-25）

修 NMI 的 IST 栈与 %gs 时看到的，不是那两处改动引入的。按严重程度排。（两条都已修：NMI 路径打日志见 `docs/changes/260925/x86-kernel.md` 第 3 条，`sysretq`/`iretq` 的非规范返回地址见同文件第 4 条与 `object.md` 同日条目。本文件已无待修项。）
