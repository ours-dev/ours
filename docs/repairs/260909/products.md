# products —— 待修项（2026-09-09）

## fake.obi 起不来：physboot 报 `Invalid kernel package: 4`

**严重程度**：中。`//kernel/products/image/fake` 存在的理由就是"vmours 之外还有一条
能单独验证 phys 启动链的路"，它自己不通的时候，这条路等于没有。

**复现**

```bash
prebuilt/ninja/ninja -C out/bringup.x86
IMAGE=$PWD/out/bringup.x86/fake.obi ./scripts/qemu_boot.py
```

日志尾部：

```
[debug] Loaded phys module 'physboot' at 0x1FEBF000, entry 0x1FEC4830
[error] Invalid kernel package: 4
```

physload 把 physboot 载入并进入了，physboot 解析 OBI 里的内核存储块时拒绝。
`kernel.dev.obi` 和 `kernel.dev.dbg_1.obi` 同一天同一次构建都能正常启动到用户态，
所以坏的是 fake 这个镜像的组装，不是 phys 链本身。

**从哪儿查**
- `kernel/products/image/fake/BUILD.gn` 的 `kernel_package` / `kernel_image` deps 顺序，
  对照 `kernel/products/image/dev/BUILD.gn` 里那段注释说的拓扑约束（physload 必须是
  item 0，physboot 必须显式写 phys 工具链）。
- physboot 里报出 "Invalid kernel package" 的那处判断，看 `4` 是哪个枚举值/第几项。
- `prebuilt/obit/obit tree out/bringup.x86/fake.obi` 与 `kernel.dev.obi` 对比 item 拓扑。

**影响到的东西**
`ours.code-workspace` 的调试下拉框里本来该有 `fake` 这一项，因为这个问题暂时没放进去；
`scripts/qemu_debug.py` 里 `fake` 的映射是对的、留着，修好之后把菜单项加回来即可。
