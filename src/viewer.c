/**
 * viewer.c - Implementation of viewer state, image loading, view control,
 *            navigation, and rendering.
 *
 * Design notes:
 *   - Images are decoded via stb_image to RGBA and uploaded once as textures.
 *     Zoom/pan is GPU-accelerated; no per-frame CPU resampling.
 *   - Pan is stored in image-space pixels and scaled by zoom at render time.
 *     This makes cursor-centered zoom linear: pan compensates by
 *     (1/next - 1/old) * (cursor - center).
 *   - Directory scanning for n/p navigation shares g_file_list/g_current_dir
 *     with the browser module; viewer owns the list and browser reads it for
 *     initial path but maintains its own tree for the overlay.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "viewer.h"
#include "text.h"
#include "exif.h"

#include <SDL2/SDL.h>
#include <time.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Supported extensions (case-insensitive)
// ---------------------------------------------------------------------------
static const char *kImageExts[] = {
    "jpg", "jpeg", "png", "webp", "bmp", "ppm", "pgm", "pbm", "tiff", "tif", "gif", "hdr", "psd", "tga", NULL
};

// ---------------------------------------------------------------------------
// Global definitions
// ---------------------------------------------------------------------------
SDL_Window *g_win = NULL;
SDL_Renderer *g_ren = NULL;

int g_win_w = 1280;
int g_win_h = 720;
int g_win_x = SDL_WINDOWPOS_CENTERED;
int g_win_y = SDL_WINDOWPOS_CENTERED;
bool g_fullscreen = false;

Image g_img[2] = {0};
int g_count = 0;

float g_zoom = 1.0f;
float g_pan_x = 0.0f;
float g_pan_y = 0.0f;
bool g_sync = true;
int g_active = 0;
float g_free_zoom[2] = {1.0f, 1.0f};
float g_free_pan_x[2] = {0.0f, 0.0f};
float g_free_pan_y[2] = {0.0f, 0.0f};

bool g_show_info = true;
bool g_show_help = false;
bool g_show_metadata = false;

char **g_file_list = NULL;
int g_file_count = 0;
int g_file_index = -1;
char g_current_dir[PATH_MAX] = {0};

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

/**
 * Safely joins dir and file into dst. Handles root directory "/" correctly
 * to avoid producing "//file". Returns true if result fits in dst_size.
 */
static bool path_join(char *dst, size_t dst_size, const char *dir, const char *file) {
    if (!dst || dst_size == 0 || !dir || !file) return false;
    size_t dlen = strlen(dir);
    int n;
    if (dlen == 0) {
        n = snprintf(dst, dst_size, "%s", file);
    } else if (dir[dlen - 1] == '/') {
        n = snprintf(dst, dst_size, "%s%s", dir, file);
    } else {
        n = snprintf(dst, dst_size, "%s/%s", dir, file);
    }
    return (n > 0 && (size_t)n < dst_size);
}

/**
 * Truncate a filename to fit within max_len characters, preserving the file extension.
 * If name is longer than max_len, stems are truncated with "..." before the extension.
 * If there is no extension or it is too long, suffix truncation is applied.
 */
void viewer_truncate_filename(const char *name, char *out, size_t out_sz, int max_len) {
    if (!out || out_sz == 0) return;
    if (!name || max_len <= 0) {
        out[0] = '\0';
        return;
    }
    if (max_len >= (int)out_sz) {
        max_len = (int)out_sz - 1;
    }
    int len = (int)strlen(name);
    if (len <= max_len) {
        snprintf(out, out_sz, "%s", name);
        return;
    }
    if (max_len <= 3) {
        snprintf(out, out_sz, "%.*s", max_len, "...");
        return;
    }
    const char *dot = strrchr(name, '.');
    if (dot && dot != name && *(dot + 1) != '\0') {
        const char *ext = dot + 1;
        int ext_len = (int)strlen(ext);
        if (ext_len <= max_len - 4) {
            int prefix_len = max_len - 3 - ext_len;
            snprintf(out, out_sz, "%.*s...%s", prefix_len, name, ext);
            return;
        }
    }
    int prefix_len = max_len - 3;
    snprintf(out, out_sz, "%.*s...", prefix_len, name);
}

/**
 * Test whether a file name has a supported image extension.
 *
 * Performs case-insensitive matching against the list of supported extensions
 * (jpg, jpeg, png, webp, bmp, ppm, pgm, pbm, tiff, tif, gif, hdr, psd, tga).
 *
 * @param name Filename or path string to inspect.
 * @return true if extension is recognized as a supported image, false otherwise.
 */
bool viewer_is_image_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') return false;
    const char *ext = dot + 1;
    for (int i = 0; kImageExts[i]; i++) {
        if (strcasecmp(ext, kImageExts[i]) == 0) return true;
    }
    return false;
}

/**
 * Validate that a path is a safe, readable regular file with an image extension.
 *
 * Checks for NUL/empty string, length bounds (< PATH_MAX), absence of control
 * characters (ASCII < 32 or 127), valid image extension, and stat() confirming S_ISREG.
 * Optionally resolves canonical path into out_clean_path.
 *
 * @param path Input filesystem path.
 * @param out_clean_path Optional output buffer to receive resolved canonical path.
 * @param out_size Size of out_clean_path buffer in bytes.
 * @return true if path is valid and points to an accessible regular image file, false otherwise.
 */
bool viewer_validate_image_path(const char *path, char *out_clean_path, size_t out_size) {
    if (!path || path[0] == '\0') return false;
    size_t len = strlen(path);
    if (len >= PATH_MAX) return false;

    // Disallow non-printable control characters
    for (size_t i = 0; i < len; i++) {
        unsigned char uc = (unsigned char)path[i];
        if (uc < 32 || uc == 127) return false;
    }

    if (!viewer_is_image_file(path)) return false;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

    if (out_clean_path && out_size > 0) {
        char resolved[PATH_MAX];
        if (realpath(path, resolved)) {
            if (strlen(resolved) >= out_size) return false;
            snprintf(out_clean_path, out_size, "%s", resolved);
        } else {
            if (len >= out_size) return false;
            snprintf(out_clean_path, out_size, "%s", path);
        }
    }
    return true;
}

/**
 * Free the directory file list and reset navigation counters.
 *
 * Releases all heap-allocated filename strings in g_file_list and the list
 * pointer itself. Safe to call multiple times or on an empty list.
 */
void viewer_free_file_list(void) {
    if (!g_file_list) return;
    for (int i = 0; i < g_file_count; i++) free(g_file_list[i]);
    free(g_file_list);
    g_file_list = NULL;
    g_file_count = 0;
    g_file_index = -1;
}

static int cmp_str(const void *a, const void *b) {
    const char * const *pa = a;
    const char * const *pb = b;
    return strcmp(*pa, *pb);
}

