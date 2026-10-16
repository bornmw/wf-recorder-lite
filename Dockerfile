# Toolchain image for wf-recorder-lite (gtkmm-4.0 C++ desktop binary).
#
# Same shape as cardea: the image only carries the build toolchain;
# `make binary` runs an ephemeral container with the project volume
# mounted at /app and the binary lands in ./dist (see Makefile).
#
# ubuntu:24.04 (noble) ships gtkmm-4.0 4.10.x / GTK 4.10, matching the native
# CI runner and providing the GTK 4.10 APIs the code uses (Gtk::FileDialog,
# Gtk::StyleProvider::add_provider_for_display). Older bases (e.g.
# debian:bookworm, gtkmm 4.8) lack those.
FROM ubuntu:noble

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        libgtkmm-4.0-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
