<p align="center">
  <b>c-image-viewer — minimalist dual-pane image viewer with synchronized zoom</b><br>
  <a href="https://github.com/"><img src="https://img.shields.io/badge/C-11-blue?style=flat-square"></a>
  <a href="https://www.libsdl.org/"><img src="https://img.shields.io/badge/SDL2-2.32-green?style=flat-square"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-lightgrey?style=flat-square"></a>
</p>

Single-window viewer that shows one image full-window or two images side-by-side with a shared view transform. Zoom and pan stay synchronized for direct visual comparison; free mode allows per-pane control. Includes an in-window file browser with tree expand/collapse.

## Quick start

```bash
sudo apt install libsdl2-dev        # Debian/Ubuntu, Arch: pacman -S sdl2
git clone <repo> && cd c-image-viewer
make
./viewer image.jpg
./viewer left.jpg right.jpg
```

## Features

- Single pane full-window and dual pane side-by-side with 1px divider
- Synchronized zoom (cursor-centered, 0.05x–32x, bilinear) and pan (drag)
- Free mode: `s` toggles sync, `Tab` switches active pane (blue border highlights active)
- In-window file browser on `ESC`: tree view, expand/collapse, file/folder selection, direct load into active pane
- Drag and drop: drop file onto left/right half to replace that pane
- Directory navigation: `n`/`p` (or arrow/PageUp/PageDown) browses images in the same folder, wraps around
- Fullscreen (`f`/`F11`), fit (`0`/`F`), 1:1 (`1`), info bar (`i`), help (`h`/`?`)
- Window title always shows basename, dimensions, zoom, sync state, and navigation index
- Embedded 8x8 bitmap font; no `SDL_ttf` dependency
- `.desktop` integration for file manager “Open With” on multiple files

## Requirements

- `gcc` >= 11, `make`
- `libsdl2-dev` >= 2.0.18 (runtime `libsdl2-2.0-0` is not sufficient)
- Linux with X11 or Wayland; macOS via SDL2 also builds

`stb_image.h` (image decoding) and `font8x8.h` (glyphs) are vendored.

## Building from source

```bash
make                 # builds ./viewer
make clean
sudo make install                    # PREFIX=/usr/local
make install PREFIX=$HOME/.local     # user-local
sudo make uninstall
```

`install` copies `viewer` to `$(PREFIX)/bin/c-image-viewer` and `c-image-viewer.desktop` to `$(PREFIX)/share/applications`. Run `update-desktop-database` if your desktop requires it.

## Usage

```
viewer <image1> [image2]
```

Supported extensions: `jpg/jpeg/png/webp/bmp/ppm/pgm/pbm/tiff/tif/gif/psd/tga/hdr` via `stb_image`. No flag parsing; extra arguments are ignored beyond the second file.

Select two files in Nautilus/Dolphin and choose **Open With → c-image-viewer** after `make install`.

## Keybindings

| Key | Action |
|-----|--------|
| Mouse wheel | Zoom, cursor-centered |
| Left drag | Pan |
| `0` / `F` | Fit to window (preserves aspect, does not upscale small images) |
| `1` | 100% (1:1, centered) |
| `+` / `-` | Zoom in/out around window center |
| `f` / `F11` | Toggle fullscreen (windowed geometry is saved and restored) |
| `i` | Toggle bottom info bar |
| `h` / `?` | Toggle help panel |
| `s` | Toggle sync (SYNC: shared zoom/pan, FREE: per-pane) |
| `Tab` | Switch active pane in FREE mode |
| `n` / `Right` / `PgDn` | Next image in current folder |
| `p` / `Left` / `PgUp` | Previous image |
| `ESC` | Close help / exit fullscreen / toggle file browser |
| `q` | Quit |
| Drag & drop | Drop file onto left/right half to replace that pane |

File browser (when open):

| Key | Action |
|-----|--------|
| `Up` / `Down` / `k` / `j` | Move selection |
| `Right` / `Enter` (on folder) | Expand folder inline |
| `Left` / `Backspace` | Collapse folder or go to parent directory |
| `Enter` (on image) | Load image into active pane, close browser, fit view |
| `Home` / `End` / `PgUp` / `PgDn` | Jump |
| Mouse click | Select row |
| Double-click | Expand/collapse folder or open image |
| Click outside panel / `ESC` | Close browser |
| Mouse wheel | Scroll list |

## Interface behavior

- Window is split at `W/2`. Each pane is clipped via `SDL_RenderSetClipRect` so images do not bleed across the divider. The divider is drawn at `W/2`.
- Zoom is uniform and multiplicative (`*1.1` / `*0.9` per wheel notch). Pan is stored in image-space pixels; screen delta is divided by zoom so drag speed is constant across scales. Cursor-centered zoom compensates pan by `(1/next - 1/old) * (cursor - center)`.
- In SYNC mode, `g_zoom`/`g_pan_*` drive both panes. Toggling to FREE copies the shared transform to both per-pane slots; toggling back uses the active pane as the new shared transform. Switching `Tab` only affects which per-pane slot receives input.
- Fit computes the largest uniform scale that fits the image(s) inside their pane(s) without upscaling small images. In FREE mode each pane fits independently.
- Info bar is a 22px semi-transparent strip at the bottom; help and browser are modal overlays with a dimmed background. The browser panel is 65% of window width (clamped 400px–`W-40px`) and centered.
- Directory scan for `n`/`p` filters to supported extensions, checks `S_ISREG`, sorts alphabetically, and tracks `g_file_index` for the current image’s basename.

## Project structure

```
c-image-viewer/
├── main.c                 # SDL init, event loop, dispatch to viewer/browser
├── viewer.c / viewer.h    # Image loading (stb_image), view transforms, rendering, navigation, title
├── browser.c / browser.h  # File browser tree, expand/collapse, selection, overlay rendering
├── text.c / text.h        # 8x8 bitmap text helpers (wraps font8x8.h)
├── font8x8.h              # Public domain 8x8 font (128 glyphs)
├── stb_image.h            # Public domain image decoder
├── c-image-viewer.desktop # Desktop entry (MimeType=image/*)
├── Makefile               # all/clean/install/uninstall, multi-file build
├── LICENSE                # MIT
└── README.md
```

## Development & testing

```bash
make                          # build
SDL_VIDEODRIVER=dummy timeout 2 ./viewer image.jpg        # headless smoke test, expect exit 124 (timeout)
./viewer left.jpg right.jpg   # manual: check sync zoom, browser ESC, drag & drop
```

No automated test suite yet. CI should at least run `make` and a dummy-driver smoke test.

## Troubleshooting

### `SDL_Init: No available video device`
No `DISPLAY`/`WAYLAND_DISPLAY` and no dummy driver. Run under X11/Wayland or use `SDL_VIDEODRIVER=dummy` for headless verification.

### `stbi_load failed '...' : can't fopen`
Path is wrong or file is not a supported image. Check `file <path>` and that the extension is in the supported list.

### `SDL_CreateWindow: ...`
Missing SDL2 runtime or no windowing system. Install `libsdl2-2.0-0` and ensure a display server is running.

### Drag & drop does nothing / `Unsupported file type (drop)`
Only image extensions are accepted. Directories and non-image files are rejected. Drop position determines target pane: `x < W/2` → left, otherwise right.

### `n`/`p` does nothing
Current folder contains no other supported images, or `g_file_index` is `-1` (file not found in scan). Check `ls` in `g_current_dir` and that files are regular files.

### Browser shows empty folder
Hidden files (dot prefix) are intentionally skipped. The folder may contain only unsupported types or subdirectories.

## License

MIT, see [LICENSE](LICENSE).
