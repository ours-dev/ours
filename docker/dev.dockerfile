FROM ubuntu:26.04

WORKDIR /workspace

# First thing, configures all software sources.

## Download basic development tools
RUN apt-get update
RUN apt-get install -y \
    git \
    vim \
    curl \
    wget \
    python3 \
    xz-utils \
    ccache \
    util-linux
RUN rm -rf /var/lib/apt/lists/*

# python3   — every GN/ninja action script (pack_action.py, dist manifests)
#             is plain-stdlib python3; the base image ships none.
# xz-utils  — unpacking prebuilt toolchain tarballs if needed.
# ccache    — compiler cache; its default directory ~/.cache/ccache lives in
#             the dev-home named volume (see dev.compose), so caches survive
#             image rebuilds.
# util-linux — setpriv, used by the entrypoint to drop back to the
#             developer user after the root-level package restore.

# Installs latest stable llvm packages.
RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

# Snapshot the baseline package set.  The entrypoint diffs against it to
# maintain the persistent "user-installed packages" list and reinstalls it
# after image rebuilds.
RUN dpkg-query -W -f='${Package}\n' | sort -u > /etc/ours-dev-base-packages

# 避免交互式提示
ENV DEBIAN_FRONTEND=noninteractive

# 设置时区（可选）
RUN ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

# Root-level entrypoint: restore user packages (see docker/entrypoint.sh),
# then exec as the developer user.
COPY docker/entrypoint.sh /usr/local/bin/ours-dev-entrypoint
RUN chmod +x /usr/local/bin/ours-dev-entrypoint

# 创建非 root 用户（推荐；入口脚本会降权到该用户）
RUN useradd -m -s /bin/bash developer

ENTRYPOINT ["/usr/local/bin/ours-dev-entrypoint"]

# 默认命令
CMD ["/bin/bash"]
