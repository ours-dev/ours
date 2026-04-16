load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_cc//cc:find_cc_toolchain.bzl", "find_cc_toolchain", "use_cc_toolchain")

ConfigInfo = provider(
    doc = "Aggregated C/C++ build configuration flags",
    fields = {
        "defines": "depset of preprocessor defines",
        "copts": "depset of compiler flags",
        "linkopts": "depset of linker flags",
    },
)

_HEADER_EXTENSIONS = ["h", "hh", "hpp", "hxx", "inc", "inl"]

def _format_flag_value(value):
    """Convert a build setting value to a string suitable for C defines."""
    if type(value) == type(True):
        return "1" if value else "0"
    return str(value)

def _config_impl(ctx):
    transitive_defines = []
    transitive_copts = []
    transitive_linkopts = []

    for dep in ctx.attr.configs:
        info = dep[ConfigInfo]
        transitive_defines.append(info.defines)
        transitive_copts.append(info.copts)
        transitive_linkopts.append(info.linkopts)

    flag_defines = []
    for flag_target, define_fmt in ctx.attr.flag_defines.items():
        value = _format_flag_value(flag_target[BuildSettingInfo].value)
        flag_defines.append(define_fmt.format(value))

    return [
        ConfigInfo(
            defines = depset(ctx.attr.defines + flag_defines, transitive = transitive_defines),
            copts = depset(ctx.attr.copts, transitive = transitive_copts),
            linkopts = depset(ctx.attr.linkopts, transitive = transitive_linkopts),
        ),
    ]

config = rule(
    implementation = _config_impl,
    attrs = {
        "defines": attr.string_list(default = []),
        "copts": attr.string_list(default = []),
        "linkopts": attr.string_list(default = []),
        "configs": attr.label_list(
            default = [],
            providers = [ConfigInfo],
        ),
        "flag_defines": attr.label_keyed_string_dict(default = {}),
    },
)

def _collect_config_flags(configs):
    """Flatten all ConfigInfo providers into a single struct of lists."""
    defines = []
    copts = []
    linkopts = []
    for target in configs:
        info = target[ConfigInfo]
        defines.extend(info.defines.to_list())
        copts.extend(info.copts.to_list())
        linkopts.extend(info.linkopts.to_list())
    return struct(defines = defines, copts = copts, linkopts = linkopts)

def _partition_srcs(files):
    """Separate compilable source files from private header files."""
    srcs = []
    private_hdrs = []
    for f in files:
        if f.extension in _HEADER_EXTENSIONS:
            private_hdrs.append(f)
        else:
            srcs.append(f)
    return srcs, private_hdrs

def _ours_cc_library_impl(ctx):
    cc_toolchain = find_cc_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    config_flags = _collect_config_flags(ctx.attr.configs)
    srcs, private_hdrs = _partition_srcs(ctx.files.srcs)

    dep_compilation_contexts = [
        dep[CcInfo].compilation_context
        for dep in ctx.attr.deps
        if CcInfo in dep
    ]
    dep_linking_contexts = [
        dep[CcInfo].linking_context
        for dep in ctx.attr.deps
        if CcInfo in dep
    ]

    (compilation_context, compilation_outputs) = cc_common.compile(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        name = ctx.label.name,
        srcs = srcs,
        public_hdrs = ctx.files.hdrs,
        private_hdrs = private_hdrs,
        defines = config_flags.defines + ctx.attr.defines,
        local_defines = ctx.attr.local_defines,
        includes = ctx.attr.includes,
        user_compile_flags = config_flags.copts + ctx.attr.copts,
        compilation_contexts = dep_compilation_contexts,
    )

    (linking_context, linking_outputs) = cc_common.create_linking_context_from_compilation_outputs(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        compilation_outputs = compilation_outputs,
        name = ctx.label.name,
        user_link_flags = config_flags.linkopts + ctx.attr.linkopts,
        linking_contexts = dep_linking_contexts,
        alwayslink = ctx.attr.alwayslink,
    )

    output_files = []
    if linking_outputs.library_to_link:
        lib = linking_outputs.library_to_link
        if lib.static_library:
            output_files.append(lib.static_library)
        if lib.dynamic_library:
            output_files.append(lib.dynamic_library)

    return [
        DefaultInfo(files = depset(output_files)),
        CcInfo(
            compilation_context = compilation_context,
            linking_context = linking_context,
        ),
    ]

ours_cc_library = rule(
    implementation = _ours_cc_library_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True),
        "hdrs": attr.label_list(allow_files = True),
        "deps": attr.label_list(default = []),
        "defines": attr.string_list(default = []),
        "local_defines": attr.string_list(default = []),
        "copts": attr.string_list(default = []),
        "linkopts": attr.string_list(default = []),
        "includes": attr.string_list(default = []),
        "alwayslink": attr.bool(default = False),
        "configs": attr.label_list(
            default = [],
            providers = [ConfigInfo],
        ),
        "_cc_toolchain": attr.label(
            default = Label("@bazel_tools//tools/cpp:current_cc_toolchain"),
        ),
    },
    toolchains = use_cc_toolchain(),
    fragments = ["cpp"],
    provides = [CcInfo],
)
