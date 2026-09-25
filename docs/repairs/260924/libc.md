# kernel/lib/libc 待修项（2026-09-24）

## 1. libc++ 的 `std::format` / `basic_string` 仍引用会 panic 的 `operator new(size_t)`（可达性未查）

来源：修 kshell 命令 panic 时（`docs/changes/260924/gktl.md`）对
`out/bringup.x86/kernel_x86.dbg.0/obj/kernel/products/bin/vmours.with-tests` 做的 objdump。
`gktl::run_command()` 那一处已经去掉，镜像里还剩 7 处 `call operator new(unsigned long)`（`_Znwm`）：

- `std::__format::__allocating_buffer<char>::__prepare_write()`
- `std::basic_string<char>::basic_string(char const *, allocator const &)`
- `std::basic_string<char>::__grow_by_and_replace()`
- `std::basic_string<char>::push_back()`
- `std::__formatter::__format_floating_point<float|double|long double, ...>()`（3 处）

内核里不带 gaf 的 `operator new(size_t)` 只有 `kernel/lib/libc/cpp_dependencies.cpp` 的弱桩，
函数体是 `ours::panic(__func__)`。所以只要执行到上面任何一处，现象就和 kshell 那次一样：
`[... cpu0 error] operator new`。

已知的部分：

- 浮点那 3 处是有意的——`kernel/lib/libc/libcxx_format_glue.cpp` 写明内核不用浮点，
  相关 `to_chars` 只是为了链接过去、到达即 panic。不需要修，除非规则变了。
- `basic_string` 的成员是同一个文件里 `template class basic_string<char>;` 显式实例化出来的，
  因为 libc++ 的 `<format>` 头引用它们。它们只在格式化结果真的要落进一个 `std::string`、
  或 `<format>` 的输出缓冲需要增长时才会被调用。

没查的部分，也就是认领这条要做的事：从 `ustl::fmt::format_to` / logz4 / `ktl::name.hpp` 出发，
确认哪条内核路径能走到 `__allocating_buffer` 或 `basic_string` 的分配成员。
能走到的，要么改成固定缓冲（`format_to_n` 一类）不让它分配，要么给 libc++ 一个经内核堆的
`operator new(size_t)`——后者是全局行为变化，要先和维护者确认，因为 `heap/new.hpp`
故意只提供带 gaf 的形式，让每次分配都显式说明 gaf。
