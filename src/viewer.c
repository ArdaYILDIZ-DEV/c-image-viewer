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

#define STBI_THREAD_LOCAL _Thread_local
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
 * Safely join a directory path and filename into dst buffer.
 *
 * Handles root slashes, trailing slashes on dir, leading slashes on file,
 * and enforces buffer bounds to prevent overflow.
 */
bool viewer_path_join(char *dst, size_t dst_size, const char *dir, const char *file) {
    if (!dst || dst_size == 0 || !dir || !file) return false;

    size_t dlen = strlen(dir);
    if (dlen == 0) {
        size_t flen = strlen(file);
        if (flen >= dst_size) return false;
        memcpy(dst, file, flen + 1);
        return true;
    }

    while (*file == '/') {
        file++;
    }

    while (dlen > 1 && dir[dlen - 1] == '/') {
        dlen--;
    }

    bool needs_slash = (dir[dlen - 1] != '/' && *file != '\0');
    size_t flen = strlen(file);
    if (flen > SIZE_MAX - dlen - 2) return false;
    size_t total = dlen + (needs_slash ? 1 : 0) + flen;
    if (total >= dst_size) return false;

    memcpy(dst, dir, dlen);
    if (needs_slash) {
        dst[dlen] = '/';
        memcpy(dst + dlen + 1, file, flen + 1);
    } else {
        memcpy(dst + dlen, file, flen + 1);
    }
    return true;
}

/**
 * Truncate a filename to fit within max_len characters, preserving the file extension.
 *
 * If name is longer than max_len, stems are truncated with "..." before the extension.
 * If there is no extension or it is too long, suffix truncation is applied.
 * Guarantees safe NUL termination for all max_len and out_sz values (including <= 0).
 *
 * @param name Source filename or path string to truncate.
 * @param out Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 * @param max_len Maximum allowable output character count (excluding NUL).
 */
void viewer_truncate_filename(const char *name, char *out, size_t out_sz, int max_len) {
    if (!out || out_sz == 0) return;
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    if (!name || max_len <= 0) {
        out[0] = '\0';
        return;
    }
    if (max_len >= (int)out_sz) {
        max_len = (int)out_sz - 1;
    }
    if (max_len <= 0) {
        out[0] = '\0';
        return;
    }
    int len = (int)strlen(name);
    if (len <= max_len) {
        memcpy(out, name, (size_t)len);
        out[len] = '\0';
        return;
    }
    if (max_len <= 3) {
        memcpy(out, "...", (size_t)max_len);
        out[max_len] = '\0';
        return;
    }
    const char *dot = strrchr(name, '.');
    if (dot && dot != name && *(dot + 1) != '\0') {
        const char *ext = dot + 1;
        int ext_len = len - (int)(ext - name);
        if (ext_len <= max_len - 4) {
            int prefix_len = max_len - 3 - ext_len;
            memcpy(out, name, (size_t)prefix_len);
            memcpy(out + prefix_len, "...", 3);
            memcpy(out + prefix_len + 3, ext, (size_t)ext_len);
            out[max_len] = '\0';
            return;
        }
    }
    int prefix_len = max_len - 3;
    memcpy(out, name, (size_t)prefix_len);
    memcpy(out + prefix_len, "...", 3);
    out[max_len] = '\0';
}

/**
 * Format image color depth / channel configuration into a human-readable string.
 *
 * Maps standard STB channel counts (1, 2, 3, 4) to descriptive names (e.g.
 * "24-bit RGB", "32-bit RGBA"), formats custom channel counts as "%d channels",
 * and maps <= 0 to "Unknown".
 *
 * @param channels Number of color channels in source image.
 * @param out Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 */
void viewer_format_color_depth(int channels, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    if (channels <= 0) {
        snprintf(out, out_sz, "Unknown");
    } else if (channels == 1) {
        snprintf(out, out_sz, "8-bit Grayscale");
    } else if (channels == 2) {
        snprintf(out, out_sz, "16-bit Gray+Alpha");
    } else if (channels == 3) {
        snprintf(out, out_sz, "24-bit RGB");
    } else if (channels == 4) {
        snprintf(out, out_sz, "32-bit RGBA");
    } else {
        snprintf(out, out_sz, "%d channels", channels);
    }
    out[out_sz - 1] = '\0';
}

/**
 * Truncate a filesystem path to fit within max_chars by middle truncation.
 *
 * Preserves the basename and as much of the root/leading directory prefix as
 * fits, inserting ".../" in between. If the basename itself exceeds max_chars,
 * falls back to viewer_truncate_filename. If max_chars is very small (<= 5),
 * outputs dots.
 *
 * @param path Input filesystem path.
 * @param out Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 * @param max_chars Maximum allowable string length (excluding NUL).
 */
void viewer_truncate_path(const char *path, char *out, size_t out_sz, int max_chars) {
    if (!out || out_sz == 0) return;
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    if (!path || max_chars <= 0) {
        out[0] = '\0';
        return;
    }
    if (max_chars >= (int)out_sz) {
        max_chars = (int)out_sz - 1;
    }
    if (max_chars <= 0) {
        out[0] = '\0';
        return;
    }

    int path_len = (int)strlen(path);
    if (path_len <= max_chars) {
        memcpy(out, path, (size_t)path_len);
        out[path_len] = '\0';
        return;
    }

    if (max_chars <= 5) {
        memcpy(out, ".....", (size_t)max_chars);
        out[max_chars] = '\0';
        return;
    }

    const char *slash = strrchr(path, '/');
    const char *filename = slash ? slash + 1 : path;
    if (*filename == '\0') {
        int prefix_len = max_chars - 3;
        if (prefix_len > 0) {
            memcpy(out, path, (size_t)prefix_len);
            memcpy(out + prefix_len, "...", 3);
            out[max_chars] = '\0';
        } else {
            memcpy(out, ".....", (size_t)max_chars);
            out[max_chars] = '\0';
        }
        return;
    }

    int filename_len = path_len - (int)(filename - path);
    if (filename_len > max_chars) {
        viewer_truncate_filename(filename, out, out_sz, max_chars);
        return;
    }

    int prefix_len = max_chars - (filename_len + 4);
    if (prefix_len >= 1) {
        memcpy(out, path, (size_t)prefix_len);
        memcpy(out + prefix_len, ".../", 4);
        memcpy(out + prefix_len + 4, filename, (size_t)filename_len);
        out[max_chars] = '\0';
    } else if (filename_len + 4 <= max_chars) {
        memcpy(out, ".../", 4);
        memcpy(out + 4, filename, (size_t)filename_len);
        out[4 + filename_len] = '\0';
    } else {
        viewer_truncate_filename(filename, out, out_sz, max_chars);
    }
}

