/**
 * main.c - Entry point and event loop for c-image-viewer.
 *
 * Initializes SDL, loads initial image(s), and runs the main event loop,
 * delegating image and view transforms to viewer.* and directory tree to browser.*.
 *
 * Event dispatch priority:
 *   1. Browser overlay (when open) gets first chance at keyboard and mouse.
 *   2. Viewer controls (zoom, pan, fullscreen, navigation) handle general events.
 *   3. Global actions (quit, help, clipboard) handle system requests.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "viewer.h"
#include "browser.h"
#include "clipboard.h"
#include "icon_data.h"
#include "stb_image.h"

#include <SDL2/SDL.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/**
 * Print command-line usage information to stderr.
 *
 * @param prog Program invocation name (argv[0]).
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <image1> [image2]\n"
        "       %s --dump-icon <output.png>\n"
        "  Single image -> full window\n"
        "  Two images   -> side-by-side, synchronized zoom/pan\n"
        "  --dump-icon  -> write embedded application icon to file and exit\n"
        "\n"
        "Controls:\n"
        "  Mouse wheel        Zoom (cursor-centered)\n"
        "  Left drag          Pan\n"
        "  0 / F              Fit to window\n"
        "  1                  100%% (1:1)\n"
        "  + / -              Zoom in/out\n"
        "  f / F11            Toggle fullscreen\n"
        "  i                  Toggle info bar\n"
        "  e                  Toggle metadata (EXIF)\n"
        "  h / ?              Toggle help\n"
        "  s                  Toggle sync\n"
        "  Tab                Switch active pane (when unsynced)\n"
        "  n / Right / PgDn   Next image in folder\n"
        "  p / Left / PgUp    Previous image\n"
        "  Ctrl+C             Copy active image to clipboard\n"
        "  Ctrl+V             Paste image from clipboard\n"
        "  Ctrl+F             Browser filter\n"
        "  ESC                Browser / Exit fullscreen / Close help / Close metadata\n"
        "  q                  Quit\n"
        "  Drag & drop        Drop file onto pane to replace it\n",
        prog, prog);
}

/**
 * Handle a dropped file by replacing the pane under the cursor and updating browser root.
 *
 * @param dropped File path provided by SDL_DROPFILE event (freed with SDL_free).
 */
static void handle_drop_file(char *dropped) {
    if (!dropped) return;
    char clean_path[PATH_MAX];
    if (viewer_validate_image_path(dropped, clean_path, sizeof(clean_path))) {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        int target = (g_count == 2 && mx >= g_win_w / 2) ? 1 : 0;
        if (viewer_replace_image(target, clean_path)) {
            viewer_fit_view();
            viewer_update_title();
            browser_set_root(g_current_dir);
        }
    } else {
        fprintf(stderr, "Rejected invalid dropped file: %s\n", dropped);
    }
    SDL_free(dropped);
}

/**
 * Copy active pane image to the system clipboard via external tool.
 */
static void handle_clipboard_copy(void) {
    int pane = (g_count == 1) ? 0 : g_active;
    if (pane < g_count && g_img[pane].path) {
        bool ok = clipboard_copy_path(g_img[pane].path);
        fprintf(stderr, "Clipboard copy %s: %s\n",
            g_img[pane].path, ok ? "ok" : "failed (install xclip/wl-copy)");
    }
}

/**
 * Paste image from system clipboard into a temporary file and replace active pane.
 */
static void handle_clipboard_paste(void) {
    char *tmp = clipboard_paste_to_temp();
    if (tmp) {
        int pane = (g_count == 1) ? 0 : g_active;
        if (viewer_replace_image(pane, tmp)) {
            viewer_fit_view();
            viewer_update_title();
            browser_set_root(g_current_dir);
        }
        unlink(tmp);
        free(tmp);
    } else {
        fprintf(stderr, "Clipboard paste: no image in clipboard\n");
    }
}

/**
 * Handle keydown events for viewer shortcuts, view transformations, and navigation.
 *
 * @param key Pressed SDL keycode.
 * @param mod Active SDL key modifiers.
 * @param running Pointer to application event loop running flag.
 */
