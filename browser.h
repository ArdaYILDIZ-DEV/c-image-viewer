#pragma once

/**
 * browser.h - File browser overlay with tree navigation.
 *
 * Provides an ESC-toggled file browser that shows a navigable directory tree
 * starting from the current image's folder. Supports:
 *   - Folder expand/collapse (inline tree, not just cd)
 *   - File and folder selection via keyboard and mouse
 *   - Loading selected images into the viewer's active pane
 *
 * The browser owns its own entry list (independent from viewer's n/p list)
 * but updates viewer state when a file is opened.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>

// Initialize browser with current directory derived from viewer's g_current_dir.
// Safe to call multiple times; resets state.
void browser_init(void);

// Toggle open/closed. When opening, rebuilds tree from viewer's current dir.
// When closing, preserves state for next open (but may rebuild if directory changed).
void browser_toggle(void);

// Query whether the browser overlay is currently visible.
bool browser_is_open(void);

// Force set the browser root to a specific path and rebuild.
// Used when viewer navigates to a new folder externally.
void browser_set_root(const char *path);

// Handle keyboard input when browser is open. Returns true if event was consumed.
bool browser_handle_key(SDL_Keycode key, SDL_Keymod mod);

// Handle mouse button/motion when browser is open. Returns true if consumed.
bool browser_handle_event(SDL_Event *ev);

// Render the browser overlay. No-op if not open. Should be called after
// viewer_render() to appear on top.
void browser_render(SDL_Renderer *ren);

// Free browser resources. Call on shutdown.
void browser_cleanup(void);
