# examples/shell 改动记录（2026-09-25）

## 坏指针检查补上第三类：已映射但只读的目的地址（vDSO）（2026-09-25）

**文件**
- `examples/shell/shell.cpp` — `check_bad_pointers()` 收 `vdso_base`，新增三项；`_start` 不再把 `vdso_base` 标 `[[maybe_unused]]`

**症状**
2026-09-24 加坏指针检查时，计划里的第三类——往已映射但只读的页写——没法加：每个 x86 叶子页表项都被强加 Writable，
这类写会成功，检查会因为与故障路径无关的原因失败（见 `docs/changes/260924/examples-shell.md` 那一节的"故意没做"）。
`docs/changes/260925/x86-kernel.md` 修掉页表项之后，补上。

**原因**
这一类走的是另一条故障路径：页存在，缺页是保护违例（写只读），要由地址空间拒绝（`VmMapping::fault()` 对无 Writable
的区域拒绝写），再由用户拷贝的恢复变成 InvArgs。前两类（未映射、内核半区）碰不到这条路。

**改法**
改前：注释写明"第三类缺失"，`check_bad_pointers()` 无参数。

改后：在原有检查之后，对同样三个输出目的地址换成 vDSO：

```cpp
    ok &= expect("shell: channel_write(1 byte) = ", ..., Status::Ok);
    ok &=
        expect("shell: channel_read(into the vDSO) = ",
               ours_channel_read(b, ours::ChannelReadOptions::None, reinterpret_cast<void *>(vdso),
                                 nullptr, 1, 0, &actual_bytes, &actual_handles),
               Status::InvArgs);
    ok &= expect("shell: wait_one(observed = vDSO, poll) = ",
                 ours_object_wait_one(b, Signals::Readable, 0, reinterpret_cast<Signals *>(vdso)),
                 Status::InvArgs);
    ok &= expect("shell: channel_create(out0 = vDSO) = ",
                 ours_channel_create(ours::ChannelCreateOptions::None,
                                     reinterpret_cast<ours::HandleValue *>(vdso), &e),
                 Status::InvArgs);
```

vDSO 只当目的地址用：读它是正当的。检查之后程序继续经 vDSO 做系统调用，这本身就证明 vDSO 没被写坏。

**参照**
无对应物；本项目自己的启动自检。行为参照 Zircon：往只读映射的用户拷贝在 `user_copy` 的缺页恢复中失败，系统调用回
`ZX_ERR_INVALID_ARGS`。

**验证**
dev 与 dbg_1 都打出：

```
shell: channel_read(into the vDSO) = InvArgs
shell: wait_one(observed = vDSO, poll) = InvArgs
shell: channel_create(out0 = vDSO) = InvArgs
shell: bad-pointer checks passed
```

之后照常到 `--- end ---` 并退出。

**故意没做**
写 vDSO 的 fault_probe 探针（程序自己写只读页，应被杀）单独一条提交。
