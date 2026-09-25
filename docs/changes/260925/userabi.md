# userabi 改动记录（2026-09-25）

## 构建生成用户可见文件的白名单 `config/user_bootfs`，随内核包打入（2026-09-25）

**文件**
- `system/user_executable.gni` — 每个 `user_executable()` 以 metadata `bootfs_user_visible` 声明自己的安装名
- `kernel/lib/userabi/vdso/BUILD.gn` — vDSO 同样声明为用户可见
- `kernel/kernel_package.gni` — 新参数 `user_bootfs`：为真时用 `generated_resource` 收集 deps 的 `bootfs_user_visible`，
  写成 `config/user_bootfs` 打进包里
- `kernel/products/image/dev/BUILD.gn`、`kernel/products/image/dev.dbg_1/BUILD.gn` — 两个镜像的内核包打开 `user_bootfs`
- `docs/repairs/260925/obi.md` — 新增第 4 条（见"验证"）

**症状**
无，这是 `docs/repairs/260925/userabi.md` 第 1 条定下的黑白名单做法的第一步：内核要知道哪些文件可以交给用户态，得有一份名单。

**原因**
按主人的决定（2026-09-25），用户态看得见什么由显式的白名单决定，默认不放行；名单由构建从 GN metadata 生成，不手写。

**改法**
改前：内核包里没有任何"哪些对用户可见"的信息。

改后：

```gn
    metadata = {
      bootfs_user_visible = [ _install_name ]
      ...
    }
```

```gn
  if (_user_bootfs) {
    _user_visible = "$target_name.user_visible"
    generated_resource(_user_visible) {
      ...
      data_keys = [ "bootfs_user_visible" ]
      output_conversion = "list lines"
      outputs = [ "config/user_bootfs" ]
    }
  }
```

- **标记放在产出文件的 target 上**：`user_executable()` 一律标记，vDSO 显式标记（用户态加载器要读它的 `.dynsym`）。没人标记的——
  内核映像与补丁表、名单本身、以后新加的任何东西——默认留在内核这边。
- **显式开启**：一个镜像的所有 kernel-storage 条目会被合并成一个 BootFS，第一版无条件生成名单时，physboot 的包也生成了一份（空的），
  两个同名的 `config/user_bootfs` 并列进了包里。改为只由承载用户程序的包开启；某个包该开而没开时，内核拿不到名单，结果是什么也不给
  用户态——出错的方向是"少给"。

**参照**
Fuchsia 没有对应的名单：它的内核包与用户 bootfs 本来就是两类 ZBI 条目（`zircon/kernel/phys/kernel-package.cc:65`），分开打包。
这里在同一个包里用名单达到同样的隔离；长期仍按设计 D7 拆成独立条目。

**验证**
- 生成的名单：dev 为 `userboot vdso shell init`，dbg_1 为 `userboot vdso shell fault_probe`。
- 两个镜像启动时内核打印的包清单里各有且只有一行 `config/user_bootfs`（25 / 32 字节）；dev `init: shell closed its channel`，
  dbg_1 `probes done (11 killed, 0 survived)`。
- 第一版（无条件生成）时清单里出现过两个同名条目，打包器没有报错——记为 `docs/repairs/260925/obi.md` 第 4 条。

**故意没做**
内核读这份名单、只把名单里的文件组成交给用户态的 BootFS VMO：后两步（先补 `kernel.name` 启动选项，再建 VMO）。
