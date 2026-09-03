#include "test_common.h"
#include "text.h"

#include <SDL2/SDL.h>

extern char font8x8_basic[128][8];
extern SDL_Renderer *g_ren;

static void test_text_basic(void) {
    if (!g_ren) return;

    SDL_Color col = {255, 255, 255, 255};
    // Drawing empty string
    int nx = text_draw(g_ren, 10, 10, "", col, 1);
    TEST_ASSERT_INT_EQ(nx, 10);

    // Drawing standard characters
    nx = text_draw(g_ren, 0, 0, "ABC", col, 1);
    TEST_ASSERT_INT_EQ(nx, 24);

    // Scale 2
    nx = text_draw(g_ren, 0, 0, "AB", col, 2);
    TEST_ASSERT_INT_EQ(nx, 32);

    // Negative / high byte char safety
    text_draw_char(g_ren, 0, 0, (char)200, col, 1);
    text_draw_char(g_ren, 0, 0, (char)-5, col, 1);
    text_draw_char(g_ren, 0, 0, 0, col, 1);
}

static void test_text_clipped(void) {
    if (!g_ren) return;

    SDL_Color col = {255, 255, 255, 255};
    // max_width = 0
    int nx = text_draw_clipped(g_ren, 50, 50, "Hello World", col, 1, 0);
    TEST_ASSERT_INT_EQ(nx, 50);

    // max_width negative
    nx = text_draw_clipped(g_ren, 50, 50, "Hello World", col, 1, -10);
    TEST_ASSERT_INT_EQ(nx, 50);

    // max_width fits exactly 2 chars (16 px)
    nx = text_draw_clipped(g_ren, 50, 50, "Hello World", col, 1, 16);
    TEST_ASSERT_INT_EQ(nx, 66);
}

static void test_text_glyph_spans_pixel_equivalence(void) {
    // Verify 100% pixel-accurate equivalence for all 128 ASCII characters
    for (int c = 0; c < 128; c++) {
        SDL_Rect rects[32];
        int count = text_glyph_spans((char)c, 0, 0, 1, rects);
        TEST_ASSERT(count >= 0 && count <= 32);

        bool covered[8][8];
        memset(covered, 0, sizeof(covered));

        for (int i = 0; i < count; i++) {
            TEST_ASSERT(rects[i].h == 1);
            TEST_ASSERT(rects[i].y >= 0 && rects[i].y < 8);
            TEST_ASSERT(rects[i].x >= 0 && rects[i].x + rects[i].w <= 8);
            TEST_ASSERT(rects[i].w >= 1);

            for (int dx = 0; dx < rects[i].w; dx++) {
                int px = rects[i].x + dx;
                int py = rects[i].y;
                // Verify no overlapping spans
                TEST_ASSERT(!covered[py][px]);
                covered[py][px] = true;
            }
        }

        // Compare with ground-truth font8x8 bitmap
        for (int row = 0; row < 8; row++) {
            unsigned char bits = (unsigned char)font8x8_basic[c][row];
            for (int col_bit = 0; col_bit < 8; col_bit++) {
                bool expected = (bits & (1 << col_bit)) != 0;
                TEST_ASSERT(covered[row][col_bit] == expected);
            }
        }

        // Verify scaled coordinates
        int scale = 3;
        int sx = 15, sy = 25;
        int scount = text_glyph_spans((char)c, sx, sy, scale, rects);
        TEST_ASSERT_INT_EQ(scount, count);
        for (int i = 0; i < scount; i++) {
            TEST_ASSERT_INT_EQ(rects[i].h, scale);
            TEST_ASSERT((rects[i].x - sx) % scale == 0);
            TEST_ASSERT((rects[i].y - sy) % scale == 0);
            TEST_ASSERT(rects[i].w % scale == 0);
        }
    }

    // Invalid arguments
    SDL_Rect rects[32];
    TEST_ASSERT_INT_EQ(text_glyph_spans('A', 0, 0, 0, rects), 0);
    TEST_ASSERT_INT_EQ(text_glyph_spans('A', 0, 0, -1, rects), 0);
    TEST_ASSERT_INT_EQ(text_glyph_spans('A', 0, 0, 1, NULL), 0);
    TEST_ASSERT_INT_EQ(text_glyph_spans((char)130, 0, 0, 1, rects), 0);
}

