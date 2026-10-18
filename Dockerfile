# Toolchain image for wf-recorder-lite (gtkmm-4.0 C++ desktop binary).
#
# Same shape as cardea: the image only carries the build toolchain;
# `make binary` runs an ephemeral container with the project volume
# mounted at /app and the binary lands in ./dist (see Makefile).
#
# ubuntu:26.04 (resolute) ships gtkmm-4.0 4.20.x / GTK 4.20, matching the
# box's GTK 4.20 runtime exactly (GTK 4 minor versions are ABI compatible,
# so this also runs on older runtimes). It provides all the GTK 4.10+ APIs
# the code uses (Gtk::FileDialog,
# Gtk::StyleProvider::add_provider_for_display). Older bases (e.g.
# debian:bookworm, gtkmm 4.8) lack those.
FROM ubuntu:26.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        libgtkmm-4.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
