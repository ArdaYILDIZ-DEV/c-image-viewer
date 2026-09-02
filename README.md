# c-image-viewer

Minimalist dual-pane image viewer written in C with synchronized zoom and pan. Compare two images side-by-side at the same scale and offset, or view a single image full-window.

![C](https://img.shields.io/badge/C-11-blue)
![SDL2](https://img.shields.io/badge/SDL2-2.32-green)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

## Features

- **Single or dual pane** - `viewer image.jpg` or `viewer left.jpg right.jpg`
- **Synchronized zoom/pan** - both panes share the same view transform, ideal for before/after comparison
- **Free mode** - `s` to unsync, `Tab` to switch active pane, zoom/pan applies per-pane
- **Cursor-centered zoom** - mouse wheel keeps the point under the cursor fixed
- **Drag & drop** - drop a file onto the left or right half to replace that pane
- **Directory navigation** - `n`/`p` (or arrow keys / PageUp/PageDown) to browse images in the same folder, `ESC` to go to parent folder
- **Fullscreen** - `f` or `F11`
- **Info & help overlays** - `i` toggles bottom info bar, `h`/`?` toggles help panel, window title always shows file, zoom, and mode
- **Fit / 1:1** - `0`/`F` fit to window, `1` native 1:1

All overlays use an embedded 8x8 bitmap font (public domain). No `SDL_ttf` dependency.

## Quick Start

### Dependencies

- `gcc` and `make`
- `libsdl2-dev` (Debian/Ubuntu: `sudo apt install libsdl2-dev`, Arch: `sudo pacman -S sdl2`)

`stb_image.h` and `font8x8.h` are vendored, no extra download needed.

### Build

```bash
make
./viewer image.jpg
./viewer left.jpg right.jpg
```

### Install

```bash
sudo make install          # installs to /usr/local
make install PREFIX=$HOME/.local  # user-local
```

This installs `c-image-viewer` to `$PREFIX/bin` and the `.desktop` file to `$PREFIX/share/applications`. After install, select two images in Nautilus/Dolphin and choose **Open With -> c-image-viewer**.

```bash
sudo make uninstall
```

## Usage

```
Usage: c-image-viewer <image1> [image2]
```

Supported formats: JPEG, PNG, WebP, BMP, PPM/PGM/PBM, TIFF, GIF, PSD, TGA, HDR via `stb_image`.

## Key Bindings

| Key | Action |
|-----|--------|
| Mouse wheel | Zoom (cursor-centered) |
| Left drag | Pan |
| `0` / `F` | Fit to window |
| `1` | 100% (1:1, centered) |
| `+` / `-` | Zoom in/out (center) |
| `f` / `F11` | Toggle fullscreen |
| `i` | Toggle info bar |
| `h` / `?` | Toggle help |
| `s` | Toggle sync (SYNC / FREE) |
| `Tab` | Switch active pane (FREE mode) |
| `n` / `Right` / `PgDn` | Next image in folder |
| `p` / `Left` / `PgUp` | Previous image |
| `ESC` | Close help / exit fullscreen / go to parent folder |
| `q` | Quit |
| Drag & drop | Drop file onto left/right half to replace |

In **SYNC** mode, zoom and pan apply to both panes identically. In **FREE** mode, only the active pane (highlighted with a blue border) moves.

## Project Structure

```
.
├── main.c                 # Event loop, rendering, navigation (documented in English)
├── font8x8.h              # Public domain 8x8 bitmap font
├── stb_image.h            # Public domain image decoder
├── c-image-viewer.desktop # Desktop entry for file manager integration
├── Makefile               # Build, install, uninstall
├── LICENSE                # MIT
└── README.md
```

## Troubleshooting

- **SDL_Init failed** - ensure `DISPLAY`/`WAYLAND_DISPLAY` is set and SDL2 runtime is installed (`libsdl2-2.0-0`).
- **Blurry text** - the bitmap font is pixel-perfect at 1x; HiDPI scaling is handled by SDL2.
- **No images in folder** - `n`/`p` requires at least one more supported image in the same directory as the current file.
- **Drag & drop not working** - ensure the window manager supports `Xdnd` (X11) or Wayland data device; the drop position determines the target pane.

## License

MIT, see [LICENSE](LICENSE).