/**
 * Scan the directory containing ref_path for supported image files.
 *
 * Derives parent directory from ref_path, resolves canonical path, populates
 * g_file_list with all regular files having supported extensions (excluding hidden
 * dotfiles), sorts alphabetically, and sets g_file_index to ref_path's entry.
 *
 * @param ref_path Reference file path whose parent folder will be scanned.
 * @return true if directory was opened and scanned successfully, false on error.
 */
bool viewer_scan_current_dir(const char *ref_path) {
    viewer_free_file_list();
    if (!ref_path || ref_path[0] == '\0') return false;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", ref_path);
    char *dir = dirname(tmp);

    char abs_dir[PATH_MAX];
    if (realpath(dir, abs_dir)) {
        snprintf(g_current_dir, sizeof(g_current_dir), "%s", abs_dir);
    } else {
        snprintf(g_current_dir, sizeof(g_current_dir), "%s", dir);
    }

    DIR *d = opendir(g_current_dir);
    if (!d) return false;

    size_t cap = 64;
    g_file_list = malloc(cap * sizeof(char *));
    if (!g_file_list) { closedir(d); return false; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!viewer_is_image_file(ent->d_name)) continue;

        char full[PATH_MAX];
        if (!path_join(full, sizeof(full), g_current_dir, ent->d_name)) continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (g_file_count >= (int)cap) {
            size_t ncap = cap * 2;
            char **n = realloc(g_file_list, ncap * sizeof(char *));
            if (!n) break;
            g_file_list = n;
            cap = ncap;
        }
        char *item = strdup(full);
        if (!item) break;
        g_file_list[g_file_count++] = item;
    }
    closedir(d);

    if (g_file_count > 1) qsort(g_file_list, g_file_count, sizeof(char *), cmp_str);

    const char *base = strrchr(ref_path, '/');
    base = base ? base + 1 : ref_path;
    char abs_ref[PATH_MAX];
    const char *cmp_name = base;
    if (realpath(ref_path, abs_ref)) {
        const char *b2 = strrchr(abs_ref, '/');
        cmp_name = b2 ? b2 + 1 : abs_ref;
    }
    for (int i = 0; i < g_file_count; i++) {
        const char *b = strrchr(g_file_list[i], '/');
        b = b ? b + 1 : g_file_list[i];
        if (strcmp(b, cmp_name) == 0) { g_file_index = i; break; }
    }
    if (g_file_index == -1) {
        for (int i = 0; i < g_file_count; i++) {
            if (strcmp(g_file_list[i], ref_path) == 0) { g_file_index = i; break; }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Image lifecycle
// ---------------------------------------------------------------------------

/**
 * Release GPU texture and heap-allocated path owned by an Image struct.
 *
 * Destroys im->tex, frees im->path, and zeroes the struct. Safe to call on
 * NULL or already-zeroed structures.
 *
 * @param im Pointer to Image struct to unload.
 */
void viewer_unload_image(Image *im) {
    if (!im) return;
    if (im->tex) SDL_DestroyTexture(im->tex);
    if (im->path) free(im->path);
    memset(im, 0, sizeof(*im));
}

/**
 * Load an image from disk, decode to RGBA, and upload to an SDL GPU texture.
 *
 * Decodes pixel data using stb_image with 4 channels (RGBA32), creates an
 * SDL texture with linear scaling mode, and stores an owned copy of path.
 * On failure, out is zero-initialized and any intermediate allocations are freed.
 *
 * @param path Filesystem path to image file.
 * @param out Destination Image struct to receive texture, dimensions, and owned path.
 * @return true on successful load and texture creation, false on decode/allocation error.
 */
bool viewer_load_image(const char *path, Image *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    int w = 0, h = 0, comp = 0;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (!data) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return false;
    }

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(data);
        return false;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_FreeSurface(surf);
    stbi_image_free(data);

    if (!tex) {
        return false;
    }

    char *path_copy = strdup(path);
    if (!path_copy) {
        SDL_DestroyTexture(tex);
        return false;
    }

    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    out->tex = tex;
    out->w = w;
    out->h = h;
    out->path = path_copy;
    return true;
}

/**
 * Replace the image in the specified pane with a new file.
 *
 * Loads new image into a temporary struct before unloading the old one to ensure
 * atomic replacement on failure. Updates g_count if expanding to pane 1, and
 * rescans current directory if pane 0 or single-image mode.
 *
 * @param pane Target pane index (0 or 1).
 * @param path Filesystem path to the replacement image file.
 * @return true if replacement succeeded, false if loading failed (original preserved).
 */
bool viewer_replace_image(int pane, const char *path) {
    if (pane < 0 || pane > 1 || !path) return false;
    Image tmp = {0};
    if (!viewer_load_image(path, &tmp)) return false;
    viewer_unload_image(&g_img[pane]);
    g_img[pane] = tmp;
    if (pane >= g_count) g_count = pane + 1;
    if (pane == 0 || g_count == 1) {
        viewer_scan_current_dir(path);
    }
    return true;
}

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------

/**
 * Fit loaded images within the current window or pane dimensions.
 *
 * Computes zoom factor such that images fit entirely inside their viewport
 * (clamped to 100% maximum, 5% minimum). Resets pan offsets to (0, 0) for
 * centered display. In sync mode, fits both images to the shared pane size.
 */
void viewer_fit_view(void) {
    if (g_count == 0) return;

    if (g_win_w <= 0 || g_win_h <= 0) {
        g_zoom = 1.0f;
        g_pan_x = 0.0f;
        g_pan_y = 0.0f;
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = 1.0f;
            g_free_pan_x[i] = 0.0f;
            g_free_pan_y[i] = 0.0f;
        }
        return;
    }

    if (g_sync) {
        if (g_count == 1) {
            float zx = (g_img[0].w > 0) ? ((float)g_win_w / (float)g_img[0].w) : 1.0f;
            float zy = (g_img[0].h > 0) ? ((float)g_win_h / (float)g_img[0].h) : 1.0f;
            g_zoom = zx < zy ? zx : zy;
            if (g_zoom > 1.0f) g_zoom = 1.0f;
            if (g_zoom < 0.05f) g_zoom = 0.05f;
        } else {
            float pane_w = (float)g_win_w / 2.0f;
            float z0x = (g_img[0].w > 0) ? (pane_w / (float)g_img[0].w) : 1.0f;
            float z0y = (g_img[0].h > 0) ? ((float)g_win_h / (float)g_img[0].h) : 1.0f;
            float z1x = (g_img[1].w > 0) ? (pane_w / (float)g_img[1].w) : 1.0f;
            float z1y = (g_img[1].h > 0) ? ((float)g_win_h / (float)g_img[1].h) : 1.0f;
            float z0 = z0x < z0y ? z0x : z0y;
            float z1 = z1x < z1y ? z1x : z1y;
            if (g_img[0].w > 0 && g_img[1].w > 0) {
                g_zoom = z0 < z1 ? z0 : z1;
            } else if (g_img[0].w > 0) {
                g_zoom = z0;
            } else if (g_img[1].w > 0) {
                g_zoom = z1;
            } else {
                g_zoom = 1.0f;
            }
            if (g_zoom > 1.0f) g_zoom = 1.0f;
            if (g_zoom < 0.05f) g_zoom = 0.05f;
        }
        g_pan_x = 0;
        g_pan_y = 0;
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = 0;
            g_free_pan_y[i] = 0;
        }
    } else {
        for (int i = 0; i < g_count; i++) {
            float pane_w = (g_count == 1) ? (float)g_win_w : (float)g_win_w / 2.0f;
            float zx = (g_img[i].w > 0) ? (pane_w / (float)g_img[i].w) : 1.0f;
            float zy = (g_img[i].h > 0) ? ((float)g_win_h / (float)g_img[i].h) : 1.0f;
            float z = zx < zy ? zx : zy;
            if (z > 1.0f) z = 1.0f;
            if (z < 0.05f) z = 0.05f;
            g_free_zoom[i] = z;
            g_free_pan_x[i] = 0;
            g_free_pan_y[i] = 0;
        }
    }
}

