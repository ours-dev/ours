# scripts/qemu_boot.py 待修项（2026-09-28）

## 1. 默认 CPU 型号 Nehalem 没有 SMAP、SMEP、PCID，这些路径从来没被验证过

`scripts/qemu_boot.py` 用 `-cpu $CPU`，默认 `Nehalem`（选它是因为它是 x86-64-v2 的标准型号，
更老的型号在编译器生成的第一条 `popcntq` 上就会 #UD）。可 Nehalem 没有 SMAP、SMEP，也没有 PCID。
于是内核里依赖这些特性的代码在 `ours-verify` 的两次启动里都不会执行：

- SMAP：`stac`/`clac` 的补丁和 user_copy 的开窗关窗（`feature_has_smap`），在 Nehalem 上被跳过；
  少一条 `stac` 的 bug 在默认验证里看不出来。历史上 `clac` 引发的三重 fault 就在这条路径上。
- SMEP：内核误跳到用户页不会被拦下。
- PCID：如果以后用它省 TLB 刷新，同样测不到。

改法方向（修之前和用户确认）：`ours-verify` 至少再用一个带这些特性的型号启动一次
（例如 `Broadwell` 或 `Skylake-Server`），或者把默认型号换掉并确认它仍满足 x86-64-v2。
注意 TCG 对这些特性的模拟程度要先核实：打开后启动日志里的 feature 检测结果应当显示它们存在。
