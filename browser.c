/**
 * browser.c - File browser implementation with tree expand/collapse.
 *
 * Data model:
 *   - Browser maintains a flat visible list (s_entries) that represents the
 *     current expanded tree. Each entry stores path, name, is_dir, depth,
 *     and expanded flag. Depth controls indentation.
 *   - Initially, the list contains only the immediate children of the root
 *     directory (depth 0). Expanding a directory inserts its children directly
 *     after it with depth+1, sorted folders-first then files. Collapsing removes
 *     all descendants (depth > parent depth) until the next entry with
 *     depth <= parent depth.
 *   - This avoids a full recursive tree scan upfront and keeps operations O(n)
 *     on visible items, which is trivial for typical image folders (< few hundred).
 *
 * Rendering:
 *   - Overlay panel covers 65% of window width, centered horizontally but
 *     anchored to top with margin. Dark background with border, title bar
 *     shows current root, list area shows entries with icons and indentation,
 *     bottom bar shows help. Selection is highlighted with a blue fill.
 *
 * Interaction:
 *   - Keyboard: Up/Down moves selection (with scroll), Right/Enter expands or
 *     opens, Left/Backspace collapses or navigates to parent, Enter on image
 *     loads it into viewer, ESC closes browser.
 *   - Mouse: Click selects, double-click opens/expands, wheel scrolls list.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "browser.h"
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
// Entry type
// ---------------------------------------------------------------------------

typedef struct {
    char path[PATH_MAX];  // Full path
    char name[256];       // Base name for display
    bool is_dir;          // True for directories
    bool expanded;        // True if directory is currently expanded
    int depth;            // Indentation depth (0 = root children)
} BrowserEntry;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bool s_open = false;
static char s_root[PATH_MAX] = {0};
static BrowserEntry *s_entries = NULL;
static int s_count = 0;
static int s_cap = 0;
static int s_selected = 0;
static int s_scroll = 0;
static Uint32 s_last_click = 0;
static int s_last_click_idx = -1;

// Layout constants
static const int kPanelMargin = 20;
static const int kRowH = 18;
static const int kTitleH = 26;
static const int kFooterH = 20;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void ensure_cap(int needed) {
    if (needed <= s_cap) return;
    int ncap = s_cap ? s_cap * 2 : 64;
    while (ncap < needed) ncap *= 2;
    BrowserEntry *n = realloc(s_entries, ncap * sizeof(BrowserEntry));
    if (n) { s_entries = n; s_cap = ncap; }
}

static int entry_cmp(const void *a, const void *b) {
    const BrowserEntry *ea = a;
    const BrowserEntry *eb = b;
    // Folders first, then alphabetical case-insensitive, then case-sensitive for stability
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    int c = strcasecmp(ea->name, eb->name);
    if (c != 0) return c;
    return strcmp(ea->name, eb->name);
}

/**
 * Scan immediate children of a directory into a temporary array, sorted
 * folders-first. Hidden files (dot prefix) are skipped. For directories, we
 * include all subdirectories; for files, only supported image extensions.
 */
static BrowserEntry *scan_dir_entries(const char *dir, int *out_n, int depth) {
    DIR *d = opendir(dir);
    if (!d) { *out_n = 0; return NULL; }

    int cap = 32, n = 0;
    BrowserEntry *arr = malloc(cap * sizeof(BrowserEntry));
    if (!arr) { closedir(d); *out_n = 0; return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // Skip hidden

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_file = S_ISREG(st.st_mode);

        // For files, filter to image types only
        if (is_file && !viewer_is_image_file(ent->d_name)) continue;
        if (!is_dir && !is_file) continue;

        if (n >= cap) {
            cap *= 2;
            BrowserEntry *nn = realloc(arr, cap * sizeof(BrowserEntry));
            if (!nn) break;
            arr = nn;
        }
        BrowserEntry *e = &arr[n++];
        memset(e, 0, sizeof(*e));
        strncpy(e->path, full, sizeof(e->path) - 1);
        strncpy(e->name, ent->d_name, sizeof(e->name) - 1);
        e->is_dir = is_dir;
        e->expanded = false;
        e->depth = depth;
    }
    closedir(d);

    if (n > 1) qsort(arr, n, sizeof(BrowserEntry), entry_cmp);
    *out_n = n;
    return arr;
}

