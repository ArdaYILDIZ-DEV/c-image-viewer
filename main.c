/**
 * main.c - Entry point and event loop for c-image-viewer.
 *
 * This file is intentionally thin: it initializes SDL, loads the initial
 * image(s), and runs the main event loop, delegating image, view, and
 * browser logic to viewer.* and browser.* modules. Keeping main small
 * improves readability and makes the event flow easy to audit.
 *
 * Event dispatch priority:
 *   1. Browser overlay (when open) gets first chance at keyboard/mouse.
 *   2. Viewer controls (zoom, pan, fullscreen, navigation) handle the rest.
 *   3. Global actions (quit, help) are handled last.
 */

#include "viewer.h"
#include "browser.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/**
 * Print minimal usage to stderr.
 *
 * @param prog Program name (argv[0]).
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <image1> [image2]\n"
        "  Single image -> full window\n"
        "  Two images   -> side-by-side, synchronized zoom/pan\n"
        "\n"
        "Controls:\n"
        "  Mouse wheel        Zoom (cursor-centered)\n"
        "  Left drag          Pan\n"
        "  0 / F              Fit to window\n"
        "  1                  100%% (1:1)\n"
        "  + / -              Zoom in/out\n"
        "  f / F11            Toggle fullscreen\n"
        "  i                  Toggle info bar\n"
        "  h / ?              Toggle help\n"
        "  s                  Toggle sync\n"
        "  Tab                Switch active pane (when unsynced)\n"
        "  n / Right / PgDn   Next image in folder\n"
        "  p / Left / PgUp    Previous image\n"
        "  ESC                Browser / Exit fullscreen / Close help\n"
        "  q                  Quit\n"
        "  Drag & drop        Drop file onto pane to replace it\n",
        prog);
}

int main(int argc, char *argv[]) {
    // Validate argument count: 1 or 2 images. No flag parsing to keep CLI minimal.
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return argc == 1 ? 0 : 1;
    }

    g_count = argc - 1;

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

    // Prefer accelerated renderer with vsync; fall back to software.
    g_ren = SDL_CreateRenderer(g_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) {
        g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    // Load initial images; failure is fatal to avoid partial comparison.
    for (int i = 0; i < g_count; i++) {
        if (!viewer_load_image(argv[i + 1], &g_img[i])) {
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
                // Also handle drop while browser is open (load file and close browser via browser logic)
                if (ev.type == SDL_DROPFILE) {
                    char *dropped = ev.drop.file;
                    int mx = 0, my = 0;
                    SDL_GetMouseState(&mx, &my);
                    int target = (g_count == 2 && mx >= g_win_w / 2) ? 1 : 0;
                    if (viewer_is_image_file(dropped)) {
                        if (viewer_replace_image(target, dropped)) {
                            viewer_fit_view();
                            viewer_update_title();
                        }
                    }
                    SDL_free(dropped);
                    continue;
                }
            }

            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    g_win_w = ev.window.data1;
                    g_win_h = ev.window.data2;
                }
                break;
            case SDL_DROPFILE: {
                char *dropped = ev.drop.file;
                int mx = 0, my = 0;
                SDL_GetMouseState(&mx, &my);
                int target = (g_count == 2 && mx >= g_win_w / 2) ? 1 : 0;
                if (viewer_is_image_file(dropped)) {
                    if (viewer_replace_image(target, dropped)) {
                        viewer_fit_view();
                        viewer_update_title();
                        // Sync browser root to new file's directory
                        browser_set_root(g_current_dir);
                    }
                } else {
                    fprintf(stderr, "Unsupported file type (drop): %s\n", dropped);
                }
                SDL_free(dropped);
                break;
            }
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
                // Browser gets ESC first when open (handled above)
                switch (ev.key.keysym.sym) {
                case SDLK_q:
                    running = false;
                    break;
                case SDLK_ESCAPE:
                    if (g_show_help) {
                        g_show_help = false;
                    } else if (g_fullscreen) {
                        viewer_toggle_fullscreen();
                        viewer_update_title();
                    } else {
                        // Toggle file browser overlay
                        browser_toggle();
                    }
                    break;
                case SDLK_0:
                    viewer_fit_view();
                    viewer_update_title();
                    break;
                case SDLK_f:
                    if (ev.key.keysym.mod & KMOD_CTRL) {
                        viewer_fit_view();
                    } else {
                        viewer_toggle_fullscreen();
                    }
                    viewer_update_title();
                    break;
                case SDLK_F11:
                    viewer_toggle_fullscreen();
                    viewer_update_title();
                    break;
                case SDLK_1:
                case SDLK_KP_1:
                    if (g_sync) {
                        g_zoom = 1.0f; g_pan_x = 0; g_pan_y = 0;
                        for (int i = 0; i < 2; i++) { g_free_zoom[i] = 1.0f; g_free_pan_x[i] = 0; g_free_pan_y[i] = 0; }
                    } else {
                        int p = (g_active < g_count) ? g_active : 0;
                        g_free_zoom[p] = 1.0f; g_free_pan_x[p] = 0; g_free_pan_y[p] = 0;
                    }
                    viewer_update_title();
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    viewer_do_zoom(1.1f, g_win_w/2, g_win_h/2);
                    viewer_update_title();
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    viewer_do_zoom(0.9f, g_win_w/2, g_win_h/2);
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
                        g_active = 1 - g_active;
                        viewer_update_title();
                    }
                    break;
                case SDLK_n:
                case SDLK_RIGHT:
                case SDLK_PAGEDOWN:
                    if (!browser_is_open() && viewer_navigate(+1)) { viewer_fit_view(); viewer_update_title(); }
                    else if (browser_is_open()) browser_handle_key(ev.key.keysym.sym, ev.key.keysym.mod);
                    break;
                case SDLK_p:
                case SDLK_LEFT:
                case SDLK_PAGEUP:
                    if (!browser_is_open() && viewer_navigate(-1)) { viewer_fit_view(); viewer_update_title(); }
                    else if (browser_is_open()) browser_handle_key(ev.key.keysym.sym, ev.key.keysym.mod);
                    break;
                default:
                    // Forward other keys to browser if open (e.g., Up/Down)
                    if (browser_is_open()) browser_handle_key(ev.key.keysym.sym, ev.key.keysym.mod);
                    break;
                }
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
