"""
Comprehensive toolchain variant suite system supporting multi-dimensional combinations
of architectures, platforms, compilers, and build variants.

This module provides a flexible way to generate multiple toolchain variants
with different combinations of:
  - Architectures (x86_64, aarch64, arm, etc.)
  - Platforms (linux, macos, windows, etc.)
  - Compilers (gcc, clang, msvc, etc.)
  - Build variants (debug, release, sanitizer, etc.)
"""

load("@bazel_skylib//lib:dicts.bzl", "dicts")
load("@bazel_skylib//lib:sets.bzl", "sets")
load("@bazel_skylib//lib:paths.bzl", "paths")

# ============================================================================
# Core Data Structures and Helpers
# ============================================================================

def _make_variant_name(platform, arch, compiler, variant_type, suffix = ""):
    """Generate a standardized variant name."""
    parts = [platform, arch, compiler, variant_type]
    name = "_".join([p for p in parts if p])
    return name + suffix if suffix else name

def _make_variant_key(platform, arch, compiler):
    """Generate a key for platform+arch+compiler combination."""
    return "{}_{}_{}" .format(platform, arch, compiler)

def _apply_platform_config(base_defines, base_copts, platform_config):
    """Apply platform-specific configuration."""
    defines = dict(base_defines)
    copts = list(base_copts)
    
    defines.update(platform_config.get("defines", {}))
    copts.extend(platform_config.get("copts", []))
    
    return defines, copts

def _apply_arch_config(defines, copts, arch_config):
    """Apply architecture-specific configuration."""
    defines = dict(defines)
    copts = list(copts)
    
    defines.update(arch_config.get("defines", {}))
    copts.extend(arch_config.get("copts", []))
    
    return defines, copts

def _apply_compiler_config(defines, copts, compiler_config):
    """Apply compiler-specific configuration."""
    defines = dict(defines)
    copts = list(copts)
    
    defines.update(compiler_config.get("defines", {}))
    copts.extend(compiler_config.get("copts", []))
    
    return defines, copts

def _apply_variant_config(defines, copts, variant_config):
    """Apply build variant-specific configuration."""
    defines = dict(defines)
    copts = list(copts)
    linkopt = list(variant_config.get("linkopt", []))
    
    defines.update(variant_config.get("defines", {}))
    copts.extend(variant_config.get("copts", []))
    
    return defines, copts, linkopt

# ============================================================================
# Multi-dimensional Variant Suite Rule
# ============================================================================

def _multi_dim_variant_suite_impl(ctx):
    """Generate variants based on multi-dimensional combinations."""
    
    # 获取配置
    platforms = ctx.attr.platforms
    architectures = ctx.attr.architectures
    compilers = ctx.attr.compilers
    variants = ctx.attr.variants
    
    # 组合生成逻辑
    exclude_combinations = sets.make(ctx.attr.exclude_combinations)
    enable_combinations = ctx.attr.enable_combinations
    
    generated_files = []
    variant_info = {}
    
    # 基础定义（应用到所有变体）
    base_defines = dict(ctx.attr.default_defines)
    base_copts = list(ctx.attr.default_copts)
    
    # 遍历所有可能的组合
    variant_count = 0
    for platform_name, platform_config in platforms.items():
        for arch_name, arch_config in architectures.items():
            for compiler_name, compiler_config in compilers.items():
                for variant_type, variant_config in variants.items():
                    # 生成变体名称和键
                    variant_name = _make_variant_name(
                        platform_name,
                        arch_name,
                        compiler_name,
                        variant_type,
                    )
                    variant_key = _make_variant_key(
                        platform_name,
                        arch_name,
                        compiler_name,
                    )
                    
                    # 检查组合是否被排除
                    if variant_key in exclude_combinations:
                        continue
                    
                    # 检查启用列表（如果设置了）
                    if enable_combinations and variant_key not in enable_combinations:
                        continue
                    
                    # 逐步应用配置
                    defines = dict(base_defines)
                    copts = list(base_copts)
                    
                    # 1. 应用平台配置
                    defines, copts = _apply_platform_config(
                        defines,
                        copts,
                        platform_config,
                    )
                    
                    # 2. 应用架构配置
                    defines, copts = _apply_arch_config(
                        defines,
                        copts,
                        arch_config,
                    )
                    
                    # 3. 应用编译器配置
                    defines, copts = _apply_compiler_config(
                        defines,
                        copts,
                        compiler_config,
                    )
                    
                    # 4. 应用构建变体配置
                    defines, copts, linkopt = _apply_variant_config(
                        defines,
                        copts,
                        variant_config,
                    )
                    
                    # 添加组合信息到定义
                    defines["VARIANT_PLATFORM"] = '"{}"'.format(platform_name)
                    defines["VARIANT_ARCH"] = '"{}"'.format(arch_name)
                    defines["VARIANT_COMPILER"] = '"{}"'.format(compiler_name)
                    defines["VARIANT_TYPE"] = '"{}"'.format(variant_type)
                    
                    # 生成配置头文件
                    header_file = _generate_config_header(
                        ctx,
                        variant_name,
                        defines,
                        ctx.attr.header_prefix,
                    )
                    generated_files.append(header_file)
                    
                    # 记录变体信息
                    variant_info[variant_name] = {
                        "platform": platform_name,
                        "arch": arch_name,
                        "compiler": compiler_name,
                        "variant_type": variant_type,
                        "defines": defines,
                        "copts": copts,
                        "linkopt": linkopt,
                        "header": header_file,
                    }
                    
                    variant_count += 1
    
    # 生成汇总文件
    summary_file = _generate_multi_dim_summary(ctx, variant_info)
    generated_files.append(summary_file)
    
    return [
        DefaultInfo(files = depset(generated_files)),
        struct(
            variants = variant_info,
            variant_count = variant_count,
            headers = [f for f in generated_files if f.extension == "h"],
        )
    ]

