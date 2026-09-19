# userabi 待修项（2026-09-19）

## 1. ~~间歇性：shell 第一次 syscall 调用跳进未映射地址~~ —— 已结案，并非本文档原先写的那样

**结论：这个故障不是间歇的，它是确定性的；而且在我着手调查之前就已经被修掉了。**
真因是 `a51f4cd fix(eftl): read a PLT relocation table that has no DT_RELAENT beside it`，
它的提交信息里逐字写着同一个 `rip=0x15c6 against an image mapped at 0x200000`，
以及「page fault before, both bound and the shell's full output after」。

机制（引自该提交）：`relocation_table_from_tags()` 要求 entry-size 标签存在，而 PLT 表的
调用方传的是 `DT_RELAENT`——那是描述 `DT_RELA` 的，链接器只在真有 `DT_RELA` 时才发它。
一个重定位全是 PLT 槽的程序两者皆无，于是整张 `.rela.plt` 被读成「不存在」。
`bind()` 又把「读不出」当成「没有」返回 `Ok`，一条重定位都没应用，`.got.plt` 保留链接期
初值——lld 写进去的是 `.plt` 项的 VA `+6`，`0x15c6 - 6 = 0x15c0`，正好压在 RX 段末尾
`0x15bc` 之后。

**「未应用的槽读出来是什么」是区分两种形状的指纹**：GLOB_DAT 读出 0，JUMP_SLOT 读出
`.plt + 6`。我之所以在 `r12=0x0` 和 `rip=0x15c6` 之间卡住，正是因为我拿着一份
**GLOB_DAT 形状**的二进制去解释一次 **JUMP_SLOT 形状**的故障。

## 2. 我为什么把确定性故障误判成间歇的 —— 这条比那个 bug 更值得记

**我启动的镜像，是另一个会话正在重建的镜像。**

这个 worktree 当天有两个会话交替提交：调度器那条线（`abe6f1d` 16:49 … `2ed56f1` 17:20）
和 userabi 那条线（`4f5c209` 16:43、`966704b` 16:51、`a51f4cd` 17:21、`ba6d2de` 17:21、
`1af605e` 17:22）。`out/bringup.x86/kernel.dev.obi` 是**同一个文件名**，内容在我两次启动
之间被换掉了。

所以：

- 故障命中的那一代镜像，带的是 JUMP_SLOT 形状的 shell，`a51f4cd` 尚未落地。
- 我随后跑的「6 次对照」和「14 次复现」用的是**已经修好**的镜像。
- 「同一镜像连跑 3 次未复现」这句话字面为真，而它毫无意义——那个文件名下已经是另一份内容。

**教训**：在这个共享 worktree 里，一次启动的结果只有在能说清「当时跑的是哪一份二进制」
时才成立。见第 3 条。

另外订正本文档原先的一处归因错误：shell 从 1784 字节变成 1712 字节，**不是** `1af605e`
（`//system/lib/ou`）造成的——出故障那份 1784 字节的 shell 已经链了 ou。把它变小的是
`ba6d2de`，它把 `-fno-plt` 移进 `user.standalone`，删掉了 `.plt` 与 `.got.plt`
（72 字节 ≈ 2×16 + 3×8 + 对齐），顺带把重定位形状从 JUMP_SLOT 换成了 GLOB_DAT。

## 3. 镜像不自报家门，所以故障日志钉不回二进制

一份故障日志里没有任何东西能说明它跑的是哪一份镜像。上面整场调查、以及四条分析路线里
被浪费掉的大半，都源于缺这一行。

shell 有 `PT_NOTE`（build-id），`kernel/lib/eftl/include/eftl/elf/note.hpp` 已经能读。
两条可做的（互补，不互斥）：

1. `scripts/qemu_boot.py` 在启动前打印所用 `.obi` 的 mtime 与 sha256。**成本最低，
   立刻消除本次这类误判。**
2. `userboot.cpp` 的两行加载日志里各带上程序的 build-id，让**内核自己**说出它加载了哪一份。

## 4. binder 对它不会应用的重定位格式仍然沉默

`bind()` 只质询 `DT_RELA`（`binder.cpp:122`）和 `DT_JMPREL`（`:126`）。
`DT_REL` 与 `DT_RELR` 既不应用也不拒绝——哪天给用户目标加上
`-Wl,-z,pack-relative-relocs`，`.relr.dyn` 里每一条都会被静默跳过，然后在 ring 3 以
**和 `a51f4cd` 完全相同的方式**炸掉。这正是 `binder.cpp:38-46` 那段注释声称已经堵上的
失败模式，它只堵住了两个标签里的两个。
