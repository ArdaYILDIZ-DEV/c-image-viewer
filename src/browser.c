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

// Filter state: incremental search within visible entries
static char s_filter[256] = {0};
static size_t s_filter_len = 0;
static bool s_filtering = false; // true when filter has content

// Layout constants
static const int kPanelMargin = 20;
static const int kRowH = 18;
static const int kTitleH = 26;
static const int kFooterH = 20;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool ensure_cap(int needed) {
    if (needed <= s_cap) return true;
    int ncap = s_cap ? s_cap * 2 : 64;
    while (ncap < needed && ncap > 0) ncap *= 2;
    if (ncap < needed) ncap = needed;
    BrowserEntry *n = realloc(s_entries, (size_t)ncap * sizeof(BrowserEntry));
    if (!n) return false;
    s_entries = n;
    s_cap = ncap;
    return true;
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

// ---------------------------------------------------------------------------
// Filter helpers (case-insensitive substring)
// ---------------------------------------------------------------------------

static bool strcasestr_fast(const char *haystack, const char *needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (!haystack) return false;
    size_t hlen = strlen(haystack);
    if (needle_len > hlen) return false;
    for (size_t i = 0; i <= hlen - needle_len; i++) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static bool matches_filter(const BrowserEntry *e) {
    if (s_filter_len == 0) return true;
    if (!e) return false;
    return strcasestr_fast(e->name, s_filter, s_filter_len);
}

// Count visible entries matching filter (for scroll calculations)
static int filtered_count(void) {
    if (s_filter_len == 0) return s_count;
    int c = 0;
    for (int i = 0; i < s_count; i++) if (matches_filter(&s_entries[i])) c++;
    return c;
}

// Convert filtered index to real index (0-based among visible)
static int filtered_to_real(int filtered_idx) {
    if (filtered_idx < 0) return -1;
    if (s_filter_len == 0) return (filtered_idx < s_count) ? filtered_idx : -1;
    int cur = -1;
    for (int i = 0; i < s_count; i++) {
        if (matches_filter(&s_entries[i])) {
            cur++;
            if (cur == filtered_idx) return i;
        }
    }
    return -1;
}

// Convert real index to filtered index, or -1 if not visible
static int real_to_filtered(int real_idx) {
    if (s_filter_len == 0) return real_idx;
    if (real_idx < 0 || real_idx >= s_count) return -1;
    if (!matches_filter(&s_entries[real_idx])) return -1;
    int c = 0;
    for (int i = 0; i < real_idx; i++) if (matches_filter(&s_entries[i])) c++;
    return c;
}

// Find next visible index after real_idx (wraps if needed), -1 if none
static int next_visible(int real_idx) {
    for (int i = real_idx + 1; i < s_count; i++) if (matches_filter(&s_entries[i])) return i;
    return -1;
}
static int prev_visible(int real_idx) {
    for (int i = real_idx - 1; i >= 0; i--) if (matches_filter(&s_entries[i])) return i;
    return -1;
}
static void clear_filter(void) {
    s_filter[0] = '\0';
    s_filter_len = 0;
    s_filtering = false;
}
static void update_filter_active(void) {
    s_filter_len = strlen(s_filter);
    s_filtering = (s_filter_len > 0);
}

/**
 * Retrieve the current filter string.
 *
 * @return Const pointer to the internal NUL-terminated filter buffer.
 */
const char *browser_get_filter(void) {
    return s_filter;
}

/**
 * Clear the active incremental search filter string.
 *
 * Resets the filter buffer to empty and marks filtering inactive without
 * altering directory expansion state.
 */
void browser_clear_filter(void) {
    clear_filter();
}

/**
 * Append a character to the incremental search filter buffer.
 *
 * Validates that c is a printable ASCII character (32..126) and that buffer
 * capacity is not exceeded. Updates the active filter and moves selection
 * to the first matching entry if the current selection is filtered out.
 *
 * @param c Character to append to the filter string.
 * @return true if character was appended, false if rejected or buffer full.
 */
bool browser_filter_add_char(char c) {
    // Only accept printable ASCII characters (32..126)
    if (c < 32 || c > 126) return false;

    if (s_filter_len >= sizeof(s_filter) - 1) return false;

    s_filter[s_filter_len] = c;
    s_filter_len++;
    s_filter[s_filter_len] = '\0';
    s_filtering = true;

    if (s_selected >= 0 && s_selected < s_count && !matches_filter(&s_entries[s_selected])) {
        // Move to first matching entry
        for (int i = 0; i < s_count; i++) {
            if (matches_filter(&s_entries[i])) {
                s_selected = i;
                break;
            }
        }
    }
    return true;
}

/**
 * Scan immediate children of a directory into a temporary array, sorted
 * folders-first. Hidden files (dot prefix) are skipped. For directories, we
 * include all subdirectories; for files, only supported image extensions.
 */
static BrowserEntry *scan_dir_entries(const char *dir, int *out_n, int depth) {
    if (!out_n) return NULL;
    *out_n = 0;
    if (!dir || dir[0] == '\0') return NULL;

    DIR *d = opendir(dir);
    if (!d) return NULL;

    int cap = 64, n = 0;
    BrowserEntry *arr = malloc((size_t)cap * sizeof(BrowserEntry));
    if (!arr) { closedir(d); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // Skip hidden

        char full[PATH_MAX];
        if (!viewer_path_join(full, sizeof(full), dir, ent->d_name)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_file = S_ISREG(st.st_mode);

        // For files, filter to image types only
        if (is_file && !viewer_is_image_file(ent->d_name)) continue;
        if (!is_dir && !is_file) continue;

        size_t name_len = strlen(ent->d_name);
        if (name_len >= sizeof(((BrowserEntry*)0)->name)) continue;
        size_t full_len = strlen(full);
        if (full_len >= sizeof(((BrowserEntry*)0)->path)) continue;

        if (n >= cap) {
            int ncap = cap * 2;
            BrowserEntry *nn = realloc(arr, (size_t)ncap * sizeof(BrowserEntry));
            if (!nn) break;
            arr = nn;
            cap = ncap;
        }
        BrowserEntry *e = &arr[n++];
        memset(e, 0, sizeof(*e));
        memcpy(e->path, full, full_len + 1);
        memcpy(e->name, ent->d_name, name_len + 1);
        e->is_dir = is_dir;
        e->expanded = false;
        e->depth = depth;
    }
    closedir(d);

    if (n == 0) {
        free(arr);
        return NULL;
    }

    if (n > 1) qsort(arr, (size_t)n, sizeof(BrowserEntry), entry_cmp);
    *out_n = n;
    return arr;
}

static void rebuild_root(const char *root) {
    s_count = 0;
    s_selected = 0;
    s_scroll = 0;

    if (!root || root[0] == '\0') {
        s_root[0] = '\0';
        return;
    }
    snprintf(s_root, sizeof(s_root), "%s", root);

    int n = 0;
    BrowserEntry *arr = scan_dir_entries(s_root, &n, 0);
    if (!arr || n <= 0) return;
    if (ensure_cap(n) && s_entries) {
        memcpy(s_entries, arr, (size_t)n * sizeof(BrowserEntry));
        s_count = n;
    }
    free(arr);
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

/**
 * Expand a directory entry at idx by inserting its children after it.
 * Returns true if expansion occurred.
 */
static bool expand_entry(int idx) {
    if (idx < 0 || idx >= s_count) return false;
    if (!s_entries[idx].is_dir || s_entries[idx].expanded) return false;
    if (s_entries[idx].depth >= 32) return false;

    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s", s_entries[idx].path);

    struct stat target_st;
    if (stat(dir_path, &target_st) != 0) return false;

    // Detect symlink loops by checking ancestors
    int anc = find_parent(idx);
    while (anc >= 0) {
        struct stat anc_st;
        if (stat(s_entries[anc].path, &anc_st) == 0) {
            if (anc_st.st_ino == target_st.st_ino && anc_st.st_dev == target_st.st_dev) {
                return false; // loop detected
            }
        }
        anc = find_parent(anc);
    }

    int parent_depth = s_entries[idx].depth;

    int n = 0;
    BrowserEntry *children = scan_dir_entries(dir_path, &n, parent_depth + 1);
    if (!children || n == 0) {
        free(children);
        // Mark expanded even if empty to avoid retry
        s_entries[idx].expanded = true;
        return true;
    }

    if (!ensure_cap(s_count + n)) {
        free(children);
        return false;
    }

    // Shift tail after idx
    memmove(&s_entries[idx + 1 + n], &s_entries[idx + 1], (size_t)(s_count - idx - 1) * sizeof(BrowserEntry));
    memcpy(&s_entries[idx + 1], children, (size_t)n * sizeof(BrowserEntry));
    s_count += n;
    s_entries[idx].expanded = true;
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Initialize file browser overlay state with viewer's current directory.
 *
 * Safe to call multiple times; queries viewer's g_current_dir (or falls back
 * to getcwd()) and populates immediate directory entries at depth 0.
 */
void browser_init(void) {
    // Initialize from viewer's current directory if available
    if (g_current_dir[0] != '\0') {
        rebuild_root(g_current_dir);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) rebuild_root(cwd);
    }
}

/**
 * Set the browser root to a specific directory path and rebuild entries.
 *
 * Resolves symlinks via realpath(). Validates that path exists and is a
 * directory before resetting entries and rebuilding the root listing.
 *
 * @param path Target filesystem directory path.
 */
void browser_set_root(const char *path) {
    if (!path || path[0] == '\0') return;
    if (strlen(path) >= PATH_MAX) return;
    char abs_path[PATH_MAX];
    const char *use = path;
    if (realpath(path, abs_path)) use = abs_path;
    struct stat st;
    if (stat(use, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    rebuild_root(use);
}

/**
 * Toggle browser overlay visibility.
 *
 * When opening: checks whether g_current_dir has changed externally and
 * rebuilds the root tree if needed, clears any active search filter, clamps
 * selection to valid bounds, and starts SDL text input.
 * When closing: stops SDL text input and hides the overlay.
 */
void browser_toggle(void) {
    if (s_open) {
        s_open = false;
        SDL_StopTextInput();
        return;
    }
    // Opening: refresh from viewer's current dir to reflect external navigation
    if (g_current_dir[0] != '\0') {
        char abs_cur[PATH_MAX];
        const char *cur = g_current_dir;
        if (realpath(g_current_dir, abs_cur)) cur = abs_cur;
        if (strcmp(s_root, cur) != 0) {
            rebuild_root(cur);
        }
    } else {
        browser_init();
    }
    clear_filter();
    if (s_selected < 0) s_selected = 0;
    if (s_selected >= s_count) s_selected = s_count - 1;
    // Ensure selected is visible when filtered
    if (s_count > 0 && !matches_filter(&s_entries[s_selected])) {
        int nxt = next_visible(s_selected);
        if (nxt == -1) nxt = prev_visible(s_selected);
        if (nxt != -1) s_selected = nxt;
    }
    s_open = true;
    SDL_StartTextInput();
}

/**
 * Query whether the browser overlay is currently visible.
 *
 * @return true if the overlay is open, false otherwise.
 */
bool browser_is_open(void) { return s_open; }

static void clamp_scroll(int visible_rows) {
    if (s_count <= 0) {
        s_scroll = 0;
        s_selected = 0;
        return;
    }
    if (s_filter[0] == '\0') {
        if (s_selected < s_scroll) s_scroll = s_selected;
        if (s_selected >= s_scroll + visible_rows) s_scroll = s_selected - visible_rows + 1;
        if (s_scroll < 0) s_scroll = 0;
        if (s_scroll > s_count - visible_rows) s_scroll = s_count - visible_rows;
        if (s_scroll < 0) s_scroll = 0;
        return;
    }
    // Filtered mode: s_scroll is filtered offset, s_selected is real index
    int sel_f = real_to_filtered(s_selected);
    int fcount = filtered_count();
    if (fcount <= 0 || sel_f == -1) {
        s_scroll = 0;
        return;
    }
    if (sel_f < s_scroll) s_scroll = sel_f;
    if (sel_f >= s_scroll + visible_rows) s_scroll = sel_f - visible_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > fcount - visible_rows) s_scroll = fcount - visible_rows;
    if (s_scroll < 0) s_scroll = 0;
}

/**
 * Handle keyboard input when the browser overlay is active.
 *
 * Intercepts navigation keys (Up/Down/PgUp/PgDn/Home/End), expand/collapse
 * (Left/Right, Backspace), activation (Return/Space to expand folder or load image),
 * search filter input (Ctrl+F, Backspace, printable characters), and dismiss (ESC).
 *
 * @param key SDL keycode of the pressed key.
 * @param mod Active SDL key modifier bitmask.
 * @return true if the key event was consumed by the browser, false otherwise.
 */
bool browser_handle_key(SDL_Keycode key, SDL_Keymod mod) {
    if (!s_open) return false;

    int panel_h = g_win_h - 2 * kPanelMargin;
    int list_h = panel_h - kTitleH - kFooterH;
    int visible = list_h / kRowH;
    if (visible < 1) visible = 1;

    // Ctrl+F: focus filter (clear and start typing)
    if ((mod & KMOD_CTRL) && key == SDLK_f) {
        clear_filter();
        s_filtering = true;
        return true;
    }
    // ESC: clear filter if active, otherwise close browser
    if (key == SDLK_ESCAPE) {
        if (s_filter[0] != '\0') {
            clear_filter();
            // Ensure selected is visible after clearing
            if (s_selected < 0) s_selected = 0;
            clamp_scroll(visible);
            return true;
        }
        s_open = false;
        SDL_StopTextInput();
        return true;
    }
    // Backspace: when filter active, edit filter instead of collapsing
    if (key == SDLK_BACKSPACE) {
        if (s_filter[0] != '\0') {
            size_t len = strlen(s_filter);
            if (len > 0) s_filter[len-1] = '\0';
            update_filter_active();
            // Ensure selected still matches filter
            if (s_selected >= 0 && s_selected < s_count && !matches_filter(&s_entries[s_selected])) {
                int nxt = next_visible(s_selected);
                if (nxt == -1) nxt = prev_visible(s_selected);
                if (nxt != -1) s_selected = nxt;
            }
            clamp_scroll(visible);
            return true;
        }
        // If no filter, fall through to collapse handling below
    }
    // Filter typing: handle printable characters (without Ctrl/Alt) as filter input
    // We also handle this via SDL_TEXTINPUT for proper unicode, but SDL_KEYDOWN covers simple ASCII.
    // Only handle if not a control key we already processed and no Ctrl/Alt held.
    if (!(mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) && key >= 32 && key <= 126) {
        // Avoid handling keys that are already bound (arrows, etc.) - those are <32 or special
        // For letters/numbers/symbols, treat as filter input when browser is open and not in a special context
        // Check if it's a simple character key (not Return, Tab, etc. which have codes <32 or are handled)
        bool is_printable = false;
        if ((key >= SDLK_a && key <= SDLK_z) || (key >= SDLK_0 && key <= SDLK_9) ||
            key == SDLK_SPACE || key == SDLK_MINUS || key == SDLK_EQUALS || key == SDLK_PERIOD ||
            key == SDLK_COMMA || key == SDLK_SLASH || key == SDLK_BACKSLASH || key == SDLK_SEMICOLON ||
            key == SDLK_QUOTE || key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET || key == SDLK_BACKQUOTE) {
            is_printable = true;
        }
        // Also allow uppercase via Shift: SDLK_a still reports 'a' but we check shift separately for display
        if (is_printable) {
            if (browser_filter_add_char((char)key)) {
                clamp_scroll(visible);
            }
            return true;
        }
    }

    switch (key) {
    case SDLK_UP:
    case SDLK_k: {
        if (s_count <= 0) return true;
        int prev = prev_visible(s_selected);
        if (prev != -1) s_selected = prev;
        else if (s_filter[0] == '\0' && s_selected > 0) s_selected--;
        clamp_scroll(visible);
        return true;
    }
    case SDLK_DOWN:
    case SDLK_j: {
        if (s_count <= 0) return true;
        int nxt = next_visible(s_selected);
        if (nxt != -1) s_selected = nxt;
        else if (s_filter[0] == '\0' && s_selected + 1 < s_count) s_selected++;
        clamp_scroll(visible);
        return true;
    }
    case SDLK_HOME: {
        if (s_count <= 0) { s_selected = 0; s_scroll = 0; return true; }
        // First visible
        for (int i = 0; i < s_count; i++) if (matches_filter(&s_entries[i])) { s_selected = i; break; }
        s_scroll = 0;
        return true;
    }
    case SDLK_END: {
        if (s_count <= 0) { s_selected = 0; s_scroll = 0; return true; }
        for (int i = s_count-1; i >=0; i--) if (matches_filter(&s_entries[i])) { s_selected = i; break; }
        clamp_scroll(visible);
        return true;
    }
    case SDLK_PAGEUP: {
        if (s_count <= 0) return true;
        for (int i = 0; i < visible; i++) {
            int prev = prev_visible(s_selected);
            if (prev == -1) break;
            s_selected = prev;
        }
        clamp_scroll(visible);
        return true;
    }
    case SDLK_PAGEDOWN: {
        if (s_count <= 0) return true;
        for (int i = 0; i < visible; i++) {
            int nxt = next_visible(s_selected);
            if (nxt == -1) break;
            s_selected = nxt;
        }
        clamp_scroll(visible);
        return true;
    }
    case SDLK_RIGHT:
        if (s_selected >= 0 && s_selected < s_count && s_entries[s_selected].is_dir) {
            if (!s_entries[s_selected].expanded) expand_entry(s_selected);
            else {
                int nxt = next_visible(s_selected);
                if (nxt != -1) { s_selected = nxt; clamp_scroll(visible); }
            }
        }
        return true;
    case SDLK_LEFT:
    case SDLK_BACKSPACE: {
        if (s_selected < 0 || s_selected >= s_count) {
            char tmp[PATH_MAX];
            snprintf(tmp, sizeof(tmp), "%s", s_root);
            char *par = dirname(tmp);
            if (par && strcmp(par, s_root) != 0 && strcmp(par, ".") != 0) {
                rebuild_root(par);
                snprintf(g_current_dir, sizeof(g_current_dir), "%s", par);
            }
            return true;
        }
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
                snprintf(tmp, sizeof(tmp), "%s", s_root);
                char *par = dirname(tmp);
                if (par && strcmp(par, s_root) != 0 && strcmp(par, ".") != 0) {
                    rebuild_root(par);
                    // Update viewer's dir to reflect navigation (but don't load file yet)
                    snprintf(g_current_dir, sizeof(g_current_dir), "%s", par);
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
            int pane = viewer_get_active_pane();
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

/**
 * Handle mouse events when the browser overlay is active.
 *
 * Consumes clicks within the panel to select entries, double-clicks to
 * expand/collapse folders or load images into the active pane, mouse wheel
 * to scroll visible items, and clicks outside the panel bounds to dismiss.
 *
 * @param ev Pointer to the SDL event (mouse button, motion, or wheel).
 * @return true if the event was consumed, false otherwise.
 */
bool browser_handle_event(const SDL_Event *ev) {
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
        // Click inside list area (respect filter)
        if (s_count > 0 && my >= list_y && my < list_y + list_h) {
            int row = (my - list_y) / kRowH;
            int f_idx = s_scroll + row;
            int idx;
            if (s_filter[0] == '\0') idx = f_idx;
            else idx = filtered_to_real(f_idx);
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
                        int pane = viewer_get_active_pane();
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
        int fcount = (s_filter[0] == '\0') ? s_count : filtered_count();
        if (fcount <= 0) {
            s_scroll = 0;
            return true;
        }
        if (ev->wheel.y > 0) {
            s_scroll -= 3;
            if (s_scroll < 0) s_scroll = 0;
        } else if (ev->wheel.y < 0) {
            s_scroll += 3;
            int max_scroll = fcount - visible;
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

/**
 * Render the file browser overlay onto the target SDL renderer.
 *
 * No-op if the browser is closed or window dimensions are invalid.
 * Renders semi-transparent backdrop, panel border, title bar with path
 * and active filter query, indented entry list with folder expand indicators,
 * selection highlight, and footer keyboard shortcut hints.
 *
 * @param ren Target SDL renderer.
 */
void browser_render(SDL_Renderer *ren) {
    if (!s_open || !ren || g_win_w <= 0 || g_win_h <= 0) return;

    int panel_w = (g_win_w * 65) / 100;
    if (panel_w < 400) panel_w = g_win_w - 2 * kPanelMargin;
    if (panel_w > g_win_w - 2 * kPanelMargin) panel_w = g_win_w - 2 * kPanelMargin;
    if (panel_w <= 0) return;
    int panel_h = g_win_h - 2 * kPanelMargin;
    if (panel_h <= 0) return;
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

    // Title bar (shows filter when active)
    SDL_Rect title_bg = {px, py, panel_w, kTitleH};
    SDL_SetRenderDrawColor(ren, 38, 38, 38, 255);
    SDL_RenderFillRect(ren, &title_bg);
    SDL_SetRenderDrawColor(ren, 70, 70, 70, 255);
    SDL_RenderDrawLine(ren, px, py + kTitleH, px + panel_w, py + kTitleH);

    SDL_Color title_col = {220, 220, 220, 255};
    SDL_Color dim_col = {160, 160, 160, 255};
    char title_buf[PATH_MAX + 300];
    if (s_filter[0] != '\0') {
        snprintf(title_buf, sizeof(title_buf), " %s  [filter: %s]", s_root, s_filter);
    } else {
        snprintf(title_buf, sizeof(title_buf), " %s", s_root);
    }
    int max_title_chars2 = (panel_w - 20) / 8;
    if (max_title_chars2 < 4) {
        title_buf[0] = '\0';
    } else if ((int)strlen(title_buf) > max_title_chars2) {
        int keep = max_title_chars2 - 3;
        size_t tlen = strlen(title_buf);
        if (keep > (int)tlen) keep = (int)tlen;
        if (keep < 0) keep = 0;
        char tmp2[PATH_MAX + 300];
        snprintf(tmp2, sizeof(tmp2), "...%s", title_buf + (tlen - (size_t)keep));
        snprintf(title_buf, sizeof(title_buf), "%s", tmp2);
    }
    text_draw(ren, px + 8, py + 9, title_buf, title_col, 1);

    // List area
    int list_y = py + kTitleH;
    int list_h = panel_h - kTitleH - kFooterH;
    int visible = list_h / kRowH;
    clamp_scroll(visible);

    // If filtered and no matches, show message
    int fcount = filtered_count();
    if (s_count == 0) {
        SDL_Rect clip2 = {px, list_y, panel_w, list_h};
        SDL_RenderSetClipRect(ren, &clip2);
        SDL_Color dim = {160, 160, 160, 255};
        text_draw(ren, px + 12, list_y + 8, "(Empty directory)", dim, 1);
        SDL_RenderSetClipRect(ren, NULL);
    } else if (s_filter[0] != '\0' && fcount == 0) {
        SDL_Rect clip2 = {px, list_y, panel_w, list_h};
        SDL_RenderSetClipRect(ren, &clip2);
        SDL_Color dim = {160,160,160,255};
        text_draw(ren, px+12, list_y+8, "No matches", dim, 1);
        SDL_RenderSetClipRect(ren, NULL);
    }

    SDL_Rect clip = {px, list_y, panel_w, list_h};
    SDL_RenderSetClipRect(ren, &clip);

    int idx = (s_filter_len == 0) ? s_scroll : filtered_to_real(s_scroll);
    for (int i = 0; i < visible; i++) {
        if (idx < 0 || idx >= s_count) break;
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
            text_draw_clipped(ren, tx, row_y + 5, e->name, name_col, 1, panel_w - (tx - px) - 8);
        } else {
            // File: bullet
            text_draw(ren, tx, row_y + 5, " ", name_col, 1);
            tx += 16;
            text_draw_clipped(ren, tx, row_y + 5, e->name, name_col, 1, panel_w - (tx - px) - 8);
        }

        idx = (s_filter_len == 0) ? idx + 1 : next_visible(idx);
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

/**
 * Release all allocated heap memory and reset browser state.
 *
 * Frees internal entry array, resets counters and scroll offsets, and
 * closes the overlay. Safe to call multiple times.
 */
void browser_cleanup(void) {
    free(s_entries);
    s_entries = NULL;
    s_count = 0;
    s_cap = 0;
    s_selected = 0;
    s_scroll = 0;
    s_root[0] = '\0';
    clear_filter();
    s_open = false;
}
