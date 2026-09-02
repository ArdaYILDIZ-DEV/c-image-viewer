/**
 * c-image-viewer - Minimalist dual-pane image viewer with synchronized zoom/pan.
 *
 * Architecture:
 *   - Single window split into one or two panes. A single global view transform
 *     (zoom, pan_x, pan_y) is shared across panes when synchronized. When
 *     synchronization is disabled, each pane maintains its own transform.
 *   - Images are decoded on CPU via stb_image and uploaded once as SDL_Textures.
 *     All subsequent zoom/pan is GPU-accelerated via SDL_RenderCopyF with
 *     bilinear filtering, avoiding per-frame CPU resampling.
 *   - Directory navigation scans the current image's folder for supported
 *     extensions and allows sequential browsing without restarting the process.
 *   - Drag-and-drop is handled via SDL_DROPFILE; the drop position determines
 *     the target pane (left/right half when dual, full window when single).
 *   - Information and help overlays are rendered with an embedded 8x8 bitmap
 *     font (public domain, font8x8_basic) to avoid an SDL_ttf dependency.
 *
 * Dependencies:
 *   - SDL2 for windowing, input, and rendering.
 *   - stb_image.h (header-only) for JPEG/PNG/WebP/BMP/PPM decoding.
 *   - font8x8.h (header-only, embedded) for overlay text rendering.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -Wpedantic -std=c11 main.c -o viewer -lSDL2 -lm
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "font8x8.h"

#include <SDL2/SDL.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

// ---------------------------------------------------------------------------
// Types and constants
// ---------------------------------------------------------------------------

/**
 * Represents a single loaded image and its GPU texture.
 *
 * The texture is created once at load time and reused for every frame.
 * Width/height are the original decoded dimensions (before zoom).
 */
typedef struct {
    SDL_Texture *tex;   // GPU texture, owned - must be destroyed with SDL_DestroyTexture
    int w, h;           // Original image dimensions in pixels
    char *path;         // Absolute or provided file path (owned, free with free())
} Image;

// Supported image extensions (case-insensitive). Keep in sync with stb_image
// capabilities and the .desktop MimeType list.
static const char *kImageExts[] = {
    "jpg", "jpeg", "png", "webp", "bmp", "ppm", "pgm", "pbm", "tiff", "tif", "gif", "hdr", "psd", "tga", NULL
};

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static Image g_img[2] = {0};          // Loaded images, up to 2
static int g_count = 0;               // Number of loaded images (1 or 2)

static SDL_Window *g_win = NULL;      // Main window handle
static SDL_Renderer *g_ren = NULL;    // Main renderer handle

// Window geometry (updated on resize; saved for fullscreen restore)
static int g_win_w = 1280;
static int g_win_h = 720;
static int g_win_x = SDL_WINDOWPOS_CENTERED;
static int g_win_y = SDL_WINDOWPOS_CENTERED;
static bool g_fullscreen = false;

// View transform: synchronized vs per-pane.
// When g_sync is true, g_zoom/g_pan_* are authoritative for both panes.
// When false, g_free_* arrays are used per-pane and g_active selects the
// pane that receives input (zoom, pan, navigation).
static float g_zoom = 1.0f;           // Shared zoom (sync mode), clamped [0.05, 32]
static float g_pan_x = 0.0f;          // Shared pan X in image pixels (sync mode)
static float g_pan_y = 0.0f;          // Shared pan Y in image pixels (sync mode)
static bool g_sync = true;            // True: panes move together
static int g_active = 0;              // Active pane index when !g_sync (0 or 1)
static float g_free_zoom[2] = {1.0f, 1.0f};
static float g_free_pan_x[2] = {0.0f, 0.0f};
static float g_free_pan_y[2] = {0.0f, 0.0f};

// Overlay toggles
static bool g_show_info = true;       // Bottom info bar
static bool g_show_help = false;      // Centered help panel

// Directory navigation state
static char **g_file_list = NULL;     // Sorted list of image paths in current dir (owned)
static int g_file_count = 0;          // Number of entries in g_file_list
static int g_file_index = -1;         // Index of current image (for g_img[0]) in list, -1 if unknown
static char g_current_dir[PATH_MAX] = {0}; // Directory of currently displayed set

// ---------------------------------------------------------------------------
// Utility: file and path helpers
// ---------------------------------------------------------------------------

/**
 * Check if a filename has a supported image extension.
 *
 * @param name Base filename (e.g., "photo.JPG"). Case-insensitive.
 * @return true if extension matches kImageExts.
 */