def _generate_config_header(ctx, variant_name, defines, prefix):
    """Generate configuration header for a variant."""
    
    guard_name = "{}_{}".format(prefix.upper(), variant_name.upper())
    
    lines = [
        "// Auto-generated configuration header for variant: {}".format(variant_name),
        "// DO NOT EDIT - generated by multi_dim_variant_suite",
        "",
        "#ifndef {}".format(guard_name),
        "#define {}".format(guard_name),
        "",
    ]
    
    # 添加定义
    for key in sorted(defines.keys()):
        value = defines[key]
        if value == "":
            lines.append("#define {}".format(key))
        elif value.lower() == "true":
            lines.append("#define {} 1".format(key))
        elif value.lower() == "false":
            lines.append("/* #undef {} */".format(key))
        else:
            lines.append("#define {} {}".format(key, value))
    
    lines.extend([
        "",
        "#endif // {}".format(guard_name),
    ])
    
    header_name = "{}_{}.h".format(prefix, variant_name)
    header_file = ctx.actions.declare_file(header_name)
    ctx.actions.write(header_file, "\n".join(lines))
    
    return header_file

def _generate_multi_dim_summary(ctx, variant_info):
    """Generate a summary of all generated variants."""
    
    # 分组统计
    platform_set = sets.make()
    arch_set = sets.make()
    compiler_set = sets.make()
    variant_type_set = sets.make()
    
    for name, info in variant_info.items():
        sets.insert(platform_set, info["platform"])
        sets.insert(arch_set, info["arch"])
        sets.insert(compiler_set, info["compiler"])
        sets.insert(variant_type_set, info["variant_type"])
    
    lines = [
        "// Auto-generated variant summary",
        "// Total Variants: {}".format(len(variant_info)),
        "",
        "// Platforms: {} ({})".format(
            len(sets.to_list(platform_set)),
            ", ".join(sorted(sets.to_list(platform_set))),
        ),
        "// Architectures: {} ({})".format(
            len(sets.to_list(arch_set)),
            ", ".join(sorted(sets.to_list(arch_set))),
        ),
        "// Compilers: {} ({})".format(
            len(sets.to_list(compiler_set)),
            ", ".join(sorted(sets.to_list(compiler_set))),
        ),
        "// Variant Types: {} ({})".format(
            len(sets.to_list(variant_type_set)),
            ", ".join(sorted(sets.to_list(variant_type_set))),
        ),
        "",
        "// Detailed Variant List:",
    ]
    
    for name in sorted(variant_info.keys()):
        info = variant_info[name]
        lines.extend([
            "//   - {}".format(name),
            "//     Platform: {}, Arch: {}, Compiler: {}, Type: {}".format(
                info["platform"],
                info["arch"],
                info["compiler"],
                info["variant_type"],
            ),
        ])
    
    summary_file = ctx.actions.declare_file("VARIANT_SUMMARY")
    ctx.actions.write(summary_file, "\n".join(lines))
    
    return summary_file

