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

#include <SDL2/SDL.h>
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

char **g_file_list = NULL;
int g_file_count = 0;
int g_file_index = -1;
char g_current_dir[PATH_MAX] = {0};

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool viewer_is_image_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0') return false;
    const char *ext = dot + 1;
    for (int i = 0; kImageExts[i]; i++) {
        if (strcasecmp(ext, kImageExts[i]) == 0) return true;
    }
    return false;
}

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

bool viewer_scan_current_dir(const char *ref_path) {
    viewer_free_file_list();

    char tmp[PATH_MAX];
    strncpy(tmp, ref_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *dir = dirname(tmp);

    char abs_dir[PATH_MAX];
    if (realpath(dir, abs_dir)) {
        strncpy(g_current_dir, abs_dir, sizeof(g_current_dir) - 1);
    } else {
        strncpy(g_current_dir, dir, sizeof(g_current_dir) - 1);
    }
    g_current_dir[sizeof(g_current_dir) - 1] = '\0';

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

void viewer_unload_image(Image *im) {
    if (im->tex) SDL_DestroyTexture(im->tex);
    if (im->path) free(im->path);
    memset(im, 0, sizeof(*im));
}

bool viewer_load_image(const char *path, Image *out) {
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

    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    out->tex = tex;
    out->w = w;
    out->h = h;
    out->path = strdup(path);
    return true;
}

bool viewer_replace_image(int pane, const char *path) {
    if (pane < 0 || pane > 1) return false;
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

void viewer_fit_view(void) {
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
        for (int i = 0; i < 2; i++) {
            g_free_zoom[i] = g_zoom;
            g_free_pan_x[i] = 0;
            g_free_pan_y[i] = 0;
        }
    } else {
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

void viewer_do_zoom(float factor, int mx, int my) {
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
        float pane_w = (g_count == 1) ? (float)g_win_w : (float)g_win_w / 2.0f;
        float pane_cx = (g_count == 1) ? (float)g_win_w * 0.5f
                        : (p == 0 ? pane_w * 0.5f : pane_w + pane_w * 0.5f);
        float pane_cy = (float)g_win_h * 0.5f;
        g_free_pan_x[p] += (float)(mx - (int)pane_cx) * (1.0f / next - 1.0f / old);
        g_free_pan_y[p] += (float)(my - (int)pane_cy) * (1.0f / next - 1.0f / old);
        g_free_zoom[p] = next;
    }
}

void viewer_do_pan(int dx, int dy) {
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

void viewer_update_title(void) {
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

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool viewer_navigate(int delta) {
    if (g_file_count == 0 || g_file_index < 0) return false;
    int next = g_file_index + delta;
    if (next < 0) next = g_file_count - 1;
    if (next >= g_file_count) next = 0;
    if (next == g_file_index) return false;

    int pane = (g_count == 1) ? 0 : g_active;
    const char *target = g_file_list[next];
    Image tmp = {0};
    if (!viewer_load_image(target, &tmp)) return false;

    viewer_unload_image(&g_img[pane]);
    g_img[pane] = tmp;
    g_file_index = next;
    return true;
}

bool viewer_go_parent(void) {
    if (g_current_dir[0] == '\0') return false;
    char tmp[PATH_MAX];
    strncpy(tmp, g_current_dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
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
    strncpy(saved_dir, g_current_dir, sizeof(saved_dir));
    char **saved_list = g_file_list;
    int saved_count = g_file_count;
    int saved_index = g_file_index;
    g_file_list = NULL; g_file_count = 0; g_file_index = -1;

    char dummy[PATH_MAX];
    snprintf(dummy, sizeof(dummy), "%s/dummy.jpg", parent);
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
            strncpy(g_current_dir, parent, sizeof(g_current_dir) - 1);
            ok = true;
        } else {
            for (int i = 0; i < parent_count; i++) free(parent_list[i]);
            free(parent_list);
            g_file_list = saved_list;
            g_file_count = saved_count;
            g_file_index = saved_index;
            strncpy(g_current_dir, saved_dir, sizeof(g_current_dir) - 1);
        }
    } else {
        viewer_free_file_list();
        g_file_list = saved_list;
        g_file_count = saved_count;
        g_file_index = saved_index;
        strncpy(g_current_dir, saved_dir, sizeof(g_current_dir) - 1);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

void viewer_render_info_bar(SDL_Renderer *ren) {
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
        snprintf(line, sizeof(line), "%s  %dx%d  %d%%  %s  %d/%d  [s]ync [Tab] pane [f]ull [n/p] next/prev [ESC] browser",
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
    int max_chars = g_win_w / 8 - 1;
    if ((int)strlen(line) > max_chars) line[max_chars] = '\0';
    text_draw(ren, 6, g_win_h - bar_h + 7, line, white, 1);
}

void viewer_render_help(SDL_Renderer *ren) {
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
    int panel_h = count * 14 + 24;
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

void viewer_render(SDL_Renderer *ren) {
    SDL_SetRenderDrawColor(ren, 18, 18, 18, 255);
    SDL_RenderClear(ren);

    if (g_count == 1) {
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

            if (!g_sync && g_active == i) {
                SDL_SetRenderDrawColor(ren, 100, 160, 255, 200);
                SDL_Rect hl = {clip.x + 1, 1, clip.w - 2, g_win_h - 2};
                SDL_RenderDrawRect(ren, &hl);
            }
        }
        SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
        int mid = g_win_w / 2;
        SDL_RenderDrawLine(ren, mid, 0, mid, g_win_h);
    }

    viewer_render_info_bar(ren);
    viewer_render_help(ren);
    // Browser overlay is rendered by main.c after this if open.
}
