# 产品定义

本目录存放产品的声明式定义。产品 = 命名的、可选择的构建配置：选定一个产品
（加上目标架构）即可一次性产出该产品所需的全部镜像与产物，不需要手工拼装。

目录布局对标 Fuchsia 的 `//products/`：

```
products/
  <name>.gni        ← 产品定义（声明式 scope：选哪些镜像 + 什么策略）
  <name>/BUILD.gn   ← 实例化：import 产品定义，调用 product() 模板装配
  README.md         ← 本文件（轨道说明）
```

## 选择与构建

产品选择对标 `fx set <product>.<board>`：

```
tools/set.py bringup.x86          # 写 args.gn（ours_product + target_cpu）并 gn gen
ninja -C out //products/bringup:bringup
```

或直接写 args.gn：

```
gn gen out --args='target_cpu="x86" ours_product="bringup"'
```

根 `//:default` 按 `ours_product` 构建参数动态依赖所选产品，因此
`ninja -C out` 也会构建它。所有可选产品可通过 `tools/set.py --list` 查看。

## 产出物

构建一个产品目标会一次性产出：

```
out/<output_dir>/
  kernel.<image>.obi      ← 每个内核镜像（可启动 OBI 容器）
  product_info.json       ← 产物清单（JSON：镜像、cmdline、bootfs、switchset、
                             策略字段），供脚本/工具消费（对标 Fuchsia 产品包
                             的 images.json 思想）
```

## 轨道

产品按"轨道"组织，后一档在前一档之上**只增不减**（对标 Fuchsia RFC-0220 的
产品轨道思想）：

### bringup（基线）

最小的可启动镜像集合，用于 bring-up 内核与核心驱动、验证构建流水线。包含两个
内核镜像：

* `kernel.dev` — the production kernel, no tests (loaded with physload; see
  the LegacyBoot comment in `kernel/arch/x86/phys/legacy-boot.hpp`);
* `kernel.dev.dbg_1` — the test-capable debug image: kernel shell, assertions
  and the kernel tests.

Each kernel is built in the switchset its image names: `dbg.0` for
`kernel.dev`, `dbg.1` for `kernel.dev.dbg_1`.

未来产品（minimal / core 等）应以 bringup 为基础，在其上添加内容而不是削减。

## 分工

| 层 | 位置 | 职责 |
|----|------|------|
| 内核编译 | `kernel/products/`、`build/switch/set/` | 内核怎么编译（configs、switchset 策略） |
| 内核可执行 | `kernel/products/bin/` | `vmours` / `vmours.with-tests` |
| 共享镜像配方 | `kernel/products/image/` | 所有产品共用的组装零件（`common_package_items`） |
| 产品定义 | `products/<name>.gni` | 选哪些镜像 + 什么策略（本目录） |
| 装配引擎 | `build/product/product.gni` | `product()` 模板：把产品定义装配成构建目标 |
| 装配原语 | `build/obi/`、`kernel/kernel_image.gni` 等 | obi_input / kernel_package / kernel_cmdline / obi |

## 新增一个产品

1. 新建 `products/<name>.gni`，写一个 `product = { ... }` scope（字段见
   `build/product/product.gni` 头部文档：`kernel_images`、`kernel_cmdline`、
   `bootfs`、`switchset`、`output_dir` 等）；
2. 新建 `products/<name>/BUILD.gn`：

   ```gn
   import("//build/product/product.gni")
   import("../<name>.gni")

   product("<name>") {
     forward_variables_from(product, "*")
   }
   ```

3. `tools/set.py <name>.<arch>` 即可选择并构建；`tools/set.py --list` 会自动
   列出新产品（按 `products/*.gni` 发现）。

## 冒烟测试

`//tests/product-smoke:product_smoke` 用"假内核"（resource → distribution
entries）走完整 product() 装配流水线，验证 bringup 刻意留空的 cmdline/bootfs
注入与 product_info.json 生成，无需编译任何内核 C++。
