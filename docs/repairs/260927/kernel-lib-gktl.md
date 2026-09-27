# kernel/lib/gktl — 待修

## 1. 命令表不认下划线：名字里带 `_` 的命令永远找不到，而且没有任何提示

**现象**：注册名为 `user_copy.test` 的命令，在 kshell 里敲下去之后什么都不发生——命令本身的日志一行都没有，
kshell 也不报"未知命令"。dbg_1 镜像里只能看到 `(kshell) run 'user_copy.test' with 0 argument(s)`。

**原因**：`kernel/lib/gktl/src/command.cpp` 的 `CommandKeyEncoder` 只编码 `a-z`、`A-Z`、`0-9`、
`0x60`、`-`（`0x2d`）、`.`（`0x2e`），其余字节一律编成 0：

```cpp
if (byte == 0x60) {
    return 63;
}
```

`0x60` 是反引号，而紧挨着它的 `0x5f` 才是下划线。反引号不会出现在命令名里，下划线会，所以这很像是一个
差一的笔误。`_` 被编成 0 之后，`TREE.find()` 找不到这个名字，而 `gktl::run_command()` 对找不到的名字
按设计不吭声（`kshell/shell.cpp` 的注释里说明了这一点）。

**影响**：目前在册的命令都没有下划线（`mutex.stress`、`prof.*`、`log`、`dump-frame`），所以还没人撞上。
`usercopy.test` 为此改了名。

**建议修法**：把 `0x60` 改成 `0x5f`（若确实需要反引号，就两者都编码，把 `NUM_CHILDREN` 相应加一）。
另外可以考虑在 `build_radix_tree()` 里对编成 0 的字节 `DEBUG_ASSERT`，让不支持的名字在启动时就报出来，
而不是在被敲的时候静默失效。