multi_dim_variant_suite = rule(
    implementation = _multi_dim_variant_suite_impl,
    attrs = {
        "platforms": attr.string_dict(
            doc = """Dictionary of platform configurations.
            Key: platform name (e.g., 'linux', 'macos', 'windows')
            Value: dict with keys like 'defines', 'copts'
            """,
            mandatory = True,
        ),
        "architectures": attr.string_dict(
            doc = """Dictionary of architecture configurations.
            Key: architecture name (e.g., 'x86_64', 'aarch64', 'arm')
            Value: dict with keys like 'defines', 'copts'
            """,
            mandatory = True,
        ),
        "compilers": attr.string_dict(
            doc = """Dictionary of compiler configurations.
            Key: compiler name (e.g., 'gcc', 'clang', 'msvc')
            Value: dict with keys like 'defines', 'copts'
            """,
            mandatory = True,
        ),
        "variants": attr.string_dict(
            doc = """Dictionary of build variant types.
            Key: variant type (e.g., 'debug', 'release')
            Value: dict with keys like 'defines', 'copts', 'linkopt'
            """,
            mandatory = True,
        ),
        "default_defines": attr.string_dict(
            doc = "Default defines applied to all variants",
            default = {},
        ),
        "default_copts": attr.string_list(
            doc = "Default compiler options applied to all variants",
            default = [],
        ),
        "exclude_combinations": attr.string_list(
            doc = """Combinations to exclude (format: 'platform_arch_compiler').
            Example: ['linux_arm_gcc', 'windows_x86_64_clang']
            """,
            default = [],
        ),
        "enable_combinations": attr.string_list(
            doc = """If non-empty, only these combinations are enabled.
            Example: ['linux_x86_64_gcc', 'macos_aarch64_clang']
            """,
            default = [],
        ),
        "header_prefix": attr.string(
            doc = "Prefix for generated header guard names",
            default = "CONFIG",
        ),
    },
    doc = """Generate a multi-dimensional suite of toolchain variants.
    
    This rule creates variants based on combinations of:
    - Platforms (linux, macos, windows, etc.)
    - Architectures (x86_64, aarch64, arm, etc.)
    - Compilers (gcc, clang, msvc, etc.)
    - Build variants (debug, release, asan, ubsan, etc.)
    
    Each combination generates:
    1. A unique variant name
    2. A configuration header file
    3. A summary of all variants
    """,
)

# ============================================================================
# Convenience Builder Functions
# ============================================================================

def build_platform_configs(platforms):
    """
    Build platform configuration dictionary.
    
    Args:
        platforms: List of dicts with platform configurations
        
    Returns:
        Dictionary keyed by platform name
    """
    result = {}
    for platform in platforms:
        name = platform.pop("name")
        result[name] = platform
    return result

def build_arch_configs(architectures):
    """Build architecture configuration dictionary."""
    result = {}
    for arch in architectures:
        name = arch.pop("name")
        result[name] = arch
    return result

def build_compiler_configs(compilers):
    """Build compiler configuration dictionary."""
    result = {}
    for compiler in compilers:
        name = compiler.pop("name")
        result[name] = compiler
    return result

def build_variant_configs(variants):
    """Build build variant configuration dictionary."""
    result = {}
    for variant in variants:
        name = variant.pop("name")
        result[name] = variant
    return result

# ============================================================================
# Preset Configurations
# ============================================================================

# 通用平台配置
COMMON_PLATFORMS = {
    "linux": {
        "defines": {
            "PLATFORM_LINUX": "1",
            "OS_NAME": '"linux"',
        },
        "copts": [],
    },
    "macos": {
        "defines": {
            "PLATFORM_MACOS": "1",
            "OS_NAME": '"macos"',
        },
        "copts": [],
    },
    "windows": {
        "defines": {
            "PLATFORM_WINDOWS": "1",
            "OS_NAME": '"windows"',
        },
        "copts": [],
    },
    "android": {
        "defines": {
            "PLATFORM_ANDROID": "1",
            "OS_NAME": '"android"',
        },
        "copts": [],
    },
    "ios": {
        "defines": {
            "PLATFORM_IOS": "1",
            "OS_NAME": '"ios"',
        },
        "copts": [],
    },
}

