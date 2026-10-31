# storage-layer: 交接给 Cloud 的两个 virtio-blk 内核/QEMU 侧问题

你在 feat/driver-caps 分支上为用户态 virtio-blk 驱动实现的内核能力（Mmio/Iommu/BTI/Pin、
vmo_create_contiguous）工作正常，驱动已经用它们把设备驱动起来了。下面是用户态排查到底、
需要你定夺的两个问题。仓库：exp/fs-service 分支（worktree `.worktrees/fs-service`），
最新提交 395463a8（含本文档与全部证据）。

## 问题 1（阻塞）：读 DMA 恰好少最后 16 字节

**现象**：驱动提交读请求，设备完成（QEMU trace 有 `virtio_blk_handle_read` +
`virtio_blk_req_complete status 0`），但到达驱动的数据**缺最后 16 字节**：

- 512 字节读（1 块）：字节 0–495 正确，496–511 = 0；
- 1024 字节读（2 块）：字节 0–1007 正确，1008–1023 = 0。

**已知事实**（全部经 QEMU monitor `xp` 读 Guest 物理内存核实）：

- 磁盘镜像对应扇区内容正确（写路径端到端无误，文件里有完整模式，含 496–511）；
- QEMU 自己的 RAM 在争议字节上就是 0x00 —— 与 Guest CPU 视图一致，
  所以 Guest 的映射是忠实的，零在设备的 DMA 写入（或未写入）里；
- ring 布局经过 monitor 逐字段核实（desc/avail/used 地址与内容都符合预期）；
- 与访问宽度无关（自然宽度/合成读都试过）；与现代/过渡设备形态无关（都复现）。

**疑点**：16 字节恰好等于 virtio-blk 请求头的大小（它在同链的**另一个**描述符 desc[0] 里，
数据在 desc[1]，状态在 desc[2]）。每个扇区少 16 字节（496 = 512−16；1008 = 1024−16）。

**请回答**：这版 QEMU 的 `virtio_blk_handle_read` 的 DMA 路径里，是什么把数据尾部的
16 字节吞掉了？是我们的描述符链/内存形状不满足它的前提，还是 QEMU 侧的语义/缺陷？
如果是前者，指出我们该改什么（驱动在 exp/fs-service，随便改）；后者则记录并给绕法。

复现：`.agents/tasks/storage-layer/QEMU-CFG-FINDINGS.md` 增补 2 有完整步骤；驱动在
`system/driver/virtio-blk/`，启动命令（需要挂一块盘）：

```bash
python3 scripts/qemu_boot.py -device virtio-blk-pci,drive=hd0 \
  -drive file=/tmp/disk.img,format=raw,if=none,id=hd0
# routing_checks 会报: first differing byte at 496 (或 2 块读的 1008)
```

## 问题 2（预告）：R4 中断

看到你在 feat/driver-caps 上提交了 interrupt-system 的 DESIGN/PLAN（虚拟中断号）。
驱动现在轮询 used ring，等 R4 落地后换成 `interrupt_wait`，其余不动。实现时请知会：
驱动的 bring-up 顺序和错误路径都是按"无中断"写的，换中断只动等待那一处。

## 优先级

问题 1 阻塞存储层（GPT、盘上文件系统都建在读回正确的数据上）。问题 2 不阻塞。
