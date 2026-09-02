/**
 * clipboard.c - Implementation of clipboard copy/paste via external commands.
 *
 * Implementation notes:
 *   - Detection order: Wayland (WAYLAND_DISPLAY set) tries wl-copy/wl-paste
 *     first, otherwise X11 tools (xclip, then xsel). This avoids trying X11
 *     under Wayland where xclip may exist but not be functional.
 *   - Copy: A temporary PNG is created with stb_image_write, then piped via
 *     shell command `cat <tmp> | <tool> -selection clipboard -t image/png`.
 *     This avoids needing to link against X11/Wayland libraries and works with
 *     the tools' stdin interface. The temp file is unlinked immediately after.
 *   - Paste: Tries `wl-paste --type image/png` then `xclip -selection clipboard -t image/png -o`
 *     and `xclip -selection clipboard -o` (for fallback). Output is captured
 *     to a temp file via shell redirection. If the output is a valid image,
 *     the temp path is returned.
 *   - All shell commands are constructed with proper quoting via snprintf and
 *     executed via system() or popen(). Paths with spaces are handled by
 *     shell-escaping single quotes (not needed for our temp files which use
 *     mkstemp pattern).
 *   - Security: Temp files are created with mkstemp (0600) in /tmp and removed
 *     after use. No user-controlled paths are interpolated into shell commands
 *     without validation beyond the temp file we created.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "clipboard.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

// Check if a command exists in PATH via `command -v`
static bool command_exists(const char *cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    int ret = system(buf);
    return ret == 0;
}

bool clipboard_has_image_tool(void) {
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0') {
        if (command_exists("wl-copy") || command_exists("wl-paste")) return true;
    }
    if (command_exists("xclip")) return true;
    if (command_exists("xsel")) return true;
    return false;
}

bool clipboard_copy_text(const char *text) {
    if (!text) return false;
    // Use SDL clipboard as primary text fallback if available? But we have no SDL here.
    // Try external tools for text as well for consistency.
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0' && command_exists("wl-copy")) {
        char cmd[PATH_MAX + 64];
        // Use printf piped to wl-copy
        // Note: text may contain single quotes; for simplicity use echo -n with proper escaping is complex.
        // Use a temp file for text to avoid shell injection.
        char tmpl[] = "/tmp/civ_clip_txt_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd != -1) {
            FILE *f = fdopen(fd, "w");
            if (f) { fputs(text, f); fclose(f); }
            else close(fd);
            snprintf(cmd, sizeof(cmd), "cat '%s' | wl-copy 2>/dev/null; rm -f '%s'", tmpl, tmpl);
            int r = system(cmd);
            return r == 0;
        }
    }
    if (command_exists("xclip")) {
        char tmpl[] = "/tmp/civ_clip_txt_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd != -1) {
            FILE *f = fdopen(fd, "w");
            if (f) { fputs(text, f); fclose(f); }
            else close(fd);
            char cmd[PATH_MAX + 64];
            snprintf(cmd, sizeof(cmd), "cat '%s' | xclip -selection clipboard 2>/dev/null; rm -f '%s'", tmpl, tmpl);
            int r = system(cmd);
            return r == 0;
        }
    }
    return false;
}

static bool copy_temp_png_via_tool(const char *tmp_png) {
    const char *wayland = getenv("WAYLAND_DISPLAY");
    char cmd[PATH_MAX * 2 + 128];

    if (wayland && wayland[0] != '\0' && command_exists("wl-copy")) {
        // wl-copy supports --type; try image/png first
        snprintf(cmd, sizeof(cmd), "cat '%s' | wl-copy --type image/png 2>/dev/null", tmp_png);
        if (system(cmd) == 0) return true;
        // Fallback to plain copy
        snprintf(cmd, sizeof(cmd), "cat '%s' | wl-copy 2>/dev/null", tmp_png);
        if (system(cmd) == 0) return true;
    }
    if (command_exists("xclip")) {
        snprintf(cmd, sizeof(cmd), "cat '%s' | xclip -selection clipboard -t image/png 2>/dev/null", tmp_png);
        if (system(cmd) == 0) return true;
        // Try without mime type
        snprintf(cmd, sizeof(cmd), "cat '%s' | xclip -selection clipboard 2>/dev/null", tmp_png);
        if (system(cmd) == 0) return true;
    }
    if (command_exists("xsel")) {
        snprintf(cmd, sizeof(cmd), "cat '%s' | xsel --clipboard --input 2>/dev/null", tmp_png);
        if (system(cmd) == 0) return true;
    }
    return false;
}

bool clipboard_copy_rgba(const unsigned char *rgba, int w, int h, const char *fallback_path) {
    if (!rgba || w <= 0 || h <= 0) {
        if (fallback_path) return clipboard_copy_path(fallback_path);
        return false;
    }

    char tmpl[] = "/tmp/civ_clip_XXXXXX.png";
    // mkstemp requires template ending with XXXXXX, so we need separate handling
    char tmp_template[] = "/tmp/civ_clip_XXXXXX";
    int fd = mkstemp(tmp_template);
    if (fd == -1) return false;
    close(fd);
    // Append .png and rename
    char png_path[PATH_MAX];
    snprintf(png_path, sizeof(png_path), "%s.png", tmp_template);
    if (rename(tmp_template, png_path) != 0) {
        // If rename fails, use original
        strncpy(png_path, tmp_template, sizeof(png_path) - 1);
    }

    // Write PNG via stb_image_write
    int ok = stbi_write_png(png_path, w, h, 4, rgba, w * 4);
    if (!ok) {
        unlink(png_path);
        unlink(tmp_template);
        if (fallback_path) return clipboard_copy_text(fallback_path);
        return false;
    }

    bool copied = copy_temp_png_via_tool(png_path);
    unlink(png_path);
    // Also try to remove temp without .png if it still exists
    unlink(tmp_template);

    if (!copied && fallback_path) {
        // Fallback to path as text
        return clipboard_copy_text(fallback_path);
    }
    return copied;
}

bool clipboard_copy_path(const char *path) {
    if (!path) return false;
    // Try to decode and copy as image first for fidelity
    int w, h, comp;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (data) {
        bool r = clipboard_copy_rgba(data, w, h, path);
        stbi_image_free(data);
        return r;
    }
    // If decode fails, fallback to copying path as text
    return clipboard_copy_text(path);
}

char *clipboard_paste_to_temp(void) {
    char tmpl[] = "/tmp/civ_paste_XXXXXX.png";
    char template_noext[] = "/tmp/civ_paste_XXXXXX";
    int fd = mkstemp(template_noext);
    if (fd == -1) return NULL;
    close(fd);
    char out_path[PATH_MAX];
    snprintf(out_path, sizeof(out_path), "%s.png", template_noext);
    if (rename(template_noext, out_path) != 0) {
        strncpy(out_path, template_noext, sizeof(out_path) - 1);
    }

    char cmd[PATH_MAX * 2 + 128];
    bool got = false;

    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0' && command_exists("wl-paste")) {
        snprintf(cmd, sizeof(cmd), "wl-paste --type image/png > '%s' 2>/dev/null", out_path);
        if (system(cmd) == 0) {
            struct stat st;
            if (stat(out_path, &st) == 0 && st.st_size > 0) got = true;
        }
        if (!got) {
            snprintf(cmd, sizeof(cmd), "wl-paste > '%s' 2>/dev/null", out_path);
            if (system(cmd) == 0) {
                struct stat st;
                if (stat(out_path, &st) == 0 && st.st_size > 0) got = true;
            }
        }
    }
    if (!got && command_exists("xclip")) {
        snprintf(cmd, sizeof(cmd), "xclip -selection clipboard -t image/png -o > '%s' 2>/dev/null", out_path);
        if (system(cmd) == 0) {
            struct stat st;
            if (stat(out_path, &st) == 0 && st.st_size > 0) got = true;
        }
    }
    if (!got && command_exists("xclip")) {
        snprintf(cmd, sizeof(cmd), "xclip -selection clipboard -o > '%s' 2>/dev/null", out_path);
        if (system(cmd) == 0) {
            struct stat st;
            if (stat(out_path, &st) == 0 && st.st_size > 100) { // heuristic for text vs image
                // Try to validate if it's an image by attempting stbi_load
                int w, h, c;
                unsigned char *d = stbi_load(out_path, &w, &h, &c, 4);
                if (d) { stbi_image_free(d); got = true; }
                else {
                    // Might be text path; check if file contains a valid image path
                    FILE *f = fopen(out_path, "r");
                    if (f) {
                        char line[PATH_MAX];
                        if (fgets(line, sizeof(line), f)) {
                            // Trim newline
                            line[strcspn(line, "\r\n")] = '\0';
                            struct stat st2;
                            if (stat(line, &st2) == 0 && S_ISREG(st2.st_mode)) {
                                // It's a path to a file; copy that file's image instead?
                                // For now, treat as not an image paste
                                got = false;
                            }
                        }
                        fclose(f);
                    }
                }
            }
        }
    }
    if (!got && command_exists("xsel")) {
        snprintf(cmd, sizeof(cmd), "xsel --clipboard --output > '%s' 2>/dev/null", out_path);
        if (system(cmd) == 0) {
            struct stat st;
            if (stat(out_path, &st) == 0 && st.st_size > 0) got = true;
        }
    }

    if (!got) {
        unlink(out_path);
        unlink(template_noext);
        return NULL;
    }

    // Validate that the pasted file is actually a decodable image
    int w, h, c;
    unsigned char *d = stbi_load(out_path, &w, &h, &c, 4);
    if (!d) {
        unlink(out_path);
        return NULL;
    }
    stbi_image_free(d);

    char *ret = strdup(out_path);
    // Note: caller must unlink and free
    return ret;
}
