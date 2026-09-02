#pragma once

/**
 * clipboard.h - Clipboard image copy/paste via external tools.
 *
 * Uses xclip/xsel (X11) or wl-copy/wl-paste (Wayland) when available.
 * Falls back to SDL_SetClipboardText for path-as-text when image tools are
 * absent. Requires a temporary PNG file via stb_image_write for the image
 * payload; the temp file is removed after the copy command completes.
 *
 * No direct X11/Wayland library dependency is introduced to keep the build
 * minimal and portable. If neither tool is installed, copy/paste silently
 * reports failure and the caller may show a diagnostic.
 */

#include <stdbool.h>

// Attempt to copy the image at `path` (or the active pane's file) to the
// system clipboard as image/png. Returns true if an image tool succeeded.
// Falls back to copying the path as text if image tools are unavailable.
bool clipboard_copy_path(const char *path);

// Copy raw pixel data (w*h*4 RGBA) to clipboard as PNG. The caller provides
// decoded pixels (e.g., from stbi_load). This is used when the viewer wants
// to copy the current texture without re-reading the file.
bool clipboard_copy_rgba(const unsigned char *rgba, int w, int h, const char *fallback_path);

// Attempt to paste an image from the clipboard. If successful, writes the
// pasted PNG data to a temporary file, returns the temp path (owned, must
// be freed with free() and unlinked by caller). Returns NULL on failure.
// The caller should then load the temp file via viewer_replace_image and
// remove it.
char *clipboard_paste_to_temp(void);

// Check if any image clipboard tool is available (xclip, xsel, wl-copy).
bool clipboard_has_image_tool(void);

// Simple text fallback check.
bool clipboard_copy_text(const char *text);
