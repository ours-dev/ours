# elfload 待修项（2026-09-19）

## 1. 用户程序的三个段被当成一段映射

`elfload::Loader::map()` 把一个 ELF 的全部 PT_LOAD 合并成一段连续区间，按同一套权限
（`User|Readable|Writable|Executable`）映射。也就是说每个用户程序的代码段是可写的，
数据段是可执行的——W^X 正是为了禁止这件事而存在。

目前不致命（ring 3 里还没有别人的输入能落进这块内存），但它同时也挡着 PT_GNU_RELRO：
绑定完成后本应把 GOT 所在的页改回只读，而现在整段本来就是可写的。

改法：按段映射，每段用自己的 `p_flags`；重定位写入走内核侧视图（`elfload::Image::view`）
所以不受用户侧权限影响，绑定完成后再按 PT_GNU_RELRO 收权限。eftl 的
`LoadPolicy::make_plan()` 已经给出每段的 `LoadRegion` 和 `RelroInterval`。
