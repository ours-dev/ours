# Kernel products

This directory contains the officially supported _kernel images_ in subdirectory
**image**, plus the native kernel compile/link policy in BUILD.gn (configs consumed
by kernel toolchains; the switchset mechanism lives under //build/switch).

## How To Define A Kernel Image

In subdirectory **image**, each kernel image of name `$name` gets its own directory
of the same name and defines a [`kernel_image()`](/kernel/kernel_image.gni) —
mirroring Fuchsia's //zircon/kernel/image/$name.  Products select images by label;
see //products/<name>.gni and //build/product/product.gni.