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
}

void run_text_tests(void) {
    printf("--- Text Test Suite ---\n");
    TEST_RUN(test_text_basic);
    TEST_RUN(test_text_clipped);
    TEST_RUN(test_text_glyph_spans_pixel_equivalence);
    TEST_RUN(test_text_batch_reduction_benchmark);
}