# 通用架构配置
COMMON_ARCHITECTURES = {
    "x86_64": {
        "defines": {
            "ARCH_X86_64": "1",
            "ARCH_NAME": '"x86_64"',
            "ARCH_BITS": "64",
        },
        "copts": ["-m64"],
    },
    "x86": {
        "defines": {
            "ARCH_X86": "1",
            "ARCH_NAME": '"x86"',
            "ARCH_BITS": "32",
        },
        "copts": ["-m32"],
    },
    "aarch64": {
        "defines": {
            "ARCH_AARCH64": "1",
            "ARCH_NAME": '"aarch64"',
            "ARCH_BITS": "64",
        },
        "copts": ["-march=armv8-a"],
    },
    "arm": {
        "defines": {
            "ARCH_ARM": "1",
            "ARCH_NAME": '"arm"',
            "ARCH_BITS": "32",
        },
        "copts": ["-march=armv7-a"],
    },
    "riscv64": {
        "defines": {
            "ARCH_RISCV64": "1",
            "ARCH_NAME": '"riscv64"',
            "ARCH_BITS": "64",
        },
        "copts": [],
    },
}

# 通用编译器配置
COMMON_COMPILERS = {
    "gcc": {
        "defines": {
            "COMPILER_GCC": "1",
            "COMPILER_NAME": '"gcc"',
        },
        "copts": ["-Wall", "-Wextra"],
    },
    "clang": {
        "defines": {
            "COMPILER_CLANG": "1",
            "COMPILER_NAME": '"clang"',
        },
        "copts": ["-Wall", "-Wextra"],
    },
    "msvc": {
        "defines": {
            "COMPILER_MSVC": "1",
            "COMPILER_NAME": '"msvc"',
        },
        "copts": ["/W4"],
    },
}

# 通用构建变体配置
COMMON_VARIANTS = {
    "debug": {
        "defines": {
            "DEBUG": "1",
            "NDEBUG": "false",
            "LOG_LEVEL": "3",
        },
        "copts": ["-O0", "-g"],
        "linkopt": ["-g"],
    },
    "release": {
        "defines": {
            "NDEBUG": "1",
            "LOG_LEVEL": "1",
        },
        "copts": ["-O3", "-ffast-math"],
        "linkopt": ["-s"],
    },
    "asan": {
        "defines": {
            "SANITIZER": '"asan"',
            "DEBUG": "1",
        },
        "copts": ["-O1", "-g", "-fsanitize=address"],
        "linkopt": ["-fsanitize=address"],
    },
    "ubsan": {
        "defines": {
            "SANITIZER": '"ubsan"',
            "DEBUG": "1",
        },
        "copts": ["-O1", "-g", "-fsanitize=undefined"],
        "linkopt": ["-fsanitize=undefined"],
    },
}

# ============================================================================
# Helper Functions for Creating Variants
# ============================================================================

def create_variant_suite(
    name,
    platforms = None,
    architectures = None,
    compilers = None,
    variants = None,
    exclude_combinations = None,
    enable_combinations = None,
    default_defines = None,
    default_copts = None,
    **kwargs):
    """
    Convenience function to create a multi-dimensional variant suite.
    
    Args:
        name: Name of the rule
        platforms: Dict or list of platform configs (uses COMMON_PLATFORMS if None)
        architectures: Dict or list of arch configs (uses COMMON_ARCHITECTURES if None)
        compilers: Dict or list of compiler configs (uses COMMON_COMPILERS if None)
        variants: Dict or list of variant configs (uses COMMON_VARIANTS if None)
        exclude_combinations: List of combinations to exclude
        enable_combinations: List of combinations to enable
        default_defines: Default defines for all variants
        default_copts: Default copts for all variants
        **kwargs: Additional arguments
    """
    
    # Use defaults if not provided
    if platforms is None:
        platforms = COMMON_PLATFORMS
    elif isinstance(platforms, list):
        platforms = build_platform_configs(platforms)
    
    if architectures is None:
        architectures = COMMON_ARCHITECTURES
    elif isinstance(architectures, list):
        architectures = build_arch_configs(architectures)
    
    if compilers is None:
        compilers = COMMON_COMPILERS
    elif isinstance(compilers, list):
        compilers = build_compiler_configs(compilers)
    
    if variants is None:
        variants = COMMON_VARIANTS
    elif isinstance(variants, list):
        variants = build_variant_configs(variants)
    
    if exclude_combinations is None:
        exclude_combinations = []
    
    if enable_combinations is None:
        enable_combinations = []
    
    if default_defines is None:
        default_defines = {}
    
    if default_copts is None:
        default_copts = []
    
    multi_dim_variant_suite(
        name = name,
        platforms = platforms,
        architectures = architectures,
        compilers = compilers,
        variants = variants,
        exclude_combinations = exclude_combinations,
        enable_combinations = enable_combinations,
        default_defines = default_defines,
        default_copts = default_copts,
        **kwargs
    )