static void handle_keydown(SDL_Keycode key, SDL_Keymod mod, bool *running) {
    if (mod & KMOD_CTRL) {
        switch (key) {
        case SDLK_c: handle_clipboard_copy(); return;
        case SDLK_v: handle_clipboard_paste(); return;
        case SDLK_f:
            if (!browser_is_open()) browser_toggle();
            browser_handle_key(SDLK_f, KMOD_CTRL);
            return;
        default: break;
        }
    }

    switch (key) {
    case SDLK_q:
        if (!(mod & KMOD_CTRL)) *running = false;
        break;
    case SDLK_ESCAPE:
        if (g_show_help) {
            g_show_help = false;
        } else if (g_show_metadata) {
            g_show_metadata = false;
        } else if (g_fullscreen) {
            viewer_toggle_fullscreen();
            viewer_update_title();
        } else {
            browser_toggle();
        }
        break;
    case SDLK_e:
        if (!(mod & KMOD_CTRL)) viewer_toggle_metadata();
        break;
    case SDLK_0:
        viewer_fit_view();
        viewer_update_title();
        break;
    case SDLK_f:
        if (mod & KMOD_CTRL) viewer_fit_view();
        else viewer_toggle_fullscreen();
        viewer_update_title();
        break;
    case SDLK_F11:
        viewer_toggle_fullscreen();
        viewer_update_title();
        break;
    case SDLK_1:
    case SDLK_KP_1:
        if (g_sync) {
            g_zoom = 1.0f; g_pan_x = 0.0f; g_pan_y = 0.0f;
            for (int i = 0; i < 2; i++) {
                g_free_zoom[i] = 1.0f; g_free_pan_x[i] = 0.0f; g_free_pan_y[i] = 0.0f;
            }
        } else {
            int p = (g_active < g_count) ? g_active : 0;
            g_free_zoom[p] = 1.0f; g_free_pan_x[p] = 0.0f; g_free_pan_y[p] = 0.0f;
        }
        viewer_update_title();
        break;
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
        viewer_do_zoom(1.1f, g_win_w / 2, g_win_h / 2);
        viewer_update_title();
        break;
    case SDLK_MINUS:
    case SDLK_KP_MINUS:
        viewer_do_zoom(0.9f, g_win_w / 2, g_win_h / 2);
        viewer_update_title();
        break;
    case SDLK_i:
        g_show_info = !g_show_info;
        break;
    case SDLK_h:
    case SDLK_SLASH:
        g_show_help = !g_show_help;
        break;
    case SDLK_s:
        viewer_toggle_sync();
        viewer_update_title();
        break;
    case SDLK_TAB:
        if (!g_sync && g_count == 2) {
            viewer_toggle_active_pane();
            viewer_update_title();
        }
        break;
    case SDLK_n:
    case SDLK_RIGHT:
    case SDLK_PAGEDOWN:
        if (!browser_is_open() && viewer_navigate(+1)) {
            viewer_fit_view();
            viewer_update_title();
        } else if (browser_is_open()) {
            browser_handle_key(key, mod);
        }
        break;
    case SDLK_p:
    case SDLK_LEFT:
    case SDLK_PAGEUP:
        if (!browser_is_open() && viewer_navigate(-1)) {
            viewer_fit_view();
            viewer_update_title();
        } else if (browser_is_open()) {
            browser_handle_key(key, mod);
        }
        break;
    default:
        if (browser_is_open()) browser_handle_key(key, mod);
        break;
    }
}

/**
 * Load embedded application icon and assign it to the window.
 *
 * Decodes the in-memory 48x48 RGBA PNG icon using stb_image, constructs an
 * SDL surface wrapping the decoded pixel buffer, assigns it to the window via
 * SDL_SetWindowIcon, and frees both the surface and stb_image pixel buffer.
 *
 * @param win Target SDL window receiving the application icon.
 */
static void app_set_window_icon(SDL_Window *win) {
    if (!win) return;

    int w = 0, h = 0, channels = 0;
    unsigned char *pixels = stbi_load_from_memory(
        g_app_icon_png, (int)g_app_icon_png_len, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "Warning: Failed to decode embedded window icon: %s\n",
            stbi_failure_reason());
        return;
    }

    SDL_Surface *icon_surf = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon_surf) {
        SDL_SetWindowIcon(win, icon_surf);
        SDL_FreeSurface(icon_surf);
    } else {
        fprintf(stderr, "Warning: Failed to create surface for window icon: %s\n",
            SDL_GetError());
    }

    stbi_image_free(pixels);
}

