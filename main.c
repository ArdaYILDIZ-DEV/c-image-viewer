/**
 * c-image-viewer - Minimalist dual-pane image viewer with synchronized zoom/pan.
 *
 * Architecture:
 *   - Single window split into one or two panes. A single global view transform
 *     (zoom, pan_x, pan_y) is shared across panes to provide synchronized
 *     navigation. This is intentional: comparing two images requires identical
 *     scale and offset.
 *   - Images are decoded on the CPU via stb_image and uploaded once as
 *     SDL_Textures. All subsequent zoom/pan is GPU-accelerated via
 *     SDL_RenderCopyF with bilinear filtering, avoiding per-frame CPU resampling.
 *   - Pan is stored in image-space (pixels at 1:1) and scaled by zoom at
 *     render time. This keeps cursor-centered zoom math linear and independent
 *     of current scale.
 *
 * Dependencies:
 *   - SDL2 for windowing, input, and rendering.
 *   - stb_image.h (header-only) for JPEG/PNG/WebP/BMP/PPM decoding.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -Wpedantic -std=c11 main.c -o viewer -lSDL2 -lm
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/**
 * Represents a single loaded image and its GPU texture.
 *
 * The texture is created once at load time and reused for every frame.
 * Width/height are the original decoded dimensions (before zoom).
 */
typedef struct {
    SDL_Texture *tex;   // GPU texture, owned - must be destroyed with SDL_DestroyTexture
    int w, h;           // Original image dimensions in pixels
    const char *path;   // Source file path (borrowed from argv, not owned)
} Image;

// ---------------------------------------------------------------------------
// Global view state - shared across all panes for synchronized navigation.
// ---------------------------------------------------------------------------
static Image g_img[2] = {0};   // Loaded images, up to 2
static int g_count = 0;        // Number of loaded images (1 or 2)

// View transform: zoom is a uniform scale factor, pan is an offset in
// image-space pixels. Screen position = pane_center + (pan * zoom) + centering.
static float g_zoom = 1.0f;    // Current zoom level (1.0 = 100%), clamped to [0.05, 32.0]
static float g_pan_x = 0.0f;   // Horizontal pan in image pixels
static float g_pan_y = 0.0f;   // Vertical pan in image pixels

static int g_win_w = 1280;     // Current window width (updated on resize)
static int g_win_h = 720;      // Current window height (updated on resize)

// ---------------------------------------------------------------------------
// Image loading
// ---------------------------------------------------------------------------

/**
 * Decode an image file and upload it as an SDL texture.
 *
 * Uses stb_image to decode to 32-bit RGBA regardless of source format,
 * then creates an SDL_Surface wrapping that buffer and converts it to a
 * texture. Linear filtering is enabled for smooth zoomed rendering.
 *
 * @param ren  Renderer used to create the texture.
 * @param path Filesystem path to the image.
 * @param out  Output Image struct to populate on success.
 * @return true on success, false on failure (error already printed).
 */
