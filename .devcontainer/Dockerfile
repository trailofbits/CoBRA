FROM mcr.microsoft.com/devcontainers/cpp:ubuntu-24.04

ARG LLVM_VERSION=19

# Install LLVM from apt.llvm.org and project dependencies
RUN apt-get update && \
    export DEBIAN_FRONTEND=noninteractive && \
    apt-get install -y --no-install-recommends \
        software-properties-common \
        lsb-release \
        wget \
        gnupg \
        ninja-build \
        pipx \
        fd-find \
        libz3-dev && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
        | tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc && \
    echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} \
        llvm-${LLVM_VERSION}-dev \
        clang-format-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Set up alternatives so versioned tools are available without suffix
RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} 100 && \
    update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-${LLVM_VERSION} 100 && \
    update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-${LLVM_VERSION} 100 && \
    ln -s /usr/bin/fdfind /usr/bin/fd

# Install just (system-wide)
RUN curl -sSf https://just.systems/install.sh | bash -s -- --to /usr/local/bin

# Install codespell and prek as vscode user
USER vscode
RUN pipx install codespell && \
    pipx ensurepath && \
    curl -fsSL https://github.com/j178/prek/releases/latest/download/prek-installer.sh | sh
USER root

ENV PATH="/home/vscode/.local/bin:${PATH}" \
    CC=clang-${LLVM_VERSION} \
    CXX=clang++-${LLVM_VERSION}
