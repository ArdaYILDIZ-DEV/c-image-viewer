# Project Guidelines

Follows the Pitchfork layout standard and out-of-source build principles.

## Directory layout

- `src/`: C source files (`*.c`)
- `include/`: Public project headers (`*.h`)
- `external/`: Vendored single-header libraries
- `tests/`: Unit tests and test helpers
- `assets/`: Non-code assets (desktop entries, icons)

## Build hygiene

- Build out-of-source: all artifacts (`*.o`, binaries) output to `build/`. Never write artifacts into `src/` or repository root.
- Update `Makefile` when adding files or changing include paths. Ensure `-Iinclude -Iexternal` flags are passed.
- Never commit compiled binaries, object files, or test runners.

## Code and includes

- Maintain strict separation of interface (`.h`) and implementation (`.c`).
- Include headers using `-Iinclude` and `-Iexternal`:
  - Project headers: `#include "browser.h"`
  - Vendored headers: `#include "stb_image.h"`

## Stack

- C11, `gcc -O2 -Wall -Wextra -Wpedantic -std=c11`, `make`
- SDL2 2.32 (`libsdl2-dev`), no SDL_ttf or SDL_image dependency
- Vendored headers in `external/`: `stb_image.h`, `stb_image_write.h`, `font8x8.h`
- Linux X11/Wayland; `xclip`/`wl-copy` optional for clipboard

## Commands

```bash
make                          # build build/viewer and symlink ./viewer
make clean && make            # clean rebuild
make test                     # run test suite with ASan/UBSan
./build/viewer --dump-icon <dest.png>  # dump embedded icon
SDL_VIDEODRIVER=dummy timeout 2 ./build/viewer /tmp/smoke.png  # headless smoke test (exit 124 expected)
./viewer a.jpg b.jpg          # dual-pane synchronized view
sudo make install PREFIX=/usr/local  # install binary, desktop file, and icon
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

- Keep `src/main.c` thin (~300 lines): SDL init and event dispatch only. Image and view logic lives in `src/viewer.c`, file tree in `src/browser.c`.

## Architecture

```
src/main.c        -> SDL init, window icon, --dump-icon, event loop (browser overlay > viewer > global)
src/viewer.c      -> Image loading, textures, viewport state (zoom/pan/sync), layout, rendering
src/browser.c     -> Flat visible directory tree, expand/collapse, substring filter
src/text.c        -> Monospaced bitmap text rendering via font8x8.h (integer scale)
src/exif.c        -> JPEG APP1/TIFF metadata parser
src/clipboard.c   -> Process pipe clipboard copy (wl-copy, xclip) via temp PNG
```

- Globals owned by `src/viewer.c` (`g_img`, `g_zoom`, `g_file_list`, `g_current_dir`) are `extern` in `include/viewer.h` for `src/browser.c` and `src/main.c`. No circular includes.
- `STB_IMAGE_IMPLEMENTATION` is defined only in `src/viewer.c`; `STB_IMAGE_WRITE_IMPLEMENTATION` is defined only in `src/clipboard.c`.
- `include/icon_data.h`: embedded PNG byte array for window icon and `--dump-icon` extraction.

## Boundaries

- Vendored headers in `external/` are read-only; never edit beyond bumping versions.
- Do not hand-edit `include/icon_data.h`; embedded PNG bytes must match byte length.
- Do not add `SDL_ttf`, `SDL_image`, or other heavy dependencies without explicit approval. Embedded bitmap font is intentional.
- Do not `git commit` or `git push` without showing staged files, full Conventional Commits message, and exact command, then waiting for explicit approval. Each push requires separate approval.
- Never commit build artifacts or binaries.

## Git workflow

- Branch: `master` (local only).
- Commits: Conventional Commits 1.0.0, English, no AI trailers. Example: `feat(browser): add file browser overlay with tree navigation`
- Present per commit: staged files, full message, exact command; wait for approval before running. Push requires separate approval.