/**
 * Apply a zoom factor centered on a window coordinate.
 *
 * Keeps the world point under (mx, my) stationary across zoom changes by
 * adjusting pan in image-space: pan_new = pan_old + (cursor - center) * (1/next - 1/old).
 * Clamps zoom between 0.05x (5%) and 32.0x (3200%).
 *
 * @param factor Multiplicative zoom step (> 0, e.g. 1.1 for in, 0.9 for out).
 * @param mx Cursor X coordinate in window space.
 * @param my Cursor Y coordinate in window space.
 */
void viewer_do_zoom(float factor, int mx, int my) {
    if (factor <= 0.0f || isnan(factor) || isinf(factor)) return;
    if (g_win_w <= 0 || g_win_h <= 0) return;

    if (g_sync) {
        float old = g_zoom < 0.05f ? 0.05f : g_zoom;
        float next = old * factor;
        if (next < 0.05f) next = 0.05f;
        if (next > 32.0f) next = 32.0f;
        if (next == old) return;
        float cx = (float)g_win_w * 0.5f;
        float cy = (float)g_win_h * 0.5f;
        g_pan_x += (float)(mx - (int)cx) * (1.0f / next - 1.0f / old);
        g_pan_y += (float)(my - (int)cy) * (1.0f / next - 1.0f / old);
        g_zoom = next;
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
    } else {
        int p = g_active;
        if (p < 0 || p >= g_count) p = 0;
        float old = g_free_zoom[p] < 0.05f ? 0.05f : g_free_zoom[p];
        float next = old * factor;
        if (next < 0.05f) next = 0.05f;
        if (next > 32.0f) next = 32.0f;
        if (next == old) return;
        float pane_w = (g_count == 1) ? (float)g_win_w : (float)g_win_w / 2.0f;
        float pane_cx = (g_count == 1) ? (float)g_win_w * 0.5f
                        : (p == 0 ? pane_w * 0.5f : pane_w + pane_w * 0.5f);
        float pane_cy = (float)g_win_h * 0.5f;
        g_free_pan_x[p] += (float)(mx - (int)pane_cx) * (1.0f / next - 1.0f / old);
        g_free_pan_y[p] += (float)(my - (int)pane_cy) * (1.0f / next - 1.0f / old);
        g_free_zoom[p] = next;
    }
}

/**
 * Pan the viewport by screen-space pixel deltas.
 *
 * Translates dx and dy into image-space coordinates by dividing by current zoom:
 * pan += delta / zoom. Updates shared or active-pane pan offsets accordingly.
 *
 * @param dx Horizontal displacement in window pixels.
 * @param dy Vertical displacement in window pixels.
 */