static void test_text_batch_reduction_benchmark(void) {
    long total_raw_bits = 0;
    long total_merged_spans = 0;

    for (int c = 32; c <= 126; c++) {
        for (int row = 0; row < 8; row++) {
            unsigned char bits = (unsigned char)font8x8_basic[c][row];
            for (int bit = 0; bit < 8; bit++) {
                if (bits & (1 << bit)) total_raw_bits++;
            }
        }
        SDL_Rect rects[32];
        total_merged_spans += text_glyph_spans((char)c, 0, 0, 1, rects);
    }

    // Typical reduction in rect count should exceed 60%
    TEST_ASSERT(total_raw_bits > 0);
    TEST_ASSERT(total_merged_spans > 0);
    TEST_ASSERT(total_merged_spans < total_raw_bits);
    double rect_reduction_pct = (1.0 - (double)total_merged_spans / (double)total_raw_bits) * 100.0;
    printf("    [Benchmark] Printable ASCII: %ld raw pixels -> %ld merged spans (%.1f%% rect reduction)\n",
           total_raw_bits, total_merged_spans, rect_reduction_pct);
    TEST_ASSERT(rect_reduction_pct > 50.0);

    // Test a realistic 80-character status bar string
    const char *sample_line = "sample.jpg  1920x1080  100%  SYNC  1/25  [s]ync [Tab] pane [f]ull [n/p] next/prev";
    size_t line_len = strlen(sample_line);
    long line_raw_bits = 0;
    long line_spans = 0;
    for (size_t i = 0; i < line_len; i++) {
        char ch = sample_line[i];
        if ((unsigned char)ch < 128) {
            for (int r = 0; r < 8; r++) {
                unsigned char bits = (unsigned char)font8x8_basic[(unsigned char)ch][r];
                for (int b = 0; b < 8; b++) {
                    if (bits & (1 << b)) line_raw_bits++;
                }
            }
            SDL_Rect rects[32];
            line_spans += text_glyph_spans(ch, 0, 0, 1, rects);
        }
    }
    // With 256-rect batching in text_draw, draw call count is ceil(line_spans / 256)
    long batched_draw_calls = (line_spans + 223) / 224; // text_draw flushes when batch_count + 32 > 256
    printf("    [Benchmark] 80-char line: %ld raw SDL draw calls -> %ld batched SDL draw calls (>%.1f%% call reduction)\n",
           line_raw_bits, batched_draw_calls, (1.0 - (double)batched_draw_calls / (double)line_raw_bits) * 100.0);
    TEST_ASSERT(batched_draw_calls <= 4);
    TEST_ASSERT(line_raw_bits > 800);

    // Test a realistic UTF-8 status bar string with multi-byte characters
    const char *utf8_sample = "fotoğraflar_arşivi_2026.jpg  3840x2160  100%  SYNC  1/50  [s]ync [f]ull";
    long utf8_raw_bits = 0;
    long utf8_spans = 0;
    const char *up = utf8_sample;
    while (*up) {
        char ch = utf8_next_rune(&up);
        if ((unsigned char)ch < 128) {
            for (int r = 0; r < 8; r++) {
                unsigned char bits = (unsigned char)font8x8_basic[(unsigned char)ch][r];
                for (int b = 0; b < 8; b++) {
                    if (bits & (1 << b)) utf8_raw_bits++;
                }
            }
            SDL_Rect rects[32];
            utf8_spans += text_glyph_spans(ch, 0, 0, 1, rects);
        }
    }
    long utf8_batched_calls = (utf8_spans + 223) / 224;
    printf("    [Benchmark] UTF-8 line (%zu bytes, %d runes): %ld raw SDL draw calls -> %ld batched SDL draw calls (>%.1f%% call reduction)\n",
           strlen(utf8_sample), text_width(utf8_sample, 1) / 8, utf8_raw_bits, utf8_batched_calls,
           (1.0 - (double)utf8_batched_calls / (double)utf8_raw_bits) * 100.0);
    TEST_ASSERT(utf8_batched_calls <= 4);
    TEST_ASSERT(utf8_raw_bits > 800);
}

