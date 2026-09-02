/**
 * clipboard.c - Implementation of clipboard copy/paste via direct process execution.
 *
 * Design notes:
 *   - Security: No shell (/bin/sh) invocation ever occurs. All external commands
 *     (wl-copy, wl-paste, xclip, xsel) are executed directly via fork() and
 *     execvp() with explicit argv arrays, eliminating command injection risks.
 *   - Input/Output streaming: Payloads are passed directly through pipes or open
 *     file descriptors rather than shell redirection or cat pipelines.
 *   - Temporary files: When temp files are needed for images, they are created
 *     atomically via mkstemps() with 0600 permissions, written directly to the
 *     file descriptor, and reliably unlinked after use to avoid TOCTOU races.
 *   - Tool detection: PATH is inspected directly with stat() and access(X_OK),
 *     avoiding spawning shell subprocesses like "command -v".
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
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
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

/**
 * Check if a command exists in PATH using access(X_OK), without shell.
 */
static bool command_exists(const char *cmd) {
    if (!cmd || cmd[0] == '\0') return false;
    if (strchr(cmd, '/')) {
        struct stat st;
        return (stat(cmd, &st) == 0 && S_ISREG(st.st_mode) && access(cmd, X_OK) == 0);
    }
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";

    char *path_copy = strdup(path);
    if (!path_copy) return false;

    bool found = false;
    char *saveptr = NULL;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir) {
        if (dir[0] != '\0') {
            char full[PATH_MAX];
            int n = snprintf(full, sizeof(full), "%s/%s", dir, cmd);
            if (n > 0 && (size_t)n < sizeof(full)) {
                struct stat st;
                if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
                    found = true;
                    break;
                }
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(path_copy);
    return found;
}

/**
 * Check if an external image clipboard tool is available in PATH.
 *
 * Inspects WAYLAND_DISPLAY and checks for wl-copy/wl-paste under Wayland,
 * or xclip/xsel under X11, using direct stat/access checks without spawning a shell.
 *
 * @return true if at least one image-capable clipboard tool exists, false otherwise.
 */
bool clipboard_has_image_tool(void) {
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0') {
        if (command_exists("wl-copy") || command_exists("wl-paste")) return true;
    }
    if (command_exists("xclip")) return true;
    if (command_exists("xsel")) return true;
    return false;
}

/**
 * Execute command directly without shell. Stdin is redirected from in_fd.
 * Stdout and stderr are redirected to /dev/null.
 */