void viewer_do_pan(int dx, int dy) {
    if (g_sync) {
        float z = g_zoom < 0.05f ? 0.05f : g_zoom;
        g_pan_x += (float)dx / z;
        g_pan_y += (float)dy / z;
        for (int i = 0; i < 2; i++) {
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
    } else {
        int p = g_active;
        if (p < 0 || p >= g_count) p = 0;
        float z = g_free_zoom[p] < 0.05f ? 0.05f : g_free_zoom[p];
        g_free_pan_x[p] += (float)dx / z;
        g_free_pan_y[p] += (float)dy / z;
    }
}

/**
 * Toggle synchronization mode between synchronized and free transforms.
 *
 * When switching to free mode: copies current shared zoom/pan into each pane.
 * When switching to sync mode: adopts active pane's transform for shared view.
 */
void viewer_toggle_sync(void) {
    if (g_sync) {
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
        g_sync = false;
    } else {
        int p = g_active;
        if (p >= g_count) p = 0;
        g_zoom = g_free_zoom[p];
        g_pan_x = g_free_pan_x[p];
        g_pan_y = g_free_pan_y[p];
        g_sync = true;
    }
}

/**
 * Toggle between windowed and fullscreen desktop display modes.
 *
 * Saves window position and dimensions before entering fullscreen, and restores
 * them upon returning to windowed mode. Updates g_fullscreen state.
 */
void viewer_toggle_fullscreen(void) {
    if (!g_fullscreen) {
        SDL_GetWindowPosition(g_win, &g_win_x, &g_win_y);
        SDL_GetWindowSize(g_win, &g_win_w, &g_win_h);
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
        g_fullscreen = true;
    } else {
        SDL_SetWindowFullscreen(g_win, 0);
        SDL_SetWindowPosition(g_win, g_win_x, g_win_y);
        SDL_SetWindowSize(g_win, g_win_w, g_win_h);
        g_fullscreen = false;
    }
}

/**
 * Reconstruct and apply the window title bar string based on current viewer state.
 *
 * Formats filename(s), current zoom percentage, sync mode (SYNC / FREE), active
 * pane indicator ([L] / [R]), and directory index (e.g. 3/25).
 */
void viewer_update_title(void) {
    if (!g_win) return;
    char b0[256] = {0}, b1[256] = {0};
    if (g_count > 0 && g_img[0].path) {
        const char *b = strrchr(g_img[0].path, '/');
        snprintf(b0, sizeof(b0), "%s", b ? b + 1 : g_img[0].path);
    } else if (g_count > 0) {
        snprintf(b0, sizeof(b0), "(empty)");
    }
    if (g_count > 1 && g_img[1].path) {
        const char *b = strrchr(g_img[1].path, '/');
        snprintf(b1, sizeof(b1), "%s", b ? b + 1 : g_img[1].path);
    } else if (g_count > 1) {
        snprintf(b1, sizeof(b1), "(empty)");
    }

    float z = g_sync ? g_zoom : (g_active < 2 ? g_free_zoom[g_active] : 1.0f);
    int pct = (int)(z * 100.0f + 0.5f);
    char title[1024];
    if (g_count == 1) {
        snprintf(title, sizeof(title), "%s — %d%% — %s — %d/%d — [i]nfo [h]elp [q]uit",
            b0, pct, g_sync ? "SYNC" : "FREE", g_file_index + 1, g_file_count);
    } else if (g_count == 2) {
        snprintf(title, sizeof(title), "%s | %s — %d%% — %s%s — [i]nfo [h]elp",
            b0, b1, pct, g_sync ? "SYNC" : "FREE",
            g_sync ? "" : (g_active == 0 ? " [L]" : " [R]"));
    } else {
        snprintf(title, sizeof(title), "c-image-viewer");
    }
    SDL_SetWindowTitle(g_win, title);
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

/**
 * Navigate to the next or previous image in the scanned directory.
 *
 * Steps by delta (+1 or -1) through g_file_list with wraparound. Automatically
 * skips unreadable or corrupt images until a valid file loads or all files are tested.
 *
 * @param delta Direction step (+1 for next, -1 for previous).
 * @return true if an image was successfully loaded, false if no files or all unreadable.
 */
bool viewer_navigate(int delta) {
    if (g_file_count <= 1 || g_file_index < 0) return false;
    int step = delta >= 0 ? 1 : -1;
    int pane = (g_count == 1) ? 0 : g_active;
    if (pane < 0 || pane >= 2) pane = 0;

    int candidate = g_file_index;
    for (int attempts = 0; attempts < g_file_count - 1; attempts++) {
        candidate += step;
        if (candidate < 0) candidate = g_file_count - 1;
        if (candidate >= g_file_count) candidate = 0;
        if (candidate == g_file_index) break;

        const char *target = g_file_list[candidate];
        if (!target) continue;
        Image tmp = {0};
        if (viewer_load_image(target, &tmp)) {
            viewer_unload_image(&g_img[pane]);
            g_img[pane] = tmp;
            g_file_index = candidate;
            return true;
        }
    }
    return false;
}

/**
 * Navigate to the first image in the parent directory.
 *
 * Scans parent directory for image files, loads the first image into pane 0,
 * and updates g_file_list and g_current_dir on success. Preserves current state
 * on failure.
 *
 * @return true if navigation to parent succeeded, false otherwise.
 */
bool viewer_go_parent(void) {
    if (g_current_dir[0] == '\0') return false;
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", g_current_dir);
    char *parent = dirname(tmp);
    if (strcmp(parent, g_current_dir) == 0) return false;
    if (strcmp(parent, ".") == 0) return false;

    DIR *d = opendir(parent);
    if (!d) return false;

    bool has_image = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (viewer_is_image_file(ent->d_name)) { has_image = true; break; }
    }
    closedir(d);
    if (!has_image) return false;

    char parent_abs[PATH_MAX];
    if (realpath(parent, parent_abs)) parent = parent_abs;

    char saved_dir[PATH_MAX];
    snprintf(saved_dir, sizeof(saved_dir), "%s", g_current_dir);
    char **saved_list = g_file_list;
    int saved_count = g_file_count;
    int saved_index = g_file_index;
    g_file_list = NULL; g_file_count = 0; g_file_index = -1;

    char dummy[PATH_MAX];
    if (!path_join(dummy, sizeof(dummy), parent, "dummy.jpg")) return false;
    viewer_scan_current_dir(dummy);

    bool ok = false;
    if (g_file_count > 0) {
        const char *first = g_file_list[0];
        char **parent_list = g_file_list;
        int parent_count = g_file_count;

        Image tmp_img = {0};
        if (viewer_load_image(first, &tmp_img)) {
            viewer_unload_image(&g_img[0]);
            g_img[0] = tmp_img;
            for (int i = 0; i < saved_count; i++) free(saved_list[i]);
            free(saved_list);
            g_file_list = parent_list;
            g_file_count = parent_count;
            g_file_index = 0;
            snprintf(g_current_dir, sizeof(g_current_dir), "%s", parent);
            ok = true;
        } else {
            for (int i = 0; i < parent_count; i++) free(parent_list[i]);
            free(parent_list);
            g_file_list = saved_list;
            g_file_count = saved_count;
            g_file_index = saved_index;
            snprintf(g_current_dir, sizeof(g_current_dir), "%s", saved_dir);
        }
    } else {
        viewer_free_file_list();
        g_file_list = saved_list;
        g_file_count = saved_count;
        g_file_index = saved_index;
        snprintf(g_current_dir, sizeof(g_current_dir), "%s", saved_dir);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Status bar layout and formatting helpers
// ---------------------------------------------------------------------------

static const char *s_hint_single[VIEWER_HINT_TIER_COUNT] = {
    "",                                                                   // VIEWER_HINT_NONE
    "[e] exif [ESC]",                                                     // VIEWER_HINT_MINIMAL
    "[s]ync [f]ull [n/p] [e]xif [ESC]",                                   // VIEWER_HINT_COMPACT
    "[s]ync [Tab] pane [f]ull [n/p] next/prev [e] exif [ESC] browser"     // VIEWER_HINT_FULL
};

static const char *s_hint_dual[VIEWER_HINT_TIER_COUNT] = {
    "",                                                                   // VIEWER_HINT_NONE
    "[e] exif",                                                           // VIEWER_HINT_MINIMAL
    "[s]ync [Tab] pane [e] exif",                                         // VIEWER_HINT_COMPACT
    "[s]ync [Tab] pane [f]ull [e] exif [ESC] browser"                     // VIEWER_HINT_FULL
};

/**
 * Distribute available filename character budget between dual panes.
 *
 * Starts with an equal division of total_budget. If either pane needs fewer
 * characters than its initial share (len < share), the surplus is transferred to
 * the other pane. Ensures conservation of the total budget (*out0 + *out1 == total_budget)
 * for non-negative budgets, and zeroes outputs when total_budget <= 0.
 *
 * @param total_budget Total character budget available for both filenames.
 * @param len0 Required character count for pane 0 filename.
 * @param len1 Required character count for pane 1 filename.
 * @param min_len Minimum character allocation per pane (reserved/floor parameter).
 * @param out0 Output pointer for pane 0 character budget.
 * @param out1 Output pointer for pane 1 character budget.
 */
void viewer_distribute_dual_budget(int total_budget, int len0, int len1, int min_len, int *out0, int *out1) {
    if (!out0 || !out1) return;
    if (total_budget <= 0) {
        *out0 = 0;
        *out1 = 0;
        return;
    }
    (void)min_len;
    int b0 = total_budget / 2;
    int b1 = total_budget - b0;

    int n0 = (len0 < 0) ? 0 : len0;
    int n1 = (len1 < 0) ? 0 : len1;

    if (n0 < b0 && n1 >= b1) {
        int surplus = b0 - n0;
        b0 = n0;
        b1 += surplus;
    } else if (n1 < b1 && n0 >= b0) {
        int surplus = b1 - n1;
        b1 = n1;
        b0 += surplus;
    }
    *out0 = b0;
    *out1 = b1;
}

/**
 * Calculate dynamic status bar layout for single-pane view based on window width.
 *
 * Computes usable characters: (win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W.
 * Formats metadata string ("%dx%d  %d%%  %s  %d/%d") and evaluates hint tiers from
 * FULL down to NONE. If available budget allows the target filename budget
 * (VIEWER_INFO_NAME_TARGET_SINGLE), that tier is selected. All remaining usable
 * characters are allocated to layout.name_budget[0].
 *
 * @param win_w Window width in pixels.
 * @param name Filename or path string for image.
 * @param img_w Image pixel width.
 * @param img_h Image pixel height.
 * @param zoom_pct Zoom factor percentage (e.g. 100).
 * @param is_sync True if sync transform mode is active.
 * @param file_idx Current image index in folder.
 * @param file_count Total number of images in folder.
 * @return Calculated ViewerStatusBarLayout struct.
 */
ViewerStatusBarLayout viewer_calc_status_layout_single(
    int win_w, const char *name, int img_w, int img_h,
    int zoom_pct, bool is_sync, int file_idx, int file_count)
{
    (void)name;
    ViewerStatusBarLayout layout;
    memset(&layout, 0, sizeof(layout));

    int usable_chars = (win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    if (usable_chars < 0) usable_chars = 0;
    layout.usable_chars = usable_chars;

    char meta[256];
    int meta_len = snprintf(meta, sizeof(meta), "%dx%d  %d%%  %s  %d/%d",
        img_w, img_h, zoom_pct, is_sync ? "SYNC" : "FREE", file_idx, file_count);
    if (meta_len < 0) meta_len = 0;

    for (int t = (int)VIEWER_HINT_FULL; t >= (int)VIEWER_HINT_NONE; t--) {
        const char *hint = s_hint_single[t];
        int hint_len = (int)strlen(hint);
        int hint_cost = (hint_len > 0) ? (2 + hint_len) : 0;
        int cost = 2 + meta_len + hint_cost;
        int rem = usable_chars - cost;

        if (t == (int)VIEWER_HINT_NONE || rem >= VIEWER_INFO_NAME_TARGET_SINGLE) {
            layout.hint_tier = (ViewerHintTier)t;
            layout.name_budget[0] = (rem < 0) ? 0 : rem;
            layout.name_budget[1] = 0;
            return layout;
        }
    }

    layout.hint_tier = VIEWER_HINT_NONE;
    int base_cost = 2 + meta_len;
    int rem = usable_chars - base_cost;
    layout.name_budget[0] = (rem < 0) ? 0 : rem;
    layout.name_budget[1] = 0;
    return layout;
}

/**
 * Calculate dynamic status bar layout for dual-pane view based on window width.
 *
 * Computes usable characters from window width, measures fixed metadata length for
 * both panes, evaluates hint tiers from FULL down to NONE against the combined target
 * budget (2 * VIEWER_INFO_NAME_TARGET_DUAL), and distributes remaining character
 * budget between name0 and name1 using viewer_distribute_dual_budget.
 *
 * @param win_w Window width in pixels.
 * @param name0 Filename or path for pane 0.
 * @param img0_w Pixel width of pane 0 image.
 * @param img0_h Pixel height of pane 0 image.
 * @param name1 Filename or path for pane 1.
 * @param img1_w Pixel width of pane 1 image.
 * @param img1_h Pixel height of pane 1 image.
 * @param zoom_pct Zoom factor percentage.
 * @param is_sync True if sync transform mode is active.
 * @param active_pane Active pane index (0 or 1).
 * @return Calculated ViewerStatusBarLayout struct.
 */
ViewerStatusBarLayout viewer_calc_status_layout_dual(
    int win_w, const char *name0, int img0_w, int img0_h,
    const char *name1, int img1_w, int img1_h,
    int zoom_pct, bool is_sync, int active_pane)
{
    ViewerStatusBarLayout layout;
    memset(&layout, 0, sizeof(layout));

    int usable_chars = (win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    if (usable_chars < 0) usable_chars = 0;
    layout.usable_chars = usable_chars;

    char p0_buf[128];
    char p1_buf[128];
    const char *pane_ind = is_sync ? "" : (active_pane == 0 ? " [L*]" : " [R*]");
    int p0_len = snprintf(p0_buf, sizeof(p0_buf), " (%dx%d) | ", img0_w, img0_h);
    int p1_len = snprintf(p1_buf, sizeof(p1_buf), " (%dx%d)  %d%%  %s%s",
        img1_w, img1_h, zoom_pct, is_sync ? "SYNC" : "FREE", pane_ind);
    if (p0_len < 0) p0_len = 0;
    if (p1_len < 0) p1_len = 0;
    int fixed_meta_len = p0_len + p1_len;

    const char *b0 = name0 ? strrchr(name0, '/') : NULL;
    const char *fname0 = b0 ? b0 + 1 : (name0 ? name0 : "");
    const char *b1 = name1 ? strrchr(name1, '/') : NULL;
    const char *fname1 = b1 ? b1 + 1 : (name1 ? name1 : "");
    int len0 = (int)strlen(fname0);
    int len1 = (int)strlen(fname1);

    int target_dual = 2 * VIEWER_INFO_NAME_TARGET_DUAL;

    for (int t = (int)VIEWER_HINT_FULL; t >= (int)VIEWER_HINT_NONE; t--) {
        const char *hint = s_hint_dual[t];
        int hint_len = (int)strlen(hint);
        int hint_cost = (hint_len > 0) ? (2 + hint_len) : 0;
        int cost = fixed_meta_len + hint_cost;
        int rem = usable_chars - cost;

        if (t == (int)VIEWER_HINT_NONE || rem >= target_dual) {
            layout.hint_tier = (ViewerHintTier)t;
            int total_name_budget = (rem < 0) ? 0 : rem;
            viewer_distribute_dual_budget(total_name_budget, len0, len1,
                VIEWER_INFO_NAME_MIN_DUAL, &layout.name_budget[0], &layout.name_budget[1]);
            return layout;
        }
    }

    layout.hint_tier = VIEWER_HINT_NONE;
    int rem = usable_chars - fixed_meta_len;
    int total_name_budget = (rem < 0) ? 0 : rem;
    viewer_distribute_dual_budget(total_name_budget, len0, len1,
        VIEWER_INFO_NAME_MIN_DUAL, &layout.name_budget[0], &layout.name_budget[1]);
    return layout;
}

/**
 * Format complete single-pane status bar string according to calculated layout.
 *
 * Truncates filename with extension preservation to layout->name_budget[0], formats
 * metadata and responsive hint text, and ensures NUL termination.
 *
 * @param layout Layout specifying budget and hint tier.
 * @param name Filename or path for image.
 * @param img_w Image pixel width.
 * @param img_h Image pixel height.
 * @param zoom_pct Zoom level percentage.
 * @param is_sync True if sync mode is active.
 * @param file_idx Current image index.
 * @param file_count Total image count.
 * @param out_buf Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 * @return Number of characters written (excluding NUL).
 */
int viewer_format_status_single(
    const ViewerStatusBarLayout *layout, const char *name,
    int img_w, int img_h, int zoom_pct, bool is_sync,
    int file_idx, int file_count, char *out_buf, size_t out_sz)
{
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = '\0';
    if (!layout) return 0;

    const char *b = name ? strrchr(name, '/') : NULL;
    const char *fname = b ? b + 1 : (name ? name : "");

    char trunc[512];
    viewer_truncate_filename(fname, trunc, sizeof(trunc), layout->name_budget[0]);

    ViewerHintTier tier = layout->hint_tier;
    if (tier < 0 || tier >= VIEWER_HINT_TIER_COUNT) {
        tier = VIEWER_HINT_NONE;
    }
    const char *hint = s_hint_single[tier];

    if (hint && hint[0] != '\0') {
        snprintf(out_buf, out_sz, "%s  %dx%d  %d%%  %s  %d/%d  %s",
            trunc, img_w, img_h, zoom_pct, is_sync ? "SYNC" : "FREE",
            file_idx, file_count, hint);
    } else {
        snprintf(out_buf, out_sz, "%s  %dx%d  %d%%  %s  %d/%d",
            trunc, img_w, img_h, zoom_pct, is_sync ? "SYNC" : "FREE",
            file_idx, file_count);
    }
    out_buf[out_sz - 1] = '\0';
    return (int)strlen(out_buf);
}

/**
 * Format complete dual-pane status bar string according to calculated layout.
 *
 * Truncates filenames using layout->name_budget[0] and layout->name_budget[1], formats
 * metadata for both panes, active pane indicator, and responsive hint text, and ensures
 * NUL termination.
 *
 * @param layout Layout specifying budgets and hint tier.
 * @param name0 Filename or path for pane 0.
 * @param img0_w Pixel width of pane 0 image.
 * @param img0_h Pixel height of pane 0 image.
 * @param name1 Filename or path for pane 1.
 * @param img1_w Pixel width of pane 1 image.
 * @param img1_h Pixel height of pane 1 image.
 * @param zoom_pct Zoom level percentage.
 * @param is_sync True if sync mode is active.
 * @param active_pane Active pane index (0 or 1).
 * @param out_buf Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 * @return Number of characters written (excluding NUL).
 */
int viewer_format_status_dual(
    const ViewerStatusBarLayout *layout, const char *name0,
    int img0_w, int img0_h, const char *name1,
    int img1_w, int img1_h, int zoom_pct, bool is_sync,
    int active_pane, char *out_buf, size_t out_sz)
{
    if (!out_buf || out_sz == 0) return 0;
    out_buf[0] = '\0';
    if (!layout) return 0;

    const char *b0 = name0 ? strrchr(name0, '/') : NULL;
    const char *fname0 = b0 ? b0 + 1 : (name0 ? name0 : "");
    const char *b1 = name1 ? strrchr(name1, '/') : NULL;
    const char *fname1 = b1 ? b1 + 1 : (name1 ? name1 : "");

    char trunc0[512];
    char trunc1[512];
    viewer_truncate_filename(fname0, trunc0, sizeof(trunc0), layout->name_budget[0]);
    viewer_truncate_filename(fname1, trunc1, sizeof(trunc1), layout->name_budget[1]);

    const char *pane_ind = is_sync ? "" : (active_pane == 0 ? " [L*]" : " [R*]");

    ViewerHintTier tier = layout->hint_tier;
    if (tier < 0 || tier >= VIEWER_HINT_TIER_COUNT) {
        tier = VIEWER_HINT_NONE;
    }
    const char *hint = s_hint_dual[tier];

    if (hint && hint[0] != '\0') {
        snprintf(out_buf, out_sz, "%s (%dx%d) | %s (%dx%d)  %d%%  %s%s  %s",
            trunc0, img0_w, img0_h, trunc1, img1_w, img1_h, zoom_pct,
            is_sync ? "SYNC" : "FREE", pane_ind, hint);
    } else {
        snprintf(out_buf, out_sz, "%s (%dx%d) | %s (%dx%d)  %d%%  %s%s",
            trunc0, img0_w, img0_h, trunc1, img1_w, img1_h, zoom_pct,
            is_sync ? "SYNC" : "FREE", pane_ind);
    }
    out_buf[out_sz - 1] = '\0';
    return (int)strlen(out_buf);
}

/**
 * Render bottom status bar with filename, resolution, zoom, sync, and index.
 *
 * No-op if g_show_info is false or window dimensions are invalid.
 *
 * @param ren Target SDL renderer.
 */
void viewer_render_info_bar(SDL_Renderer *ren) {
    if (!g_show_info || !ren || g_win_w <= 0 || g_win_h <= 0) return;

    int bar_h = 22;
    if (bar_h > g_win_h) bar_h = g_win_h;
    if (bar_h <= 0) return;
    SDL_Rect bar = {0, g_win_h - bar_h, g_win_w, bar_h};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
    SDL_RenderFillRect(ren, &bar);

    char line[1024];
    float z = g_sync ? g_zoom : g_free_zoom[g_active];
    int pct = (int)(z * 100.0f + 0.5f);
    int len = 0;
    if (g_count == 1) {
        const char *b = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        b = b ? b + 1 : (g_img[0].path ? g_img[0].path : "?");
        ViewerStatusBarLayout layout = viewer_calc_status_layout_single(
            g_win_w, b, g_img[0].w, g_img[0].h, pct, g_sync, g_file_index + 1, g_file_count);
        len = viewer_format_status_single(
            &layout, b, g_img[0].w, g_img[0].h, pct, g_sync, g_file_index + 1, g_file_count,
            line, sizeof(line));
    } else if (g_count == 2) {
        const char *b0 = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        const char *b1 = g_img[1].path ? strrchr(g_img[1].path, '/') : NULL;
        b0 = b0 ? b0 + 1 : (g_img[0].path ? g_img[0].path : "?");
        b1 = b1 ? b1 + 1 : (g_img[1].path ? g_img[1].path : "?");
        ViewerStatusBarLayout layout = viewer_calc_status_layout_dual(
            g_win_w, b0, g_img[0].w, g_img[0].h, b1, g_img[1].w, g_img[1].h, pct, g_sync, g_active);
        len = viewer_format_status_dual(
            &layout, b0, g_img[0].w, g_img[0].h, b1, g_img[1].w, g_img[1].h, pct, g_sync, g_active,
            line, sizeof(line));
    } else {
        return;
    }
    if (len <= 0) return;
    SDL_Color white = {220, 220, 220, 255};
    int max_chars = (g_win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    if (max_chars < 0) max_chars = 0;
    if (len > max_chars) line[max_chars] = '\0';
    text_draw(ren, VIEWER_INFO_MARGIN_X, g_win_h - bar_h + 7, line, white, 1);
}

/**
 * Render keyboard shortcut and controls help overlay dialog.
 *
 * No-op if g_show_help is false. Renders semi-transparent centered dialog box
 * with control descriptions and dismiss instructions.
 *
 * @param ren Target SDL renderer.
 */
void viewer_render_help(SDL_Renderer *ren) {
    if (!g_show_help || !ren || g_win_w <= 0 || g_win_h <= 0) return;

    const char *lines[] = {
        "c-image-viewer  -  Help",
        "",
        "Mouse wheel      Zoom (cursor-centered)",
        "Left drag        Pan",
        "0 / F            Fit to window",
        "1                100% (1:1)",
        "+ / -            Zoom in/out",
        "f / F11          Toggle fullscreen",
        "i                Toggle info bar",
        "h / ?            Toggle this help",
        "s                Toggle sync (SYNC/FREE)",
        "Tab              Switch active pane (FREE mode)",
        "n / Right / PgDn Next image",
        "p / Left / PgUp  Previous image",
        "ESC              Browser / Exit fullscreen / Close help",
        "q                Quit",
        "Drag & drop      Drop onto left/right half to replace",
        "",
        "Press h or ESC to close",
        NULL
    };
    int count = 0;
    while (lines[count]) count++;

    int panel_w = 520;
    if (panel_w > g_win_w) panel_w = g_win_w;
    int panel_h = count * 14 + 24;
    if (panel_h > g_win_h) panel_h = g_win_h;
    if (panel_w <= 0 || panel_h <= 0) return;
    int px = (g_win_w - panel_w) / 2;
    int py = (g_win_h - panel_h) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_Rect dim = {0, 0, g_win_w, g_win_h};
    SDL_RenderFillRect(ren, &dim);

    SDL_Rect bg = {px, py, panel_w, panel_h};
    SDL_SetRenderDrawColor(ren, 28, 28, 28, 240);
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 80, 80, 80, 255);
    SDL_RenderDrawRect(ren, &bg);

    SDL_Color white = {230, 230, 230, 255};
    SDL_Color dim_white = {180, 180, 180, 255};
    SDL_Color title_col = {255, 220, 100, 255};
    int y = py + 12;
    for (int i = 0; lines[i]; i++) {
        SDL_Color col = (i == 0) ? title_col : (lines[i][0] == '\0' ? white : dim_white);
        int tx = px + 16;
        if (i == 0) tx = px + (panel_w - (int)strlen(lines[i]) * 8) / 2;
        text_draw(ren, tx, y, lines[i], col, 1);
        y += 14;
    }
}

/**
 * Toggle visibility of the right-hand EXIF and file metadata overlay panel.
 */
void viewer_toggle_metadata(void) {
    g_show_metadata = !g_show_metadata;
}

/**
 * Render right-side metadata panel (when g_show_metadata is true).
 *
 * Shows file system info (size, mtime, dimensions) and EXIF tags when
 * available. The panel is 380px wide, anchored to the right, with a
 * semi-transparent background and a header. Each metadata field is a
 * label/value pair drawn with the bitmap font.
 */
void viewer_render_metadata(SDL_Renderer *ren) {
    if (!g_show_metadata || !ren || g_win_w <= 0 || g_win_h <= 0) return;
    // Choose active pane's image for metadata (or pane 0 if sync)
    int pane = g_sync ? 0 : g_active;
    if (pane < 0 || pane >= g_count) pane = 0;
    if (g_count == 0 || !g_img[pane].path) return;

    const char *path = g_img[pane].path;
    Image *im = &g_img[pane];

    // Cache EXIF and file stat to avoid repeated disk reads and 128KB allocations per frame
    static char s_cached_md_path[PATH_MAX] = {0};
    static ExifData s_cached_exif;
    static struct stat s_cached_st;
    static bool s_has_stat = false;

    if (strcmp(s_cached_md_path, path) != 0) {
        snprintf(s_cached_md_path, sizeof(s_cached_md_path), "%s", path);
        s_has_stat = (stat(path, &s_cached_st) == 0);
        exif_read(path, &s_cached_exif);
    }

    struct stat st = s_cached_st;
    bool has_stat = s_has_stat;
    char size_str[32] = "?";
    char mtime_str[64] = "?";
    if (has_stat) {
        // Human readable size
        if (st.st_size < 1024) snprintf(size_str, sizeof(size_str), "%ld B", (long)st.st_size);
        else if (st.st_size < 1024*1024) snprintf(size_str, sizeof(size_str), "%.1f KB", st.st_size/1024.0);
        else if (st.st_size < 1024*1024*1024) snprintf(size_str, sizeof(size_str), "%.1f MB", st.st_size/(1024.0*1024));
        else snprintf(size_str, sizeof(size_str), "%.2f GB", st.st_size/(1024.0*1024*1024));
        struct tm *tm = localtime(&st.st_mtime);
        if (tm) strftime(mtime_str, sizeof(mtime_str), "%Y-%m-%d %H:%M", tm);
    }

    ExifData exif = s_cached_exif;

    // Panel geometry: 380px wide, 70% height centered vertically, right margin 12
    int pw = 380;
    if (pw > g_win_w - 40) pw = g_win_w - 40;
    if (pw <= 0) return;
    int ph = g_win_h - 80;
    if (ph > 520) ph = 520;
    if (ph <= 0) return;
    int px = g_win_w - pw - 12;
    if (px < 0) px = 0;
    int py = (g_win_h - ph) / 2;
    if (py < 0) py = 0;

    // Background
    SDL_Rect bg = {px, py, pw, ph};
    SDL_SetRenderDrawColor(ren, 24, 24, 24, 235);
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawRect(ren, &bg);

    // Title bar
    SDL_Rect title_bg = {px, py, pw, 26};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &title_bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, py+26, px+pw, py+26);

    SDL_Color title_col = {255, 220, 100, 255};
    SDL_Color label_col = {160, 160, 160, 255};
    SDL_Color value_col = {220, 220, 220, 255};
    SDL_Color dim_col = {140, 140, 140, 255};

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char title[256];
    snprintf(title, sizeof(title), "Metadata — %s", base);
    // Truncate title if too long
    int max_title = (pw - 16) / 8;
    if (max_title <= 0) {
        title[0] = '\0';
    } else if ((int)strlen(title) > max_title) {
        if (max_title >= 4) {
            title[max_title - 3] = '.';
            title[max_title - 2] = '.';
            title[max_title - 1] = '.';
            title[max_title] = '\0';
        } else {
            title[max_title] = '\0';
        }
    }
    text_draw(ren, px+8, py+9, title, title_col, 1);

    int y = py + 36;
    int line_h = 14;
    int label_x = px + 12;
    int value_x = px + 120;

    // Helper macro to draw label/value pairs and advance y
    #define MD_ROW(label, value) do { \
        text_draw(ren, label_x, y, label, label_col, 1); \
        text_draw_clipped(ren, value_x, y, value, value_col, 1, pw - (value_x - px) - 12); \
        y += line_h; \
    } while(0)

    char dim[64];
    snprintf(dim, sizeof(dim), "%d x %d", im->w, im->h);
    MD_ROW("Dimensions", dim);
    MD_ROW("File size", size_str);
    MD_ROW("Modified", mtime_str);
    // Path (show full, truncated)
    char path_disp[PATH_MAX];
    snprintf(path_disp, sizeof(path_disp), "%s", path);
    // Shorten home prefix
    const char *home = getenv("HOME");
    if (home && home[0] != '\0' && strcmp(home, "/") != 0) {
        size_t hlen = strlen(home);
        if (strncmp(path_disp, home, hlen) == 0 && (path_disp[hlen] == '/' || path_disp[hlen] == '\0')) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "~%s", path_disp + hlen);
            snprintf(path_disp, sizeof(path_disp), "%s", tmp);
        }
    }
    MD_ROW("Path", path_disp);

    y += 4;
    // Separator
    SDL_SetRenderDrawColor(ren, 50, 50, 50, 255);
    SDL_RenderDrawLine(ren, px+12, y, px+pw-12, y);
    y += 8;
    text_draw(ren, label_x, y, "EXIF", label_col, 1);
    y += line_h;

    if (!exif.has_exif) {
        text_draw(ren, label_x, y, "No EXIF data", dim_col, 1);
        y += line_h;
    } else {
        if (exif.make[0] || exif.model[0]) {
            char cam[128]; snprintf(cam, sizeof(cam), "%s %s", exif.make, exif.model);
            MD_ROW("Camera", cam);
        }
        if (exif.datetime[0]) MD_ROW("Date", exif.datetime);
        if (exif.software[0]) MD_ROW("Software", exif.software);
        char ori[16]; snprintf(ori, sizeof(ori), "%d", exif.orientation);
        MD_ROW("Orientation", ori);
        if (exif.iso) { char s[16]; snprintf(s, sizeof(s), "ISO %d", exif.iso); MD_ROW("ISO", s); }
        if (exif.exposure[0]) MD_ROW("Exposure", exif.exposure);
        if (exif.fnumber[0]) MD_ROW("Aperture", exif.fnumber);
        if (exif.focal[0]) MD_ROW("Focal", exif.focal);
        if (exif.exif_width && exif.exif_height) {
            char s[32]; snprintf(s, sizeof(s), "%d x %d", exif.exif_width, exif.exif_height);
            MD_ROW("EXIF size", s);
        }
    }

    #undef MD_ROW

    // Footer hint
    SDL_Rect footer = {px, py+ph-20, pw, 20};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &footer);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, py+ph-20, px+pw, py+ph-20);
    text_draw(ren, px+8, py+ph-14, "Press e to close", dim_col, 1);
}

