# Build System


## Background

项目起初使用CMake作为构建工具，只进行了简单的配置。当作者学习Google Fuchsia的GN构建系统时，决定该将构建系统迁移到Bazel去。但由于我对Bazel不熟悉并且没有设计构建系统的经验，所以需要你帮助设计一套构建系统。

内核项目本身相对复杂，会因不同的架构、目标运行环境以及某些特殊需求（如各种santizers，lto，code coverage等）最终产出不同的构建配置。

## Goals

使用Google Bazel + rules_cc为此OS内核项目设计一套支持多架构、多工具链以及多环境的C++构建系统。

## Requirements

1. 尽可能利用Bazel Repository中已有的Rules，避免重写；

1. 定义Toolchain Config

## References 

### Terms

多架构：指CPU架构，x86、arm以及riscv。

多工具链：由编译器，Target Triple等指出。

多环境：目前只有内核环境，注意并非指内核和用户环境。