static void test_text_utf8_turkish_and_width(void) {
    // Verify Turkish rune mappings
    const char *tr_chars = "çÇğĞıİöÖşŞüÜ";
    const char *p = tr_chars;
    const char *expected = "cCgGiIoOsSuU";
    for (int i = 0; expected[i]; i++) {
        char mapped = utf8_next_rune(&p);
        TEST_ASSERT_INT_EQ(mapped, expected[i]);
    }
    TEST_ASSERT_INT_EQ(*p, '\0');

    // Accented vowels
    const char *vowels = "àéîôû";
    p = vowels;
    const char *exp_vowels = "aeiou";
    for (int i = 0; exp_vowels[i]; i++) {
        char mapped = utf8_next_rune(&p);
        TEST_ASSERT_INT_EQ(mapped, exp_vowels[i]);
    }

    // Width computation based on runes vs raw bytes
    TEST_ASSERT_INT_EQ(text_width(NULL, 1), 0);
    TEST_ASSERT_INT_EQ(text_width("test", 0), 0);
    TEST_ASSERT_INT_EQ(text_width("test", -1), 0);
    TEST_ASSERT_INT_EQ(text_width("", 1), 0);

    // "fotoğraflar" has 11 runes, 12 bytes (ğ is 2 bytes)
    TEST_ASSERT_INT_EQ((int)strlen("fotoğraflar"), 12);
    TEST_ASSERT_INT_EQ(text_width("fotoğraflar", 1), 11 * 8);
    TEST_ASSERT_INT_EQ(text_width("fotoğraflar", 2), 11 * 16);

    // "İSTANBUL" has 8 runes, 9 bytes
    TEST_ASSERT_INT_EQ((int)strlen("İSTANBUL"), 9);
    TEST_ASSERT_INT_EQ(text_width("İSTANBUL", 1), 8 * 8);

    // "gözlük" has 6 runes, 8 bytes
    TEST_ASSERT_INT_EQ((int)strlen("gözlük"), 8);
    TEST_ASSERT_INT_EQ(text_width("gözlük", 1), 6 * 8);

    // Rendering Turkish text with g_ren
    if (g_ren) {
        SDL_Color col = {255, 255, 255, 255};
        int nx = text_draw(g_ren, 0, 0, "fotoğraflar", col, 1);
        TEST_ASSERT_INT_EQ(nx, 11 * 8);

        // text_draw_clipped with max_width fitting 5 runes (40 px)
        nx = text_draw_clipped(g_ren, 0, 0, "fotoğraflar", col, 1, 40);
        TEST_ASSERT_INT_EQ(nx, 40);

        // Fitting 4 runes (32 px) when max_width is 39
        nx = text_draw_clipped(g_ren, 0, 0, "fotoğraflar", col, 1, 39);
        TEST_ASSERT_INT_EQ(nx, 32);
    }
}

