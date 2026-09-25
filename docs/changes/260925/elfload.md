# kernel/lib/elfload 改动记录（2026-09-25）

## 程序映像改用 paged VMO，帧随 VMO 归还；Binder 经 Image::writer 写重定位（2026-09-25）

**文件**
- `kernel/lib/elfload/src/loader.cpp` — 不再 `get_frames_n()` 取帧、包成 physical VMO；改为 `VmoDispatcher::create_paged()`，逐段 `VmObject::write()`
- `kernel/lib/elfload/include/elfload/image.hpp` — 新增 `ImageWriter`（函数指针加上下文）；`Image::view` 换成 `Image::writer`
- `kernel/lib/elfload/src/binder.cpp` — 重定位不再经 eftl 的 `MappedMemory` 写一个指针，改经 `importer.writer`
- `kernel/lib/elfload/include/elfload/binder.hpp`、`loader.hpp` — 注释随之改
- `kernel/object/include/ours/object/vmo_dispatcher.hpp` — `create_physical()` 的注释不再说"userboot 的映像就是这样"

**症状**
潜伏：每装一个程序（以及每个进程的那份 vDSO），它的全部帧就泄漏了，永远不会归还。今天看不出来，因为进程死后
地址空间本来就不拆（`docs/repairs/260924/object.md` 第 2 条）；文件系统计划的 K6（`VmAspace::destroy`）一做，
程序映像这部分仍然还不回来——K6 的验证"每个探针归还的帧数相同、空闲帧数只差一个内核栈"就不可能成立。

**原因**
`Loader::map()` 用 `get_frames_n()` 取一段连续帧，填好后包成 **physical** VMO。physical VMO 的定义就是
"一段别人拥有的物理内存的窗口"（MMIO 的形状），它不拥有这些帧，析构时也不释放；而取帧的人（loader）把帧交出去
之后也不再记得它们。于是这些帧没有任何主人。原注释自己也写了"paged VMO 才是诚实的答案，等
`VmObjectPaged::write()` 不再回 Unimplemented 就改"——K2 让它不再回 Unimplemented 了。

顺带的一个约束：Binder 通过 `Image::view`（指向那段连续帧的内核指针）写 GOT。paged VMO 的页物理上不连续，
给不出这样一个指针。

**改法**
改前：

```cpp
    auto const nr_frames = out.span / PAGE_SIZE;
    auto *const backing = mm::get_frames_n<u8>(mm::GAF_USER, nr_frames);
    ...
    std::memset(backing, 0, out.span);
    for (...) {
        ...
        std::memcpy(backing + (VirtAddr(phdr.vaddr) - lowest), bytes.data() + phdr.offset,
                    phdr.filesz);
    }
    ...
    auto const status = object::VmoDispatcher::create_physical(mm::PhysMap::virt_to_phys(backing),
                                                               out.span, vmo, vmo_rights);
    ...
    out.view = backing;
```

```cpp
    eftl::elf::MappedMemory<Traits> memory(importer.view, importer.span, {&region, 1});
    ...
            if (!memory.write<u64>(static_cast<u64>(rela.r_offset), value)) {
```

改后：

```cpp
    if (auto const status = object::VmoDispatcher::create_paged(out.span, vmo, vmo_rights);
        Status::Ok != status) { ... }
    auto memory = vmo.dispatcher()->vmo();
    for (...) {
        ...
        if (auto const status = memory->write(bytes.data() + phdr.offset,
                                              VirtAddr(phdr.vaddr) - lowest, phdr.filesz);
            Status::Ok != status) { ... }
    }
    ...
    out.writer = {.write = write_through_vmo, .context = memory.as_ptr_mut()};
```

```cpp
struct ImageWriter {
    auto (*write)(void *context, usize offset, void const *bytes, usize size)
        -> ours::Status = nullptr;
    void *context = nullptr;
};
```

```cpp
    auto store = [&importer](u64 const r_offset, u64 const value) -> Status {
        if (r_offset < importer.image_base) {
            return Status::OutOfRange;
        }
        auto const at = r_offset - importer.image_base;
        if (at > importer.span || importer.span - at < sizeof(value)) {
            return Status::OutOfRange;
        }
        return importer.writer.write(importer.writer.context, at, &value, sizeof(value));
    };
```

- **.bss 的零**：新 VMO 处处读为零（`VmCowPages::alloc_pages()` 一律加 `Gaf::Zero`），`filesz` 与 `memsz` 之间从不写，
  所以不再需要先 `memset`。
- **段越界**：file 内越界照旧回 InvArgs，且改成不会回绕的写法；段在映像里越界（`filesz > memsz` 之类）交给 VMO 的
  范围检查，回 OutOfRange，不做短写。
- **为什么是写入函数，而不是暂存区**：另一种做法是 loader 先在一段连续暂存帧里拼好映像、Binder 照旧写指针，
  最后再加一个 `finish()` 把暂存区写进 VMO。被否掉：它多一段帧、多一个必须记得调用的收尾步骤，而文件系统计划
  §4.6 的用户态 launcher 本来就要"Binder 用 vmo_write 写 GOT"——两处都能给出的，只有"把这些字节写到映像的这个
  偏移"。写入函数用函数指针加上下文，不用虚接口，Binder 仍然不认识任何内核类型。
- **eftl 的 `MappedMemory` 去掉**：它做的是"多个 load region 之间的地址翻译"，而 loader 一直只给一个 region，
  翻译退化成一次减法，现在直接写出来。

**参照**
- Fuchsia 的程序映像是从文件的 VMO 映射出来的：userboot 调 `process_builder::elf_load::load_elf(&program_vmo, ...)`
  （`src/bringup/lib/userboot/rust/src/program.rs:384`），`load_elf()`（`src/lib/process_builder/src/elf_load.rs:123`）
  按段映射那个 VMO，可写段先做一个 `SNAPSHOT_AT_LEAST_ON_WRITE` 子 VMO（同文件 :192）。页始终归 VMO 所有，
  不存在"包一段借来的帧"的做法；`VmObjectPhysical` 的定位是"VMO representing a physical range of memory"
  （`zircon/kernel/vm/include/vm/vm_object_physical.h:29`）。
- 按段映射和子 VMO 是 ours 的 K20（`vmo_create_child(Slice)`）与 K21（带偏移的 `vmar_map`）之后的事；这一步只把
  "谁拥有这些页"改对。

**验证**
- 构建通过；clang-format、clang-tidy 在改动的 elfload 文件上无告警。
- dev：init 收到 shell 的 "pong"，`shell: bad-pointer checks passed`，到 `--- end ---`；每个程序的 `elfload: bound ...`
  行与改前一致（绑定的地址相同）。
- dbg_1：`probes done (10 killed, 0 survived)`，`bad-pointer checks passed`。10 个 fault_probe 都经这条新路径装载、绑定、
  跑到各自的违规指令，说明 GOT 经 VMO 写入后用户态看得见。
- 帧确实随 VMO 归还，要等 K6 拆地址空间时核对（届时按设计比对 PMM 空闲帧数）。

**故意没做**
- 按段映射、各段各自的权限：仍然整体 RWX 映射（`docs/repairs/260919/elfload.md` 第 1 条）。
- `create_physical()` 现在没有调用者，但它是 MMIO 的形状，按"空桩可能是脚手架"的约定保留，只改了注释。