int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[1], "--dump-icon") == 0) {
        FILE *fp = fopen(argv[2], "wb");
        if (!fp) {
            perror("fopen");
            return 1;
        }
        if (fwrite(g_app_icon_png, 1, g_app_icon_png_len, fp) != g_app_icon_png_len) {
            fprintf(stderr, "Error: Failed to write icon data to %s\n", argv[2]);
            fclose(fp);
            return 1;
        }
        if (fclose(fp) != 0) {
            perror("fclose");
            return 1;
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--dump-icon") == 0) {
        fprintf(stderr, "Error: --dump-icon requires an output path\n");
        print_usage(argv[0]);
        return 1;
    }

    // Validate argument count: 1 or 2 images. No flag parsing to keep CLI minimal.
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return argc == 1 ? 0 : 1;
    }

    g_count = argc - 1;

    SDL_SetHint(SDL_HINT_APP_NAME, "c-image-viewer");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "c-image-viewer");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "c-image-viewer");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Create resizable window with HiDPI support.
    g_win = SDL_CreateWindow(
        "c-image-viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    app_set_window_icon(g_win);

    // Prefer accelerated renderer with vsync; fall back to software.
    g_ren = SDL_CreateRenderer(g_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    // Load initial images; failure is fatal to avoid partial comparison.
    for (int i = 0; i < g_count; i++) {
        char clean_path[PATH_MAX];
        if (!viewer_validate_image_path(argv[i + 1], clean_path, sizeof(clean_path)) ||
            !viewer_load_image(clean_path, &g_img[i])) {
            fprintf(stderr, "Error: Failed to load image: %s\n", argv[i + 1]);
            for (int j = 0; j < i; j++) viewer_unload_image(&g_img[j]);
            SDL_DestroyRenderer(g_ren);
            SDL_DestroyWindow(g_win);
            SDL_Quit();
            return 1;
        }
    }

    // Finalize window size (HiDPI may differ from requested) and fit view.
    SDL_GetWindowSize(g_win, &g_win_w, &g_win_h);
    viewer_fit_view();
    viewer_scan_current_dir(g_img[0].path);
    browser_init();
    viewer_update_title();

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    bool dragging = false;
    int last_x = 0, last_y = 0;
    bool running = true;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // Let browser handle events first when open.
            if (browser_is_open()) {
                if (ev.type == SDL_KEYDOWN) {
                    if (browser_handle_key(ev.key.keysym.sym, ev.key.keysym.mod)) continue;
                    // ESC and q are handled by browser, but q should still quit even with browser open.
                    if (ev.key.keysym.sym == SDLK_q) { running = false; continue; }
                }
                if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEWHEEL || ev.type == SDL_MOUSEMOTION) {
                    if (browser_handle_event(&ev)) continue;
                }
            }

            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    if (ev.window.data1 > 0 && ev.window.data2 > 0) {
                        g_win_w = ev.window.data1;
                        g_win_h = ev.window.data2;
                    }
                }
                break;
            case SDL_DROPFILE:
                handle_drop_file(ev.drop.file);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    // In free mode, clicking a pane makes it active.
                    if (!g_sync && g_count == 2) {
                        g_active = (ev.button.x < g_win_w / 2) ? 0 : 1;
                        viewer_update_title();
                    }
                    dragging = true;
                    last_x = ev.button.x;
                    last_y = ev.button.y;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;
            case SDL_MOUSEMOTION:
                if (dragging && !browser_is_open()) {
                    int dx = ev.motion.x - last_x;
                    int dy = ev.motion.y - last_y;
                    viewer_do_pan(dx, dy);
                    viewer_update_title();
                    last_x = ev.motion.x;
                    last_y = ev.motion.y;
                }
                break;
            case SDL_MOUSEWHEEL: {
                if (browser_is_open()) {
                    browser_handle_event(&ev);
                    break;
                }
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                float factor = ev.wheel.y > 0 ? 1.1f : 0.9f;
                if (ev.wheel.y > 1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (ev.wheel.y < -1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (factor < 0.2f) factor = 0.2f;
                viewer_do_zoom(factor, mx, my);
                viewer_update_title();
                break;
            }
            case SDL_KEYDOWN:
                handle_keydown(ev.key.keysym.sym, ev.key.keysym.mod, &running);
                break;
            default: break;
            }
        }

        // Render viewer first, then browser on top if open.
        viewer_render(g_ren);
        if (browser_is_open()) browser_render(g_ren);
        SDL_RenderPresent(g_ren);
        SDL_Delay(16);
    }

    browser_cleanup();
    viewer_free_file_list();
    for (int i = 0; i < g_count; i++) viewer_unload_image(&g_img[i]);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
