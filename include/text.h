#ifndef TEXT_H
#define TEXT_H

/**
 * text.h - Bitmap text rendering using embedded 8x8 public domain font.
 *
 * Provides low-level primitives for drawing ASCII text directly with
 * SDL_Renderer without requiring SDL_ttf. Each glyph is 8x8 pixels and can
 * be scaled by an integer factor for larger sizes. Font data resides in
 * external/font8x8.h (font8x8_basic[128][8]).
 *
 * Rendering merges contiguous pixels into horizontal spans, minimizing
 * SDL render draw calls.
 */

#include <SDL2/SDL.h>

/**
 * Draw a single 8x8 glyph at (x, y) with foreground color and integer scale.
 *
 * Extracts merged horizontal spans for character c and issues a batched
 * SDL_RenderFillRects call. If character code is outside ASCII (0..127) or
 * scale <= 0, the call is a no-op.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in window pixels.
 * @param y Origin Y coordinate in window pixels.
 * @param c ASCII character code (0..127).
 * @param col Foreground draw color.
 * @param scale Integer scaling factor (1 = 8x8px, 2 = 16x16px, etc.).
 */
void text_draw_char(SDL_Renderer *ren, int x, int y, char c, SDL_Color col, int scale);

/**
 * Extract horizontal merged spans for a single glyph into rectangle buffer.
 *
 * Scans each of the 8 rows in font8x8_basic[uc] and merges consecutive active
 * bits into horizontal spans of width span * scale. Produces at most 32 rects.
 *
 * @param c Character code (0..127).
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param scale Integer scaling factor (>= 1).
 * @param out_rects Destination buffer with capacity for at least 32 SDL_Rect elements.
 * @return Number of rectangles written to out_rects (0 if invalid or empty glyph).
 */
int text_glyph_spans(char c, int x, int y, int scale, SDL_Rect *out_rects);

/**
 * Draw a NUL-terminated ASCII string left-to-right.
 *
 * Batches glyph spans across characters into an internal chunk buffer (up to 256 rects)
 * and sets renderer draw color once per string, significantly reducing GPU draw calls.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text NUL-terminated ASCII string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @return X coordinate after the last character, suitable for cursor chaining.
 */
int text_draw(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale);

/**
 * Draw a NUL-terminated ASCII string with clipping to a maximum pixel width.
 *
 * Emits characters until the next glyph would exceed max_width pixels from x.
 * Used for info bars, status lines, and overlays where text must not overflow.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text NUL-terminated ASCII string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @param max_width Maximum allowable pixel width (<= 0 draws nothing).
 * @return X coordinate after the last drawn character.
 */
int text_draw_clipped(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale, int max_width);

#endif /* TEXT_H */
