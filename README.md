# c-image-viewer

<p align="center">
  <strong>Dual-pane image viewer in C11 and SDL2 with synchronized viewport transforms and in-window tree navigation.</strong>
</p>

<p align="center">
  <a href="https://en.wikipedia.org/wiki/C11_(C_standard_revision)"><img src="https://img.shields.io/badge/standard-C11-blue.svg?style=flat-square" alt="C11"></a>
  <a href="https://www.libsdl.org/"><img src="https://img.shields.io/badge/SDL2-2.32-green.svg?style=flat-square" alt="SDL2 2.32"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/tests-41%20passed-brightgreen.svg?style=flat-square" alt="Tests: 41 passed">
</p>

`c-image-viewer` is a lightweight dual-pane image viewer designed for pixel-level visual inspection and side-by-side comparison under synchronized affine transforms. It renders images via SDL2 GPU textures, provides an in-window directory tree browser with live substring filtering, and extracts JPEG EXIF metadata without external image or font libraries.

## Quick start

### Install dependencies

Debian / Ubuntu:
```bash
sudo apt update && sudo apt install -y build-essential libsdl2-dev wl-clipboard xclip
```

Arch Linux:
```bash
sudo pacman -S --needed base-devel sdl2 wl-clipboard xclip
```

Fedora:
```bash
sudo dnf install -y gcc make SDL2-devel wl-clipboard xclip
```

### Build and run

```bash
git clone https://github.com/ArdaYILDIZ-DEV/c-image-viewer.git
cd c-image-viewer
make
./viewer path/to/left.jpg path/to/right.png
```

## Features

- **Synchronized dual-pane inspection**: Side-by-side display of two images split at `W/2` with independent viewport clipping rectangles (`SDL_RenderSetClipRect`) preventing visual bleeding across the central divider.
- **Cursor-centered zoom (0.05x to 32x)**: Multiplicative zooming with image-space pan compensation that keeps the world point under the mouse cursor stationary.
- **In-window tree browser**: Overlay directory browser (`ESC`) with inline expand/collapse for subdirectories, parent traversal, and live case-insensitive substring search filtering.
- **Process-isolated clipboard**: Native copy and paste using `fork` and `execvp` with direct pipes to `wl-copy`/`wl-paste` (Wayland) or `xclip`/`xsel` (X11) without invoking `/bin/sh`. Temporary files are created with `0600` permissions via `mkstemps` and immediately unlinked.
- **Robust EXIF parser**: Embedded JPEG APP1 and TIFF metadata parser with circular IFD pointer detection, byte-offset boundary validation, zero-denominator checks, and ASCII sanitization.
- **Batched 8x8 font rendering**: Monospaced bitmap font engine that coalesces contiguous glyph pixels into horizontal spans, reducing SDL draw calls by over 99% (>99.8% on 80-character strings) compared to point-by-point drawing.

## Requirements

- **Compiler**: GCC >= 11 or Clang >= 13 (C11 support required)
- **Build tool**: GNU Make or POSIX-compliant `make`
- **Graphics backend**: SDL2 development libraries (`libsdl2-dev` >= 2.0.18)
- **C runtime**: Standard C library with POSIX.1-2008 extensions and math library (`-lm`)
- **Clipboard utilities (optional runtime)**:
  - Wayland: `wl-clipboard` (`wl-copy`, `wl-paste`)
  - X11: `xclip` or `xsel`

Embedded single-header libraries (`external/stb_image.h`, `external/stb_image_write.h`, `external/font8x8.h`) are vendored in the tree and require no separate package installation.

## Building from source and installation

The build system follows the Pitchfork layout standard and uses an out-of-source directory (`build/`):

```bash
# Build the main viewer binary (build/viewer) and create root symlink (./viewer)
make

# Run the test suite with AddressSanitizer and UndefinedBehaviorSanitizer enabled
make test

# Remove all object files, binaries, and temporary artifacts
make clean

# Install binary and desktop file to system path (default: /usr/local)
sudo make install

# Install to custom directory prefix (e.g. ~/.local)
make install PREFIX=$HOME/.local

# Remove installed binary and desktop entry
sudo make uninstall PREFIX=/usr/local
```

