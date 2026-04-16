FROM ubuntu:26.04

WORKDIR /workspace

# First thing, configures all software sources.

## Download basic development tools 
RUN apt-get update
RUN apt-get install -y \
    git \
    vim \
    curl \
    wget
RUN rm -rf /var/lib/apt/lists/*

# Installs latest stable llvm packages.
RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

# 避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive
 
# 设置时区（可选）
RUN ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

# 创建非 root 用户（推荐）
RUN useradd -m -s /bin/bash developer
USER developer

# 默认命令
CMD ["/bin/bash"]