/**
 * Render a single image pane inside the given clip rectangle.
 *
 * Computes destination rectangle using precomputed img->w / img->h and zoom/pan.
 * Skips SDL_RenderCopyF if the image destination is entirely outside clip bounds.
 */
static void viewer_render_pane(SDL_Renderer *ren, int pane_idx, SDL_Rect clip) {
    if (!ren || pane_idx < 0 || pane_idx >= 2) return;
    Image *im = &g_img[pane_idx];

    SDL_RenderSetClipRect(ren, &clip);

    if (im->tex && im->w > 0 && im->h > 0) {
        float z, px, py;
        if (g_sync) {
            z = g_zoom;
            px = g_pan_x;
            py = g_pan_y;
        } else {
            z = g_free_zoom[pane_idx];
            px = g_free_pan_x[pane_idx];
            py = g_free_pan_y[pane_idx];
        }
        if (z < 0.05f) z = 0.05f;

        float dw = (float)im->w * z;
        float dh = (float)im->h * z;
        float pane_cx = (float)clip.x + (float)clip.w * 0.5f;
        float pane_cy = (float)clip.y + (float)clip.h * 0.5f;
        float dx = pane_cx - dw * 0.5f + px * z;
        float dy = pane_cy - dh * 0.5f + py * z;

        // Viewport culling: only issue RenderCopy if destination rect intersects pane
        if (dx + dw > (float)clip.x && dx < (float)(clip.x + clip.w) &&
            dy + dh > (float)clip.y && dy < (float)(clip.y + clip.h)) {
            SDL_FRect dst = {dx, dy, dw, dh};
            SDL_RenderCopyF(ren, im->tex, NULL, &dst);
        }
    } else {
        SDL_SetRenderDrawColor(ren, 25, 25, 25, 255);
        SDL_RenderFillRect(ren, &clip);
    }
    SDL_RenderSetClipRect(ren, NULL);

    // Free-mode active pane highlight
    if (!g_sync && g_active == pane_idx) {
        SDL_SetRenderDrawColor(ren, 100, 160, 255, (g_count == 1) ? 120 : 200);
        SDL_Rect hl = {
            clip.x + 1,
            clip.y + 1,
            clip.w - 2 > 0 ? clip.w - 2 : 1,
            clip.h - 2 > 0 ? clip.h - 2 : 1
        };
        SDL_RenderDrawRect(ren, &hl);
    }
}

