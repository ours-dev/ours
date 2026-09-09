# obi —— 改动记录（2026-09-09）

## `obi()` 把调用方的 metadata 转发进了三个目标，任何 walk 都会重复收（2026-09-09）

**文件**
- `build/obi/obi.gni` —— 调用方的 `metadata` 只挂在对外的 `group(_main_target)` 上

**症状**
潜伏。给 `obi()` 传 `metadata` 的调用方，其声明的**每一个 key 都会被走图收两遍**。

第一次撞上是在给 `kernel_image()` 加一份镜像清单时。往 `obi()` 传了一条
`debug_images` 记录，一个镜像一行，结果 `gn gen` 出来的 json 是这样：

```json
[
  { "name": "kernel.dev",       "obi": "kernel.dev.obi",       ... },
  { "name": "kernel.dev",       "obi": "kernel.dev.obi",       ... },
  { "name": "kernel.dev.dbg_1", "obi": "kernel.dev.dbg_1.obi", ... },
  { "name": "kernel.dev.dbg_1", "obi": "kernel.dev.dbg_1.obi", ... },
  { "name": "fake",             "obi": "fake.obi",             ... },
  { "name": "fake",             "obi": "fake.obi",             ... }
]
```

三个镜像，六行，两两完全相同。

**原因**
`obi()` 展开出三个目标：`generated_file(_rsp_target)`（拼 obit 的 rsp）、
`action(_pack_target)`（真正打包）、`group(_main_target)`（对外的那个）。前两个**各自**
`forward_variables_from(invoker.metadata, "*")`：

```gn
  generated_file(_rsp_target) {
    ...
    metadata = {
      if (defined(invoker.metadata)) {
        forward_variables_from(invoker.metadata, "*")     # ← 第一份
      }
    }
  }

  action(_pack_target) {
    ...
    metadata = {
      obi_input_args = []
      obi_input_args_barrier = []
      obi_input_args += [ "--type=container", rebase_path(_obi, root_build_dir) ]

      if (defined(invoker.metadata)) {
        forward_variables_from(invoker.metadata, "*")     # ← 第二份
      }
    }
  }
```

而 `group(_main_target)` `public_deps = [":$_pack_target"]`，`_pack_target`
`deps = [":$_rsp_target", ...]` —— 也就是说这两个目标**都在从依赖方下行到 deps 的那条
路径上**。GN 的 metadata walk 从起点做 DFS，把沿途每个目标里名字命中 `data_keys` 的
list 依次拼起来；同一个 key 出现在路径上的两个目标里，就被收两次。

之所以一直没人发现：现有调用方声明的 key 都没有对应的 walk。`obi_input_args` 是
`obi()` 自己在 `_pack_target` 上发的（那条 `--type=container` 自注册，用于嵌套镜像），
不走 `invoker.metadata` 这条路，所以不受影响。**第一个真的去 walk 自己 key 的调用方，
拿到的就是双份。**

**改法**
调用方的 metadata 只挂在唯一对外的那个目标上，另外两个不再转发：

```gn
  group(_main_target) {
    forward_variables_from(invoker, [ "testonly", "visibility" ])
    public_deps = [ ":$_pack_target" ]

    metadata = {
      if (defined(invoker.metadata)) {
        forward_variables_from(invoker.metadata, "*")
      }
    }
  }
```

`obi()` **自己**的 `obi_input_args` 自注册留在 `_pack_target` 上没动——那条记录命名的
正是这个目标的产物，挂在别处才是错的。

被否掉的方案：在消费端去重。行不通也不该做——GN 的 list 拼接没有集合语义，去重要求
每个 key 的记录都可比较且可去重，而 metadata 的值是任意 scope；而且真正错的是发的一方
发了两遍，不是收的一方收得太多。

**参照**
Fuchsia `//build/zbi/zbi.gni` 的 `zbi()`：调用方 metadata 走 `group()`，模板自己的
`zbi_input_args` 自注册走打包 action，正是这里改成的形状。

**验证**
改前改后各跑一次 `gn meta`，覆盖全部五个 `obi()` 调用方
（`//kernel/products/image/{dev,dev.dbg_1,fake}`、`//tests/dist-smoke:obi_smoke`、
`//tests/product-smoke:product_smoke`）× 全部五个相关 key（`images`、
`debug_symbol_files`、`distribution_entries`、`obi_input_args`、以及探针用的
`debug_images`），对比走图**值**（去掉 `Extracted from:` 那段目标清单）：

- `images` / `debug_symbol_files` / `distribution_entries` / `obi_input_args`：
  25 组里 20 组**逐字节相同**——现有行为一点没变。
- `debug_images`（探针）：从每镜像 2 条变成 1 条。

`prebuilt/ninja/ninja -C out/bringup.x86` 全绿（32/32），三个 `.obi` 正常重打。

**故意没做**
- **没有给 `obi()` 加 metadata barrier**。`obi_input_args_barrier` 只管自己那个 key；
  调用方的 key 要不要 barrier 是调用方的事，替它决定等于替它猜。
- **没有动 `obi_input()`**。它只往内层 `fini_manifest` 转发一次，没有同一个问题。
