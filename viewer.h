#pragma once

#include <linux/limits.h>

/**
 * viewer.h - Image viewing state and rendering.
 *
 * Owns all viewer-specific globals: window/renderer handles, loaded textures,
 * shared and per-pane view transforms (zoom/pan), fullscreen and overlay
 * flags, and directory navigation state. Provides functions to load images,
 * manipulate the view, and render the image panes with overlays.
 *
 * This module is intentionally UI-toolkit agnostic beyond SDL2; it does
 * not handle file browser state (see browser.h).
 */

#include <SDL2/SDL.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/**
 * Single loaded image and its GPU texture.
 *
 * The texture is created once at load time and reused. Path is an owned
 * heap copy (free with free()) for stable title rendering.
 */
typedef struct {
    SDL_Texture *tex;  // GPU texture, owned
    int w, h;          // Original dimensions
    char *path;        // Owned file path
} Image;

// ---------------------------------------------------------------------------
// Viewer globals (defined in viewer.c, shared with main and browser)
// ---------------------------------------------------------------------------
extern SDL_Window *g_win;
extern SDL_Renderer *g_ren;

extern int g_win_w, g_win_h;          // Current window size
extern int g_win_x, g_win_y;          // Saved windowed position for fullscreen restore
extern bool g_fullscreen;

extern Image g_img[2];
extern int g_count;                   // 1 or 2 loaded images

// View transforms: shared (sync) vs per-pane (free)
extern float g_zoom, g_pan_x, g_pan_y;
extern bool g_sync;
extern int g_active;                  // Active pane in free mode (0/1)
extern float g_free_zoom[2], g_free_pan_x[2], g_free_pan_y[2];

extern bool g_show_info;
extern bool g_show_help;
extern bool g_show_metadata;

// Directory navigation (current image's folder)
extern char **g_file_list;
extern int g_file_count;
extern int g_file_index;
extern char g_current_dir[PATH_MAX];

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------
bool viewer_is_image_file(const char *name);
void viewer_free_file_list(void);
bool viewer_scan_current_dir(const char *ref_path);

// ---------------------------------------------------------------------------
// Image lifecycle
// ---------------------------------------------------------------------------
bool viewer_load_image(const char *path, Image *out);
void viewer_unload_image(Image *im);
bool viewer_replace_image(int pane, const char *path);

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------
void viewer_fit_view(void);
void viewer_do_zoom(float factor, int mx, int my);
void viewer_do_pan(int dx, int dy);
void viewer_toggle_sync(void);
void viewer_toggle_fullscreen(void);
void viewer_update_title(void);

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
bool viewer_navigate(int delta);
bool viewer_go_parent(void);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void viewer_render(SDL_Renderer *ren);
void viewer_render_info_bar(SDL_Renderer *ren);
void viewer_render_help(SDL_Renderer *ren);
void viewer_render_metadata(SDL_Renderer *ren);
void viewer_toggle_metadata(void);
