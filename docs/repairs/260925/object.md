# kernel/object 待修项（2026-09-25）

## 1. `VmoDispatcher::create_paged` 的注释说 size 会按页向上取整，实际不会

**现象**：注释错误，行为本身没有问题。`kernel/object/include/ours/object/vmo_dispatcher.hpp` 里 `create_paged` 的注释写着
"`size` is rounded up to a page by the VMO itself"。但 `VmObjectPaged::create()` 把 size 原样交给 `VmCowPages::create()`，
`VmCowPages` 的构造函数原样存下（`kernel/mm/vm_cow_pages.cpp:11`，`m_size(size)`）。`size()` 返回的是创建时给的字节数，
`read()`/`write()`/`vmo_read` 的范围检查也以它为界。

**原因**：注释描述的是 Zircon 的行为，但 Zircon 的取整也不在 VMO 里，而在系统调用路径上
（`zircon/kernel/object/vm_object_dispatcher.cc:52` 的 `VmObject::RoundSize()`）。本仓库的接口设计同样把取整放在
`ours_vmo_create`（`docs/design/260923/userspace-interface.md` 2.3）。今天 `create_paged` 的调用者传的都是页整数倍，所以没有暴露。

**改法**：改注释，写明 size 原样保存、取整是 `vmo_create`（K17）的事。不要在 VMO 里补取整：那会让内核自己建的 VMO
（如 K12 的 BootFS VMO）大小和它的内容长度对不上。

**验收**：注释与 `VmCowPages` 的行为一致；两个镜像照常构建、启动。