### Makefile targets

| Target | Description |
| :--- | :--- |
| `all` (default) | Compiles `build/viewer` with `-O2 -Wall -Wextra -Wpedantic -std=c11` and updates `./viewer` symlink |
| `test` | Compiles and executes `build/test_runner` under ASan and UBSan (`-fsanitize=address,undefined -g`) |
| `clean` | Deletes `build/` directory and symlinks (`viewer`, `test_runner`) |
| `install` | Installs `c-image-viewer` to `$(BINDIR)` and desktop entry to `$(DESKTOPDIR)` |
| `uninstall` | Deletes `c-image-viewer` and `c-image-viewer.desktop` from target paths |

## Usage

### Command line syntax

Single-image mode (full-window view):
```bash
./viewer /path/to/image.jpg
```

Dual-pane comparison mode (synchronized split view):
```bash
./viewer /path/to/reference.png /path/to/distorted.png
```

Paths may be relative or absolute. All arguments are validated using `stat()` to ensure they reference regular readable files (`S_ISREG`) with valid image extensions before initialization.

### Drag and drop

Files can be dragged and dropped directly onto the viewer window:
- Dropping an image onto the left half of the window (`x < W/2`) replaces the image in pane 0.
- Dropping an image onto the right half of the window (`x >= W/2`) replaces the image in pane 1.
- In single-image mode, dropping onto either side replaces the current image.

### Supported image formats

Decoding is handled by vendored `stb_image.h`:
- JPEG / JPG (`.jpg`, `.jpeg`): Baseline and progressive, EXIF orientation and tags
- PNG (`.png`): 1-bit through 16-bit channel depth
- WebP (`.webp`): Lossless and lossy WebP
- BMP (`.bmp`): Non-runlength and runlength encoded
- Netpbm (`.ppm`, `.pgm`, `.pbm`): Binary and ASCII formats
- TIFF (`.tiff`, `.tif`): Baseline TIFF images
- GIF (`.gif`): Static images and first frame of animated files
- Radiance HDR (`.hdr`): High dynamic range RGBE format
- Adobe Photoshop (`.psd`): Composited view
- Truevision TGA (`.tga`): Uncompressed and RLE compressed

## Keybindings

### Viewer controls

| Key / Input | Action |
| :--- | :--- |
| `Mouse Wheel` | Multiplicative zoom centered on mouse cursor position |
| `Left Mouse Drag` | Pan active viewport in image space |
| `0` or `Ctrl+F` | Fit image(s) to pane dimensions preserving aspect ratio |
| `1` or `Keypad 1` | Reset zoom to 100% (1:1 pixel scale) |
| `+` / `=` / `Keypad +` | Zoom in centered on window center |
| `-` / `Keypad -` | Zoom out centered on window center |
| `f` / `F11` | Toggle fullscreen mode (restores previous window geometry) |
| `s` | Toggle synchronization mode (`SYNC` vs `FREE`) |
| `Tab` | Switch active pane when in `FREE` mode |
| `n` / `Right` / `Page Down` | Navigate to next image in directory (skips corrupt files) |
| `p` / `Left` / `Page Up` | Navigate to previous image in directory |
| `i` | Toggle bottom information status bar |
| `e` | Toggle right-hand EXIF metadata overlay panel |
| `h` / `?` | Toggle keyboard shortcuts help dialog |
| `Ctrl+C` | Copy active pane image to system clipboard as PNG |
| `Ctrl+V` | Paste image from system clipboard into active pane |
| `Ctrl+F` | Open directory tree browser with search filter active |
| `ESC` | Dismiss dialogs / exit fullscreen / toggle directory browser |
| `q` | Terminate and quit viewer |
| `Drag & Drop` | Drop image onto left or right half to replace pane |

### Directory browser controls

The directory browser is toggled by pressing `ESC` while in windowed view.

