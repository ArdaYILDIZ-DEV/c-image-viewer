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

#include <limits.h>
#include <stdint.h>
#include <string.h>

/**
 * Map a Unicode code point to a fallback ASCII character.
 *
 * Maps Turkish and Latin accented characters to their base ASCII equivalents
 * so directory paths and filenames render legibly using the 8x8 font.
 * Unmapped non-ASCII code points return '?'.
 *
 * @param cp Decoded Unicode code point.
 * @return ASCII character representation or '?'.
 */
static char map_unicode_to_ascii(uint32_t cp) {
    if (cp < 128) return (char)cp;

    switch (cp) {
        // Turkish characters
        case 0x00E7: return 'c'; // ç
        case 0x00C7: return 'C'; // Ç
        case 0x011F: return 'g'; // ğ
        case 0x011E: return 'G'; // Ğ
        case 0x0131: return 'i'; // ı (dotless i)
        case 0x0130: return 'I'; // İ (capital I with dot)
        case 0x00F6: return 'o'; // ö
        case 0x00D6: return 'O'; // Ö
        case 0x015F: return 's'; // ş
        case 0x015E: return 'S'; // Ş
        case 0x00FC: return 'u'; // ü
        case 0x00DC: return 'U'; // Ü

        // Turkish/Romanian comma-below s and t
        case 0x0219: return 's'; // ș
        case 0x0218: return 'S'; // Ș
        case 0x021B: return 't'; // ț
        case 0x021A: return 'T'; // Ț

        // Accented A / a
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3:
        case 0x00C4: case 0x00C5: case 0x0100: case 0x0102:
        case 0x0104: return 'A';
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3:
        case 0x00E4: case 0x00E5: case 0x0101: case 0x0103:
        case 0x0105: return 'a';
        case 0x00C6: return 'A'; // Æ
        case 0x00E6: return 'a'; // æ

        // Accented C / c
        case 0x0106: case 0x0108: case 0x010A: case 0x010C: return 'C';
        case 0x0107: case 0x0109: case 0x010B: case 0x010D: return 'c';

        // Accented D / d
        case 0x010E: case 0x0110: return 'D';
        case 0x010F: case 0x0111: return 'd';

        // Accented E / e
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        case 0x0112: case 0x0114: case 0x0116: case 0x0118:
        case 0x011A: return 'E';
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        case 0x0113: case 0x0115: case 0x0117: case 0x0119:
        case 0x011B: return 'e';

        // Accented G / g
        case 0x011C: case 0x0120: case 0x0122: return 'G';
        case 0x011D: case 0x0121: case 0x0123: return 'g';

        // Accented H / h
        case 0x0124: case 0x0126: return 'H';
        case 0x0125: case 0x0127: return 'h';

        // Accented I / i
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        case 0x0128: case 0x012A: case 0x012C: case 0x012E: return 'I';
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
        case 0x0129: case 0x012B: case 0x012D: case 0x012F: return 'i';

        // J / j
        case 0x0134: return 'J';
        case 0x0135: return 'j';

        // Accented K / k
        case 0x0136: return 'K';
        case 0x0137: return 'k';

        // Accented L / l
        case 0x0139: case 0x013B: case 0x013D: case 0x0141: return 'L';
        case 0x013A: case 0x013C: case 0x013E: case 0x0142: return 'l';

        // Accented N / n
        case 0x00D1: case 0x0143: case 0x0145: case 0x0147: return 'N';
        case 0x00F1: case 0x0144: case 0x0146: case 0x0148: return 'n';

        // Accented O / o
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5:
        case 0x00D8: case 0x014C: case 0x014E: case 0x0150: return 'O';
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5:
        case 0x00F8: case 0x014D: case 0x014F: case 0x0151: return 'o';

        // Accented R / r
        case 0x0154: case 0x0156: case 0x0158: return 'R';
        case 0x0155: case 0x0157: case 0x0159: return 'r';

        // Accented S / s
        case 0x00DF: return 's'; // ß
        case 0x015A: case 0x015C: case 0x0160: return 'S';
        case 0x015B: case 0x015D: case 0x0161: return 's';

        // Accented T / t
        case 0x0162: case 0x0164: case 0x0166: return 'T';
        case 0x0163: case 0x0165: case 0x0167: return 't';

        // Accented U / u
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x0168:
        case 0x016A: case 0x016C: case 0x016E: case 0x0170:
        case 0x0172: return 'U';
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x0169:
        case 0x016B: case 0x016D: case 0x016F: case 0x0171:
        case 0x0173: return 'u';

        // Accented W / w
        case 0x0174: return 'W';
        case 0x0175: return 'w';

        // Accented Y / y
        case 0x00DD: case 0x0176: case 0x0178: return 'Y';
        case 0x00FD: case 0x00FF: case 0x0177: return 'y';

        // Accented Z / z
        case 0x0179: case 0x017B: case 0x017D: return 'Z';
        case 0x017A: case 0x017C: case 0x017E: return 'z';

        // Punctuation and whitespace
        case 0x00A0: return ' '; // Non-breaking space
        case 0x2018: case 0x2019: case 0x201A: return '\'';
        case 0x201C: case 0x201D: case 0x201E: return '"';
        case 0x2013: case 0x2014: case 0x2212: return '-';

        default:
            return '?';
    }
}

