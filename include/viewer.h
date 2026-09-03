#ifndef VIEWER_H
#define VIEWER_H

/**
 * viewer.h - Image viewing state, transformations, and rendering.
 *
 * Owns all viewer-specific globals: window/renderer handles, loaded textures,
 * shared and per-pane view transforms (zoom/pan), fullscreen and overlay
 * flags, and directory navigation state. Provides functions to load images,
 * manipulate the view, and render the image panes with overlays.
 *
 * All globals are declared extern here and defined strictly in src/viewer.c.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VIEWER_INFO_NAME_MAX 24
#define VIEWER_INFO_DUAL_NAME_MAX 18

#define VIEWER_INFO_MARGIN_X 6
#define VIEWER_INFO_FONT_W 8
#define VIEWER_INFO_NAME_MIN_SINGLE 12
#define VIEWER_INFO_NAME_TARGET_SINGLE 32
#define VIEWER_INFO_NAME_MIN_DUAL 8
#define VIEWER_INFO_NAME_TARGET_DUAL 18

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/**
 * Status bar hint tiers, degraded responsively based on available horizontal budget.
 */
typedef enum {
    VIEWER_HINT_NONE = 0,
    VIEWER_HINT_MINIMAL,
    VIEWER_HINT_COMPACT,
    VIEWER_HINT_FULL,
    VIEWER_HINT_TIER_COUNT
} ViewerHintTier;

/**
 * Calculated status bar character budget and responsive hint tier.
 */
typedef struct {
    int usable_chars;
    ViewerHintTier hint_tier;
    int name_budget[2];
} ViewerStatusBarLayout;

/**
 * Single loaded image and its GPU texture.
 *
 * The texture is created once at load time and reused. Path is an owned
 * heap copy (free with free()) for stable title rendering.
 */
typedef struct {
    SDL_Texture *tex;  // GPU texture, owned
    int w, h;          // Original image dimensions in pixels
    int channels;      // Original decoded channel count (1=Gray, 2=Gray+Alpha, 3=RGB, 4=RGBA)
    char *path;        // Owned heap-allocated file path
} Image;

// ---------------------------------------------------------------------------
// Viewer globals (defined strictly in src/viewer.c, shared with main and browser)
// ---------------------------------------------------------------------------
extern SDL_Window *g_win;
extern SDL_Renderer *g_ren;

extern int g_win_w, g_win_h;          // Current window size in pixels
extern int g_win_x, g_win_y;          // Saved windowed position for fullscreen restore
extern bool g_fullscreen;

extern Image g_img[2];
extern int g_count;                   // 1 or 2 loaded images

// View transforms: shared (sync) vs per-pane (free)
extern float g_zoom, g_pan_x, g_pan_y;
extern bool g_sync;
extern int g_active;                  // Active pane in free mode (0 or 1)
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

/**
 * Test whether a file name has a supported image extension.
 *
 * Performs case-insensitive matching against the list of supported extensions
 * (jpg, jpeg, png, webp, bmp, ppm, pgm, pbm, tiff, tif, gif, hdr, psd, tga).
 *
 * @param name Filename or path string to inspect.
 * @return true if extension is recognized as a supported image, false otherwise.
 */
bool viewer_is_image_file(const char *name);

/**
 * Truncate a filename to fit within max_len characters, preserving the file extension.
 * If name is longer than max_len, stems are truncated with "..." before the extension.
 * If there is no extension or it is too long, suffix truncation is applied.
 */
void viewer_truncate_filename(const char *name, char *out, size_t out_sz, int max_len);

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
void viewer_format_color_depth(int channels, char *out, size_t out_sz);

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
void viewer_truncate_path(const char *path, char *out, size_t out_sz, int max_chars);

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
void viewer_format_file_size(off_t size, char *out, size_t out_sz);

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
bool viewer_validate_image_path(const char *path, char *out_clean_path, size_t out_size);

/**
 * Free the directory file list and reset navigation counters.
 *
 * Releases all heap-allocated filename strings in g_file_list and the list
 * pointer itself. Safe to call multiple times or on an empty list.
 */
void viewer_free_file_list(void);

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
bool viewer_scan_current_dir(const char *ref_path);

// ---------------------------------------------------------------------------
// Image lifecycle
// ---------------------------------------------------------------------------

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
bool viewer_load_image(const char *path, Image *out);

/**
 * Release GPU texture and heap-allocated path owned by an Image struct.
 *
 * Destroys im->tex, frees im->path, and zeroes the struct. Safe to call on
 * NULL or already-zeroed structures.
 *
 * @param im Pointer to Image struct to unload.
 */
void viewer_unload_image(Image *im);

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
bool viewer_replace_image(int pane, const char *path);

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
void viewer_fit_view(void);

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
void viewer_do_zoom(float factor, int mx, int my);

/**
 * Pan the viewport by screen-space pixel deltas.
 *
 * Translates dx and dy into image-space coordinates by dividing by current zoom:
 * pan += delta / zoom. Updates shared or active-pane pan offsets accordingly.
 *
 * @param dx Horizontal displacement in window pixels.
 * @param dy Vertical displacement in window pixels.
 */
void viewer_do_pan(int dx, int dy);

/**
 * Toggle synchronization mode between synchronized and free transforms.
 *
 * When switching to free mode: copies current shared zoom/pan into each pane.
 * When switching to sync mode: adopts active pane's transform for shared view.
 */
void viewer_toggle_sync(void);