static bool is_image_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') return false;
    const char *ext = dot + 1;
    for (int i = 0; kImageExts[i]; i++) {
        if (strcasecmp(ext, kImageExts[i]) == 0) return true;
    }
    return false;
}

/**
 * Free the directory scan list and reset navigation state.
 *
 * Safe to call when no list is allocated.
 */
static void free_file_list(void) {
    if (!g_file_list) return;
    for (int i = 0; i < g_file_count; i++) free(g_file_list[i]);
    free(g_file_list);
    g_file_list = NULL;
    g_file_count = 0;
    g_file_index = -1;
}

/**
 * Comparator for qsort on file paths (alphabetical, case-sensitive to
 * preserve deterministic order matching `ls`).
 */
static int cmp_str(const void *a, const void *b) {
    const char * const *pa = a;
    const char * const *pb = b;
    return strcmp(*pa, *pb);
}

/**
 * Extract directory component from a path into g_current_dir and build a
 * sorted list of all images in that directory.
 *
 * Also locates the index of ref_path within the list (by basename) for
 * sequential navigation via n/p.
 *
 * @param ref_path Reference image path whose directory will be scanned.
 * @return true if directory could be opened (even if empty).
 */
static bool scan_current_dir(const char *ref_path) {
    free_file_list();

    // Resolve directory: use dirname on a copy to avoid mutating ref_path.
    char tmp[PATH_MAX];
    strncpy(tmp, ref_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *dir = dirname(tmp);

    // Normalize to absolute path when possible for stable titles.
    char abs_dir[PATH_MAX];
    if (realpath(dir, abs_dir)) {
        strncpy(g_current_dir, abs_dir, sizeof(g_current_dir) - 1);
    } else {
        strncpy(g_current_dir, dir, sizeof(g_current_dir) - 1);
    }
    g_current_dir[sizeof(g_current_dir) - 1] = '\0';

    DIR *d = opendir(g_current_dir);
    if (!d) return false;

    // Collect matching entries.
    size_t cap = 64;
    g_file_list = malloc(cap * sizeof(char *));
    if (!g_file_list) { closedir(d); return false; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        // Skip hidden files and directories.
        if (ent->d_name[0] == '.') continue;
        if (!is_image_file(ent->d_name)) continue;

        // Verify it is a regular file (avoid directories named *.jpg).
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", g_current_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (g_file_count >= (int)cap) {
            cap *= 2;
            char **n = realloc(g_file_list, cap * sizeof(char *));
            if (!n) break;
            g_file_list = n;
        }
        g_file_list[g_file_count] = strdup(full);
        if (g_file_list[g_file_count]) g_file_count++;
    }
    closedir(d);

    if (g_file_count > 1) qsort(g_file_list, g_file_count, sizeof(char *), cmp_str);

    // Locate ref_path's basename in the sorted list.
    const char *base = strrchr(ref_path, '/');
    base = base ? base + 1 : ref_path;
    // Also try absolute resolution for comparison robustness.
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
    // Fallback: if not found, try full string match.
    if (g_file_index == -1) {
        for (int i = 0; i < g_file_count; i++) {
            if (strcmp(g_file_list[i], ref_path) == 0) { g_file_index = i; break; }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Image loading
// ---------------------------------------------------------------------------

/**
 * Release an Image's GPU and path resources.
 *
 * Safe to call on a zero-initialized or already-freed Image.
 */
static void unload_image(Image *im) {
    if (im->tex) SDL_DestroyTexture(im->tex);
    if (im->path) free(im->path);
    memset(im, 0, sizeof(*im));
}

/**
 * Decode an image file and upload it as an SDL texture.
 *
 * Uses stb_image to decode to 32-bit RGBA regardless of source format,
 * then creates an SDL_Surface wrapping that buffer and converts it to a
 * texture. Linear filtering is enabled for smooth zoomed rendering.
 *
 * @param path Filesystem path to the image (stored as owned copy in out->path).
 * @param out  Output Image struct to populate on success. Caller must unload
 *             it before reuse.
 * @return true on success, false on failure (error already printed).
 */
static bool load_image(const char *path, Image *out) {
    int w, h, comp;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (!data) {
        fprintf(stderr, "stbi_load failed '%s': %s\n", path, stbi_failure_reason());
        return false;
    }

    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        fprintf(stderr, "SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        stbi_image_free(data);
        return false;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
    SDL_FreeSurface(surf);
    stbi_image_free(data);

    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    // Enable bilinear filtering so zoomed images remain smooth.
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    out->tex = tex;
    out->w = w;
    out->h = h;
    out->path = strdup(path);
    return true;
}

/**
 * Replace the image in a specific pane with a new file.
 *
 * Handles texture cleanup, load, and directory rescan. On failure the pane
 * is left empty (caller may decide to keep old image; this version frees
 * first for simplicity - navigation callers handle missing files gracefully).
 *
 * @param pane 0 or 1
 * @param path New file path
 * @return true on success
 */
static bool replace_image(int pane, const char *path) {
    if (pane < 0 || pane > 1) return false;
    // Keep old image on failure to avoid blanking the pane.
    Image tmp = {0};
    if (!load_image(path, &tmp)) return false;
    unload_image(&g_img[pane]);
    g_img[pane] = tmp;
    // Ensure g_count reflects occupancy.
    if (pane >= g_count) g_count = pane + 1;
    // Refresh directory index to the new file if it is in the same folder.
    // If the file is from a different directory, rescan that directory.
    if (pane == 0 || g_count == 1) {
        scan_current_dir(path);
    }
    return true;
}

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------

/**
 * Reset zoom and pan to fit all loaded images inside their panes.
 *
 * For a single image, computes the largest uniform scale that fits the
 * image inside the full window. For dual panes, computes per-pane fit
 * scales and takes the minimum so both images are fully visible with the
 * same synchronized zoom. Small images (smaller than pane) are shown at
 * 1:1 rather than upscaled.
 *
 * In free (non-sync) mode, each pane's transform is reset individually to
 * its own fit scale.
 */
static void fit_view(void) {
    if (g_count == 0) return;

    if (g_sync) {
        if (g_count == 1) {
            float zx = (float)g_win_w / (float)g_img[0].w;
            float zy = (float)g_win_h / (float)g_img[0].h;
            g_zoom = zx < zy ? zx : zy;
            if (g_zoom > 1.0f) g_zoom = 1.0f;
        } else {
            float pane_w = (float)g_win_w / 2.0f;
            float z0x = pane_w / (float)g_img[0].w;
            float z0y = (float)g_win_h / (float)g_img[0].h;
            float z1x = pane_w / (float)g_img[1].w;
            float z1y = (float)g_win_h / (float)g_img[1].h;
            float z0 = z0x < z0y ? z0x : z0y;
            float z1 = z1x < z1y ? z1x : z1y;
            g_zoom = z0 < z1 ? z0 : z1;
            if (g_zoom > 1.0f) g_zoom = 1.0f;
        }
        g_pan_x = 0;
        g_pan_y = 0;
        // Keep free transforms in sync for seamless toggling.
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = 0;
            g_free_pan_y[i] = 0;
        }
    } else {
        // Per-pane fit
        for (int i = 0; i < g_count; i++) {
            float pane_w = (g_count == 1) ? (float)g_win_w : (float)g_win_w / 2.0f;
            float zx = pane_w / (float)g_img[i].w;
            float zy = (float)g_win_h / (float)g_img[i].h;
            float z = zx < zy ? zx : zy;
            if (z > 1.0f) z = 1.0f;
            g_free_zoom[i] = z;
            g_free_pan_x[i] = 0;
            g_free_pan_y[i] = 0;
        }
    }
}

/**
 * Apply a zoom factor centered on a specific screen coordinate.
 *
 * Keeps the image point under the cursor stationary by compensating pan.
 * Derivation: pan is in image-space, so world point under cursor is
 *   world = (cursor - window_center) / zoom - pan
 * Setting world_before == world_after and solving for pan_new yields the
 * adjustment below. This provides intuitive cursor-anchored zoom.
 *
 * In sync mode the shared transform is adjusted; otherwise only the active
 * pane's transform is affected.
 *
 * @param factor Multiplicative zoom factor (>1 zooms in, <1 out).
 * @param mx     Cursor X in window coordinates.
 * @param my     Cursor Y in window coordinates.
 */
static void do_zoom(float factor, int mx, int my) {
    if (g_sync) {
        float old = g_zoom;
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
        if (p >= g_count) p = 0;
        float old = g_free_zoom[p];
        float next = old * factor;
        if (next < 0.05f) next = 0.05f;
        if (next > 32.0f) next = 32.0f;
        if (next == old) return;
        // For free mode, center compensation is per-pane.
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
 * Apply a pan delta (in screen pixels).
 *
 * The delta is divided by zoom to keep drag speed consistent across scales.
 *
 * @param dx Horizontal screen delta.
 * @param dy Vertical screen delta.
 */
static void do_pan(int dx, int dy) {
    if (g_sync) {
        g_pan_x += (float)dx / g_zoom;
        g_pan_y += (float)dy / g_zoom;
        for (int i = 0; i < 2; i++) {
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
    } else {
        int p = g_active;
        if (p >= g_count) p = 0;
        g_free_pan_x[p] += (float)dx / g_free_zoom[p];
        g_free_pan_y[p] += (float)dy / g_free_zoom[p];
    }
}

/**
 * Toggle between synchronized and free (per-pane) navigation.
 *
 * When entering free mode, both panes inherit the current shared transform
 * so there is no visual jump. When re-entering sync mode, the active pane's
 * transform becomes the shared one.
 */
static void toggle_sync(void) {
    if (g_sync) {
        // Sync -> Free: copy shared to both panes.
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
        g_sync = false;
    } else {
        // Free -> Sync: use active pane as source.
        int p = g_active;
        if (p >= g_count) p = 0;
        g_zoom = g_free_zoom[p];
        g_pan_x = g_free_pan_x[p];
        g_pan_y = g_free_pan_y[p];
        g_sync = true;
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

/**
 * Navigate to the next/previous image in the current directory.
 *
 * Replaces the active pane (or the single pane) with the target file.
 * Wraps around at the ends. Preserves current zoom/pan unless fit is
 * explicitly requested.
 *
 * @param delta +1 for next, -1 for previous.
 * @return true if navigation succeeded and image was loaded.
 */
static bool navigate(int delta) {
    if (g_file_count == 0 || g_file_index < 0) return false;
    int next = g_file_index + delta;
    // Wrap around for continuous browsing.
    if (next < 0) next = g_file_count - 1;
    if (next >= g_file_count) next = 0;
    if (next == g_file_index) return false;

    int pane = (g_count == 1) ? 0 : g_active;
    const char *target = g_file_list[next];
    Image tmp = {0};
    if (!load_image(target, &tmp)) return false;

    // Success: replace pane and update index.
    unload_image(&g_img[pane]);
    g_img[pane] = tmp;
    g_file_index = next;
    // Update current_dir's scan to keep index consistent; g_file_list remains
    // valid but we update g_file_index only.
    return true;
}

/**
 * Navigate to the parent directory's first image.
 *
 * Computes parent of g_current_dir, scans it, and loads its first image
 * into the primary pane. No-op if already at filesystem root or parent
 * contains no images.
 *
 * @return true if parent image was loaded.
 */
static bool go_parent(void) {
    if (g_current_dir[0] == '\0') return false;
    // Handle root: dirname("/") == "/"
    char tmp[PATH_MAX];
    strncpy(tmp, g_current_dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *parent = dirname(tmp);
    if (strcmp(parent, g_current_dir) == 0) return false; // already root
    if (strcmp(parent, ".") == 0) return false;

    DIR *d = opendir(parent);
    if (!d) return false;

    // Quick check: does parent contain any image?
    bool has_image = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (is_image_file(ent->d_name)) { has_image = true; break; }
    }
    closedir(d);
    if (!has_image) return false;

    // Find first image in parent (sorted).
    char parent_abs[PATH_MAX];
    if (realpath(parent, parent_abs)) parent = parent_abs;

    // Temporarily scan parent to find its first file.
    char saved_dir[PATH_MAX];
    strncpy(saved_dir, g_current_dir, sizeof(saved_dir));
    char **saved_list = g_file_list;
    int saved_count = g_file_count;
    int saved_index = g_file_index;
    g_file_list = NULL; g_file_count = 0; g_file_index = -1;

    // Build parent list by reusing scan logic with a dummy file.
    char dummy[PATH_MAX];
    snprintf(dummy, sizeof(dummy), "%s/dummy.jpg", parent);
    scan_current_dir(dummy);

    bool ok = false;
    if (g_file_count > 0) {
        const char *first = g_file_list[0];
        // Restore old list before loading to avoid double-free confusion.
        // Keep parent list for navigation after load.
        char **parent_list = g_file_list;
        int parent_count = g_file_count;
        int parent_index = 0; // first

        // Restore saved to free them separately if load fails?
        // We will replace g_file_list with parent_list on success.
        // First, free the saved list's memory after we decide.
        // Load image before discarding saved list.
        Image tmp = {0};
        if (load_image(first, &tmp)) {
            // Free old image and directory state.
            unload_image(&g_img[0]);
            g_img[0] = tmp;
            // Free old list
            for (int i = 0; i < saved_count; i++) free(saved_list[i]);
            free(saved_list);
            // Adopt parent list
            g_file_list = parent_list;
            g_file_count = parent_count;
            g_file_index = parent_index;
            strncpy(g_current_dir, parent, sizeof(g_current_dir) - 1);
            ok = true;
        } else {
            // Load failed: restore saved state.
            for (int i = 0; i < parent_count; i++) free(parent_list[i]);
            free(parent_list);
            g_file_list = saved_list;
            g_file_count = saved_count;
            g_file_index = saved_index;
            strncpy(g_current_dir, saved_dir, sizeof(g_current_dir) - 1);
        }
    } else {
        // No files in parent, restore.
        free_file_list();
        g_file_list = saved_list;
        g_file_count = saved_count;
        g_file_index = saved_index;
        strncpy(g_current_dir, saved_dir, sizeof(g_current_dir) - 1);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Fullscreen and title
// ---------------------------------------------------------------------------

/**
 * Update the window title to reflect current file(s), zoom, and mode.
 *
 * Format: "file1 [+ file2] — 125% — SYNC|FREE [active] — i/h for help"
 * Basenames are used to keep the title concise. The title is the primary
 * textual info display; the in-window overlay mirrors it.
 */
static void update_title(void) {
    char b0[256] = {0}, b1[256] = {0};
    if (g_count > 0 && g_img[0].path) {
        const char *b = strrchr(g_img[0].path, '/');
        strncpy(b0, b ? b + 1 : g_img[0].path, sizeof(b0) - 1);
    }
    if (g_count > 1 && g_img[1].path) {
        const char *b = strrchr(g_img[1].path, '/');
        strncpy(b1, b ? b + 1 : g_img[1].path, sizeof(b1) - 1);
    }

    float z = g_sync ? g_zoom : g_free_zoom[g_active];
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

/**
 * Toggle fullscreen mode, preserving windowed geometry for restore.
 */
static void toggle_fullscreen(void) {
    if (!g_fullscreen) {
        // Save windowed geometry before entering fullscreen.
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

// ---------------------------------------------------------------------------
// Text rendering (8x8 bitmap font)
// ---------------------------------------------------------------------------

/**
 * Draw a single 8x8 glyph at integer position (x,y) with foreground color.
 *
 * Each glyph is 8 rows of 8 bits; a set bit draws a pixel. The glyph is
 * scaled by `scale` (1 = 8px, 2 = 16px, etc.) using filled rectangles for
 * crisp pixel doubling.
 *
 * @param x     Left edge in window coordinates.
 * @param y     Top edge in window coordinates.
 * @param c     ASCII character (0-127, only 32-127 have visible glyphs).
 * @param col   Foreground color.
 * @param scale Integer scale factor (1 or 2 recommended for overlay).
 */
static void draw_char(SDL_Renderer *ren, int x, int y, char c, SDL_Color col, int scale) {
    if (c < 0 || c >= 128) return;
    unsigned char uc = (unsigned char)c;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)font8x8_basic[uc][row];
        for (int bit = 0; bit < 8; bit++) {
            if (bits & (1 << bit)) {
                SDL_Rect r = { x + bit * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(ren, &r);
            }
        }
    }
}

/**
 * Draw a NUL-terminated string left-to-right.
 *
 * @param ren   Renderer.
 * @param x     Starting X.
 * @param y     Starting Y.
 * @param text  String to draw (ASCII).
 * @param col   Color.
 * @param scale Glyph scale.
 * @return X coordinate after the last character (for chaining).
 */
static int draw_text(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale) {
    int cx = x;
    for (const char *p = text; *p; p++) {
        draw_char(ren, cx, y, *p, col, 1 * scale);
        cx += 8 * scale;
    }
    return cx;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/**
 * Render the bottom info bar (when g_show_info is true).
 *
 * The bar is a semi-transparent rectangle with textual info drawn on top:
 * filename(s), dimensions, zoom, sync state, and navigation index.
 */
static void render_info_bar(SDL_Renderer *ren) {
    if (!g_show_info) return;

    int bar_h = 22;
    SDL_Rect bar = {0, g_win_h - bar_h, g_win_w, bar_h};
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
    SDL_RenderFillRect(ren, &bar);

    char line[1024];
    float z = g_sync ? g_zoom : g_free_zoom[g_active];
    int pct = (int)(z * 100.0f + 0.5f);
    if (g_count == 1) {
        const char *b = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        b = b ? b + 1 : (g_img[0].path ? g_img[0].path : "?");
        snprintf(line, sizeof(line), "%s  %dx%d  %d%%  %s  %d/%d  [s]ync [Tab] pane [f]ull [n/p] next/prev [ESC] parent",
            b, g_img[0].w, g_img[0].h, pct, g_sync ? "SYNC" : "FREE", g_file_index + 1, g_file_count);
    } else if (g_count == 2) {
        const char *b0 = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        const char *b1 = g_img[1].path ? strrchr(g_img[1].path, '/') : NULL;
        b0 = b0 ? b0 + 1 : (g_img[0].path ? g_img[0].path : "?");
        b1 = b1 ? b1 + 1 : (g_img[1].path ? g_img[1].path : "?");
        snprintf(line, sizeof(line), "%s (%dx%d) | %s (%dx%d)  %d%%  %s%s",
            b0, g_img[0].w, g_img[0].h, b1, g_img[1].w, g_img[1].h, pct,
            g_sync ? "SYNC" : "FREE", g_sync ? "" : (g_active == 0 ? " [L*]" : " [R*]"));
    } else {
        return;
    }
    SDL_Color white = {220, 220, 220, 255};
    // Truncate if too wide for window.
    int max_chars = g_win_w / 8 - 1;
    if ((int)strlen(line) > max_chars) line[max_chars] = '\0';
    draw_text(ren, 6, g_win_h - bar_h + 7, line, white, 1);
}

/**
 * Render the centered help panel (when g_show_help is true).
 *
 * Displays a modal box with key bindings. The panel is centered and
 * captures no input except the toggle key; the underlying image remains
 * visible dimmed.
 */
static void render_help(SDL_Renderer *ren) {
    if (!g_show_help) return;

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
        "ESC              Exit fullscreen / Go to parent folder",
        "q                Quit",
        "Drag & drop      Drop onto left/right half to replace",
        "",
        "Press h or ESC to close",
        NULL
    };
    int count = 0;
    while (lines[count]) count++;

    int panel_w = 520;
    int panel_h = count * 14 + 24;
    int px = (g_win_w - panel_w) / 2;
    int py = (g_win_h - panel_h) / 2;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    // Dim background
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_Rect dim = {0, 0, g_win_w, g_win_h};
    SDL_RenderFillRect(ren, &dim);

    // Panel background
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
        // Center title
        int tx = px + 16;
        if (i == 0) tx = px + (panel_w - (int)strlen(lines[i]) * 8) / 2;
        draw_text(ren, tx, y, lines[i], col, 1);
        y += 14;
    }
}

/**
 * Render all panes with the current view transform.
 *
 * Each pane is clipped via SDL_RenderSetClipRect so images do not bleed
 * across the divider. Destination rectangles are computed from the active
 * transform (shared or per-pane), centered within each pane, then drawn.
 * Active pane highlight and divider are drawn on top, followed by overlays.
 *
 * @param ren Active SDL renderer (target is the window framebuffer).
 */
static void render(SDL_Renderer *ren) {
    SDL_SetRenderDrawColor(ren, 18, 18, 18, 255);
    SDL_RenderClear(ren);

    if (g_count == 1) {
        // Single-pane: image centered in full window using active transform.
        Image *im = &g_img[0];
        float z = g_sync ? g_zoom : g_free_zoom[0];
        float px = g_sync ? g_pan_x : g_free_pan_x[0];
        float py = g_sync ? g_pan_y : g_free_pan_y[0];
        SDL_Rect clip = {0, 0, g_win_w, g_win_h};
        SDL_RenderSetClipRect(ren, &clip);

        float dw = (float)im->w * z;
        float dh = (float)im->h * z;
        float dx = (float)g_win_w * 0.5f - dw * 0.5f + px * z;
        float dy = (float)g_win_h * 0.5f - dh * 0.5f + py * z;
        SDL_FRect dst = {dx, dy, dw, dh};
        SDL_RenderCopyF(ren, im->tex, NULL, &dst);
        SDL_RenderSetClipRect(ren, NULL);

        // Active pane highlight in free mode (full window border)
        if (!g_sync && g_active == 0) {
            SDL_SetRenderDrawColor(ren, 100, 160, 255, 120);
            SDL_Rect hl = {1, 1, g_win_w - 2, g_win_h - 2};
            SDL_RenderDrawRect(ren, &hl);
        }
    } else if (g_count == 2) {
        for (int i = 0; i < 2; i++) {
            Image *im = &g_img[i];
            float z, px, py;
            if (g_sync) { z = g_zoom; px = g_pan_x; py = g_pan_y; }
            else { z = g_free_zoom[i]; px = g_free_pan_x[i]; py = g_free_pan_y[i]; }

            int pane_w = g_win_w / 2;
            SDL_Rect clip = {i * pane_w, 0, pane_w, g_win_h};
            if (i == 1) {
                clip.x = pane_w;
                clip.w = g_win_w - pane_w;
            }
            SDL_RenderSetClipRect(ren, &clip);

            float dw = (float)im->w * z;
            float dh = (float)im->h * z;
            float pane_cx = (float)clip.x + (float)clip.w * 0.5f;
            float pane_cy = (float)g_win_h * 0.5f;
            float dx = pane_cx - dw * 0.5f + px * z;
            float dy = pane_cy - dh * 0.5f + py * z;
            SDL_FRect dst = {dx, dy, dw, dh};
            SDL_RenderCopyF(ren, im->tex, NULL, &dst);
            SDL_RenderSetClipRect(ren, NULL);

            // Highlight active pane in free mode
            if (!g_sync && g_active == i) {
                SDL_SetRenderDrawColor(ren, 100, 160, 255, 200);
                SDL_Rect hl = {clip.x + 1, 1, clip.w - 2, g_win_h - 2};
                SDL_RenderDrawRect(ren, &hl);
            }
        }
        // Visual separator between panes
        SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
        int mid = g_win_w / 2;
        SDL_RenderDrawLine(ren, mid, 0, mid, g_win_h);
    }

    render_info_bar(ren);
    render_help(ren);
    SDL_RenderPresent(ren);
}

// ---------------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------------

/**
 * Print usage information to stderr.
 *
 * @param prog Program name (argv[0]) for the usage line.
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <image1> [image2]\n"
        "  Single image -> full window\n"
        "  Two images   -> side-by-side, synchronized zoom/pan\n"
        "\n"
        "Controls:\n"
        "  Mouse wheel        Zoom (cursor-centered)\n"
        "  Left drag          Pan\n"
        "  0 / F              Fit to window\n"
        "  1                  100%% (1:1)\n"
        "  + / -              Zoom in/out\n"
        "  f / F11            Toggle fullscreen\n"
        "  i                  Toggle info bar\n"
        "  h / ?              Toggle help\n"
        "  s                  Toggle sync\n"
        "  Tab                Switch active pane (when unsynced)\n"
        "  n / Right / PgDn   Next image in folder\n"
        "  p / Left / PgUp    Previous image\n"
        "  ESC                Exit fullscreen / Go to parent folder\n"
        "  q                  Quit\n"
        "  Drag & drop        Drop file onto pane to replace it\n",
        prog);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return argc == 1 ? 0 : 1;
    }

    g_count = argc - 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    g_win = SDL_CreateWindow(
        "c-image-viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Prefer accelerated renderer with vsync for smooth panning. Fall back
    // to software renderer on systems without GPU acceleration.
    g_ren = SDL_CreateRenderer(g_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) {
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    // Load all requested images upfront. Failure to load any image is fatal
    // to avoid showing a partially valid comparison.
    for (int i = 0; i < g_count; i++) {
        if (!load_image(argv[i + 1], &g_img[i])) {
            for (int j = 0; j < i; j++) unload_image(&g_img[j]);
            SDL_DestroyRenderer(g_ren);
            SDL_DestroyWindow(g_win);
            SDL_Quit();
            return 1;
        }
    }

    SDL_GetWindowSize(g_win, &g_win_w, &g_win_h);
    fit_view();
    // Build navigation index from the first image's directory.
    scan_current_dir(g_img[0].path);
    update_title();

    // Enable drag-and-drop events.
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool dragging = false;
    int last_x = 0, last_y = 0;
    bool running = true;

    // Main event/render loop. Polls events, updates view state, and renders
    // at display refresh rate (vsync) or ~60fps fallback.
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    g_win_w = ev.window.data1;
                    g_win_h = ev.window.data2;
                }
                break;
            case SDL_DROPFILE: {
                // Determine target pane by drop position. For single-pane,
                // always replace pane 0. For dual, left half -> 0, right -> 1.
                char *dropped = ev.drop.file; // must be freed with SDL_free
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);

                int target = 0;
                if (g_count == 2) {
                    target = (mx < g_win_w / 2) ? 0 : 1;
                } else {
                    // Single -> check if dropped file is valid first.
                    target = 0;
                }

                if (is_image_file(dropped)) {
                    if (replace_image(target, dropped)) {
                        fit_view();
                        update_title();
                    } else {
                        fprintf(stderr, "Failed to load dropped file: %s\n", dropped);
                    }
                } else {
                    fprintf(stderr, "Unsupported file type (drop): %s\n", dropped);
                }
                SDL_free(dropped);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    // In free mode, clicking a pane makes it active.
                    if (!g_sync && g_count == 2) {
                        g_active = (ev.button.x < g_win_w / 2) ? 0 : 1;
                        update_title();
                    }
                    dragging = true;
                    last_x = ev.button.x;
                    last_y = ev.button.y;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    int dx = ev.motion.x - last_x;
                    int dy = ev.motion.y - last_y;
                    do_pan(dx, dy);
                    update_title();
                    last_x = ev.motion.x;
                    last_y = ev.motion.y;
                }
                break;
            case SDL_MOUSEWHEEL: {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                float factor = ev.wheel.y > 0 ? 1.1f : 0.9f;
                if (ev.wheel.y > 1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (ev.wheel.y < -1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (factor < 0.2f) factor = 0.2f;
                do_zoom(factor, mx, my);
                update_title();
                break;
            }
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_q:
                    running = false;
                    break;
                case SDLK_ESCAPE:
                    if (g_show_help) {
                        g_show_help = false;
                    } else if (g_fullscreen) {
                        toggle_fullscreen();
                        update_title();
                    } else {
                        // Try to go to parent folder; if not possible, Quit is via 'q'.
                        // To keep ESC useful for navigation, attempt parent traversal.
                        if (!go_parent()) {
                            // No parent to go to - do nothing (user can press q to quit).
                            // This avoids accidental quit when trying to navigate.
                        } else {
                            fit_view();
                            update_title();
                        }
                    }
                    break;
                case SDLK_0:
                    fit_view();
                    update_title();
                    break;
                case SDLK_f:
                    if (ev.key.keysym.mod & KMOD_CTRL) {
                        fit_view();
                    } else {
                        toggle_fullscreen();
                    }
                    update_title();
                    break;
                case SDLK_F11:
                    toggle_fullscreen();
                    update_title();
                    break;
                case SDLK_1:
                case SDLK_KP_1:
                    if (g_sync) {
                        g_zoom = 1.0f; g_pan_x = 0; g_pan_y = 0;
                        for (int i = 0; i < 2; i++) { g_free_zoom[i] = 1.0f; g_free_pan_x[i] = 0; g_free_pan_y[i] = 0; }
                    } else {
                        int p = (g_active < g_count) ? g_active : 0;
                        g_free_zoom[p] = 1.0f; g_free_pan_x[p] = 0; g_free_pan_y[p] = 0;
                    }
                    update_title();
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    do_zoom(1.1f, g_win_w/2, g_win_h/2);
                    update_title();
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    do_zoom(0.9f, g_win_w/2, g_win_h/2);
                    update_title();
                    break;
                case SDLK_i:
                    g_show_info = !g_show_info;
                    break;
                case SDLK_h:
                case SDLK_SLASH:
                    // '?' is Shift+/ on US layouts; also handle h.
                    g_show_help = !g_show_help;
                    break;
                case SDLK_s:
                    toggle_sync();
                    update_title();
                    break;
                case SDLK_TAB:
                    if (!g_sync && g_count == 2) {
                        g_active = 1 - g_active;
                        update_title();
                    }
                    break;
                case SDLK_n:
                case SDLK_RIGHT:
                case SDLK_PAGEDOWN:
                    if (navigate(+1)) { fit_view(); update_title(); }
                    break;
                case SDLK_p:
                case SDLK_LEFT:
                case SDLK_PAGEUP:
                    if (navigate(-1)) { fit_view(); update_title(); }
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }
        render(g_ren);
        SDL_Delay(16);
    }

    free_file_list();
    for (int i = 0; i < g_count; i++) unload_image(&g_img[i]);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
