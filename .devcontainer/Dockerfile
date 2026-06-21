FROM ubuntu:24.04

# 先创建非 root 用户，后续操作都用这个用户
RUN useradd -m -s /bin/bash developer

# 暴露端口
EXPOSE 11408

# 设置时区
RUN ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

# 更新包列表并安装基础工具
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        apt-transport-https \
        software-properties-common \
        ca-certificates \
        lsb-release \
        cmake \
        git \
        wget \
        gnupg \
        build-essential \
        zlib1g-dev \
        python3-venv \
        curl \
        vim

# 设置 PATH
RUN mkdir -p /home/developer/bin /home/developer/.local/bin
ENV PATH="/home/developer/bin:/home/developer/.local/bin:${PATH}"

# 安装 Bazel 8.6.0 + Buildifier（arm64）
# Bazel 8.6.0 是你已验证的版本；Buildifier 用于 BUILD.bazel 文件格式化
RUN curl -fSL -o /home/developer/bin/bazel \
        "https://github.com/bazelbuild/bazel/releases/download/8.6.0/bazel-8.6.0-linux-arm64" && \
    curl -fSL -o /home/developer/bin/buildifier \
        "https://github.com/bazelbuild/buildtools/releases/latest/download/buildifier-linux-arm64" && \
    chmod +x /home/developer/bin/bazel /home/developer/bin/buildifier && \
    chown -R developer:developer /home/developer/bin

# 验证安装
RUN /home/developer/bin/bazel --version && \
    /home/developer/bin/buildifier --version

# 安装 LLVM/Clang 22 工具链
# 提供 /usr/bin/clang-22、/usr/bin/clang++-22、/usr/bin/ld.lld-22 等
RUN wget https://apt.llvm.org/llvm.sh && \
    chmod +x llvm.sh && \
    ./llvm.sh 22

# 安装 GCC 13，提供 libstdc++ 头文件
# 工具链 cxx_builtin_include_directories 硬编码了 /usr/include/c++/13 路径
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        g++-13 \
        libstdc++-13-dev && \
    rm -rf /var/lib/apt/lists/*

# x86_64 交叉编译支持
# 通过 --platforms=//:x86_64-linux-clang 编译时需要 amd64 版 libstdc++
RUN dpkg --add-architecture amd64 && \
    apt-get update && \
    apt-get install -y --no-install-recommends libstdc++-13-dev:amd64 && \
    rm -rf /var/lib/apt/lists/*

# Rust 工具链（含 nightly）
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/home/developer/.cargo/bin:${PATH}"
RUN rustup toolchain install nightly

# Claude Code CLI
RUN curl -fsSL https://claude.ai/install.sh | bash

# 避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /home/developer/workspaces
RUN chown -R developer:developer /home/developer/workspaces

USER developer

CMD ["bash"]
