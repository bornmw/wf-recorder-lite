# Toolchain image for wf-recorder-lite (gtkmm-4.0 C++ desktop binary).
#
# Same shape as cardea: the image only carries the build toolchain;
# `make binary` runs an ephemeral container with the project volume
# mounted at /app and the binary lands in ./dist (see Makefile).
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        libgtkmm-4.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
