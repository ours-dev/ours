# Shared Build Configurations For Ours And Ours-Based OS

This directory includes those configuration files for bazel building tools.

## Rules

1. Each language, architecture and compiler should has configurations themselves to indicate
the requirements of building environment. For instance, it should give out the ABI, compiler flags and etc.

2. Everything based on 