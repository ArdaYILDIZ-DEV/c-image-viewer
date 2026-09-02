#pragma once

/**
 * text.h - Bitmap text rendering using embedded 8x8 public domain font.
 *
 * Provides two low-level primitives for drawing ASCII text directly with
 * SDL_Renderer without requiring SDL_ttf. Each glyph is 8x8 pixels and can
 * be scaled by an integer factor for larger sizes. The font data lives in
 * font8x8.h (font8x8_basic[128][8]).
 *
 * All functions assume the renderer is already set up with blend mode and
 * that clipping (if needed) is handled by the caller.
 */

#include <SDL2/SDL.h>

// Draw a single 8x8 glyph at (x,y) with foreground color and integer scale.
// Scale 1 = 8px, 2 = 16px, etc. Uses filled rectangles for pixel doubling.
void text_draw_char(SDL_Renderer *ren, int x, int y, char c, SDL_Color col, int scale);

// Draw a NUL-terminated ASCII string left-to-right.
// Returns the X coordinate after the last character for chaining.
int text_draw(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale);

// Draw string with truncation to fit within max_width (in pixels). Used for
// info bars where text must not overflow the window.
int text_draw_clipped(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale, int max_width);