static void test_text_utf8_defensive_and_invalid(void) {
    // NULL string pointers
    TEST_ASSERT_INT_EQ(utf8_next_rune(NULL), '\0');
    const char *pnull = NULL;
    TEST_ASSERT_INT_EQ(utf8_next_rune(&pnull), '\0');
    const char *pempty = "";
    TEST_ASSERT_INT_EQ(utf8_next_rune(&pempty), '\0');
    TEST_ASSERT_INT_EQ(*pempty, '\0');

    // Truncated 2-byte UTF-8 sequence at end of string
    const char *trunc2 = "\xC4";
    const char *p = trunc2;
    char c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    TEST_ASSERT_INT_EQ(*p, '\0');

    // Truncated 3-byte UTF-8 sequence at end of string
    const char *trunc3 = "\xE2\x82";
    p = trunc3;
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    TEST_ASSERT_INT_EQ(*p, '\0');

    // Truncated 4-byte UTF-8 sequence at end of string
    const char *trunc4 = "\xF0\x9F\x8E";
    p = trunc4;
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    TEST_ASSERT_INT_EQ(*p, '\0');

    // Invalid continuation byte (lead byte followed by non-continuation byte)
    const char *inv_cont = "\xC4 hello";
    p = inv_cont;
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    TEST_ASSERT_INT_EQ(*p, ' ');
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, ' ');
    TEST_ASSERT_INT_EQ(*p, 'h');

    // Overlong 2-byte sequence (0xC0 0x80)
    const char *overlong = "\xC0\x80";
    p = overlong;
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');

    // Stray continuation byte (0x80..0xBF)
    const char *stray = "\x85\x90";
    p = stray;
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    c = utf8_next_rune(&p);
    TEST_ASSERT_INT_EQ(c, '?');
    TEST_ASSERT_INT_EQ(*p, '\0');

    // Safe width calculation on corrupted input
    int w = text_width("bad\xC4\xFF\xFEstr", 1);
    TEST_ASSERT(w > 0);

    // Safe rendering on corrupted input
    if (g_ren) {
        SDL_Color col = {255, 255, 255, 255};
        int nx = text_draw(g_ren, 0, 0, "bad\xC4\xFF\xFEstr", col, 1);
        TEST_ASSERT_INT_EQ(nx, w);
    }
}

