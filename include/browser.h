#ifndef BROWSER_H
#define BROWSER_H

/**
 * browser.h - File browser overlay with tree navigation.
 *
 * Provides an ESC-toggled file browser that shows a navigable directory tree
 * starting from the current image's folder. Supports:
 *   - Folder expand/collapse (inline tree, not just cd)
 *   - File and folder selection via keyboard and mouse
 *   - Loading selected images into the viewer's active pane
 *   - Incremental substring filtering
 *
 * The browser owns its own entry list (independent from viewer's n/p list)
 * but updates viewer state when a file is opened.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>

/**
 * Initialize file browser overlay state with viewer's current directory.
 *
 * Safe to call multiple times; queries viewer's g_current_dir (or falls back
 * to getcwd()) and populates immediate directory entries at depth 0.
 */
void browser_init(void);

/**
 * Toggle browser overlay visibility.
 *
 * When opening: checks whether g_current_dir has changed externally and
 * rebuilds the root tree if needed, clears any active search filter, clamps
 * selection to valid bounds, and starts SDL text input.
 * When closing: stops SDL text input and hides the overlay.
 */
void browser_toggle(void);

/**
 * Query whether the browser overlay is currently visible.
 *
 * @return true if the overlay is open, false otherwise.
 */
bool browser_is_open(void);

/**
 * Set the browser root to a specific directory path and rebuild entries.
 *
 * Resolves symlinks via realpath(). Validates that path exists and is a
 * directory before resetting entries and rebuilding the root listing.
 *
 * @param path Target filesystem directory path.
 */
void browser_set_root(const char *path);

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
bool browser_handle_key(SDL_Keycode key, SDL_Keymod mod);

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
bool browser_handle_event(const SDL_Event *ev);

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
bool browser_filter_add_char(char c);

/**
 * Retrieve the current filter string.
 *
 * @return Const pointer to the internal NUL-terminated filter buffer.
 */
const char *browser_get_filter(void);

/**
 * Clear the active incremental search filter string.
 *
 * Resets the filter buffer to empty and marks filtering inactive without
 * altering directory expansion state.
 */
void browser_clear_filter(void);

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
void browser_render(SDL_Renderer *ren);

/**
 * Release all allocated heap memory and reset browser state.
 *
 * Frees internal entry array, resets counters and scroll offsets, and
 * closes the overlay. Safe to call multiple times.
 */
void browser_cleanup(void);

#endif /* BROWSER_H */
