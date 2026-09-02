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
#include "font8x8.h"

#include <string.h>

void text_draw_char(SDL_Renderer *ren, int x, int y, char c, SDL_Color col, int scale) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) return; // Only basic Latin is defined

    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    for (int row = 0; row < 8; row++) {
        unsigned char bits = (unsigned char)font8x8_basic[uc][row];
        for (int col_bit = 0; col_bit < 8; col_bit++) {
            if (bits & (1 << col_bit)) {
                SDL_Rect r = { x + col_bit * scale, y + row * scale, scale, scale };
                SDL_RenderFillRect(ren, &r);
            }
        }
    }
}

int text_draw(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale) {
    int cx = x;
    for (const char *p = text; *p; p++) {
        text_draw_char(ren, cx, y, *p, col, scale);
        cx += 8 * scale;
    }
    return cx;
}

int text_draw_clipped(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale, int max_width) {
    int cx = x;
    int char_w = 8 * scale;
    for (const char *p = text; *p; p++) {
        if (cx + char_w > x + max_width) break;
        text_draw_char(ren, cx, y, *p, col, scale);
        cx += char_w;
    }
    return cx;
}