| Key / Input | Action |
| :--- | :--- |
| `Up` / `k` | Move selection cursor to previous item |
| `Down` / `j` | Move selection cursor to next item |
| `Page Up` / `Page Down` | Scroll visible tree by page |
| `Home` / `End` | Jump to first / last matching directory entry |
| `Right` | Expand selected directory inline |
| `Left` | Collapse selected directory; jump to parent if already collapsed |
| `Backspace` | Delete last search filter character; collapses folder if query is empty |
| `Return` / `Keypad Enter` | Expand/collapse directory or load selected image into active pane |
| `Space` | Toggle directory expansion |
| `Ctrl+F` | Clear existing search query and focus search filter |
| `Printable ASCII` (32..126) | Append character to live search filter query |
| `Mouse Click` | Select row under mouse cursor |
| `Mouse Double-Click` | Expand/collapse folder or load image file |
| `Mouse Wheel` | Scroll directory listing |
| `Click Outside` / `ESC` | Clear search query if active; dismiss browser overlay |

## Interface behavior and transform math

### Cursor-centered zoom compensation

When zooming by multiplicative factor $s$ around cursor coordinate $(m_x, m_y)$, the world coordinate under the cursor is maintained stationary. The pan offset is tracked in image space and updated via:

$$\text{pan}_{\text{new}} = \text{pan}_{\text{old}} + (m_x - c_x) \cdot \left(\frac{1}{z_{\text{next}}} - \frac{1}{z_{\text{old}}}\right)$$

where:
- $(c_x, c_y)$ is the center coordinate of the pane in window pixels.
- $z_{\text{old}}$ is the zoom factor prior to modification, clamped to $[0.05, 32.0]$.
- $z_{\text{next}} = \text{clamp}(z_{\text{old}} \cdot s, 0.05, 32.0)$.

### Image-space panning

Mouse displacement deltas $(\Delta x, \Delta y)$ in screen pixels are converted to image-space displacements:

$$\text{pan}_{x,\text{new}} = \text{pan}_{x,\text{old}} + \frac{\Delta x}{z}, \quad \text{pan}_{y,\text{new}} = \text{pan}_{y,\text{old}} + \frac{\Delta y}{z}$$

This ensures pan speed across image features remains physically constant regardless of zoom magnification.

### Dual-pane layout and clipping rects

In dual-pane mode (`g_count == 2`):
- The split divider is positioned at integer coordinate $\text{mid} = \lfloor W / 2 \rfloor$.
- Pane 0 viewport rectangle: `{x: 0, y: 0, w: mid, h: H}`.
- Pane 1 viewport rectangle: `{x: mid, y: 0, w: W - mid, h: H}`.
- A 1-pixel vertical divider line is rendered at $x = \text{mid}$ with color RGB(60, 60, 60).
- Hardware clipping is enforced for each pane render via `SDL_RenderSetClipRect()`.
- Viewport culling calculates the projected image destination rectangle; if the bounding box does not intersect the clipping rectangle, the draw call (`SDL_RenderCopyF`) is discarded before GPU dispatch.

### SYNC versus FREE mode

- In `SYNC` mode (default), both panes share `g_zoom`, `g_pan_x`, and `g_pan_y`. Zooming or panning on either side updates both panes identically.
- In `FREE` mode (toggled via `s`), each pane stores independent coordinates (`g_free_zoom[i]`, `g_free_pan_x[i]`, `g_free_pan_y[i]`). Transformations only apply to the active pane (`g_active`). The active pane is highlighted with an inset border line (RGB 100, 160, 255) and can be toggled using `Tab`. Switching back to `SYNC` synchronizes both panes to the active pane's current transform.

## Project structure

The repository follows the Pitchfork layout standard:

