# 待修记录

```
docs/repairs/<YYMMDD>/<MODULE>.md
```

一天一个目录，一个模块一个文件，只写**还没修的**：按严重程度排，每条要能被独立认领，
所以要带上文件、位置和判断依据。修掉一条就从这里删掉，同时在
`docs/changes/<YYMMDD>/<MODULE>.md` 追加一条改动记录。

改动记录（改了什么、为什么）在 `docs/changes/`，格式见 `docs/changes/TEMPLATE.md`。

## 历史

`docs/repairs-20260830.md` 是这套结构之前的单一流水账，2026-08-30 起按时间追加。
它已经三千多行，而且**多个 agent 在并发往里写**，章节编号都撞车了（同时存在两个"第三十一节"，
一个是 cpu-local 一个是 kernel/time）——这正是要拆开的原因。

kernel/time 和 x86 时钟这两块的改动记录已经复制到 `docs/changes/260903/`，以那里为准。
原文件**没有删内容**：它正被并发写入，整文件重写会冲掉别人在写的东西。
mm / cpu-local / task / 调度器的章节仍然只在那份文件里。