/**
 * Format file size in bytes into a human-readable string with byte count.
 *
 * Formats sizes into B, KB, MB, or GB with exact byte count appended in
 * parentheses for values >= 1024. Formats negative values and 0 as "0 B".
 *
 * @param size File size in bytes (from stat st_size).
 * @param out Destination character buffer.
 * @param out_sz Size of destination buffer in bytes.
 */
void viewer_format_file_size(off_t size, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    if (size <= 0) {
        snprintf(out, out_sz, "0 B");
    } else if (size < 1024) {
        snprintf(out, out_sz, "%lld B", (long long)size);
    } else if (size < 1024LL * 1024) {
        snprintf(out, out_sz, "%.1f KB (%lld B)", (double)size / 1024.0, (long long)size);
    } else if (size < 1024LL * 1024 * 1024) {
        snprintf(out, out_sz, "%.1f MB (%lld B)", (double)size / (1024.0 * 1024.0), (long long)size);
    } else {
        snprintf(out, out_sz, "%.2f GB (%lld B)", (double)size / (1024.0 * 1024.0 * 1024.0), (long long)size);
    }
    out[out_sz - 1] = '\0';
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
    if (!name) return false;
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
    if (g_file_list) {
        for (int i = 0; i < g_file_count; i++) free(g_file_list[i]);
        free(g_file_list);
        g_file_list = NULL;
    }
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
    g_file_count = 0;
    g_file_index = -1;
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
        if (!viewer_path_join(full, sizeof(full), g_current_dir, ent->d_name)) continue;
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

// Static metadata and EXIF cache to avoid repeated disk reads and allocations per frame
static char s_cached_md_path[PATH_MAX] = {0};
static ExifData s_cached_exif;
static struct stat s_cached_st;
static bool s_has_stat = false;
static char s_cached_size_str[64] = "?";
static char s_cached_mtime_str[64] = "?";
static char s_cached_path_disp[PATH_MAX] = {0};

/**
 * Reset the cached metadata and EXIF data.
 *
 * Clears s_cached_md_path and zeroes cached structs so subsequent calls to
 * viewer_render_metadata will re-stat and re-parse EXIF from disk.
 */
void viewer_reset_metadata_cache(void) {
    s_cached_md_path[0] = '\0';
    memset(&s_cached_exif, 0, sizeof(s_cached_exif));
    memset(&s_cached_st, 0, sizeof(s_cached_st));
    s_has_stat = false;
    snprintf(s_cached_size_str, sizeof(s_cached_size_str), "?");
    snprintf(s_cached_mtime_str, sizeof(s_cached_mtime_str), "?");
    s_cached_path_disp[0] = '\0';
}

/**
 * Query the cached metadata stat status and formatted size/mtime strings.
 *
 * @param size_out Optional buffer to receive cached size string.
 * @param size_sz Size of size_out buffer.
 * @param mtime_out Optional buffer to receive cached mtime string.
 * @param mtime_sz Size of mtime_out buffer.
 * @return true if s_has_stat is true, false otherwise.
 */
bool viewer_get_cached_stat_info(char *size_out, size_t size_sz, char *mtime_out, size_t mtime_sz) {
    if (size_out && size_sz > 0) {
        snprintf(size_out, size_sz, "%s", s_cached_size_str);
    }
    if (mtime_out && mtime_sz > 0) {
        snprintf(mtime_out, mtime_sz, "%s", s_cached_mtime_str);
    }
    return s_has_stat;
}

// ---------------------------------------------------------------------------
// Image lifecycle and decoding
// ---------------------------------------------------------------------------

/**
 * Decode image file from disk into a 32-bit RGBA pixel buffer.
 *
 * Runs CPU-intensive stbi_load isolated from SDL rendering contexts.
 * Returns dynamically allocated pixel buffer that caller must free via
 * stbi_image_free, or NULL on failure.
 *
 * @param path Filesystem path to image file.
 * @param out_w Destination pointer for image width in pixels.
 * @param out_h Destination pointer for image height in pixels.
 * @param out_channels Destination pointer for original channel count.
 * @return Allocated RGBA pixel buffer on success, NULL on failure.
 */
static unsigned char *decode_image_surface(const char *path, int *out_w, int *out_h, int *out_channels) {
    if (!path || !out_w || !out_h || !out_channels) return NULL;

    int w = 0, h = 0, comp = 0;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (!data) {
        return NULL;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        return NULL;
    }

    *out_w = w;
    *out_h = h;
    *out_channels = comp;
    return data;
}

/**
 * State machine for background image decoding tasks.
 */
typedef enum {
    ASYNC_IDLE = 0,
    ASYNC_RUNNING,
    ASYNC_DONE
} AsyncState;

/**
 * Per-pane background decode task state and synchronization handles.
 */
typedef struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    AsyncState state;
    char *path;
    unsigned char *data;
    int w;
    int h;
    int channels;
    bool success;
} AsyncDecodeTask;

static AsyncDecodeTask s_async_tasks[2];

/**
 * Worker thread entry point for off-thread image file decoding.
 *
 * Invokes decode_image_surface without holding the mutex during heavy I/O and decode.
 * Locks mutex only to record results and transition task state to ASYNC_DONE.
 *
 * Concurrency note: Runs in a dedicated background worker thread per pane.
 *
 * @param arg Pointer to AsyncDecodeTask for the target pane.
 * @return Thread status (always 0).
 */