static void rebuild_root(const char *root) {
    // Free old list
    free(s_entries);
    s_entries = NULL;
    s_count = 0;
    s_cap = 0;
    s_selected = 0;
    s_scroll = 0;

    if (!root || root[0] == '\0') return;
    strncpy(s_root, root, sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = '\0';

    int n = 0;
    BrowserEntry *arr = scan_dir_entries(s_root, &n, 0);
    if (!arr) return;
    ensure_cap(n);
    memcpy(s_entries, arr, n * sizeof(BrowserEntry));
    s_count = n;
    free(arr);
}

/**
 * Expand a directory entry at idx by inserting its children after it.
 * Returns true if expansion occurred.
 */
static bool expand_entry(int idx) {
    if (idx < 0 || idx >= s_count) return false;
    BrowserEntry *e = &s_entries[idx];
    if (!e->is_dir || e->expanded) return false;

    int n = 0;
    BrowserEntry *children = scan_dir_entries(e->path, &n, e->depth + 1);
    if (!children || n == 0) {
        free(children);
        // Mark expanded even if empty to avoid retry
        e->expanded = true;
        return true;
    }

    ensure_cap(s_count + n);
    // Shift tail after idx
    memmove(&s_entries[idx + 1 + n], &s_entries[idx + 1], (s_count - idx - 1) * sizeof(BrowserEntry));
    memcpy(&s_entries[idx + 1], children, n * sizeof(BrowserEntry));
    s_count += n;
    e->expanded = true;
    free(children);
    return true;
}

/**
 * Collapse a directory entry at idx by removing all its descendants.
 * Returns true if collapse occurred.
 */
static bool collapse_entry(int idx) {
    if (idx < 0 || idx >= s_count) return false;
    BrowserEntry *e = &s_entries[idx];
    if (!e->is_dir || !e->expanded) return false;

    int depth = e->depth;
    int end = idx + 1;
    while (end < s_count && s_entries[end].depth > depth) end++;
    int remove = end - (idx + 1);
    if (remove > 0) {
        memmove(&s_entries[idx + 1], &s_entries[end], (s_count - end) * sizeof(BrowserEntry));
        s_count -= remove;
        // Clamp selection if it was inside collapsed region
        if (s_selected > idx && s_selected < end) s_selected = idx;
        if (s_selected >= s_count) s_selected = s_count - 1;
        if (s_scroll > s_selected) s_scroll = s_selected;
    }
    e->expanded = false;
    return true;
}

/**
 * Find the parent index of the entry at idx (nearest entry with depth = entry.depth -1)
 */
static int find_parent(int idx) {
    if (idx <= 0 || idx >= s_count) return -1;
    int d = s_entries[idx].depth;
    for (int i = idx - 1; i >= 0; i--) {
        if (s_entries[i].depth == d - 1) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void browser_init(void) {
    // Initialize from viewer's current directory if available
    if (g_current_dir[0] != '\0') {
        rebuild_root(g_current_dir);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) rebuild_root(cwd);
    }
}

void browser_set_root(const char *path) {
    if (!path) return;
    char abs_path[PATH_MAX];
    const char *use = path;
    if (realpath(path, abs_path)) use = abs_path;
    rebuild_root(use);
}

void browser_toggle(void) {
    if (s_open) {
        s_open = false;
        return;
    }
    // Opening: refresh from viewer's current dir to reflect external navigation
    if (g_current_dir[0] != '\0') {
        // Only rebuild if root differs to preserve expanded state when possible
        char abs_cur[PATH_MAX];
        const char *cur = g_current_dir;
        if (realpath(g_current_dir, abs_cur)) cur = abs_cur;
        if (strcmp(s_root, cur) != 0) {
            rebuild_root(cur);
        }
    } else {
        browser_init();
    }
    // Ensure selection is valid and visible
    if (s_selected < 0) s_selected = 0;
    if (s_selected >= s_count) s_selected = s_count - 1;
    s_open = true;
}

bool browser_is_open(void) { return s_open; }

static void clamp_scroll(int visible_rows) {
    if (s_selected < s_scroll) s_scroll = s_selected;
    if (s_selected >= s_scroll + visible_rows) s_scroll = s_selected - visible_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_count - visible_rows) s_scroll = s_count - visible_rows;
    if (s_scroll < 0) s_scroll = 0;
}

bool browser_handle_key(SDL_Keycode key, SDL_Keymod mod) {
    (void)mod;
    if (!s_open) return false;

    // Compute visible rows for scrolling (updated per frame, but approximate here)
    int panel_h = g_win_h - 2 * kPanelMargin;
    int list_h = panel_h - kTitleH - kFooterH;
    int visible = list_h / kRowH;
    if (visible < 1) visible = 1;

    switch (key) {
    case SDLK_ESCAPE:
        s_open = false;
        return true;
    case SDLK_UP:
    case SDLK_k:
        if (s_selected > 0) s_selected--;
        clamp_scroll(visible);
        return true;
    case SDLK_DOWN:
    case SDLK_j:
        if (s_selected + 1 < s_count) s_selected++;
        clamp_scroll(visible);
        return true;
    case SDLK_HOME:
        s_selected = 0;
        s_scroll = 0;
        return true;
    case SDLK_END:
        s_selected = s_count - 1;
        clamp_scroll(visible);
        return true;
    case SDLK_PAGEUP:
        s_selected -= visible;
        if (s_selected < 0) s_selected = 0;
        clamp_scroll(visible);
        return true;
    case SDLK_PAGEDOWN:
        s_selected += visible;
        if (s_selected >= s_count) s_selected = s_count - 1;
        clamp_scroll(visible);
        return true;
    case SDLK_RIGHT:
        if (s_selected >= 0 && s_selected < s_count && s_entries[s_selected].is_dir) {
            if (!s_entries[s_selected].expanded) expand_entry(s_selected);
            else if (s_selected + 1 < s_count) { s_selected++; clamp_scroll(visible); }
        }
        return true;
    case SDLK_LEFT:
    case SDLK_BACKSPACE: {
        if (s_selected < 0 || s_selected >= s_count) return true;
        BrowserEntry *e = &s_entries[s_selected];
        if (e->is_dir && e->expanded) {
            collapse_entry(s_selected);
        } else {
            int parent = find_parent(s_selected);
            if (parent >= 0) {
                // Collapse parent if expanded and select it
                if (s_entries[parent].expanded) collapse_entry(parent);
                s_selected = parent;
                clamp_scroll(visible);
            } else {
                // At top level, go to parent directory
                char tmp[PATH_MAX];
                strncpy(tmp, s_root, sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                char *par = dirname(tmp);
                if (par && strcmp(par, s_root) != 0 && strcmp(par, ".") != 0) {
                    rebuild_root(par);
                    // Update viewer's dir to reflect navigation (but don't load file yet)
                    strncpy(g_current_dir, par, sizeof(g_current_dir) - 1);
                }
            }
        }
        return true;
    }
    case SDLK_RETURN:
    case SDLK_KP_ENTER: {
        if (s_selected < 0 || s_selected >= s_count) return true;
        BrowserEntry *e = &s_entries[s_selected];
        if (e->is_dir) {
            if (e->expanded) collapse_entry(s_selected);
            else expand_entry(s_selected);
        } else {
            // Image file: load into active pane and close browser
            int pane = (g_count == 1) ? 0 : g_active;
            if (viewer_replace_image(pane, e->path)) {
                viewer_fit_view();
                viewer_update_title();
                s_open = false;
            }
        }
        return true;
    }
    case SDLK_SPACE:
        // Space toggles expand for directories, preview for files (same as Enter)
        if (s_selected >= 0 && s_selected < s_count && s_entries[s_selected].is_dir) {
            if (s_entries[s_selected].expanded) collapse_entry(s_selected);
            else expand_entry(s_selected);
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool browser_handle_event(SDL_Event *ev) {
    if (!s_open) return false;

    // Panel geometry (must match browser_render)
    int panel_w = (g_win_w * 65) / 100;
    if (panel_w < 400) panel_w = g_win_w - 2 * kPanelMargin;
    if (panel_w > g_win_w - 2 * kPanelMargin) panel_w = g_win_w - 2 * kPanelMargin;
    int panel_h = g_win_h - 2 * kPanelMargin;
    int px = (g_win_w - panel_w) / 2;
    int py = kPanelMargin;
    int list_y = py + kTitleH;
    int list_h = panel_h - kTitleH - kFooterH;
    int visible = list_h / kRowH;

    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        int mx = ev->button.x;
        int my = ev->button.y;
        // Check if click inside panel
        if (mx < px || mx >= px + panel_w || my < py || my >= py + panel_h) {
            // Click outside closes browser
            if (ev->button.button == SDL_BUTTON_LEFT) s_open = false;
            return true;
        }
        // Click inside list area
        if (my >= list_y && my < list_y + list_h) {
            int row = (my - list_y) / kRowH;
            int idx = s_scroll + row;
            if (idx >= 0 && idx < s_count) {
                s_selected = idx;
                // Double-click detection (300ms)
                Uint32 now = SDL_GetTicks();
                if (ev->button.button == SDL_BUTTON_LEFT && s_last_click_idx == idx && (now - s_last_click) < 300) {
                    // Double-click: open/expand
                    BrowserEntry *e = &s_entries[idx];
                    if (e->is_dir) {
                        if (e->expanded) collapse_entry(idx);
                        else expand_entry(idx);
                    } else {
                        int pane = (g_count == 1) ? 0 : g_active;
                        if (viewer_replace_image(pane, e->path)) {
                            viewer_fit_view();
                            viewer_update_title();
                            s_open = false;
                        }
                    }
                }
                s_last_click = now;
                s_last_click_idx = idx;
            }
        }
        // Consume all mouse clicks when browser is open to prevent panning behind
        return true;
    } else if (ev->type == SDL_MOUSEWHEEL) {
        // Scroll list
        if (ev->wheel.y > 0) {
            s_scroll -= 3;
            if (s_scroll < 0) s_scroll = 0;
        } else if (ev->wheel.y < 0) {
            s_scroll += 3;
            int max_scroll = s_count - visible;
            if (max_scroll < 0) max_scroll = 0;
            if (s_scroll > max_scroll) s_scroll = max_scroll;
        }
        return true;
    } else if (ev->type == SDL_MOUSEMOTION) {
        // Hover highlight? We only change selection on click to avoid accidental moves
        return true;
    }
    return false;
}

void browser_render(SDL_Renderer *ren) {
    if (!s_open) return;

    int panel_w = (g_win_w * 65) / 100;
    if (panel_w < 400) panel_w = g_win_w - 2 * kPanelMargin;
    if (panel_w > g_win_w - 2 * kPanelMargin) panel_w = g_win_w - 2 * kPanelMargin;
    int panel_h = g_win_h - 2 * kPanelMargin;
    int px = (g_win_w - panel_w) / 2;
    int py = kPanelMargin;

    // Dim background behind panel
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 160);
    SDL_Rect dim = {0, 0, g_win_w, g_win_h};
    SDL_RenderFillRect(ren, &dim);

    // Panel background
    SDL_Rect bg = {px, py, panel_w, panel_h};
    SDL_SetRenderDrawColor(ren, 24, 24, 24, 240);
    SDL_RenderFillRect(ren, &bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawRect(ren, &bg);

    // Title bar
    SDL_Rect title_bg = {px, py, panel_w, kTitleH};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &title_bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, py + kTitleH, px + panel_w, py + kTitleH);

    SDL_Color title_col = {220, 220, 220, 255};
    SDL_Color dim_col = {160, 160, 160, 255};
    char title[PATH_MAX + 32];
    snprintf(title, sizeof(title), " %s", s_root);
    // Clip title to panel width
    int max_title_chars = (panel_w - 20) / 8;
    if ((int)strlen(title) > max_title_chars) {
        // Show ellipsis for long paths
        const char *ellipsis = "...";
        int keep = max_title_chars - 3;
        if (keep < 10) keep = 10;
        char tmp[PATH_MAX + 32];
        snprintf(tmp, sizeof(tmp), "...%s", s_root + strlen(s_root) - keep);
        strncpy(title, tmp, sizeof(title) - 1);
    }
    text_draw(ren, px + 8, py + 9, title, title_col, 1);

    // List area
    int list_y = py + kTitleH;
    int list_h = panel_h - kTitleH - kFooterH;
    int visible = list_h / kRowH;
    clamp_scroll(visible);

    SDL_Rect clip = {px, list_y, panel_w, list_h};
    SDL_RenderSetClipRect(ren, &clip);

    for (int i = 0; i < visible; i++) {
        int idx = s_scroll + i;
        if (idx >= s_count) break;
        BrowserEntry *e = &s_entries[idx];
        int row_y = list_y + i * kRowH;

        // Selection highlight
        if (idx == s_selected) {
            SDL_Rect sel = {px, row_y, panel_w, kRowH};
            SDL_SetRenderDrawColor(ren, 50, 90, 160, 255);
            SDL_RenderFillRect(ren, &sel);
        }

        // Indentation and expand indicator
        int indent = 12 + e->depth * 16;
        int tx = px + indent;
        SDL_Color name_col = e->is_dir ? title_col : dim_col;
        if (idx == s_selected) name_col = (SDL_Color){255, 255, 255, 255};

        char prefix[8] = "  ";
        if (e->is_dir) {
            prefix[0] = e->expanded ? '-' : '+';
            prefix[1] = '\0';
            // Draw prefix in dimmer color
            SDL_Color pre_col = {140, 140, 140, 255};
            if (idx == s_selected) pre_col = (SDL_Color){255, 220, 100, 255};
            text_draw(ren, tx, row_y + 5, prefix, pre_col, 1);
            tx += 16;
            // Folder icon simulation: add slash for dir
            // Name
            text_draw_clipped(ren, tx, row_y + 5, e->name, name_col, 1, panel_w - (tx - px) - 8);
            // Trailing slash for clarity
            // text_draw(ren, tx + (int)strlen(e->name)*8, row_y + 5, "/", name_col, 1);
        } else {
            // File: bullet
            text_draw(ren, tx, row_y + 5, " ", name_col, 1);
            tx += 16;
            text_draw_clipped(ren, tx, row_y + 5, e->name, name_col, 1, panel_w - (tx - px) - 8);
        }
    }
    SDL_RenderSetClipRect(ren, NULL);

    // Footer
    SDL_Rect footer_bg = {px, py + panel_h - kFooterH, panel_w, kFooterH};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &footer_bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, py + panel_h - kFooterH, px + panel_w, py + panel_h - kFooterH);

    SDL_Color foot_col = {140, 140, 140, 255};
    const char *foot = " Up/Down select  Enter open/expand  Left/Right collapse  Esc close  Click outside to close";
    text_draw_clipped(ren, px + 8, py + panel_h - kFooterH + 6, foot, foot_col, 1, panel_w - 16);

    // Border highlight
    SDL_SetRenderDrawColor(ren, 90, 90, 90, 255);
    SDL_RenderDrawRect(ren, &bg);
}

void browser_cleanup(void) {
    free(s_entries);
    s_entries = NULL;
    s_count = s_cap = 0;
}