/**
 * Toggle between windowed and fullscreen desktop display modes.
 *
 * Saves window position and dimensions before entering fullscreen, and restores
 * them upon returning to windowed mode. Updates g_fullscreen state.
 */
void viewer_toggle_fullscreen(void);

/**
 * Reconstruct and apply the window title bar string based on current viewer state.
 *
 * Formats filename(s), current zoom percentage, sync mode (SYNC / FREE), active
 * pane indicator ([L] / [R]), and directory index (e.g. 3/25).
 */
void viewer_update_title(void);

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
bool viewer_navigate(int delta);

/**
 * Navigate to the first image in the parent directory.
 *
 * Scans parent directory for image files, loads the first image into pane 0,
 * and updates g_file_list and g_current_dir on success. Preserves current state
 * on failure.
 *
 * @return true if navigation to parent succeeded, false otherwise.
 */
bool viewer_go_parent(void);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/**
 * Clear the viewport and render active panes, split divider, and overlays.
 *
 * Renders pane 0 (and pane 1 if g_count == 2) with viewport culling, divider line,
 * info bar (if enabled), help dialog (if enabled), and metadata panel (if enabled).
 *
 * @param ren Target SDL renderer.
 */
void viewer_render(SDL_Renderer *ren);

/**
 * Render bottom status bar with filename, resolution, zoom, sync, and index.
 *
 * No-op if g_show_info is false or window dimensions are invalid.
 *
 * @param ren Target SDL renderer.
 */
void viewer_render_info_bar(SDL_Renderer *ren);

// ---------------------------------------------------------------------------
// Status bar layout and formatting
// ---------------------------------------------------------------------------

/**
 * Distribute available filename character budget between dual panes.
 *
 * Divides total_budget equally between pane 0 and pane 1. If either pane needs
 * fewer characters than its share, the surplus is transferred to the other pane.
 * Preserves the conservation invariant: *out0 + *out1 == total_budget (when total_budget >= 0).
 *
 * @param total_budget Total character budget available for both filenames.
 * @param len0 Required character count for pane 0 filename.
 * @param len1 Required character count for pane 1 filename.
 * @param min_len Minimum character allocation per pane.
 * @param out0 Output pointer for pane 0 character budget.
 * @param out1 Output pointer for pane 1 character budget.
 */
void viewer_distribute_dual_budget(int total_budget, int len0, int len1, int min_len, int *out0, int *out1);

/**
 * Calculate dynamic status bar layout for single-pane view based on window width.
 *
 * Computes usable character capacity from window width, measures fixed metadata width,
 * and selects the highest responsive hint tier (FULL down to NONE) that leaves at least
 * VIEWER_INFO_NAME_TARGET_SINGLE characters for the filename. Allocates all remaining
 * usable character budget to layout.name_budget[0].
 *
 * @param win_w Window width in pixels.
 * @param name Filename or path for image.
 * @param img_w Image pixel width.
 * @param img_h Image pixel height.
 * @param zoom_pct Zoom level percentage (e.g. 100).
 * @param is_sync True if sync transform mode is active.
 * @param file_idx Current image index in folder.
 * @param file_count Total image count in folder.
 * @return Calculated ViewerStatusBarLayout struct.
 */
ViewerStatusBarLayout viewer_calc_status_layout_single(
    int win_w, const char *name, int img_w, int img_h,
    int zoom_pct, bool is_sync, int file_idx, int file_count);

/**
 * Calculate dynamic status bar layout for dual-pane view based on window width.
 *
 * Computes usable character capacity from window width, measures fixed metadata for
 * both panes, selects the highest responsive hint tier that leaves sufficient filename
 * budget, and distributes remaining character budget between pane 0 and pane 1 using
 * viewer_distribute_dual_budget.
 *
 * @param win_w Window width in pixels.
 * @param name0 Filename or path for pane 0 image.
 * @param img0_w Pixel width of pane 0 image.
 * @param img0_h Pixel height of pane 0 image.
 * @param name1 Filename or path for pane 1 image.
 * @param img1_w Pixel width of pane 1 image.
 * @param img1_h Pixel height of pane 1 image.
 * @param zoom_pct Zoom level percentage.
 * @param is_sync True if sync transform mode is active.
 * @param active_pane Active pane index in free mode (0 or 1).
 * @return Calculated ViewerStatusBarLayout struct.
 */
ViewerStatusBarLayout viewer_calc_status_layout_dual(
    int win_w, const char *name0, int img0_w, int img0_h,
    const char *name1, int img1_w, int img1_h,
    int zoom_pct, bool is_sync, int active_pane);

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
    int file_idx, int file_count, char *out_buf, size_t out_sz);

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
    int active_pane, char *out_buf, size_t out_sz);

/**
 * Render keyboard shortcut and controls help overlay dialog.
 *
 * No-op if g_show_help is false. Renders semi-transparent centered dialog box
 * with control descriptions and dismiss instructions.
 *
 * @param ren Target SDL renderer.
 */
void viewer_render_help(SDL_Renderer *ren);

/**
 * Render right-hand metadata panel showing file details and EXIF tags.
 *
 * Caches stat and EXIF parsing results per image path to prevent disk I/O on
 * every frame. Shows file dimensions, file size, modification timestamp, path,
 * and camera/exposure EXIF data.
 *
 * @param ren Target SDL renderer.
 */
void viewer_render_metadata(SDL_Renderer *ren);

/**
 * Toggle visibility of the right-hand EXIF and file metadata overlay panel.
 */
void viewer_toggle_metadata(void);

#endif /* VIEWER_H */