static int async_decode_worker(void *arg) {
    AsyncDecodeTask *task = (AsyncDecodeTask *)arg;
    if (!task) return 0;

    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = decode_image_surface(task->path, &w, &h, &channels);

    if (task->mutex) SDL_LockMutex(task->mutex);
    task->data = pixels;
    task->w = w;
    task->h = h;
    task->channels = channels;
    task->success = (pixels != NULL);
    task->state = ASYNC_DONE;
    if (task->mutex) SDL_UnlockMutex(task->mutex);

    return 0;
}

/**
 * Safely wait for a pane's active worker thread and clean up allocated buffers.
 *
 * Concurrency note: Blocks caller until the target pane thread finishes via
 * SDL_WaitThread, then locks mutex to safely free decoded pixels and path.
 * Preserves task mutex for subsequent task reuse without reallocation churn.
 *
 * @param pane Target pane index (0 or 1).
 */
static void cancel_async_task(int pane) {
    if (pane < 0 || pane >= 2) return;
    AsyncDecodeTask *task = &s_async_tasks[pane];

    if (task->thread) {
        SDL_WaitThread(task->thread, NULL);
        task->thread = NULL;
    }
    if (task->mutex) {
        SDL_LockMutex(task->mutex);
    }
    if (task->data) {
        stbi_image_free(task->data);
        task->data = NULL;
    }
    if (task->path) {
        free(task->path);
        task->path = NULL;
    }
    task->state = ASYNC_IDLE;
    task->w = 0;
    task->h = 0;
    task->channels = 0;
    task->success = false;
    if (task->mutex) {
        SDL_UnlockMutex(task->mutex);
    }
}

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
    if (im == &g_img[0]) {
        cancel_async_task(0);
    } else if (im == &g_img[1]) {
        cancel_async_task(1);
    }
    if (im->tex) SDL_DestroyTexture(im->tex);
    if (im->path) free(im->path);
    memset(im, 0, sizeof(*im));
    viewer_reset_metadata_cache();
}

/**
 * Load an image from disk, decode to RGBA, and upload to an SDL GPU texture.
 *
 * Decodes pixel data using stb_image with 4 channels (RGBA32) via
 * decode_image_surface, creates an SDL texture with linear scaling mode,
 * and stores an owned copy of path. On failure, out is zero-initialized
 * and any intermediate allocations are freed.
 *
 * @param path Filesystem path to image file.
 * @param out Destination Image struct to receive texture, dimensions, and owned path.
 * @return true on successful load and texture creation, false on decode/allocation error.
 */
