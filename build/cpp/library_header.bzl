load("@rules_cc//cc:defs.bzl", "cc_library")

def library_header(name, **args):
    # Create a cc_library for header-only code.
    includes = args.pop("includes", ["include"])
    deps = args.pop("deps", [])
    hdrs = args.pop("hdrs", [])

    cc_library(
        name = name,
        hdrs = hdrs,
        deps = deps,
        includes = includes,
        **args
    )
