load("@rules_cc//cc:cc_toolchain.bzl", "cc_toolchain")
load("@rules_cc//cc/toolchain:args.bzl", "cc_args")
load("@rules_cc//cc/toolchains:tool.bzl", "cc_tool")

cc_tool(
    name = "clang",
    src = "@llvm_toolchain//:bin/clang",
    # Suppose clang needs libc to run.
    data = ["@llvm_toolchain//:lib/x86_64-linux-gnu/libc.so.6"]
    capabilities = ["@rules_cc//cc/toolchains/capabilities:supports_pic"],
)

cc_toolchain(
    name = "ours_toolchain",
    args = [],
    enabled_features = [
        "ours.kernel",
    ],
    known_features = [],
)