static void test_text_utf8_boundary_stress(void) {
    // 1. Truncated multi-byte UTF-8 at string boundaries
    // Pointer must NEVER advance past the NUL terminator.
    const char *trunc_cases[] = {
        // 1-byte prefix of 2-byte sequence
        "\xC2",
        "\xDF",
        // 1-byte prefix of 3-byte sequence
        "\xE0",
        "\xE1",
        "\xEF",
        // 2-byte prefix of 3-byte sequence
        "\xE0\xA0",
        "\xE2\x82",
        "\xEF\xBF",
        // 1-byte prefix of 4-byte sequence
        "\xF0",
        "\xF1",
        "\xF4",
        // 2-byte prefix of 4-byte sequence
        "\xF0\x90",
        "\xF0\x9F",
        "\xF4\x8F",
        // 3-byte prefix of 4-byte sequence
        "\xF0\x90\x80",
        "\xF0\x9F\x98",
        "\xF4\x8F\xBF",
    };
    size_t num_trunc = sizeof(trunc_cases) / sizeof(trunc_cases[0]);
    for (size_t i = 0; i < num_trunc; i++) {
        const char *p = trunc_cases[i];
        int steps = 0;
        while (*p != '\0') {
            char c = utf8_next_rune(&p);
            TEST_ASSERT(c != '\0');
            steps++;
            TEST_ASSERT(steps <= 4); // Cannot take more steps than bytes
        }
        // Ensure pointer stops EXACTLY at null terminator
        TEST_ASSERT_INT_EQ(*p, '\0');

        // Calling again at null terminator must return '\0' and NOT advance pointer
        const char *p_saved = p;
        char end_c = utf8_next_rune(&p);
        TEST_ASSERT_INT_EQ(end_c, '\0');
        TEST_ASSERT(p == p_saved);

        // Width calculation must not crash or loop infinitely
        int w = text_width(trunc_cases[i], 1);
        TEST_ASSERT(w >= 0);
    }

    // 2. Overlong UTF-8 encodings
    // Must be rejected (return '?') and consume 1 byte per error fallback
    const char *overlong_cases[] = {
        "\xC0\x80",         // Overlong NUL (2 bytes)
        "\xC0\xAF",         // Overlong '/' (2 bytes)
        "\xC1\xBF",         // Overlong 0x7F (2 bytes)
        "\xE0\x80\x80",     // Overlong NUL (3 bytes)
        "\xE0\x9F\xBF",     // Overlong 0x07FF (3 bytes)
        "\xF0\x80\x80\x80", // Overlong NUL (4 bytes)
        "\xF0\x8F\xBF\xBF", // Overlong 0xFFFF (4 bytes)
        "\xED\xA0\x80",     // UTF-16 surrogate U+D800 (3 bytes)
        "\xED\xBF\xBF",     // UTF-16 surrogate U+DFFF (3 bytes)
        "\xF4\x90\x80\x80", // Out of Unicode range U+110000 (4 bytes)
        "\xF4\xBF\xBF\xBF", // Maximum 4-byte sequence (4 bytes)
    };
    size_t num_overlong = sizeof(overlong_cases) / sizeof(overlong_cases[0]);
    for (size_t i = 0; i < num_overlong; i++) {
        const char *p = overlong_cases[i];
        while (*p != '\0') {
            char c = utf8_next_rune(&p);
            TEST_ASSERT_INT_EQ(c, '?');
        }
        TEST_ASSERT_INT_EQ(*p, '\0');

        int w = text_width(overlong_cases[i], 1);
        TEST_ASSERT(w > 0);
    }

    // 3. Invalid lead bytes (0xF5..0xFF)
    const char *invalid_leads[] = {
        "\xF5", "\xF6", "\xF7", "\xF8", "\xF9", "\xFA", "\xFB", "\xFC", "\xFD", "\xFE", "\xFF",
        "\xF5\x80\x80\x80",
        "\xFF\xFE\xFD"
    };
    size_t num_inv = sizeof(invalid_leads) / sizeof(invalid_leads[0]);
    for (size_t i = 0; i < num_inv; i++) {
        const char *p = invalid_leads[i];
        while (*p != '\0') {
            char c = utf8_next_rune(&p);
            TEST_ASSERT_INT_EQ(c, '?');
        }
        TEST_ASSERT_INT_EQ(*p, '\0');
    }

    // 4. Valid 4-byte astral symbols (emojis, math symbols)
    const char *astral_symbols[] = {
        "\xF0\x9F\x98\x80", // U+1F600 Grinning Face
        "\xF0\x9F\x91\x8D", // U+1F44D Thumbs Up
        "\xF0\x9F\x8E\x89", // U+1F389 Party Popper
        "\xF0\x9F\x8C\x8D", // U+1F30D Earth Globe
        "\xF0\x9D\x94\xB8", // U+1D538 Mathematical Double-Struck Capital A
        "\xF0\x9D\x95\x8B", // U+1D54B Mathematical Double-Struck Capital T
    };
    size_t num_astral = sizeof(astral_symbols) / sizeof(astral_symbols[0]);
    for (size_t i = 0; i < num_astral; i++) {
        const char *p = astral_symbols[i];
        char c = utf8_next_rune(&p);
        // Consumed 4 bytes in a single rune step
        TEST_ASSERT_INT_EQ(c, '?');
        TEST_ASSERT_INT_EQ(*p, '\0');

        // text_width must treat single 4-byte astral symbol as exactly 1 rune
        TEST_ASSERT_INT_EQ(text_width(astral_symbols[i], 1), 8);
        TEST_ASSERT_INT_EQ(text_width(astral_symbols[i], 2), 16);
    }

    // Mixed string: ASCII + Turkish + Astral symbols
    // "Foto <astral-1> ve <astral-2>!" ->
    // "Foto " (5) + astral-1 (1) + " ve " (4) + astral-2 (1) + "!" (1) = 12 runes
    const char *mixed = "Foto \xF0\x9F\x98\x80 ve \xF0\x9D\x94\xB8!";
    TEST_ASSERT_INT_EQ(text_width(mixed, 1), 12 * 8);
    TEST_ASSERT_INT_EQ(text_width(mixed, 2), 12 * 16);
}

void run_text_tests(void) {
    printf("--- Text Test Suite ---\n");
    TEST_RUN(test_text_basic);
    TEST_RUN(test_text_clipped);
    TEST_RUN(test_text_glyph_spans_pixel_equivalence);
    TEST_RUN(test_text_batch_reduction_benchmark);
    TEST_RUN(test_text_utf8_turkish_and_width);
    TEST_RUN(test_text_utf8_defensive_and_invalid);
    TEST_RUN(test_text_utf8_boundary_stress);
}
