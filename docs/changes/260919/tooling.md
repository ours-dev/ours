## 启动脚本报出它烧的是哪一份镜像

### 为什么这是个错

`out/` 是这个 worktree 里所有并发会话共用的。`IMAGE=out/bringup.x86/kernel.dev.obi
python3 scripts/qemu_boot.py` 这条命令不说明它拿到的是这个名字下的哪一次构建，
于是日志也不说明。

代价是一整轮白费的排查：一个缺页被当成「间歇性」追了 6 次对照运行、14 次复现尝试，
最后发现它是确定性的，而且在排查开始之前就已经被 `a51f4cd` 修掉了——那些启动里有
一半悄悄用上了别人正在重建的产物。一份不写明自己镜像的日志无法和另一份日志比较，
这不是在键盘前小心一点能解决的，身份必须落在日志里。

### 改动

`scripts/qemu_boot.py`，`build_esp()` 拷完之后：

```python
    copied = digest(esp / "boot" / "image.obi")
    if copied != digest(image):
        die(f"{image} changed while it was being copied -- another build is "
            "writing it; wait for that build to finish and run this again")
    identify("booting", image, copied)
    identify("via shim", shim, digest(esp / "boot" / "bootshim"))
```

两件事：

一是打印身份。摘要是关键的一半，光有 mtime 不够——产出相同字节的重建也会推进
mtime，而两次不同的构建可能落在同一秒里。

二是拒绝撕裂的镜像。摘要取自拷贝出来的那一份，再回头和源文件对一次：并发的
`ninja` 正在改写 `.obi` 的话，拿到的会是半新半旧的字节和一个谁都不描述的摘要。

### 验证

```
qemu-boot: booting out/bringup.x86/kernel.dev.obi
qemu-boot:   709960 bytes, built 2026-09-19 03:11:09, sha256 e079519cac4a34d3
```

`shasum -a 256` 对同一文件给出同样的 `e079519cac4a34d3`。两个镜像都正常启动到 kshell。