```
.
├── assets/
│   └── c-image-viewer.desktop       # XDG desktop entry specification file
├── external/
│   ├── font8x8.h                    # Public domain 8x8 monochrome bitmap font
│   ├── stb_image.h                  # Image decoding library (JPEG, PNG, WebP, etc.)
│   └── stb_image_write.h            # PNG encoding library for clipboard export
├── include/
│   ├── browser.h                    # In-window directory tree browser interface
│   ├── clipboard.h                  # Process-isolated clipboard operations interface
│   ├── exif.h                       # JPEG APP1 and TIFF metadata parser interface
│   ├── text.h                       # Batched bitmap font rendering interface
│   └── viewer.h                     # Core viewer state, view transforms, and rendering
├── src/
│   ├── browser.c                    # Directory scanning, inline expand/collapse, filter
│   ├── clipboard.c                  # Safe fork/exec tool execution with anonymous pipes
│   ├── exif.c                       # TIFF IFD parser, tag extraction, cycle detection
│   ├── main.c                       # SDL initialization, event loop, and key dispatch
│   ├── text.c                       # Horizontal span merging and batch draw submission
│   └── viewer.c                     # Texture management, zoom/pan math, dual-pane rendering
├── tests/
│   ├── test_browser.c               # Browser tree, symlink safety, and filtering tests
│   ├── test_clipboard.c             # Process isolation and command injection tests
│   ├── test_common.h                # Minimal test runner macros and assertions
│   ├── test_exif.c                  # Corrupted header, circular IFD, and tag tests
│   ├── test_main.c                  # Test suite runner entry point
│   ├── test_text.c                  # Span equivalence and draw call reduction benchmarks
│   └── test_viewer.c                # Viewport culling, path validation, and math tests
├── Makefile                         # Out-of-source compilation and test targets
└── README.md                        # Documentation
```

## Testing and verification

### Automated test runner

The test suite tests memory safety, bounds checks, circular IFD detection, process execution security, directory navigation, and draw call reduction:

```bash
make test
```

This compiles the test runner with `-fsanitize=address,undefined -g` and executes all unit tests under `SDL_VIDEODRIVER=dummy`.

Test coverage includes:
- EXIF parsing: Synthetic Little-Endian/Big-Endian headers, circular IFD reference guards, corrupt APP1 markers, zero-denominator rational checks, and ASCII sanitization.
- Bitmap text engine: Span equivalence tests, draw call reduction verification (>99% reduction over individual pixel rects).
- Clipboard security: Argument vector validation, absence of shell metacharacter expansion, anonymous UNIX pipe isolation, and safe temporary file lifecycle (`0600` permissions).
- Viewport and navigation: Dual-pane boundary cases, degenerate window sizes (0x0, negative dimensions), corrupted file skipping during directory traversal, and offscreen culling.
- Directory browser: Inline expand/collapse logic, depth limit enforcement, symlink loop handling, and incremental filter performance.

### Headless smoke test

To verify startup, window initialization, texture creation, and clean shutdown on continuous integration systems without a display server:

```bash
SDL_VIDEODRIVER=dummy timeout 2 ./build/viewer /path/to/test.png
```

The process should run normally and exit with code `124` when the timeout terminates the event loop.

## Troubleshooting

### 'SDL_Init: No available video device'

**Diagnostic command:**
```bash
echo "DISPLAY=$DISPLAY WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
```

**Cause:** The application cannot connect to an active X11 or Wayland display server.

**Resolution:**
- For interactive desktop sessions, ensure your display manager or compositor is running and the `DISPLAY` or `WAYLAND_DISPLAY` environment variable is set.
- For headless environments, automated testing, or SSH sessions without X forwarding, export `SDL_VIDEODRIVER=dummy` before executing the viewer.

### 'Unsupported file format'

**Diagnostic command:**
```bash
file --mime-type /path/to/target_file
```

**Cause:** The target file does not have a supported extension or contains corrupted headers that `stb_image` cannot decode.

**Resolution:**
- Verify the file is one of the supported formats: `.jpg`, `.jpeg`, `.png`, `.webp`, `.bmp`, `.ppm`, `.pgm`, `.pbm`, `.tiff`, `.tif`, `.gif`, `.hdr`, `.psd`, `.tga`.
- Check file permissions and confirm the target is a regular file (`test -f target_file`).

### 'Command not found: wl-copy / xclip'

**Diagnostic command:**
```bash
which wl-copy xclip xsel 2>&1
```

**Cause:** System clipboard integration could not locate an external clipboard utility in `$PATH`. When absent, image copying falls back to copying the plain text file path.

**Resolution:**
- On Wayland: Install `wl-clipboard` (`sudo apt install wl-clipboard` or `sudo pacman -S wl-clipboard`).
- On X11: Install `xclip` (`sudo apt install xclip` or `sudo pacman -S xclip`) or `xsel`.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE) for the full text.