bool viewer_load_image(const char *path, Image *out) {
    if (!path || !out) return false;
    viewer_unload_image(out);

    int w = 0, h = 0, comp = 0;
    unsigned char *data = decode_image_surface(path, &w, &h, &comp);
    if (!data) {
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
    out->channels = comp;
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
    cancel_async_task(pane);
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
// Asynchronous image decoding
// ---------------------------------------------------------------------------

/**
 * Start asynchronous decoding of an image file for the specified pane.
 *
 * Cancels any active task on the pane, and spawns a background worker thread
 * via SDL_CreateThread to decode RGBA pixels. The current image in the pane
 * is preserved until viewer_pump_async_loads uploads the newly decoded texture.
 * Texture creation is deferred to viewer_pump_async_loads on the main thread.
 *
 * Concurrency note: Dispatches an SDL_Thread that decodes pixels off-thread
 * without holding locks or stalling the main rendering loop.
 *
 * @param pane Target pane index (0 or 1).
 * @param path Filesystem path to image file.
 * @return true if worker thread was dispatched successfully, false on invalid args or thread failure.
 */
bool viewer_load_image_async(int pane, const char *path) {
    if (pane < 0 || pane >= 2 || !path) return false;

    cancel_async_task(pane);

    if (pane >= g_count) {
        g_count = pane + 1;
    }

    AsyncDecodeTask *task = &s_async_tasks[pane];
    if (!task->mutex) {
        task->mutex = SDL_CreateMutex();
        if (!task->mutex) return false;
    }

    char *path_copy = strdup(path);
    if (!path_copy) return false;

    SDL_LockMutex(task->mutex);
    task->path = path_copy;
    task->data = NULL;
    task->w = 0;
    task->h = 0;
    task->channels = 0;
    task->success = false;
    task->state = ASYNC_RUNNING;
    SDL_UnlockMutex(task->mutex);

    char thread_name[32];
    snprintf(thread_name, sizeof(thread_name), "img_decode_%d", pane);
    task->thread = SDL_CreateThread(async_decode_worker, thread_name, task);
    if (!task->thread) {
        SDL_LockMutex(task->mutex);
        task->state = ASYNC_IDLE;
        free(task->path);
        task->path = NULL;
        SDL_UnlockMutex(task->mutex);
        return false;
    }

    return true;
}

/**
 * Query whether an asynchronous decoding task is currently active for a pane.
 *
 * Concurrency note: Thread-safe; acquires the pane task mutex to safely inspect
 * running or pending decode state.
 *
 * @param pane Target pane index (0 or 1).
 * @return true if pane is actively loading or awaiting texture creation, false otherwise.
 */
bool viewer_is_loading(int pane) {
    if (pane < 0 || pane >= 2) return false;
    AsyncDecodeTask *task = &s_async_tasks[pane];
    if (!task->mutex) return false;

    SDL_LockMutex(task->mutex);
    bool loading = (task->state == ASYNC_RUNNING || task->state == ASYNC_DONE);
    SDL_UnlockMutex(task->mutex);
    return loading;
}

/**
 * Process completed asynchronous image decode tasks on the main thread.
 *
 * Inspects both panes for finished worker threads. When a worker completes decoding,
 * joins the thread, uploads decoded RGBA pixels into an SDL GPU texture on the
 * main rendering context, frees intermediate pixel memory, unloads any previous
 * image, updates g_img[pane], and recalculates view fitting and window title.
 * On failure, cleanly unloads any previous image and reclaims task resources.
 *
 * Concurrency note: Must be called strictly from the main thread owning the
 * SDL_Renderer context to safely upload GPU textures.
 *
 * @return true if at least one async task completed and was processed, false otherwise.
 */
bool viewer_pump_async_loads(void) {
    bool processed_any = false;
    for (int pane = 0; pane < 2; pane++) {
        AsyncDecodeTask *task = &s_async_tasks[pane];
        if (!task->mutex) continue;

        SDL_LockMutex(task->mutex);
        if (task->state != ASYNC_DONE) {
            SDL_UnlockMutex(task->mutex);
            continue;
        }

        SDL_Thread *th = task->thread;
        task->thread = NULL;
        unsigned char *data = task->data;
        task->data = NULL;
        int w = task->w;
        int h = task->h;
        int channels = task->channels;
        bool success = task->success && (data != NULL);
        char *path = task->path;
        task->path = NULL;
        task->state = ASYNC_IDLE;
        SDL_UnlockMutex(task->mutex);

        if (th) {
            SDL_WaitThread(th, NULL);
        }

        if (success) {
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
                data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
            SDL_Texture *tex = (surf && g_ren) ? SDL_CreateTextureFromSurface(g_ren, surf) : NULL;
            if (surf) SDL_FreeSurface(surf);
            stbi_image_free(data);

            if (tex) {
                SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
                viewer_unload_image(&g_img[pane]);
                g_img[pane].tex = tex;
                g_img[pane].w = w;
                g_img[pane].h = h;
                g_img[pane].channels = channels;
                g_img[pane].path = path;
                viewer_fit_view();
                viewer_update_title();
            } else {
                viewer_unload_image(&g_img[pane]);
                free(path);
            }
        } else {
            viewer_unload_image(&g_img[pane]);
            if (data) stbi_image_free(data);
            free(path);
        }
        processed_any = true;
    }
    return processed_any;
}

/**
 * Wait for all running async decoding threads and release thread/task resources.
 *
 * Concurrency note: Blocks until active worker threads finish via SDL_WaitThread,
 * frees pending pixel buffers and paths, and destroys synchronization primitives.
 * Safe to call multiple times or during shutdown.
 */
void viewer_cleanup_async(void) {
    for (int i = 0; i < 2; i++) {
        cancel_async_task(i);
        AsyncDecodeTask *task = &s_async_tasks[i];
        if (task->mutex) {
            SDL_DestroyMutex(task->mutex);
            task->mutex = NULL;
        }
    }
}

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------

/**
 * Reset view scale and offsets to 1:1 pixel mapping (100% zoom, zero pan).
 *
 * Resets shared transform (g_zoom = 1.0f, g_pan_x = 0, g_pan_y = 0) and
 * each pane's independent transform in g_free_zoom and g_free_pan.
 */
void viewer_reset_1to1(void) {
    g_zoom = 1.0f;
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
    for (int i = 0; i < 2; i++) {
        g_free_zoom[i] = 1.0f;
        g_free_pan_x[i] = 0.0f;
        g_free_pan_y[i] = 0.0f;
    }
}

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
        viewer_reset_1to1();
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
    if (g_count == 0) return;
    if (factor <= 0.0f || isnan(factor) || isinf(factor)) return;
    if (g_win_w <= 0 || g_win_h <= 0) return;

    if (g_sync) {
        float old = g_zoom < 0.05f ? 0.05f : g_zoom;
        float next = old * factor;
        if (next < 0.05f) next = 0.05f;
        if (next > 32.0f) next = 32.0f;
        if (next == old) return;
        float pane_cx;
        if (g_count == 2) {
            int mid = g_win_w / 2;
            pane_cx = (mx < mid) ? ((float)mid * 0.5f) : ((float)mid + (float)(g_win_w - mid) * 0.5f);
        } else {
            pane_cx = (float)g_win_w * 0.5f;
        }
        float pane_cy = (float)g_win_h * 0.5f;
        g_pan_x += ((float)mx - pane_cx) * (1.0f / next - 1.0f / old);
        g_pan_y += ((float)my - pane_cy) * (1.0f / next - 1.0f / old);
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
        float pane_cx;
        if (g_count == 2) {
            int mid = g_win_w / 2;
            pane_cx = (p == 0) ? ((float)mid * 0.5f) : ((float)mid + (float)(g_win_w - mid) * 0.5f);
        } else {
            pane_cx = (float)g_win_w * 0.5f;
        }
        float pane_cy = (float)g_win_h * 0.5f;
        g_free_pan_x[p] += ((float)mx - pane_cx) * (1.0f / next - 1.0f / old);
        g_free_pan_y[p] += ((float)my - pane_cy) * (1.0f / next - 1.0f / old);
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
    if (g_count == 0) return;
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
        float z = g_zoom < 0.05f ? 0.05f : (g_zoom > 32.0f ? 32.0f : g_zoom);
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = z;
            g_free_pan_x[i] = g_pan_x;
            g_free_pan_y[i] = g_pan_y;
        }
        g_sync = false;
    } else {
        int p = (g_active == 1) ? 1 : 0;
        if (p >= g_count && g_count > 0) p = 0;
        float z = g_free_zoom[p] < 0.05f ? 0.05f : (g_free_zoom[p] > 32.0f ? 32.0f : g_free_zoom[p]);
        g_zoom = z;
        g_pan_x = g_free_pan_x[p];
        g_pan_y = g_free_pan_y[p];
        g_sync = true;
    }
}

/**
 * Toggle active pane between pane 0 and pane 1 in dual-pane view.
 *
 * In dual-pane mode (g_count == 2), switches g_active between 0 and 1.
 * In single-pane mode, ensures g_active is 0.
 */
