# kernel/include/ours/boot — 改动记录（2026-09-25）

## vmours 的 BootOptions 读 `kernel.name`（2026-09-25）

**文件**
- `kernel/include/ours/boot/options.hpp` — 加 `KERNEL_NAME_OPTION`、`kernel_name()` 和 64 字节的 `m_kernel_name`，默认 "vmours"
- `kernel/top/boot_options.cpp` — `BootOptions::apply()` 按 `parse_name()` 读这一项

**症状**
无。给文件系统计划 K12 第 3 步做准备：userabi 建交给用户态的 BootFS VMO 时，要把内核自己的目录列进黑名单
（`docs/repairs/260925/userabi.md` 第 1 条），而 vmours 不知道自己在内核包里叫什么。

**原因**
内核包里哪个目录是内核自己的，由启动选项 `kernel.name` 决定。phys 一直在读它（`phys::BootOptions::kernel_name()`），
`physboot_init()` 靠它找到要装的 ELF；vmours 的 `BootOptions` 只有 `kernel.select.userboot` 和 `kernel.shell`。
黑名单要写成"内核的目录"而不是写死 "vmours"，否则换一个名字打包就悄悄失效。

**改法**
改前：

```cpp
  private:
    bool m_shell{false};
    char m_userboot[64]{"userboot"};
};
```

改后：

```cpp
    [[nodiscard]]
    auto kernel_name() const -> std::string_view {
        return m_kernel_name;
    }

  private:
    bool m_shell{false};
    char m_userboot[64]{"userboot"};
    char m_kernel_name[64]{"vmours"};
};
```

```cpp
    if (auto const value = cmdline.value(KERNEL_NAME_OPTION); value.is_some()) {
        parse_name(KERNEL_NAME_OPTION, value.value(), m_kernel_name);
    }
```

- 默认值和缓冲大小都与 phys 的那份相同：两边必须指同一个目录，phys 能接受的名字这里也要放得下。
- **两个读者可能不一致**，写进了注释：phys 把 bootloader 的命令线叠在镜像的 Config 条目之上，vmours 只读 Config 条目，
  所以在 GRUB 里敲 `kernel.name=` 会让 phys 装另一个目录，而这里仍是镜像里写的那个。对黑名单来说可以接受——它只是兜底，
  真正决定放行什么的是它前面的白名单。哪天有用途必须知道 phys *实际*装的是哪个目录，应该从 handoff 里拿，而不是读这个选项。

**参照**
Zircon `kernel.select`（`zircon/kernel/lib/boot-options/include/lib/boot-options/options.inc:904`），默认 "zircon"，
选 STORAGE_KERNEL 里的哪个内核包。Zircon 不会出现上面的不一致：内核直接接过 physboot 解析好的 gBootOptions。

**验证**
两个镜像构建、启动：dev `init: shell closed its channel`，dbg_1 `shell: probes done (11 killed, 0 survived)`。
这一项还没有读者，行为不变。

**故意没做**
把 phys 实际装载的目录经 handoff 传给 vmours：黑名单用不着那么准，而 handoff 不带命令线是有意的（见类注释）。
