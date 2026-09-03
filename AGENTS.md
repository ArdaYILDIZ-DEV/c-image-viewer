# C Project Architecture & Guidelines (Pitchfork Layout)

This project strictly follows the **Pitchfork Layout** standard and **Out-of-Source Build** principles. All modifications and code generation MUST adhere to the following rules:

## 1. Directory Structure Standards
* **`src/`**: All C source files (`*.c`) MUST be placed here.
* **`include/`**: All public/project header files (`*.h`) MUST be placed here (`include/icon_data.h`: embedded 64x64 PNG for window icon and self-extracting install).
* **`external/`**: Third-party or single-header libraries (e.g., `stb_image.h`, `font8x8.h`) MUST reside here. Do NOT place third-party headers in `src/` or `include/`.
* **`tests/`**: Unit tests and test helpers MUST remain isolated in this folder.
* **`assets/`**: Non-code application files (e.g., `.desktop`, icons) MUST go here.

## 2. Build Hygiene & File Management
* **Never write build artifacts into source folders.** All object files (`*.o`), static/shared libraries, and final executables MUST be output to a temporary `build/` directory.
* Always update the `Makefile` or `CMakeLists.txt` when adding new files or changing include paths. Ensure `-Iinclude -Iexternal` flags are passed during compilation.
* Never commit compiled binaries, `.o` files, or temporary test runners to Git.

## 3. Code & Include Rules
* Use relative include quotes for internal headers where appropriate, or system-style includes matching the set include paths:
  * Local headers: `#include "browser.h"` (when included via `-Iinclude`)
  * External vendors: `#include "stb_image.h"` (when included via `-Iexternal`)
* Maintain separation of interface (`.h`) and implementation (`.c`).

## Stack

- C11, `gcc -O2 -Wall -Wextra -Wpedantic -std=c11`, `make`
- SDL2 2.32 (`libsdl2-dev`), no SDL_ttf/SDL_image dependency
- Vendored headers in `external/`: `stb_image.h`, `stb_image_write.h` (public domain), `font8x8.h` (8x8 bitmap font)
- Linux X11/Wayland; `xclip`/`wl-copy` optional for clipboard

## Commands

```bash
make                          # builds build/viewer (and symlinks ./viewer)
make clean && make            # clean rebuild
make test                     # runs test suite with ASan/UBSan
./build/viewer --dump-icon <dest.png>  # dumps embedded 64x64 PNG icon
SDL_VIDEODRIVER=dummy timeout 2 ./build/viewer /tmp/smoke.png  # headless smoke test (expect exit 124)
./viewer a.jpg b.jpg          # dual-pane, synchronized
sudo make install PREFIX=/usr/local  # installs binary, desktop file, and extracts icon to hicolor
```

Single test / direct build: `gcc -O2 -Wall -Wextra -Wpedantic -std=c11 -Iinclude -Iexternal src/*.c -o build/viewer -lSDL2 -lm`

## Code style

- Comments in English, purpose-driven, directly above the function/struct they describe. No Turkish, no emojis.
- Every exported function has a doc block with purpose, params, and algorithm note.

```c
// CORRECT: explains why pan is in image-space and how zoom compensates
/**
 * Apply a zoom factor centered on the cursor. Keeps the world point
 * under the cursor stationary by: pan_new = pan_old + (cursor-center)*(1/next - 1/old)
 */
void viewer_do_zoom(float factor, int mx, int my) { ... }

// WRONG: trivial restatement
// This function does zoom
void do_zoom(float f) { ... }
```

- Keep `src/main.c` thin (~300 lines): SDL init and event dispatch only. Image/view logic lives in `src/viewer.c`, file tree in `src/browser.c`.

## Architecture

```
src/main.c        -> SDL init, --dump-icon handling, window icon, event loop, priority: browser overlay > viewer > global
src/viewer.c      -> Image (stb_image -> SDL_Texture), view state (zoom/pan/sync/free), title, fit, navigation (g_file_list), rendering
src/browser.c     -> Flat visible list with depth/expanded, expand/collapse by inserting/removing descendants, filter via substring
src/text.c        -> draw_char/draw_text over font8x8.h, integer scale only
src/exif.c        -> JPEG APP1/TIFF parser (Orientation, Make/Model, DateTime, ISO, etc.)
src/clipboard.c   -> xclip/wl-copy via temp PNG (stb_image_write), fallback to text path
```

- Globals owned by `src/viewer.c` (`g_img`, `g_zoom`, `g_file_list`, `g_current_dir`) are `extern` in `include/viewer.h` for `src/browser.c`/`src/main.c`. No circular includes.
- Only one TU defines `STB_IMAGE_IMPLEMENTATION` (`src/viewer.c`); `src/clipboard.c` defines `STB_IMAGE_WRITE_IMPLEMENTATION` only.
- include/icon_data.h: embedded 64x64 PNG for window icon and self-extracting install via --dump-icon.

## Boundaries

- Never edit vendored `external/stb_image.h`, `external/stb_image_write.h`, `external/font8x8.h` beyond bumping versions. Treat as read-only.
- include/icon_data.h contains pre-rendered embedded PNG icon bytes; do not hand-edit without keeping byte length in sync.
- Do not add `SDL_ttf`, `SDL_image`, or other heavy dependencies without explicit approval. Embedded bitmap font is intentional.
- Do not `git commit` or `git push` without showing files, full Conventional Commits message, and exact command, then waiting for explicit user approval. Each push needs its own approval.
- Generated `build/` directory, binaries, and `*.o` are ignored; never commit them.

## Git workflow

- Branch: `master` (local only until user requests GitHub remote). No `main` alias needed.
- Commits: Conventional Commits 1.0.0 + Angular types, English, no AI trailers. Example: `feat(browser): add file browser overlay with tree navigation`
- Present per commit: staged files, full message, exact `git commit` command; wait for approval before running. Same for `git push`.