static bool run_command_with_stdin_fd(char *const argv[], int in_fd) {
    if (!argv || !argv[0]) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        if (in_fd >= 0) {
            if (dup2(in_fd, STDIN_FILENO) < 0) _exit(127);
        } else {
            int null_in = open("/dev/null", O_RDONLY);
            if (null_in >= 0) {
                dup2(null_in, STDIN_FILENO);
                if (null_in > STDERR_FILENO) close(null_in);
            }
        }

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        if (in_fd > STDERR_FILENO) close(in_fd);

        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * Execute command directly without shell, feeding data into stdin via pipe.
 * Stdout and stderr are redirected to /dev/null.
 */
static bool run_command_with_pipe_input(char *const argv[], const void *data, size_t len) {
    if (!argv || !argv[0]) return false;

    int pfd[2];
    if (pipe(pfd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }

    if (pid == 0) {
        close(pfd[1]);
        if (dup2(pfd[0], STDIN_FILENO) < 0) _exit(127);
        close(pfd[0]);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    close(pfd[0]);

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, &old_sa);

    if (data && len > 0) {
        const char *ptr = (const char *)data;
        size_t rem = len;
        while (rem > 0) {
            ssize_t n = write(pfd[1], ptr, rem);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            ptr += n;
            rem -= (size_t)n;
        }
    }
    close(pfd[1]);
    sigaction(SIGPIPE, &old_sa, NULL);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * Execute command directly without shell. Stdout is redirected to out_fd.
 * Stdin and stderr are redirected to /dev/null.
 */
static bool run_command_with_stdout_fd(char *const argv[], int out_fd) {
    if (!argv || !argv[0] || out_fd < 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        int null_in = open("/dev/null", O_RDONLY);
        if (null_in >= 0) {
            dup2(null_in, STDIN_FILENO);
            if (null_in > STDERR_FILENO) close(null_in);
        }

        if (dup2(out_fd, STDOUT_FILENO) < 0) _exit(127);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }

        if (out_fd > STDERR_FILENO) close(out_fd);

        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void stbi_write_to_fd_cb(void *context, void *data, int size) {
    int fd = *(int *)context;
    const char *ptr = (const char *)data;
    while (size > 0) {
        ssize_t n = write(fd, ptr, (size_t)size);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ptr += n;
        size -= (int)n;
    }
}

static bool write_png_to_fd(int fd, const unsigned char *rgba, int w, int h) {
    if (fd < 0 || !rgba || w <= 0 || h <= 0) return false;
    return stbi_write_png_to_func(stbi_write_to_fd_cb, &fd, w, h, 4, rgba, w * 4) != 0;
}

/**
 * Copy a plain text string to the system clipboard.
 *
 * Passes text directly through an anonymous pipe to wl-copy, xclip, or xsel
 * without shell execution.
 *
 * @param text NUL-terminated text string to copy.
 * @return true if the tool accepted the input and exited successfully, false otherwise.
 */
bool clipboard_copy_text(const char *text) {
    if (!text) return false;
    size_t len = strlen(text);

    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0' && command_exists("wl-copy")) {
        char *const argv[] = {"wl-copy", NULL};
        if (run_command_with_pipe_input(argv, text, len)) return true;
    }

    if (command_exists("xclip")) {
        char *const argv[] = {"xclip", "-selection", "clipboard", NULL};
        if (run_command_with_pipe_input(argv, text, len)) return true;
    }

    if (command_exists("xsel")) {
        char *const argv[] = {"xsel", "--clipboard", "--input", NULL};
        if (run_command_with_pipe_input(argv, text, len)) return true;
    }

    return false;
}

static bool copy_fd_via_tool(int fd) {
    const char *wayland = getenv("WAYLAND_DISPLAY");

    if (wayland && wayland[0] != '\0' && command_exists("wl-copy")) {
        if (lseek(fd, 0, SEEK_SET) != (off_t)-1) {
            char *const argv1[] = {"wl-copy", "--type", "image/png", NULL};
            if (run_command_with_stdin_fd(argv1, fd)) return true;
        }

        if (lseek(fd, 0, SEEK_SET) != (off_t)-1) {
            char *const argv2[] = {"wl-copy", NULL};
            if (run_command_with_stdin_fd(argv2, fd)) return true;
        }
    }

    if (command_exists("xclip")) {
        if (lseek(fd, 0, SEEK_SET) != (off_t)-1) {
            char *const argv1[] = {"xclip", "-selection", "clipboard", "-t", "image/png", NULL};
            if (run_command_with_stdin_fd(argv1, fd)) return true;
        }

        if (lseek(fd, 0, SEEK_SET) != (off_t)-1) {
            char *const argv2[] = {"xclip", "-selection", "clipboard", NULL};
            if (run_command_with_stdin_fd(argv2, fd)) return true;
        }
    }

    if (command_exists("xsel")) {
        if (lseek(fd, 0, SEEK_SET) != (off_t)-1) {
            char *const argv[] = {"xsel", "--clipboard", "--input", NULL};
            if (run_command_with_stdin_fd(argv, fd)) return true;
        }
    }

    return false;
}

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
bool clipboard_copy_rgba(const unsigned char *rgba, int w, int h, const char *fallback_path) {
    if (!rgba || w <= 0 || h <= 0) {
        if (fallback_path) return clipboard_copy_path(fallback_path);
        return false;
    }

    char tmpl[] = "/tmp/civ_clip_XXXXXX.png";
    int fd = mkstemps(tmpl, 4);
    if (fd == -1) {
        if (fallback_path) return clipboard_copy_text(fallback_path);
        return false;
    }

    if (!write_png_to_fd(fd, rgba, w, h)) {
        close(fd);
        unlink(tmpl);
        if (fallback_path) return clipboard_copy_text(fallback_path);
        return false;
    }

    bool copied = copy_fd_via_tool(fd);
    close(fd);
    unlink(tmpl);

    if (!copied && fallback_path) {
        return clipboard_copy_text(fallback_path);
    }
    return copied;
}

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
bool clipboard_copy_path(const char *path) {
    if (!path) return false;
    if (strlen(path) >= PATH_MAX) return false;

    // Check if file exists and is a regular file
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return clipboard_copy_text(path);
    }

    int w = 0, h = 0, comp = 0;
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (data) {
        bool r = clipboard_copy_rgba(data, w, h, path);
        stbi_image_free(data);
        return r;
    }

    return clipboard_copy_text(path);
}

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
char *clipboard_paste_to_temp(void) {
    char tmpl[] = "/tmp/civ_paste_XXXXXX.png";
    int fd = mkstemps(tmpl, 4);
    if (fd == -1) return NULL;

    bool got = false;
    const char *wayland = getenv("WAYLAND_DISPLAY");

    if (wayland && wayland[0] != '\0' && command_exists("wl-paste")) {
        lseek(fd, 0, SEEK_SET);
        if (ftruncate(fd, 0) == 0) {
            char *const argv[] = {"wl-paste", "--type", "image/png", NULL};
            if (run_command_with_stdout_fd(argv, fd)) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0) got = true;
            }
        }
        if (!got) {
            lseek(fd, 0, SEEK_SET);
            if (ftruncate(fd, 0) == 0) {
                char *const argv[] = {"wl-paste", NULL};
                if (run_command_with_stdout_fd(argv, fd)) {
                    struct stat st;
                    if (fstat(fd, &st) == 0 && st.st_size > 0) got = true;
                }
            }
        }
    }

    if (!got && command_exists("xclip")) {
        lseek(fd, 0, SEEK_SET);
        if (ftruncate(fd, 0) == 0) {
            char *const argv[] = {"xclip", "-selection", "clipboard", "-t", "image/png", "-o", NULL};
            if (run_command_with_stdout_fd(argv, fd)) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0) got = true;
            }
        }
    }

    if (!got && command_exists("xclip")) {
        lseek(fd, 0, SEEK_SET);
        if (ftruncate(fd, 0) == 0) {
            char *const argv[] = {"xclip", "-selection", "clipboard", "-o", NULL};
            if (run_command_with_stdout_fd(argv, fd)) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 100) got = true;
            }
        }
    }

    if (!got && command_exists("xsel")) {
        lseek(fd, 0, SEEK_SET);
        if (ftruncate(fd, 0) == 0) {
            char *const argv[] = {"xsel", "--clipboard", "--output", NULL};
            if (run_command_with_stdout_fd(argv, fd)) {
                struct stat st;
                if (fstat(fd, &st) == 0 && st.st_size > 0) got = true;
            }
        }
    }

    close(fd);

    if (!got) {
        unlink(tmpl);
        return NULL;
    }

    // Validate that the pasted file is actually a decodable image
    int w = 0, h = 0, c = 0;
    unsigned char *d = stbi_load(tmpl, &w, &h, &c, 4);
    if (!d) {
        unlink(tmpl);
        return NULL;
    }
    stbi_image_free(d);

    char *ret = strdup(tmpl);
    if (!ret) {
        unlink(tmpl);
        return NULL;
    }
    return ret;
}