static bool load_image(SDL_Renderer *ren, const char *path, Image *out) {
    int w, h, comp;

    // Force 4 channels (RGBA) to simplify surface creation. stb_image handles
    // format conversion internally (e.g., JPEG 3ch -> RGBA).
    unsigned char *data = stbi_load(path, &w, &h, &comp, 4);
    if (!data) {
        fprintf(stderr, "stbi_load failed '%s': %s\n", path, stbi_failure_reason());
        return false;
    }

    // Wrap the decoded buffer in an SDL_Surface without copying. The surface
    // does not own the pixel data - we free it explicitly after texture creation.
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        data, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        fprintf(stderr, "SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        stbi_image_free(data);
        return false;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    stbi_image_free(data);

    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    // Enable bilinear filtering so zoomed images remain smooth rather than
    // pixelated. This is a GPU sampler setting, no CPU cost.
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    out->tex = tex;
    out->w = w;
    out->h = h;
    out->path = path;
    return true;
}

// ---------------------------------------------------------------------------
// View control
// ---------------------------------------------------------------------------

/**
 * Reset zoom and pan to fit all loaded images inside their panes.
 *
 * For a single image, computes the largest uniform scale that fits the
 * image inside the full window. For dual panes, computes per-pane fit
 * scales and takes the minimum so both images are fully visible with the
 * same synchronized zoom. Small images (smaller than pane) are shown at
 * 1:1 rather than upscaled to preserve pixel fidelity.
 */
static void fit_view(void) {
    if (g_count == 0) return;

    if (g_count == 1) {
        // Fit single image to full window.
        float zx = (float)g_win_w / (float)g_img[0].w;
        float zy = (float)g_win_h / (float)g_img[0].h;
        g_zoom = zx < zy ? zx : zy;
        // Avoid upscaling small images; show them at native resolution centered.
        if (g_zoom > 1.0f) g_zoom = 1.0f;
    } else {
        // Fit both images to half-window panes with a shared zoom.
        float pane_w = (float)g_win_w / 2.0f;
        float z0x = pane_w / (float)g_img[0].w;
        float z0y = (float)g_win_h / (float)g_img[0].h;
        float z1x = pane_w / (float)g_img[1].w;
        float z1y = (float)g_win_h / (float)g_img[1].h;
        float z0 = z0x < z0y ? z0x : z0y;
        float z1 = z1x < z1y ? z1x : z1y;
        // Use the smaller fit so neither image is clipped.
        g_zoom = z0 < z1 ? z0 : z1;
        if (g_zoom > 1.0f) g_zoom = 1.0f;
    }
    g_pan_x = 0;
    g_pan_y = 0;
}

/**
 * Apply a zoom factor centered on a specific screen coordinate.
 *
 * Keeps the image point under the cursor stationary by compensating pan.
 * Derivation: pan is in image-space, so the world point under the cursor is
 *   world = (cursor - window_center) / zoom - pan
 * Setting world_before == world_after and solving for pan_new yields the
 * adjustment below. This provides intuitive cursor-anchored zoom behavior
 * similar to maps and professional image comparators.
 *
 * @param factor Multiplicative zoom factor (>1 zooms in, <1 zooms out).
 * @param mx     Cursor X in window coordinates.
 * @param my     Cursor Y in window coordinates.
 */
static void do_zoom(float factor, int mx, int my) {
    float old = g_zoom;
    float next = old * factor;

    // Clamp to usable range: 5% allows overview of large images,
    // 3200% allows pixel-level inspection.
    if (next < 0.05f) next = 0.05f;
    if (next > 32.0f) next = 32.0f;
    if (next == old) return;

    // Compensate pan so the point under the cursor does not drift.
    float cx = (float)g_win_w * 0.5f;
    float cy = (float)g_win_h * 0.5f;
    g_pan_x += (float)(mx - (int)cx) * (1.0f / next - 1.0f / old);
    g_pan_y += (float)(my - (int)cy) * (1.0f / next - 1.0f / old);
    g_zoom = next;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

/**
 * Render all panes with the current view transform.
 *
 * Each pane is clipped via SDL_RenderSetClipRect so images do not bleed
 * across the divider. Destination rectangles are computed from the shared
 * zoom/pan, centered within each pane, then drawn with SDL_RenderCopyF.
 * A 1px vertical divider is drawn in dual-pane mode for visual separation.
 *
 * @param ren Active SDL renderer (target is the window framebuffer).
 */
static void render(SDL_Renderer *ren) {
    // Dark neutral background minimizes eye strain and provides contrast
    // for both light and dark images.
    SDL_SetRenderDrawColor(ren, 18, 18, 18, 255);
    SDL_RenderClear(ren);

    if (g_count == 1) {
        // Single-pane: image centered in full window.
        Image *im = &g_img[0];
        SDL_Rect clip = {0, 0, g_win_w, g_win_h};
        SDL_RenderSetClipRect(ren, &clip);

        float dw = (float)im->w * g_zoom;
        float dh = (float)im->h * g_zoom;
        // Center the zoomed image, then apply pan scaled by zoom.
        float dx = (float)g_win_w * 0.5f - dw * 0.5f + g_pan_x * g_zoom;
        float dy = (float)g_win_h * 0.5f - dh * 0.5f + g_pan_y * g_zoom;

        SDL_FRect dst = {dx, dy, dw, dh};
        SDL_RenderCopyF(ren, im->tex, NULL, &dst);
    } else if (g_count == 2) {
        // Dual-pane: each image centered in its half, sharing zoom/pan.
        for (int i = 0; i < 2; i++) {
            Image *im = &g_img[i];
            int pane_w = g_win_w / 2;
            SDL_Rect clip = {i * pane_w, 0, pane_w, g_win_h};
            // Handle odd window widths by giving the remainder to the right pane.
            if (i == 1) {
                clip.x = pane_w;
                clip.w = g_win_w - pane_w;
            }
            SDL_RenderSetClipRect(ren, &clip);

            float dw = (float)im->w * g_zoom;
            float dh = (float)im->h * g_zoom;
            float pane_cx = (float)clip.x + (float)clip.w * 0.5f;
            float pane_cy = (float)g_win_h * 0.5f;
            float dx = pane_cx - dw * 0.5f + g_pan_x * g_zoom;
            float dy = pane_cy - dh * 0.5f + g_pan_y * g_zoom;

            SDL_FRect dst = {dx, dy, dw, dh};
            SDL_RenderCopyF(ren, im->tex, NULL, &dst);
        }
        // Visual separator between panes.
        SDL_RenderSetClipRect(ren, NULL);
        SDL_SetRenderDrawColor(ren, 60, 60, 60, 255);
        int mid = g_win_w / 2;
        SDL_RenderDrawLine(ren, mid, 0, mid, g_win_h);
    }

    SDL_RenderSetClipRect(ren, NULL);
    SDL_RenderPresent(ren);
}

// ---------------------------------------------------------------------------
// CLI helpers
// ---------------------------------------------------------------------------

/**
 * Print usage information to stderr.
 *
 * @param prog Program name (argv[0]) for the usage line.
 */
static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <image1> [image2]\n"
        "  Single image -> full window\n"
        "  Two images   -> side-by-side, synchronized zoom/pan\n"
        "\n"
        "Controls:\n"
        "  Mouse wheel     Synchronized zoom (cursor-centered)\n"
        "  Left drag       Synchronized pan\n"
        "  0 / F           Fit to window\n"
        "  1               100%% (1:1)\n"
        "  + / - / =       Zoom in/out\n"
        "  Q / ESC         Quit\n",
        prog);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    // Validate argument count: 1 or 2 images only. No flag parsing to keep
    // the CLI minimal and predictable.
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return argc == 1 ? 0 : 1;
    }

    g_count = argc - 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "c-image-viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Prefer accelerated renderer with vsync for smooth panning. Fall back
    // to software renderer on systems without GPU acceleration (e.g., headless).
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    // Load all requested images upfront. Failure to load any image is fatal
    // to avoid showing a partially valid comparison.
    for (int i = 0; i < g_count; i++) {
        if (!load_image(ren, argv[i + 1], &g_img[i])) {
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }

    // Initialize view to fit after window size is finalized (handles HiDPI
    // where drawable size may differ from requested size).
    SDL_GetWindowSize(win, &g_win_w, &g_win_h);
    fit_view();

    // Input state for drag-to-pan.
    bool dragging = false;
    int last_x = 0, last_y = 0;
    bool running = true;

    // Main event/render loop. Polls events, updates view state, and renders
    // at display refresh rate (vsync) or ~60fps fallback.
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                // Track window size for fit calculations and rendering.
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    g_win_w = ev.window.data1;
                    g_win_h = ev.window.data2;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dragging = true;
                    last_x = ev.button.x;
                    last_y = ev.button.y;
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) dragging = false;
                break;
            case SDL_MOUSEMOTION:
                if (dragging) {
                    // Pan is in image-space, so divide screen delta by zoom
                    // to keep drag speed consistent across zoom levels.
                    int dx = ev.motion.x - last_x;
                    int dy = ev.motion.y - last_y;
                    g_pan_x += (float)dx / g_zoom;
                    g_pan_y += (float)dy / g_zoom;
                    last_x = ev.motion.x;
                    last_y = ev.motion.y;
                }
                break;
            case SDL_MOUSEWHEEL: {
                // Zoom centered on current mouse position for intuitive navigation.
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                float factor = ev.wheel.y > 0 ? 1.1f : 0.9f;
                // Support high-resolution scroll wheels that report larger deltas.
                if (ev.wheel.y > 1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (ev.wheel.y < -1) factor = 1.0f + 0.1f * (float)ev.wheel.y;
                if (factor < 0.2f) factor = 0.2f;
                do_zoom(factor, mx, my);
                break;
            }
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_0:
                case SDLK_f:
                    fit_view();
                    break;
                case SDLK_1:
                    // Reset to native 1:1 scale at center.
                    g_zoom = 1.0f;
                    g_pan_x = 0;
                    g_pan_y = 0;
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    do_zoom(1.1f, g_win_w/2, g_win_h/2);
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    do_zoom(0.9f, g_win_w/2, g_win_h/2);
                    break;
                default: break;
                }
                break;
            default: break;
            }
        }
        render(ren);
        // Cap to ~60fps when vsync is unavailable to avoid busy-looping.
        SDL_Delay(16);
    }

    // Release GPU resources before shutting down SDL.
    for (int i = 0; i < g_count; i++) {
        if (g_img[i].tex) SDL_DestroyTexture(g_img[i].tex);
    }
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