/**
 * Draw vertical divider between split panes.
 */
static void viewer_render_split_divider(SDL_Renderer *ren, int mid, int h) {
    if (!ren || mid <= 0 || h <= 0) return;
    SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
    SDL_RenderDrawLine(ren, mid, 0, mid, h);
}

/**
 * Clear the viewport and render active panes, split divider, and overlays.
 *
 * Renders pane 0 (and pane 1 if g_count == 2) with viewport culling, divider line,
 * info bar (if enabled), help dialog (if enabled), and metadata panel (if enabled).
 *
 * @param ren Target SDL renderer.
 */
void viewer_render(SDL_Renderer *ren) {
    if (!ren) return;
    SDL_SetRenderDrawColor(ren, 18, 18, 18, 255);
    SDL_RenderClear(ren);
    if (g_win_w <= 0 || g_win_h <= 0) return;

    if (g_count == 1) {
        SDL_Rect pane = {0, 0, g_win_w, g_win_h};
        viewer_render_pane(ren, 0, pane);
    } else if (g_count == 2) {
        int mid = g_win_w / 2;
        SDL_Rect p0 = {0, 0, mid, g_win_h};
        SDL_Rect p1 = {mid, 0, g_win_w - mid, g_win_h};
        viewer_render_pane(ren, 0, p0);
        viewer_render_pane(ren, 1, p1);
        viewer_render_split_divider(ren, mid, g_win_h);
    }

    viewer_render_info_bar(ren);
    viewer_render_help(ren);
    viewer_render_metadata(ren);
    // Browser overlay is rendered by main.c after this if open.
}