/**
 * Safely consume the next UTF-8 code point and map to an ASCII character.
 *
 * Validates continuation bytes defensively without buffer overruns.
 * Advances *p past the decoded sequence. Accented Latin/Turkish characters
 * are mapped to their ASCII base equivalents; unsupported or invalid code points
 * return '?'. Returns '\0' when *p is NULL or points to '\0'.
 *
 * Algorithm:
 * Evaluates the leading byte to determine sequence length (1-4 bytes).
 * Ensures subsequent continuation bytes (0x80..0xBF) exist within string bounds.
 * Checks for overlong encodings and invalid UTF-16 surrogates. On invalid lead or
 * continuation bytes, advances *p by 1 byte and returns '?' as a fallback.
 * Valid non-ASCII code points are passed to map_unicode_to_ascii().
 *
 * @param p Pointer to string pointer; advanced upon return.
 * @return Mapped ASCII character, '?' for unsupported/invalid, or '\0' at end.
 */
char utf8_next_rune(const char **p) {
    if (!p || !*p || !**p) return '\0';

    const unsigned char *s = (const unsigned char *)*p;
    unsigned char b0 = s[0];

    // ASCII 1-byte
    if (b0 < 0x80) {
        (*p)++;
        return (char)b0;
    }

    // 2-byte sequence (110xxxxx 10xxxxxx)
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if ((s[1] & 0xC0) == 0x80) {
            uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) |
                          ((uint32_t)(s[1] & 0x3F));
            *p += 2;
            return map_unicode_to_ascii(cp);
        }
        (*p)++;
        return '?';
    }

    // 3-byte sequence (1110xxxx 10xxxxxx 10xxxxxx)
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (s[1] != '\0' && (s[1] & 0xC0) == 0x80 &&
            s[2] != '\0' && (s[2] & 0xC0) == 0x80) {
            // Guard overlong sequences and UTF-16 surrogates
            if ((b0 == 0xE0 && s[1] < 0xA0) || (b0 == 0xED && s[1] >= 0xA0)) {
                (*p)++;
                return '?';
            }
            uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                          ((uint32_t)(s[1] & 0x3F) << 6) |
                          ((uint32_t)(s[2] & 0x3F));
            *p += 3;
            return map_unicode_to_ascii(cp);
        }
        (*p)++;
        return '?';
    }

    // 4-byte sequence (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (s[1] != '\0' && (s[1] & 0xC0) == 0x80 &&
            s[2] != '\0' && (s[2] & 0xC0) == 0x80 &&
            s[3] != '\0' && (s[3] & 0xC0) == 0x80) {
            // Guard overlong sequences and Unicode upper bound (> 0x10FFFF)
            if ((b0 == 0xF0 && s[1] < 0x90) || (b0 == 0xF4 && s[1] > 0x8F)) {
                (*p)++;
                return '?';
            }
            uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                          ((uint32_t)(s[1] & 0x3F) << 12) |
                          ((uint32_t)(s[2] & 0x3F) << 6) |
                          ((uint32_t)(s[3] & 0x3F));
            *p += 4;
            return map_unicode_to_ascii(cp);
        }
        (*p)++;
        return '?';
    }

    // Invalid leading byte or stray continuation byte
    (*p)++;
    return '?';
}

