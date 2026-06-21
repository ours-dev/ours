load("@rules_cc//cc/toolchains:args.bzl", _cc_args = "cc_args")
load("@rules_cc//cc/toolchains:args_list.bzl", _cc_args_list = "cc_args_list")

def _cc_args_macro_impl(name, visibility, defines, include_dirs, copts, linkopts):
    # 1. 拼装编译阶段的参数 (defines, include_dirs, copts)
    compile_args = []
    compile_args.append(copts)
    compile_args.append(["-D" + d for d in defines])
    compile_args.append(["-I" + d for d in include_dirs])
    
    # 2. 创建 name.on_compile 目标 (必须严格使用 . 或 _ 后缀以符合符号宏命名规范)
    _cc_args(
        name = name + ".on_compile",
        actions = ["@rules_cc//cc/toolchains/actions:compile_actions"],
        args = compile_args,
        visibility = ["//visibility:private"],
    )

    # 3. 创建 name.on_link 目标
    _cc_args(
        name = name + ".on_link",
        actions = ["@rules_cc//cc/toolchains/actions:link_actions"],
        args = linkopts,
        visibility = ["//visibility:private"],
    )

    # 4. 用 cc_args_list 封装前两步生成的 cc_args
    _cc_args_list(
        name = name,
        args = [
            ":" + name + ".on_compile",
            ":" + name + ".on_link",
        ],
        visibility = visibility,
    )

# 暴露给外部调用的符号宏
cc_args = macro(
    implementation = _cc_args_macro_impl,
    doc = "聚合编译和链接参数的自定义宏",
    attrs = {
        "defines": attr.string_list(default = []),
        "include_dirs": attr.string_list(default = []),
        "copts": attr.string_list(default = []),
        "linkopts": attr.string_list(default = []),
    },
)