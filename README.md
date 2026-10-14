# wl-recorder-ui

Small Qt6 control panel for **wf-recorder** on Wayland: select a screen
area, give it a file name, hit Start/Stop.

## Features

- **Select Area** — fullscreen transparent overlay; drag a rectangle
  (Esc cancels). The rectangle is converted to physical pixels for
  `wf-recorder -g` (correct under fractional scaling, e.g. scale 2.0).
- **Format** — choose the output:
  - *MP4 — H.264 (default)*: small, lossy.
  - *MP4 — lossless (pixel-perfect)*: `libx264 -p qp=0`, bit-exact — the
    best source for converting to a GIF. Larger files.
  - *GIF (direct)*: records straight to an animated GIF (`-c gif`). Note a
    GIF is always limited to 256 colours/frame, so it can't be fully
    pixel-perfect.
  - The file extension is kept in sync with the chosen format.
- **Save As… / Browse…** — standard file picker; the full path is always
  shown (in the field, in a hover tooltip, and in a persistent label below
  it). Default location: `~/Videos` if it exists, else `~`.
- **Start** — spawns `wf-recorder` (with `-g <geom>` when an area is
  selected, otherwise the full output) writing to the given file.
- **Stop** — sends SIGINT (wf-recorder's clean-stop signal); the file
  is finalized.
- Live status: elapsed time + file size; "saved …" on finish.

## Requirements

- Wayland compositor with `wlr-screencopy` support (wayfire ✓)
- `wf-recorder` in `PATH`
- Qt6 Widgets runtime

## Build

```
make binary       # Docker toolchain image + ephemeral build -> dist/wl-recorder-ui
make build-local  # direct build on a box with Qt6/cmake/ninja -> build/wl-recorder-ui
make run          # run the dist binary
```

The Docker build (debian bookworm, Qt 6.6) produces a binary that runs
against the box's Qt 6.11 runtime (Qt6 minor versions are ABI
compatible).
