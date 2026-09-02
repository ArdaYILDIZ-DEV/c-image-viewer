#ifndef CLIPBOARD_H
#define CLIPBOARD_H

/**
 * clipboard.h - Clipboard image copy/paste via external tools.
 *
 * Uses xclip/xsel (X11) or wl-copy/wl-paste (Wayland) when available.
 * Falls back to text copy of image path when image tools are absent.
 * Directly executes tools via fork/exec without invoking /bin/sh to
 * eliminate command injection risks. Requires a temporary PNG file
 * via stb_image_write for raw pixel payloads; temporary files are
 * atomically created with 0600 permissions and immediately unlinked.
 */

#include <stdbool.h>

/**
 * Copy an image file to the system clipboard as image/png.
 *
 * Decodes the file into RGBA pixels via stb_image and forwards to
 * clipboard_copy_rgba. If image tools are unavailable or decoding fails,
 * falls back to copying the path string as plain text.
 *
 * @param path Filesystem path to the image file to copy.
 * @return true if an image tool or text copy succeeded, false on error.
 */
bool clipboard_copy_path(const char *path);

/**
 * Copy raw RGBA pixel data to the clipboard as PNG.
 *
 * Writes pixel data to a secure temporary PNG file via stb_image_write
 * and feeds it into wl-copy or xclip via direct process execution. If image
 * clipboard tools fail or are unavailable, falls back to copying fallback_path
 * as plain text if provided.
 *
 * @param rgba Pointer to decoded RGBA pixel buffer (w * h * 4 bytes).
 * @param w Image width in pixels (> 0).
 * @param h Image height in pixels (> 0).
 * @param fallback_path Optional file path used for plain text fallback.
 * @return true if copying succeeded via image tool or text fallback, false otherwise.
 */
bool clipboard_copy_rgba(const unsigned char *rgba, int w, int h, const char *fallback_path);

/**
 * Paste image data from the clipboard into a temporary PNG file.
 *
 * Invokes wl-paste, xclip, or xsel to retrieve image/png data, writing
 * to a temporary file created via mkstemps(). Validates that the resulting
 * file is a valid decodable image via stb_image before returning.
 *
 * @return Owned heap-allocated path to the temporary file on success
 *         (caller must unlink and free() with free()), or NULL on failure.
 */
char *clipboard_paste_to_temp(void);

/**
 * Check if an external image clipboard tool is available in PATH.
 *
 * Inspects WAYLAND_DISPLAY and checks for wl-copy/wl-paste under Wayland,
 * or xclip/xsel under X11, using direct stat/access checks without spawning a shell.
 *
 * @return true if at least one image-capable clipboard tool exists, false otherwise.
 */
bool clipboard_has_image_tool(void);

/**
 * Copy a plain text string to the system clipboard.
 *
 * Passes text directly through an anonymous pipe to wl-copy, xclip, or xsel
 * without shell execution.
 *
 * @param text NUL-terminated text string to copy.
 * @return true if the tool accepted the input and exited successfully, false otherwise.
 */
bool clipboard_copy_text(const char *text);

#endif /* CLIPBOARD_H */