void viewer_toggle_active_pane(void) {
    if (g_count == 2) {
        g_active = (g_active == 1) ? 0 : 1;
    } else {
        g_active = 0;
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

    int active_idx = (g_active == 1) ? 1 : 0;
    float z = g_sync ? g_zoom : g_free_zoom[active_idx];
    if (z < 0.05f) z = 0.05f;
    int pct = (int)(z * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    int fidx = (g_file_index >= 0) ? (g_file_index + 1) : 0;
    int fcnt = (g_file_count >= 0) ? g_file_count : 0;
    char title[1024];
    if (g_count == 1) {
        snprintf(title, sizeof(title), "%s — %d%% — %s — %d/%d — [i]nfo [h]elp [q]uit",
            b0, pct, g_sync ? "SYNC" : "FREE", fidx, fcnt);
    } else if (g_count == 2) {
        snprintf(title, sizeof(title), "%s | %s — %d%% — %s%s — [i]nfo [h]elp",
            b0, b1, pct, g_sync ? "SYNC" : "FREE",
            g_sync ? "" : (active_idx == 0 ? " [L]" : " [R]"));
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
    if (!g_file_list || g_file_count <= 1 || g_file_index < 0 || g_file_index >= g_file_count) return false;
    int step = delta >= 0 ? 1 : -1;
    int pane = (g_count <= 1) ? 0 : ((g_active == 1) ? 1 : 0);

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
            if (pane >= g_count) g_count = pane + 1;
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
    if (!viewer_path_join(dummy, sizeof(dummy), parent, "dummy.jpg")) {
        g_file_list = saved_list;
        g_file_count = saved_count;
        g_file_index = saved_index;
        return false;
    }
    viewer_scan_current_dir(dummy);

    bool ok = false;
    if (g_file_count > 0) {
        char **parent_list = g_file_list;
        int parent_count = g_file_count;

        Image tmp_img = {0};
        int chosen_idx = -1;
        for (int i = 0; i < parent_count; i++) {
            if (viewer_load_image(parent_list[i], &tmp_img)) {
                chosen_idx = i;
                break;
            }
        }

        if (chosen_idx >= 0) {
            viewer_unload_image(&g_img[0]);
            g_img[0] = tmp_img;
            for (int i = 0; i < saved_count; i++) free(saved_list[i]);
            free(saved_list);
            g_file_list = parent_list;
            g_file_count = parent_count;
            g_file_index = chosen_idx;
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

static const int s_hint_single_cost[VIEWER_HINT_TIER_COUNT] = {
    0,   // NONE: 0 chars, 0 hint_cost
    (int)sizeof("[e] exif [ESC]") - 1 + 2,
    (int)sizeof("[s]ync [f]ull [n/p] [e]xif [ESC]") - 1 + 2,
    (int)sizeof("[s]ync [Tab] pane [f]ull [n/p] next/prev [e] exif [ESC] browser") - 1 + 2
};

static const char *s_hint_dual[VIEWER_HINT_TIER_COUNT] = {
    "",                                                                   // VIEWER_HINT_NONE
    "[e] exif",                                                           // VIEWER_HINT_MINIMAL
    "[s]ync [Tab] pane [e] exif",                                         // VIEWER_HINT_COMPACT
    "[s]ync [Tab] pane [f]ull [e] exif [ESC] browser"                     // VIEWER_HINT_FULL
};

static const int s_hint_dual_cost[VIEWER_HINT_TIER_COUNT] = {
    0,   // NONE: 0 chars, 0 hint_cost
    (int)sizeof("[e] exif") - 1 + 2,
    (int)sizeof("[s]ync [Tab] pane [e] exif") - 1 + 2,
    (int)sizeof("[s]ync [Tab] pane [f]ull [e] exif [ESC] browser") - 1 + 2
};

/**
 * Return number of decimal digits required to represent a non-negative integer.
 */
static inline int viewer_int_digits(int v) {
    if (v < 10) return 1;
    if (v < 100) return 2;
    if (v < 1000) return 3;
    if (v < 10000) return 4;
    if (v < 100000) return 5;
    if (v < 1000000) return 6;
    if (v < 10000000) return 7;
    if (v < 100000000) return 8;
    if (v < 1000000000) return 9;
    return 10;
}

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
    (void)is_sync;
    ViewerStatusBarLayout layout;
    memset(&layout, 0, sizeof(layout));

    int usable_chars = 0;
    if (win_w > 2 * VIEWER_INFO_MARGIN_X) {
        usable_chars = (win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    }
    layout.usable_chars = usable_chars;

    if (img_w < 0) img_w = 0;
    if (img_h < 0) img_h = 0;
    if (zoom_pct < 0) zoom_pct = 0;
    if (file_idx < 0) file_idx = 0;
    if (file_count < 0) file_count = 0;

    // Fixed metadata length: "%dx%d  %d%%  %s  %d/%d"
    // Constants: 'x'(1) + "  "(2) + "%  "(3) + "SYNC"/"FREE"(4) + "  "(2) + '/'(1) = 13
    int meta_len = 13 + viewer_int_digits(img_w) + viewer_int_digits(img_h) +
                   viewer_int_digits(zoom_pct) + viewer_int_digits(file_idx) +
                   viewer_int_digits(file_count);

    for (int t = (int)VIEWER_HINT_FULL; t >= (int)VIEWER_HINT_NONE; t--) {
        int cost = 2 + meta_len + s_hint_single_cost[t];
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
    (void)active_pane;
    ViewerStatusBarLayout layout;
    memset(&layout, 0, sizeof(layout));

    int usable_chars = 0;
    if (win_w > 2 * VIEWER_INFO_MARGIN_X) {
        usable_chars = (win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    }
    layout.usable_chars = usable_chars;

    if (img0_w < 0) img0_w = 0;
    if (img0_h < 0) img0_h = 0;
    if (img1_w < 0) img1_w = 0;
    if (img1_h < 0) img1_h = 0;
    if (zoom_pct < 0) zoom_pct = 0;

    // Fixed dual metadata length: " (%dx%d) | " + " (%dx%d)  %d%%  %s%s"
    // p0 constants: " ("(2) + 'x'(1) + ") | "(4) = 7
    // p1 constants: " ("(2) + 'x'(1) + ")  "(3) + "%  "(3) + "SYNC"/"FREE"(4) = 13, pane_ind: is_sync ? 0 : 5
    // Total constants = 7 + 13 + (is_sync ? 0 : 5) = 20 + (is_sync ? 0 : 5)
    int fixed_meta_len = 20 + (is_sync ? 0 : 5) +
                         viewer_int_digits(img0_w) + viewer_int_digits(img0_h) +
                         viewer_int_digits(img1_w) + viewer_int_digits(img1_h) +
                         viewer_int_digits(zoom_pct);

    const char *b0 = name0 ? strrchr(name0, '/') : NULL;
    const char *fname0 = b0 ? b0 + 1 : (name0 ? name0 : "");
    const char *b1 = name1 ? strrchr(name1, '/') : NULL;
    const char *fname1 = b1 ? b1 + 1 : (name1 ? name1 : "");
    int len0 = (int)strlen(fname0);
    int len1 = (int)strlen(fname1);

    int target_dual = 2 * VIEWER_INFO_NAME_TARGET_DUAL;

    for (int t = (int)VIEWER_HINT_FULL; t >= (int)VIEWER_HINT_NONE; t--) {
        int cost = fixed_meta_len + s_hint_dual_cost[t];
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
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    out_buf[0] = '\0';
    if (!layout) return 0;

    if (img_w < 0) img_w = 0;
    if (img_h < 0) img_h = 0;
    if (zoom_pct < 0) zoom_pct = 0;
    if (file_idx < 0) file_idx = 0;
    if (file_count < 0) file_count = 0;

    const char *b = name ? strrchr(name, '/') : NULL;
    const char *fname = b ? b + 1 : (name ? name : "");

    char trunc[512];
    viewer_truncate_filename(fname, trunc, sizeof(trunc), layout->name_budget[0]);

    ViewerHintTier tier = layout->hint_tier;
    if (tier < 0 || tier >= VIEWER_HINT_TIER_COUNT) {
        tier = VIEWER_HINT_NONE;
    }
    const char *hint = s_hint_single[tier];

    int written = 0;
    if (viewer_is_loading(0)) {
        if (hint && hint[0] != '\0') {
            written = snprintf(out_buf, out_sz, "%s  Loading...  %s  %d/%d  %s",
                trunc, is_sync ? "SYNC" : "FREE",
                file_idx, file_count, hint);
        } else {
            written = snprintf(out_buf, out_sz, "%s  Loading...  %s  %d/%d",
                trunc, is_sync ? "SYNC" : "FREE",
                file_idx, file_count);
        }
    } else {
        if (hint && hint[0] != '\0') {
            written = snprintf(out_buf, out_sz, "%s  %dx%d  %d%%  %s  %d/%d  %s",
                trunc, img_w, img_h, zoom_pct, is_sync ? "SYNC" : "FREE",
                file_idx, file_count, hint);
        } else {
            written = snprintf(out_buf, out_sz, "%s  %dx%d  %d%%  %s  %d/%d",
                trunc, img_w, img_h, zoom_pct, is_sync ? "SYNC" : "FREE",
                file_idx, file_count);
        }
    }
    if (written < 0) {
        out_buf[0] = '\0';
        return 0;
    }
    if ((size_t)written >= out_sz) {
        out_buf[out_sz - 1] = '\0';
        return (int)out_sz - 1;
    }
    return written;
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
    if (out_sz > (size_t)INT_MAX) out_sz = (size_t)INT_MAX;
    out_buf[0] = '\0';
    if (!layout) return 0;

    if (img0_w < 0) img0_w = 0;
    if (img0_h < 0) img0_h = 0;
    if (img1_w < 0) img1_w = 0;
    if (img1_h < 0) img1_h = 0;
    if (zoom_pct < 0) zoom_pct = 0;
    int act = (active_pane == 1) ? 1 : 0;

    const char *b0 = name0 ? strrchr(name0, '/') : NULL;
    const char *fname0 = b0 ? b0 + 1 : (name0 ? name0 : "");
    const char *b1 = name1 ? strrchr(name1, '/') : NULL;
    const char *fname1 = b1 ? b1 + 1 : (name1 ? name1 : "");

    char trunc0[512];
    char trunc1[512];
    viewer_truncate_filename(fname0, trunc0, sizeof(trunc0), layout->name_budget[0]);
    viewer_truncate_filename(fname1, trunc1, sizeof(trunc1), layout->name_budget[1]);

    const char *pane_ind = is_sync ? "" : (act == 0 ? " [L*]" : " [R*]");

    ViewerHintTier tier = layout->hint_tier;
    if (tier < 0 || tier >= VIEWER_HINT_TIER_COUNT) {
        tier = VIEWER_HINT_NONE;
    }
    const char *hint = s_hint_dual[tier];

    char dim0[64];
    if (viewer_is_loading(0)) {
        snprintf(dim0, sizeof(dim0), "Loading...");
    } else {
        snprintf(dim0, sizeof(dim0), "%dx%d", img0_w, img0_h);
    }

    char dim1[64];
    if (viewer_is_loading(1)) {
        snprintf(dim1, sizeof(dim1), "Loading...");
    } else {
        snprintf(dim1, sizeof(dim1), "%dx%d", img1_w, img1_h);
    }

    int written = 0;
    if (hint && hint[0] != '\0') {
        written = snprintf(out_buf, out_sz, "%s (%s) | %s (%s)  %d%%  %s%s  %s",
            trunc0, dim0, trunc1, dim1, zoom_pct,
            is_sync ? "SYNC" : "FREE", pane_ind, hint);
    } else {
        written = snprintf(out_buf, out_sz, "%s (%s) | %s (%s)  %d%%  %s%s",
            trunc0, dim0, trunc1, dim1, zoom_pct,
            is_sync ? "SYNC" : "FREE", pane_ind);
    }
    if (written < 0) {
        out_buf[0] = '\0';
        return 0;
    }
    if ((size_t)written >= out_sz) {
        out_buf[out_sz - 1] = '\0';
        return (int)out_sz - 1;
    }
    return written;
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
    int active_idx = (g_active == 1) ? 1 : 0;
    float z = g_sync ? g_zoom : g_free_zoom[active_idx];
    if (z < 0.05f) z = 0.05f;
    int pct = (int)(z * 100.0f + 0.5f);
    if (pct < 0) pct = 0;
    int len = 0;
    if (g_count == 1) {
        const char *b = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        b = b ? b + 1 : (g_img[0].path ? g_img[0].path : "?");
        int fidx = (g_file_index >= 0) ? (g_file_index + 1) : 0;
        int fcnt = (g_file_count >= 0) ? g_file_count : 0;
        ViewerStatusBarLayout layout = viewer_calc_status_layout_single(
            g_win_w, b, g_img[0].w, g_img[0].h, pct, g_sync, fidx, fcnt);
        len = viewer_format_status_single(
            &layout, b, g_img[0].w, g_img[0].h, pct, g_sync, fidx, fcnt,
            line, sizeof(line));
    } else if (g_count == 2) {
        const char *b0 = g_img[0].path ? strrchr(g_img[0].path, '/') : NULL;
        const char *b1 = g_img[1].path ? strrchr(g_img[1].path, '/') : NULL;
        b0 = b0 ? b0 + 1 : (g_img[0].path ? g_img[0].path : "?");
        b1 = b1 ? b1 + 1 : (g_img[1].path ? g_img[1].path : "?");
        ViewerStatusBarLayout layout = viewer_calc_status_layout_dual(
            g_win_w, b0, g_img[0].w, g_img[0].h, b1, g_img[1].w, g_img[1].h, pct, g_sync, active_idx);
        len = viewer_format_status_dual(
            &layout, b0, g_img[0].w, g_img[0].h, b1, g_img[1].w, g_img[1].h, pct, g_sync, active_idx,
            line, sizeof(line));
    } else {
        return;
    }
    if (len <= 0) return;
    SDL_Color white = {220, 220, 220, 255};
    int max_chars = 0;
    if (g_win_w > 2 * VIEWER_INFO_MARGIN_X) {
        max_chars = (g_win_w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W;
    }
    if (max_chars >= (int)sizeof(line)) {
        max_chars = (int)sizeof(line) - 1;
    }
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
 * Calculate dynamic metadata overlay panel layout based on window dimensions.
 *
 * Adapts panel width, column positions, and height to window size while ensuring
 * that labels and values do not overlap or overflow right margin, and that footer
 * never overlaps title or rows.
 *
 * @param win_w Window width in pixels.
 * @param win_h Window height in pixels.
 * @param exif_rows Number of EXIF rows to accommodate.
 * @return Calculated ViewerMetadataLayout struct.
 */
ViewerMetadataLayout viewer_calc_metadata_layout(int win_w, int win_h, int exif_rows) {
    ViewerMetadataLayout layout;
    memset(&layout, 0, sizeof(layout));

    if (win_w <= 0 || win_h <= 0) return layout;
    if (exif_rows < 0) exif_rows = 0;

    int pw = VIEWER_METADATA_STANDARD_PW;
    if (pw > win_w - 40) pw = win_w - 40;
    if (pw < VIEWER_METADATA_MIN_PW) return layout;

    const int line_h = 14;
    int ph = 36 + (5 * line_h) + 26 + (exif_rows * line_h) + 12 + 20;
    if (ph > win_h - 40) ph = win_h - 40;
    if (ph < VIEWER_METADATA_MIN_PH) return layout;

    int px = win_w - pw - 12;
    if (px < 0) px = 0;
    int py = (win_h - ph) / 2;
    if (py < 0) py = 0;

    int inner_w = pw - 24;
    if (inner_w <= 0) return layout;

    // Adapt column layout: standard 100px label if pw >= 300, else proportional
    int label_col_w = (pw >= 300) ? 100 : (inner_w * 38 / 100);
    if (label_col_w < 48) label_col_w = 48;
    int label_x = px + 12;

    int gap = 8;
    int value_x = label_x + label_col_w + gap;
    int value_w = px + pw - 12 - value_x;
    if (value_w <= 0) {
        label_col_w = (inner_w - gap) / 2;
        value_x = label_x + label_col_w + gap;
        value_w = px + pw - 12 - value_x;
        if (value_w <= 0) return layout;
    }

    layout.visible = true;
    layout.px = px;
    layout.py = py;
    layout.pw = pw;
    layout.ph = ph;
    layout.label_x = label_x;
    layout.label_w = label_col_w;
    layout.value_x = value_x;
    layout.value_w = value_w;
    layout.footer_y = py + ph - 20;

    return layout;
}

/**
 * Render right-side metadata panel (when g_show_metadata is true).
 *
 * Shows file system info (dimensions, color format, size, mtime, path) and EXIF
 * tags when available. The panel is 380px wide, anchored to the right, with a
 * semi-transparent background and a header. Height is calculated to fit all
 * active rows without footer overlap. Each metadata field is a label/value
 * pair drawn with the bitmap font.
 */
void viewer_render_metadata(SDL_Renderer *ren) {
    if (!g_show_metadata || !ren || g_win_w <= 0 || g_win_h <= 0) return;
    if (g_count <= 0) return;
    int pane = (g_count == 1) ? 0 : g_active;
    if (pane < 0 || pane >= g_count) pane = 0;
    if (!g_img[pane].path) return;

    const char *path = g_img[pane].path;
    Image *im = &g_img[pane];

    if (strcmp(s_cached_md_path, path) != 0) {
        snprintf(s_cached_md_path, sizeof(s_cached_md_path), "%s", path);
        s_has_stat = (stat(path, &s_cached_st) == 0);
        exif_read(path, &s_cached_exif);

        if (s_has_stat) {
            viewer_format_file_size(s_cached_st.st_size, s_cached_size_str, sizeof(s_cached_size_str));
            struct tm tm;
            if (localtime_r(&s_cached_st.st_mtime, &tm)) {
                if (strftime(s_cached_mtime_str, sizeof(s_cached_mtime_str), "%Y-%m-%d %H:%M", &tm) == 0) {
                    snprintf(s_cached_mtime_str, sizeof(s_cached_mtime_str), "?");
                }
            } else {
                snprintf(s_cached_mtime_str, sizeof(s_cached_mtime_str), "?");
            }
        } else {
            memset(&s_cached_st, 0, sizeof(s_cached_st));
            snprintf(s_cached_size_str, sizeof(s_cached_size_str), "?");
            snprintf(s_cached_mtime_str, sizeof(s_cached_mtime_str), "?");
        }

        // Shorten home prefix once
        const char *home = getenv("HOME");
        if (home && home[0] != '\0' && strcmp(home, "/") != 0) {
            size_t hlen = strlen(home);
            if (strncmp(path, home, hlen) == 0 && (path[hlen] == '/' || path[hlen] == '\0')) {
                snprintf(s_cached_path_disp, sizeof(s_cached_path_disp), "~%s", path + hlen);
            } else {
                snprintf(s_cached_path_disp, sizeof(s_cached_path_disp), "%s", path);
            }
        } else {
            snprintf(s_cached_path_disp, sizeof(s_cached_path_disp), "%s", path);
        }
    }

    ExifData exif = s_cached_exif;

    int line_h = 14;
    int exif_rows = 0;
    if (!exif.has_exif) {
        exif_rows = 1;
    } else {
        exif_rows = 1; // orientation
        if (exif.make[0] || exif.model[0]) exif_rows++;
        if (exif.datetime[0]) exif_rows++;
        if (exif.software[0]) exif_rows++;
        if (exif.iso) exif_rows++;
        if (exif.exposure[0]) exif_rows++;
        if (exif.fnumber[0]) exif_rows++;
        if (exif.focal[0]) exif_rows++;
        if (exif.exif_width && exif.exif_height) exif_rows++;
    }

    ViewerMetadataLayout layout = viewer_calc_metadata_layout(g_win_w, g_win_h, exif_rows);
    if (!layout.visible) return;

    int px = layout.px;
    int py = layout.py;
    int pw = layout.pw;
    int ph = layout.ph;

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
    if (g_count == 2) {
        const char *pane_label = (pane == 0) ? "[Left Pane] " : "[Right Pane] ";
        snprintf(title, sizeof(title), "Metadata %s— %s", pane_label, base);
    } else {
        snprintf(title, sizeof(title), "Metadata — %s", base);
    }
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
    int label_x = layout.label_x;
    int label_w = layout.label_w;
    int value_x = layout.value_x;
    int value_w = layout.value_w;

    // Helper macro to draw label/value pairs and advance y
    #define MD_ROW(label, value) do { \
        if (y + line_h <= layout.footer_y) { \
            text_draw_clipped(ren, label_x, y, label, label_col, 1, label_w); \
            text_draw_clipped(ren, value_x, y, value, value_col, 1, value_w); \
        } \
        y += line_h; \
    } while(0)

    char dim[64];
    if (viewer_is_loading(pane)) {
        snprintf(dim, sizeof(dim), "Loading...");
    } else {
        snprintf(dim, sizeof(dim), "%d x %d", im->w > 0 ? im->w : 0, im->h > 0 ? im->h : 0);
    }
    MD_ROW("Dimensions", dim);

    char color_str[32];
    if (viewer_is_loading(pane)) {
        snprintf(color_str, sizeof(color_str), "Loading...");
    } else {
        viewer_format_color_depth(im->channels, color_str, sizeof(color_str));
    }
    MD_ROW("Format", color_str);

    MD_ROW("File size", s_cached_size_str);
    MD_ROW("Modified", s_cached_mtime_str);

    int max_path_chars = (value_w > 0) ? (value_w / 8) : 0;
    char trunc_path[PATH_MAX];
    viewer_truncate_path(s_cached_path_disp, trunc_path, sizeof(trunc_path), max_path_chars);
    MD_ROW("Path", trunc_path);

    y += 4;
    // Separator (only draw if separator, spacing, and EXIF header fit above footer)
    if (y + 8 + line_h <= layout.footer_y) {
        SDL_SetRenderDrawColor(ren, 50, 50, 50, 255);
        SDL_RenderDrawLine(ren, px+12, y, px+pw-12, y);
    }
    y += 8;
    if (y + line_h <= layout.footer_y) {
        text_draw_clipped(ren, label_x, y, "EXIF", label_col, 1, pw - 24);
    }
    y += line_h;

    if (!exif.has_exif) {
        if (y + line_h <= layout.footer_y) {
            text_draw_clipped(ren, label_x, y, "No EXIF data", dim_col, 1, pw - 24);
        }
        y += line_h;
    } else {
        if (exif.make[0] || exif.model[0]) {
            char cam[160]; snprintf(cam, sizeof(cam), "%s %s", exif.make, exif.model);
            MD_ROW("Camera", cam);
        }
        if (exif.datetime[0]) MD_ROW("Date", exif.datetime);
        if (exif.software[0]) MD_ROW("Software", exif.software);
        char ori[32]; snprintf(ori, sizeof(ori), "%d", exif.orientation);
        MD_ROW("Orientation", ori);
        if (exif.iso) { char s[32]; snprintf(s, sizeof(s), "ISO %d", exif.iso); MD_ROW("ISO", s); }
        if (exif.exposure[0]) MD_ROW("Exposure", exif.exposure);
        if (exif.fnumber[0]) MD_ROW("Aperture", exif.fnumber);
        if (exif.focal[0]) MD_ROW("Focal", exif.focal);
        if (exif.exif_width && exif.exif_height) {
            char s[64]; snprintf(s, sizeof(s), "%d x %d", exif.exif_width, exif.exif_height);
            MD_ROW("EXIF size", s);
        }
    }

    #undef MD_ROW

    // Footer hint
    SDL_Rect footer = {px, layout.footer_y, pw, 20};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &footer);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, layout.footer_y, px+pw, layout.footer_y);
    text_draw_clipped(ren, px+8, layout.footer_y + 6, "Press e to close", dim_col, 1, pw - 16);
}

/**
 * Render a single image pane inside the given clip rectangle.
 *
 * Computes destination rectangle using precomputed img->w / img->h and zoom/pan.
 * Skips SDL_RenderCopyF if the image destination is entirely outside clip bounds.
 */
static void viewer_render_pane(SDL_Renderer *ren, int pane_idx, SDL_Rect clip) {
    if (!ren || pane_idx < 0 || pane_idx >= 2) return;
    if (clip.w <= 0 || clip.h <= 0) return;
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
    } else if (!im->tex && viewer_is_loading(pane_idx)) {
        SDL_SetRenderDrawColor(ren, 25, 25, 25, 255);
        SDL_RenderFillRect(ren, &clip);

        const char *loading_text = "Loading...";
        int scale = 2;
        int text_w = (int)strlen(loading_text) * 8 * scale;
        int text_h = 8 * scale;
        int tx = clip.x + (clip.w - text_w) / 2;
        int ty = clip.y + (clip.h - text_h) / 2;
        if (tx < clip.x) tx = clip.x;
        if (ty < clip.y) ty = clip.y;
        SDL_Color splash_col = {180, 180, 180, 255};
        text_draw_clipped(ren, tx, ty, loading_text, splash_col, scale, clip.w);
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