/**
 * Calculate the pixel width of a UTF-8 string at given scale.
 *
 * Counts decoded UTF-8 runes (each occupying 8 * scale pixels) rather than
 * raw bytes, ensuring multi-byte characters do not inflate width metrics.
 *
 * Algorithm:
 * Iterates through text using utf8_next_rune() until string termination,
 * tallying rune count, and returns runes * 8 * scale.
 *
 * @param text NUL-terminated UTF-8 string.
 * @param scale Integer scaling factor (>= 1).
 * @return Width in pixels (0 if text is NULL or scale <= 0).
 */
int text_width(const char *text, int scale) {
    if (!text || scale <= 0) return 0;
    int runes = 0;
    const char *p = text;
    while (*p) {
        utf8_next_rune(&p);
        runes++;
    }
    return runes * 8 * scale;
}

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
 * Internal helper for left-to-right UTF-8 string rendering with optional clipping.
 *
 * Batches glyph spans across characters into a chunk buffer (up to 256 rects)
 * and sets draw color once per string, reducing SDL render calls.
 * Iterates by UTF-8 rune, advancing cx by 8 * scale per rune.
 * If max_width is not INT_MAX, emission stops when the next glyph would overflow.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text UTF-8 string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @param max_width Maximum allowable width in pixels, or INT_MAX for unclipped.
 * @return X coordinate after the last drawn character.
 */
static int text_draw_internal(SDL_Renderer *ren, int x, int y, const char *text,
                              SDL_Color col, int scale, int max_width) {
    if (!ren || !text || scale <= 0 || max_width <= 0) return x;
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    int cx = x;
    int char_w = 8 * scale;
    SDL_Rect batch[256];
    int batch_count = 0;

    const char *p = text;
    while (*p) {
        if (max_width != INT_MAX && cx + char_w > x + max_width) break;
        char c = utf8_next_rune(&p);
        if ((unsigned char)c < 128) {
            if (batch_count + 32 > 256) {
                SDL_RenderFillRects(ren, batch, batch_count);
                batch_count = 0;
            }
            batch_count += text_glyph_spans(c, cx, y, scale, batch + batch_count);
        }
        cx += char_w;
    }
    if (batch_count > 0) {
        SDL_RenderFillRects(ren, batch, batch_count);
    }
    return cx;
}

/**
 * Draw a NUL-terminated UTF-8 string left-to-right.
 *
 * Batches glyph spans across characters into a chunk buffer (up to 256 rects)
 * and sets draw color once per string, reducing SDL render calls from thousands
 * down to 1-3 per line.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text UTF-8 string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @return X coordinate after the last character.
 */
int text_draw(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale) {
    return text_draw_internal(ren, x, y, text, col, scale, INT_MAX);
}

/**
 * Draw UTF-8 string with truncation to fit within max_width (in pixels).
 *
 * Stops emitting characters when next glyph would overflow max_width. Batches
 * glyph spans into a chunk buffer and sets draw color once.
 *
 * @param ren Target SDL renderer.
 * @param x Origin X coordinate in pixels.
 * @param y Origin Y coordinate in pixels.
 * @param text UTF-8 string.
 * @param col Foreground color.
 * @param scale Integer scaling factor (>= 1).
 * @param max_width Maximum allowable width in pixels.
 * @return X coordinate after the last drawn character.
 */
int text_draw_clipped(SDL_Renderer *ren, int x, int y, const char *text, SDL_Color col, int scale, int max_width) {
    return text_draw_internal(ren, x, y, text, col, scale, max_width);
}
