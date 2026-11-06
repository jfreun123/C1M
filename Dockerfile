# Toolchain and tools for building and running every experiment in this repo.
#
# The base image is pinned by digest. The apt packages are whatever the Ubuntu
# 24.04 archive serves on the day the image is built; that release only takes
# bug-fix updates, so versions drift very little.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        clang \
        clang-format \
        clang-tidy \
        cmake \
        cppcheck \
        git \
        iproute2 \
        libclang-rt-18-dev \
        liburing-dev \
        llvm \
        lsof \
        ninja-build \
        procps \
        strace \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
