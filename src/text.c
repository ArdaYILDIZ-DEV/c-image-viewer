/**
 * text.c - Implementation of bitmap text rendering.
 *
 * Each glyph in font8x8_basic is 8 rows of 8 bits. Bit 0 is the leftmost
 * pixel in the original VGA font layout (mirrored compared to typical
 * bit order, but the array already accounts for this - we test bit per column).
 *
 * Rendering is intentionally simple: for each set bit, fill a scale x scale
 * rectangle. This keeps the code dependency-free and ensures crisp pixels at
 * any integer scale without texture atlas management.
 */

#include "text.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#endif
#include "font8x8.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <string.h>

/**
 * Extract horizontal merged spans for a single glyph (up to 32 rects).
 *
 * Scans each of the 8 rows in font8x8_basic[uc]. Merges consecutive active bits
 * into horizontal rectangles of width span*scale, reducing draw calls by 75-90%
 * compared to per-pixel rendering.
 *
 * @param c Character code (0..127).
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param scale Integer scaling factor (>= 1).
 * @param out_rects Buffer with at least 32 SDL_Rect elements.
 * @return Number of rects written to out_rects (0 if invalid or empty glyph).
 */
int text_glyph_spans(char c, int x, int y, int scale, SDL_Rect *out_rects) {
    if (!out_rects || scale <= 0) return 0;
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) return 0;

    int count = 0;
    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)font8x8_basic[uc][row];
        int col_bit = 0;
        while (col_bit < 8) {
            if (bits & (1 << col_bit)) {
                int start = col_bit;
                while (col_bit < 8 && (bits & (1 << col_bit))) {
                    col_bit++;
                }
                int span = col_bit - start;
                out_rects[count++] = (SDL_Rect){
                    x + start * scale,
                    y + row * scale,
                    span * scale,
                    scale
                };
            } else {
                col_bit++;
            }
        }
    }
    return count;
}

/**
 * Draw a single 8x8 glyph at (x,y) with foreground color and integer scale.
 *
 * Extracts merged horizontal spans and issues a single batched SDL_RenderFillRects
 * call instead of per-bit SDL_RenderFillRect calls.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param c ASCII character (0..127).
 * @param col Foreground color.
 * @param scale Integer scaling factor (1 = 8x8px, 2 = 16x16px, etc.).
 */
void text_draw_char(SDL_Renderer *ren, int x, int y, char c, SDL_Color col, int scale) {
    if (!ren || scale <= 0) return;
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) return; // Only basic Latin is defined

    SDL_Rect rects[32];
    int count = text_glyph_spans(c, x, y, scale, rects);
    if (count > 0) {
        SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
        SDL_RenderFillRects(ren, rects, count);
    }
}

/**
 * Draw a NUL-terminated ASCII string left-to-right.
 *
 * Batches glyph spans across characters into a chunk buffer (up to 256 rects)
 * and sets draw color once per string, reducing SDL render calls from thousands
 * down to 1-3 per line.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text ASCII string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @return X coordinate after the last character.
 */
int text_draw(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale) {
    if (!ren || !text || scale <= 0) return x;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    int cx = x;
    int char_w = 8 * scale;
    SDL_Rect batch[256];
    int batch_count = 0;

    for (const char *p = text; *p; p++) {
        if ((unsigned char)*p < 128) {
            if (batch_count + 32 > 256) {
                SDL_RenderFillRects(ren, batch, batch_count);
                batch_count = 0;
            }
            batch_count += text_glyph_spans(*p, cx, y, scale, batch + batch_count);
        }
        cx += char_w;
    }
    if (batch_count > 0) {
        SDL_RenderFillRects(ren, batch, batch_count);
    }
    return cx;
}

/**
 * Draw string with truncation to fit within max_width (in pixels).
 *
 * Stops emitting characters when next glyph would overflow max_width. Batches
 * glyph spans into a chunk buffer and sets draw color once.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text ASCII string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @param max_width Maximum allowable width in pixels.
 * @return X coordinate after the last drawn character.
 */
int text_draw_clipped(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale, int max_width) {
    if (!ren || !text || scale <= 0 || max_width <= 0) return x;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    int cx = x;
    int char_w = 8 * scale;
    SDL_Rect batch[256];
    int batch_count = 0;

    for (const char *p = text; *p; p++) {
        if (cx + char_w > x + max_width) break;
        if ((unsigned char)*p < 128) {
            if (batch_count + 32 > 256) {
                SDL_RenderFillRects(ren, batch, batch_count);
                batch_count = 0;
            }
            batch_count += text_glyph_spans(*p, cx, y, scale, batch + batch_count);
        }
        cx += char_w;
    }
    if (batch_count > 0) {
        SDL_RenderFillRects(ren, batch, batch_count);
    }
    return cx;
}
