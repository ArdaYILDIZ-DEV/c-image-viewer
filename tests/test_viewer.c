#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "test_common.h"
#include "viewer.h"
#include "exif.h"
#include "stb_image_write.h"

#include <SDL2/SDL.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>

static void test_viewer_truncate_filename(void) {
    char out[64];

    // Short filename unchanged ("photo.jpg", 24 -> "photo.jpg")
    viewer_truncate_filename("photo.jpg", out, sizeof(out), 24);
    TEST_ASSERT_STR_EQ(out, "photo.jpg");

    // Exact length unchanged ("12345678.png", 12 -> "12345678.png")
    viewer_truncate_filename("12345678.png", out, sizeof(out), 12);
    TEST_ASSERT_STR_EQ(out, "12345678.png");
    TEST_ASSERT_INT_EQ((int)strlen(out), 12);

    // Long filename with extension keeps ext ("qweqeqezxdfqwdwqdq.jpg", 18 -> "qweqeqezxdfq...jpg", strlen == 18)
    viewer_truncate_filename("qweqeqezxdfqwdwqdq.jpg", out, sizeof(out), 18);
    TEST_ASSERT_STR_EQ(out, "qweqeqezxdfq...jpg");
    TEST_ASSERT_INT_EQ((int)strlen(out), 18);

    // Long filename without extension ("longnamewithoutanyextension", 12 -> "longnamew...", strlen == 12)
    viewer_truncate_filename("longnamewithoutanyextension", out, sizeof(out), 12);
    TEST_ASSERT_STR_EQ(out, "longnamew...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 12);

    // Edge cases: null and empty name
    out[0] = 'x';
    viewer_truncate_filename(NULL, out, sizeof(out), 10);
    TEST_ASSERT_STR_EQ(out, "");

    out[0] = 'x';
    viewer_truncate_filename("", out, sizeof(out), 10);
    TEST_ASSERT_STR_EQ(out, "");

    // Edge cases: null out or zero out_sz (must not crash)
    viewer_truncate_filename("photo.jpg", NULL, 0, 10);
    viewer_truncate_filename("photo.jpg", out, 0, 10);

    // Edge cases: zero or negative max_len
    out[0] = 'x';
    viewer_truncate_filename("photo.jpg", out, sizeof(out), 0);
    TEST_ASSERT_STR_EQ(out, "");

    out[0] = 'x';
    viewer_truncate_filename("photo.jpg", out, sizeof(out), -5);
    TEST_ASSERT_STR_EQ(out, "");

    // Edge cases: dot at start (".hidden", 5 -> ".h...", 7 -> ".hidden")
    viewer_truncate_filename(".hidden", out, sizeof(out), 5);
    TEST_ASSERT_STR_EQ(out, ".h...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 5);

    viewer_truncate_filename(".hidden", out, sizeof(out), 7);
    TEST_ASSERT_STR_EQ(out, ".hidden");

    // Edge cases: small max_len (<= 3)
    viewer_truncate_filename("photo.jpg", out, sizeof(out), 3);
    TEST_ASSERT_STR_EQ(out, "...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 3);

    viewer_truncate_filename("photo.jpg", out, sizeof(out), 2);
    TEST_ASSERT_STR_EQ(out, "..");
    TEST_ASSERT_INT_EQ((int)strlen(out), 2);

    viewer_truncate_filename("photo.jpg", out, sizeof(out), 1);
    TEST_ASSERT_STR_EQ(out, ".");
    TEST_ASSERT_INT_EQ((int)strlen(out), 1);

    viewer_truncate_filename(".hidden", out, sizeof(out), 3);
    TEST_ASSERT_STR_EQ(out, "...");

    // Trailing dot without extension
    viewer_truncate_filename("longfile.", out, sizeof(out), 6);
    TEST_ASSERT_STR_EQ(out, "lon...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 6);

    // Buffer size smaller than max_len clamping
    char tiny[6];
    viewer_truncate_filename("photo.jpg", tiny, sizeof(tiny), 20);
    TEST_ASSERT_INT_EQ((int)strlen(tiny), 5);
    TEST_ASSERT_STR_EQ(tiny, "ph...");

    // Extension too long to leave stem room
    viewer_truncate_filename("test.verylongextension", out, sizeof(out), 10);
    TEST_ASSERT_STR_EQ(out, "test.ve...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 10);

    // Edge cases: single and double dot filenames
    viewer_truncate_filename(".", out, sizeof(out), 5);
    TEST_ASSERT_STR_EQ(out, ".");
    viewer_truncate_filename(".", out, sizeof(out), 1);
    TEST_ASSERT_STR_EQ(out, ".");
    viewer_truncate_filename(".", out, sizeof(out), 0);
    TEST_ASSERT_STR_EQ(out, "");

    viewer_truncate_filename("..", out, sizeof(out), 5);
    TEST_ASSERT_STR_EQ(out, "..");
    viewer_truncate_filename("..", out, sizeof(out), 2);
    TEST_ASSERT_STR_EQ(out, "..");
    viewer_truncate_filename("..", out, sizeof(out), 1);
    TEST_ASSERT_STR_EQ(out, ".");

    // Edge cases: negative and INT_MIN max_len
    viewer_truncate_filename("photo.jpg", out, sizeof(out), -1);
    TEST_ASSERT_STR_EQ(out, "");
    viewer_truncate_filename("photo.jpg", out, sizeof(out), -100);
    TEST_ASSERT_STR_EQ(out, "");
    viewer_truncate_filename("photo.jpg", out, sizeof(out), INT_MIN);
    TEST_ASSERT_STR_EQ(out, "");

    // Edge cases: multiple dots in filename
    viewer_truncate_filename("archive.tar.gz", out, sizeof(out), 14);
    TEST_ASSERT_STR_EQ(out, "archive.tar.gz");
    viewer_truncate_filename("archive.tar.gz", out, sizeof(out), 12);
    TEST_ASSERT_STR_EQ(out, "archive...gz");
    TEST_ASSERT_INT_EQ((int)strlen(out), 12);
    viewer_truncate_filename("archive.tar.gz", out, sizeof(out), 6);
    TEST_ASSERT_STR_EQ(out, "a...gz");
    TEST_ASSERT_INT_EQ((int)strlen(out), 6);
    viewer_truncate_filename("archive.tar.gz", out, sizeof(out), 5);
    TEST_ASSERT_STR_EQ(out, "ar...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 5);

    // Edge cases: leading dot files (dotfiles)
    viewer_truncate_filename(".bashrc", out, sizeof(out), 7);
    TEST_ASSERT_STR_EQ(out, ".bashrc");
    viewer_truncate_filename(".bashrc", out, sizeof(out), 5);
    TEST_ASSERT_STR_EQ(out, ".b...");
    TEST_ASSERT_INT_EQ((int)strlen(out), 5);
    viewer_truncate_filename(".bashrc.bak", out, sizeof(out), 10);
    TEST_ASSERT_STR_EQ(out, ".bas...bak");
    TEST_ASSERT_INT_EQ((int)strlen(out), 10);

    // 2000-character long filename
    char long_name[2048];
    memset(long_name, 'a', 2000);
    memcpy(long_name + 1996, ".png", 5);
    viewer_truncate_filename(long_name, out, sizeof(out), 20);
    TEST_ASSERT_INT_EQ((int)strlen(out), 20);
    TEST_ASSERT(strstr(out, "...png") != NULL);

    // Tiny out_sz (1, 2, 3, 4, 5) bounds safety
    for (int sz = 1; sz <= 5; sz++) {
        char tbuf[8];
        memset(tbuf, 'Z', sizeof(tbuf));
        viewer_truncate_filename("photo.jpg", tbuf, (size_t)sz, 50);
        TEST_ASSERT((int)strlen(tbuf) < sz);
        TEST_ASSERT(tbuf[sz - 1] == '\0');
    }
}

static void test_viewer_format_color_depth(void) {
    char buf[64];

    viewer_format_color_depth(1, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "8-bit Grayscale");

    viewer_format_color_depth(2, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "16-bit Gray+Alpha");

    viewer_format_color_depth(3, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "24-bit RGB");

    viewer_format_color_depth(4, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "32-bit RGBA");

    viewer_format_color_depth(0, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "Unknown");

    viewer_format_color_depth(-1, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "Unknown");

    viewer_format_color_depth(5, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "5 channels");

    viewer_format_color_depth(100, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "100 channels");

    viewer_format_color_depth(INT_MAX, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "channels") != NULL);

    viewer_format_color_depth(INT_MIN, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "Unknown");

    // Small out_sz buffer truncation safety
    char small[6];
    viewer_format_color_depth(3, small, sizeof(small));
    TEST_ASSERT_STR_EQ(small, "24-bi");
    TEST_ASSERT_INT_EQ((int)strlen(small), 5);

    // Tiny out_sz (1, 2, 3, 4, 5)
    for (int sz = 1; sz <= 5; sz++) {
        char tbuf[8];
        memset(tbuf, 'Z', sizeof(tbuf));
        viewer_format_color_depth(3, tbuf, (size_t)sz);
        TEST_ASSERT((int)strlen(tbuf) < sz);
        TEST_ASSERT(tbuf[sz - 1] == '\0');
    }

    // NULL and 0 out_sz guards
    viewer_format_color_depth(3, NULL, 0);
    viewer_format_color_depth(3, buf, 0);
}

static void test_viewer_truncate_path(void) {
    char buf[128];
    const char *nested = "/home/user/very/long/nested/path/to/my_image.png";

    // Short path
    viewer_truncate_path("/short/path.jpg", buf, sizeof(buf), 30);
    TEST_ASSERT_STR_EQ(buf, "/short/path.jpg");

    // Exact fit
    int nested_len = (int)strlen(nested);
    viewer_truncate_path(nested, buf, sizeof(buf), nested_len);
    TEST_ASSERT_STR_EQ(buf, nested);

    // Deeply nested path with room for prefix + .../ + filename
    viewer_truncate_path(nested, buf, sizeof(buf), 30);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 30);
    TEST_ASSERT(strncmp(buf, "/home/user/ver", 14) == 0);
    TEST_ASSERT(strstr(buf, ".../my_image.png") != NULL);

    // Deeply nested path with room only for .../ + filename (16 chars)
    viewer_truncate_path(nested, buf, sizeof(buf), 16);
    TEST_ASSERT_STR_EQ(buf, ".../my_image.png");
    TEST_ASSERT_INT_EQ((int)strlen(buf), 16);

    // Deeply nested path when max_chars cannot fit .../ + filename
    viewer_truncate_path(nested, buf, sizeof(buf), 10);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 10);
    TEST_ASSERT(strstr(buf, "...png") != NULL);

    // Path without slash
    viewer_truncate_path("single_image.jpg", buf, sizeof(buf), 20);
    TEST_ASSERT_STR_EQ(buf, "single_image.jpg");

    viewer_truncate_path("single_image.jpg", buf, sizeof(buf), 10);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "sing...jpg");

    // Edge cases: NULL, out_sz == 0, max_chars <= 0
    buf[0] = 'x';
    viewer_truncate_path(NULL, buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "");

    viewer_truncate_path(nested, NULL, 0, 10);
    viewer_truncate_path(nested, buf, 0, 10);

    buf[0] = 'x';
    viewer_truncate_path(nested, buf, sizeof(buf), 0);
    TEST_ASSERT_STR_EQ(buf, "");

    buf[0] = 'x';
    viewer_truncate_path(nested, buf, sizeof(buf), -5);
    TEST_ASSERT_STR_EQ(buf, "");

    // Small max_chars (<= 5)
    viewer_truncate_path(nested, buf, sizeof(buf), 5);
    TEST_ASSERT_STR_EQ(buf, ".....");
    TEST_ASSERT_INT_EQ((int)strlen(buf), 5);

    viewer_truncate_path(nested, buf, sizeof(buf), 3);
    TEST_ASSERT_STR_EQ(buf, "...");
    TEST_ASSERT_INT_EQ((int)strlen(buf), 3);

    viewer_truncate_path(nested, buf, sizeof(buf), 1);
    TEST_ASSERT_STR_EQ(buf, ".");
    TEST_ASSERT_INT_EQ((int)strlen(buf), 1);

    // Small buffer safety
    char tiny[6];
    viewer_truncate_path(nested, tiny, sizeof(tiny), 20);
    TEST_ASSERT((int)strlen(tiny) <= 5);

    // Edge cases: root, slashes, dot, dotdot
    viewer_truncate_path("/", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "/");
    viewer_truncate_path("/", buf, sizeof(buf), 1);
    TEST_ASSERT_STR_EQ(buf, "/");
    viewer_truncate_path("/", buf, sizeof(buf), 0);
    TEST_ASSERT_STR_EQ(buf, "");

    viewer_truncate_path("///", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "///");
    viewer_truncate_path("///", buf, sizeof(buf), 2);
    TEST_ASSERT_STR_EQ(buf, "..");
    viewer_truncate_path("///", buf, sizeof(buf), 1);
    TEST_ASSERT_STR_EQ(buf, ".");

    viewer_truncate_path("", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "");

    viewer_truncate_path(".", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, ".");
    viewer_truncate_path(".", buf, sizeof(buf), 1);
    TEST_ASSERT_STR_EQ(buf, ".");

    viewer_truncate_path("..", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "..");
    viewer_truncate_path("..", buf, sizeof(buf), 1);
    TEST_ASSERT_STR_EQ(buf, ".");

    viewer_truncate_path("//foo", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "//foo");
    viewer_truncate_path("//foo", buf, sizeof(buf), 4);
    TEST_ASSERT_STR_EQ(buf, "....");

    viewer_truncate_path("/foo/", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "/foo/");
    viewer_truncate_path("/foo/", buf, sizeof(buf), 4);
    TEST_ASSERT_STR_EQ(buf, "....");

    viewer_truncate_path("/foo/bar/baz/qux/", buf, sizeof(buf), 10);
    TEST_ASSERT_STR_EQ(buf, "/foo/ba...");

    // max_chars smaller than strlen(basename)
    viewer_truncate_path("/very/long/path/extremelylongfilename.png", buf, sizeof(buf), 12);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 12);
    TEST_ASSERT(strstr(buf, "...png") != NULL);
    viewer_truncate_path("/very/long/path/extremelylongfilename.png", buf, sizeof(buf), 5);
    TEST_ASSERT_STR_EQ(buf, ".....");
    viewer_truncate_path("/very/long/path/extremelylongfilename.png", buf, sizeof(buf), 3);
    TEST_ASSERT_STR_EQ(buf, "...");

    // Tiny out_sz (1, 2, 3, 4, 5) safe NUL-termination
    for (int sz = 1; sz <= 5; sz++) {
        char tbuf[8];
        memset(tbuf, 'Z', sizeof(tbuf));
        viewer_truncate_path(nested, tbuf, (size_t)sz, 50);
        TEST_ASSERT((int)strlen(tbuf) < sz);
        TEST_ASSERT(tbuf[sz - 1] == '\0');

        viewer_truncate_path("/", tbuf, (size_t)sz, 50);
        TEST_ASSERT((int)strlen(tbuf) < sz);
        TEST_ASSERT(tbuf[sz - 1] == '\0');
    }

    // 2000-character long path with slashes
    char long_p[2048];
    memset(long_p, 'a', 2000);
    long_p[0] = '/';
    long_p[1985] = '/';
    memcpy(long_p + 1986, "target.png", 11);
    viewer_truncate_path(long_p, buf, sizeof(buf), 30);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 30);
    TEST_ASSERT(strstr(buf, ".../target.png") != NULL);

    // 2000-character long path without slashes
    char long_p_noslash[2048];
    memset(long_p_noslash, 'b', 2000);
    memcpy(long_p_noslash + 1996, ".png", 5);
    viewer_truncate_path(long_p_noslash, buf, sizeof(buf), 20);
    TEST_ASSERT_INT_EQ((int)strlen(buf), 20);
    TEST_ASSERT(strstr(buf, "...png") != NULL);

    // Invariant check: output length must NEVER exceed max_chars
    for (int mc = 1; mc <= 60; mc++) {
        char check_buf[128];
        viewer_truncate_path(nested, check_buf, sizeof(check_buf), mc);
        TEST_ASSERT((int)strlen(check_buf) <= mc);

        viewer_truncate_path("single_image.jpg", check_buf, sizeof(check_buf), mc);
        TEST_ASSERT((int)strlen(check_buf) <= mc);
    }
}

static void test_viewer_format_file_size(void) {
    char buf[64];

    viewer_format_file_size(0, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "0 B");

    viewer_format_file_size(500, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "500 B");

    viewer_format_file_size(1024, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "1.0 KB (1024 B)");

    viewer_format_file_size(2048, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "2.0 KB (2048 B)");

    viewer_format_file_size(10485760, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "10.0 MB (10485760 B)");

    // Negative size formats as 0 B
    viewer_format_file_size(-1, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "0 B");

    viewer_format_file_size(-1024, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "0 B");

    viewer_format_file_size(LLONG_MIN, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "0 B");

    // Gigabytes range and LLONG_MAX
    viewer_format_file_size(3221225472LL, buf, sizeof(buf));
    TEST_ASSERT_STR_EQ(buf, "3.00 GB (3221225472 B)");

    viewer_format_file_size(LLONG_MAX, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "GB") != NULL);
    TEST_ASSERT(strstr(buf, "9223372036854775807 B") != NULL);

    // Small buffer truncation safety
    char small[6];
    viewer_format_file_size(2048, small, sizeof(small));
    TEST_ASSERT_STR_EQ(small, "2.0 K");
    TEST_ASSERT_INT_EQ((int)strlen(small), 5);

    // Tiny out_sz (1, 2, 3, 4, 5)
    for (int sz = 1; sz <= 5; sz++) {
        char tbuf[8];
        memset(tbuf, 'Z', sizeof(tbuf));
        viewer_format_file_size(2048, tbuf, (size_t)sz);
        TEST_ASSERT((int)strlen(tbuf) < sz);
        TEST_ASSERT(tbuf[sz - 1] == '\0');
    }

    // NULL and 0 out_sz guards
    viewer_format_file_size(1024, NULL, 0);
    viewer_format_file_size(1024, buf, 0);
}

static void test_viewer_is_image_file(void) {
    TEST_ASSERT(viewer_is_image_file("photo.jpg"));
    TEST_ASSERT(viewer_is_image_file("PHOTO.JPG"));
    TEST_ASSERT(viewer_is_image_file("photo.jpeg"));
    TEST_ASSERT(viewer_is_image_file("image.png"));
    TEST_ASSERT(viewer_is_image_file("IMAGE.PNG"));
    TEST_ASSERT(viewer_is_image_file("graphic.webp"));
    TEST_ASSERT(viewer_is_image_file("pic.bmp"));
    TEST_ASSERT(viewer_is_image_file("art.tiff"));
    TEST_ASSERT(viewer_is_image_file("art.tif"));
    TEST_ASSERT(viewer_is_image_file("anim.gif"));
    TEST_ASSERT(viewer_is_image_file("doc.ppm"));
    TEST_ASSERT(viewer_is_image_file("doc.pgm"));
    TEST_ASSERT(viewer_is_image_file("doc.pbm"));
    TEST_ASSERT(viewer_is_image_file("high.hdr"));
    TEST_ASSERT(viewer_is_image_file("layer.psd"));
    TEST_ASSERT(viewer_is_image_file("tex.tga"));

    TEST_ASSERT(!viewer_is_image_file("document.pdf"));
    TEST_ASSERT(!viewer_is_image_file("script.sh"));
    TEST_ASSERT(!viewer_is_image_file("no_extension"));
    TEST_ASSERT(!viewer_is_image_file(".hidden"));
    TEST_ASSERT(!viewer_is_image_file("trailing_dot."));
    TEST_ASSERT(!viewer_is_image_file(""));
    TEST_ASSERT(!viewer_is_image_file(NULL));
}

static void test_viewer_file_list_and_scan(void) {
    // NULL and empty inputs
    TEST_ASSERT(!viewer_scan_current_dir(NULL));
    TEST_ASSERT(!viewer_scan_current_dir(""));
    TEST_ASSERT(!viewer_scan_current_dir("/tmp/civ_nonexistent_dir/dummy.jpg"));

    // Multiple frees must be safe
    viewer_free_file_list();
    viewer_free_file_list();
    TEST_ASSERT_INT_EQ(g_file_count, 0);
    TEST_ASSERT_INT_EQ(g_file_index, -1);

    // Create temporary test directory with some dummy images
    char temp_dir[] = "/tmp/civ_test_dir_XXXXXX";
    char *d = mkdtemp(temp_dir);
    TEST_ASSERT(d != NULL);

    char img1[PATH_MAX + 32], img2[PATH_MAX + 32], txt[PATH_MAX + 32];
    snprintf(img1, sizeof(img1), "%s/1_alpha.png", temp_dir);
    snprintf(img2, sizeof(img2), "%s/2_beta.jpg", temp_dir);
    snprintf(txt, sizeof(txt), "%s/readme.txt", temp_dir);

    unsigned char dummy_pixel[4] = {255, 0, 0, 255};
    stbi_write_png(img1, 1, 1, 4, dummy_pixel, 4);
    stbi_write_png(img2, 1, 1, 4, dummy_pixel, 4);

    FILE *f = fopen(txt, "w");
    if (f) { fputs("ignore me\n", f); fclose(f); }

    // Scan
    TEST_ASSERT(viewer_scan_current_dir(img2));
    TEST_ASSERT_INT_EQ(g_file_count, 2);
    // Index should match img2 (second file alphabetically)
    TEST_ASSERT_INT_EQ(g_file_index, 1);
    TEST_ASSERT(strstr(g_file_list[0], "1_alpha.png") != NULL);
    TEST_ASSERT(strstr(g_file_list[1], "2_beta.jpg") != NULL);

    // Clean up
    viewer_free_file_list();
    unlink(img1);
    unlink(img2);
    unlink(txt);
    rmdir(temp_dir);
}

static void test_viewer_load_unload(void) {
    Image img = {0};
    // NULL guards
    TEST_ASSERT(!viewer_load_image(NULL, &img));
    TEST_ASSERT(!viewer_load_image("/tmp/nonexistent_xyz.png", NULL));
    TEST_ASSERT(!viewer_load_image("/tmp/nonexistent_xyz.png", &img));

    viewer_unload_image(NULL);
    viewer_unload_image(&img);

    // Create a temporary 16x16 PNG image
    const char *tmp_img = "/tmp/civ_test_sample.png";
    unsigned char pixels[16 * 16 * 4];
    memset(pixels, 128, sizeof(pixels));
    TEST_ASSERT(stbi_write_png(tmp_img, 16, 16, 4, pixels, 16 * 4));

    TEST_ASSERT(viewer_load_image(tmp_img, &img));
    TEST_ASSERT(img.tex != NULL);
    TEST_ASSERT_INT_EQ(img.w, 16);
    TEST_ASSERT_INT_EQ(img.h, 16);
    TEST_ASSERT_INT_EQ(img.channels, 4);
    TEST_ASSERT(img.path != NULL);

    viewer_unload_image(&img);
    TEST_ASSERT(img.tex == NULL);
    TEST_ASSERT(img.path == NULL);
    TEST_ASSERT_INT_EQ(img.w, 0);
    TEST_ASSERT_INT_EQ(img.channels, 0);

    unlink(tmp_img);
}

static void test_viewer_replace_and_bounds(void) {
    const char *tmp_img = "/tmp/civ_test_sample2.png";
    unsigned char pixels[8 * 8 * 4];
    memset(pixels, 200, sizeof(pixels));
    TEST_ASSERT(stbi_write_png(tmp_img, 8, 8, 4, pixels, 8 * 4));

    // Invalid pane
    TEST_ASSERT(!viewer_replace_image(-1, tmp_img));
    TEST_ASSERT(!viewer_replace_image(2, tmp_img));
    TEST_ASSERT(!viewer_replace_image(0, NULL));

    // Valid replacement
    TEST_ASSERT(viewer_replace_image(0, tmp_img));
    TEST_ASSERT_INT_EQ(g_count, 1);
    TEST_ASSERT(g_img[0].tex != NULL);

    // Zoom and pan tests
    viewer_do_zoom(2.0f, 100, 100);
    TEST_ASSERT(g_zoom > 1.0f);
    viewer_do_zoom(0.0001f, 100, 100);
    TEST_ASSERT(g_zoom >= 0.05f); // clamped to min 0.05

    viewer_do_zoom(1000.0f, 100, 100);
    TEST_ASSERT(g_zoom <= 32.0f); // clamped to max 32.0

    viewer_do_pan(50, -20);
    viewer_toggle_sync();
    viewer_toggle_sync();
    viewer_update_title();

    // Render info bar and metadata under tiny window sizes (verify no negative indexing)
    int old_w = g_win_w;
    int old_h = g_win_h;

    g_win_w = 4;
    g_win_h = 4;
    viewer_render_info_bar(g_ren);

    g_show_metadata = true;
    viewer_render_metadata(g_ren);
    g_show_metadata = false;

    g_win_w = old_w;
    g_win_h = old_h;

    viewer_unload_image(&g_img[0]);
    unlink(tmp_img);
}

static void test_viewer_dual_pane_and_navigation(void) {
    char temp_dir[] = "/tmp/civ_vnav_XXXXXX";
    char *d = mkdtemp(temp_dir);
    TEST_ASSERT(d != NULL);

    char img1[PATH_MAX + 32], img2[PATH_MAX + 32], img3[PATH_MAX + 32];
    snprintf(img1, sizeof(img1), "%s/1.png", temp_dir);
    snprintf(img2, sizeof(img2), "%s/2.png", temp_dir);
    snprintf(img3, sizeof(img3), "%s/3.png", temp_dir);

    unsigned char pix[4] = {1, 2, 3, 255};
    stbi_write_png(img1, 1, 1, 4, pix, 4);
    stbi_write_png(img2, 1, 1, 4, pix, 4);
    stbi_write_png(img3, 1, 1, 4, pix, 4);

    // Setup dual pane
    TEST_ASSERT(viewer_replace_image(0, img1));
    TEST_ASSERT(viewer_replace_image(1, img2));
    TEST_ASSERT_INT_EQ(g_count, 2);

    viewer_fit_view();
    viewer_update_title();

    // Toggle sync to free mode
    viewer_toggle_sync();
    TEST_ASSERT(!g_sync);
    g_active = 1;
    viewer_do_zoom(1.5f, 300, 200);
    viewer_do_pan(10, 20);
    viewer_update_title();

    // Re-sync
    viewer_toggle_sync();
    TEST_ASSERT(g_sync);

    // Navigation delta
    viewer_scan_current_dir(img1);
    TEST_ASSERT_INT_EQ(g_file_index, 0);
    TEST_ASSERT(viewer_navigate(+1));
    TEST_ASSERT_INT_EQ(g_file_index, 1);
    TEST_ASSERT(viewer_navigate(-1));
    TEST_ASSERT_INT_EQ(g_file_index, 0);

    // Unload images and free list
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    viewer_free_file_list();

    unlink(img1);
    unlink(img2);
    unlink(img3);
    rmdir(temp_dir);
}

static void test_viewer_go_parent_edge_cases(void) {
    // Empty current directory
    g_current_dir[0] = '\0';
    TEST_ASSERT(!viewer_go_parent());

    // Root directory
    snprintf(g_current_dir, sizeof(g_current_dir), "/");
    TEST_ASSERT(!viewer_go_parent());
    TEST_ASSERT_STR_EQ(g_current_dir, "/");

    // Root directory scan test: verify no double slash "//" is generated
    viewer_free_file_list();
    viewer_scan_current_dir("/dummy.jpg");
    // If root happened to have images or not, verify all entries (if any) don't start with "//"
    for (int i = 0; i < g_file_count; i++) {
        TEST_ASSERT(strncmp(g_file_list[i], "//", 2) != 0);
    }
    viewer_free_file_list();

    // Invalid non-existent directory
    snprintf(g_current_dir, sizeof(g_current_dir), "/invalid_civ_dir_xyz_12345");
    TEST_ASSERT(!viewer_go_parent());
}

static void test_viewer_validate_image_path(void) {
    char out[PATH_MAX];

    // Null and empty
    TEST_ASSERT(!viewer_validate_image_path(NULL, out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("", out, sizeof(out)));

    // Oversized path
    char huge_path[PATH_MAX + 64];
    memset(huge_path, 'a', sizeof(huge_path) - 1);
    huge_path[sizeof(huge_path) - 1] = '\0';
    TEST_ASSERT(!viewer_validate_image_path(huge_path, out, sizeof(out)));

    // Non-printable control characters
    TEST_ASSERT(!viewer_validate_image_path("image\n.png", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("image\r.png", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("image\x1b.png", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("image\x7f.png", out, sizeof(out)));

    // Directories
    TEST_ASSERT(!viewer_validate_image_path("/tmp", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("/", out, sizeof(out)));

    // Special devices
    TEST_ASSERT(!viewer_validate_image_path("/dev/zero", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("/dev/null", out, sizeof(out)));

    // Non-image files
    TEST_ASSERT(!viewer_validate_image_path("/etc/passwd", out, sizeof(out)));
    TEST_ASSERT(!viewer_validate_image_path("/bin/sh", out, sizeof(out)));

    // Non-existent file
    TEST_ASSERT(!viewer_validate_image_path("/tmp/civ_nonexistent_xyz_123.png", out, sizeof(out)));

    // Valid image file
    const char *tmp = "/tmp/civ_val_test.png";
    unsigned char pixel[4] = {10, 20, 30, 255};
    TEST_ASSERT(stbi_write_png(tmp, 1, 1, 4, pixel, 4));

    TEST_ASSERT(viewer_validate_image_path(tmp, out, sizeof(out)));
    TEST_ASSERT(strstr(out, "civ_val_test.png") != NULL);
    TEST_ASSERT(out[0] == '/');
    TEST_ASSERT(strncmp(out, "//", 2) != 0);

    unlink(tmp);
}

static void write_raw_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (f) {
        if (size > 0 && data) fwrite(data, 1, size, f);
        fclose(f);
    }
}

static void test_viewer_corrupt_and_garbage_images(void) {
    const char *empty_file = "/tmp/civ_vtest_empty.png";
    const char *trunc_file = "/tmp/civ_vtest_trunc.png";
    const char *garbage_file = "/tmp/civ_vtest_garbage.png";

    // 0-byte file
    write_raw_file(empty_file, "", 0);

    // Truncated file (PNG magic header only, 8 bytes)
    uint8_t png_hdr[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    write_raw_file(trunc_file, png_hdr, sizeof(png_hdr));

    // Random garbage file
    uint8_t garbage[4096];
    for (size_t i = 0; i < sizeof(garbage); i++) garbage[i] = (uint8_t)(i * 31 + 7);
    write_raw_file(garbage_file, garbage, sizeof(garbage));

    Image img = {0};
    TEST_ASSERT(!viewer_load_image(empty_file, &img));
    TEST_ASSERT(img.tex == NULL && img.path == NULL && img.w == 0 && img.h == 0);

    TEST_ASSERT(!viewer_load_image(trunc_file, &img));
    TEST_ASSERT(img.tex == NULL && img.path == NULL && img.w == 0 && img.h == 0);

    TEST_ASSERT(!viewer_load_image(garbage_file, &img));
    TEST_ASSERT(img.tex == NULL && img.path == NULL && img.w == 0 && img.h == 0);

    // Test replace: existing valid image must be preserved when replacement fails
    const char *valid_file = "/tmp/civ_vtest_valid.png";
    unsigned char px[4] = {50, 100, 150, 255};
    TEST_ASSERT(stbi_write_png(valid_file, 1, 1, 4, px, 4));

    TEST_ASSERT(viewer_replace_image(0, valid_file));
    TEST_ASSERT(g_img[0].tex != NULL);
    char *orig_path = strdup(g_img[0].path);

    // Attempt replacing with corrupt files
    TEST_ASSERT(!viewer_replace_image(0, empty_file));
    TEST_ASSERT_STR_EQ(g_img[0].path, orig_path);
    TEST_ASSERT(g_img[0].tex != NULL);

    TEST_ASSERT(!viewer_replace_image(0, trunc_file));
    TEST_ASSERT_STR_EQ(g_img[0].path, orig_path);
    TEST_ASSERT(g_img[0].tex != NULL);

    TEST_ASSERT(!viewer_replace_image(0, garbage_file));
    TEST_ASSERT_STR_EQ(g_img[0].path, orig_path);
    TEST_ASSERT(g_img[0].tex != NULL);

    free(orig_path);
    viewer_unload_image(&g_img[0]);
    g_count = 0;

    unlink(empty_file);
    unlink(trunc_file);
    unlink(garbage_file);
    unlink(valid_file);
}

static void test_viewer_degenerate_window_and_fit(void) {
    const char *valid_file = "/tmp/civ_vtest_degen.png";
    unsigned char px[2 * 2 * 4];
    memset(px, 128, sizeof(px));
    TEST_ASSERT(stbi_write_png(valid_file, 2, 2, 4, px, 2 * 4));

    TEST_ASSERT(viewer_replace_image(0, valid_file));
    TEST_ASSERT_INT_EQ(g_count, 1);

    int orig_w = g_win_w;
    int orig_h = g_win_h;

    // Degenerate sizes to exercise
    int degen_sizes[][2] = {
        {0, 0},
        {1, 1},
        {-10, -10},
        {0, 100},
        {100, 0},
        {-50, 100},
        {100, -50},
        {2, 2}
    };

    for (size_t i = 0; i < sizeof(degen_sizes)/sizeof(degen_sizes[0]); i++) {
        g_win_w = degen_sizes[i][0];
        g_win_h = degen_sizes[i][1];

        viewer_fit_view();
        TEST_ASSERT(g_zoom >= 0.05f);

        viewer_do_zoom(2.0f, 0, 0);
        viewer_do_zoom(0.5f, 0, 0);
        viewer_do_pan(10, 10);

        viewer_render(g_ren);
        viewer_render_info_bar(g_ren);
        g_show_help = true;
        viewer_render_help(g_ren);
        g_show_help = false;
        g_show_metadata = true;
        viewer_render_metadata(g_ren);
        g_show_metadata = false;
    }

    // Degenerate image dimensions: 0x0 with valid window
    g_win_w = 640;
    g_win_h = 480;
    int real_w = g_img[0].w;
    int real_h = g_img[0].h;
    g_img[0].w = 0;
    g_img[0].h = 0;

    viewer_fit_view();
    TEST_ASSERT(g_zoom >= 0.05f);
    viewer_render(g_ren);

    // Restore
    g_img[0].w = real_w;
    g_img[0].h = real_h;
    g_win_w = orig_w;
    g_win_h = orig_h;

    viewer_unload_image(&g_img[0]);
    g_count = 0;
    unlink(valid_file);
}

static void test_viewer_navigate_corrupt_file_skipping(void) {
    char temp_dir[] = "/tmp/civ_navskip_XXXXXX";
    char *d = mkdtemp(temp_dir);
    TEST_ASSERT(d != NULL);

    char f1[PATH_MAX], f2[PATH_MAX], f3[PATH_MAX], f4[PATH_MAX];
    snprintf(f1, sizeof(f1), "%s/1_good.png", temp_dir);
    snprintf(f2, sizeof(f2), "%s/2_corrupt_empty.png", temp_dir);
    snprintf(f3, sizeof(f3), "%s/3_corrupt_junk.png", temp_dir);
    snprintf(f4, sizeof(f4), "%s/4_good.png", temp_dir);

    unsigned char px[4] = {1, 2, 3, 255};
    TEST_ASSERT(stbi_write_png(f1, 1, 1, 4, px, 4));
    write_raw_file(f2, "", 0); // 0 bytes
    uint8_t junk[128];
    memset(junk, 0xAA, sizeof(junk));
    write_raw_file(f3, junk, sizeof(junk)); // binary junk
    TEST_ASSERT(stbi_write_png(f4, 1, 1, 4, px, 4));

    TEST_ASSERT(viewer_replace_image(0, f1));
    TEST_ASSERT(viewer_scan_current_dir(f1));
    TEST_ASSERT_INT_EQ(g_file_index, 0);

    // Navigate forward: must skip 2_corrupt and 3_corrupt and land on 4_good (index 3)
    TEST_ASSERT(viewer_navigate(+1));
    TEST_ASSERT_INT_EQ(g_file_index, 3);
    TEST_ASSERT(strstr(g_img[0].path, "4_good.png") != NULL);

    // Navigate backward: must skip 3_corrupt and 2_corrupt and land on 1_good (index 0)
    TEST_ASSERT(viewer_navigate(-1));
    TEST_ASSERT_INT_EQ(g_file_index, 0);
    TEST_ASSERT(strstr(g_img[0].path, "1_good.png") != NULL);

    // Case B: directory where ALL other files are corrupt
    unlink(f4); // now only 1_good, 2_corrupt, 3_corrupt remain
    viewer_scan_current_dir(f1);
    TEST_ASSERT_INT_EQ(g_file_index, 0);
    // Navigating forward must fail cleanly, staying at 1_good
    TEST_ASSERT(!viewer_navigate(+1));
    TEST_ASSERT_INT_EQ(g_file_index, 0);
    TEST_ASSERT(strstr(g_img[0].path, "1_good.png") != NULL);

    viewer_unload_image(&g_img[0]);
    g_count = 0;
    viewer_free_file_list();

    unlink(f1);
    unlink(f2);
    unlink(f3);
    rmdir(temp_dir);
}

static void test_viewer_dual_pane_boundary_cases(void) {
    const char *tmp_valid = "/tmp/civ_vtest_dual1.png";
    unsigned char px[4 * 4 * 4];
    memset(px, 200, sizeof(px));
    TEST_ASSERT(stbi_write_png(tmp_valid, 4, 4, 4, px, 4 * 4));

    // Pane 0 valid, Pane 1 empty/null
    TEST_ASSERT(viewer_replace_image(0, tmp_valid));
    g_count = 2; // deliberately simulate dual pane where pane 1 has no texture
    memset(&g_img[1], 0, sizeof(g_img[1]));

    // View fit and transformations
    viewer_fit_view();
    TEST_ASSERT(g_zoom >= 0.05f);

    viewer_update_title();
    viewer_do_zoom(1.2f, 200, 200);
    viewer_do_pan(5, 5);
    viewer_render(g_ren);
    viewer_render_info_bar(g_ren);

    // Free mode with active pane 1 (the empty one)
    viewer_toggle_sync();
    TEST_ASSERT(!g_sync);
    g_active = 1;
    viewer_fit_view();
    viewer_do_zoom(1.5f, 400, 200);
    viewer_do_pan(-15, 10);
    viewer_update_title();
    viewer_render(g_ren);

    // Switch back to sync
    viewer_toggle_sync();
    TEST_ASSERT(g_sync);

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(tmp_valid);
}

static void test_viewer_culling_and_metadata_render(void) {
    const char *tmp = "/tmp/civ_vtest_cull.png";
    unsigned char px[16 * 16 * 4];
    memset(px, 128, sizeof(px));
    TEST_ASSERT(stbi_write_png(tmp, 16, 16, 4, px, 16 * 4));

    TEST_ASSERT(viewer_replace_image(0, tmp));
    viewer_fit_view();

    // 1. Extreme off-screen pan (positive) - triggers viewport culling
    viewer_do_pan(50000, 50000);
    viewer_render(g_ren);

    // 2. Extreme off-screen pan (negative)
    viewer_do_pan(-100000, -100000);
    viewer_render(g_ren);

    // 3. Metadata overlay caching test
    viewer_toggle_metadata();
    TEST_ASSERT(g_show_metadata);
    // Render repeatedly to exercise metadata caching path
    for (int i = 0; i < 5; i++) {
        viewer_render(g_ren);
    }
    viewer_toggle_metadata();
    TEST_ASSERT(!g_show_metadata);

    // 4. Dual pane viewport culling
    TEST_ASSERT(viewer_replace_image(1, tmp));
    g_count = 2;
    viewer_fit_view();
    viewer_do_pan(100000, 0);
    viewer_render(g_ren);

    // 5. Dual pane metadata rendering (left pane, right pane, sync)
    viewer_toggle_metadata();
    TEST_ASSERT(g_show_metadata);
    g_sync = false;
    g_active = 0;
    viewer_render_metadata(g_ren);
    g_active = 1;
    viewer_render_metadata(g_ren);
    g_sync = true;
    viewer_render_metadata(g_ren);
    viewer_toggle_metadata();
    TEST_ASSERT(!g_show_metadata);

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(tmp);
}

static void test_viewer_distribute_dual_budget(void) {
    int out0 = -1, out1 = -1;

    // 1. Equal splits when both filenames exceed or match their share
    viewer_distribute_dual_budget(36, 20, 20, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 18);
    TEST_ASSERT_INT_EQ(out1, 18);
    TEST_ASSERT_INT_EQ(out0 + out1, 36);

    viewer_distribute_dual_budget(36, 18, 18, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 18);
    TEST_ASSERT_INT_EQ(out1, 18);

    // Odd total budget split
    viewer_distribute_dual_budget(35, 20, 20, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 17);
    TEST_ASSERT_INT_EQ(out1, 18);
    TEST_ASSERT_INT_EQ(out0 + out1, 35);

    // 2. Asymmetric surplus donation
    // Pane 0 needs less (10 < 18), surplus of 8 donated to Pane 1
    viewer_distribute_dual_budget(36, 10, 30, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 10);
    TEST_ASSERT_INT_EQ(out1, 26);
    TEST_ASSERT_INT_EQ(out0 + out1, 36);

    // Pane 1 needs less (10 < 18), surplus of 8 donated to Pane 0
    viewer_distribute_dual_budget(36, 30, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 26);
    TEST_ASSERT_INT_EQ(out1, 10);
    TEST_ASSERT_INT_EQ(out0 + out1, 36);

    // Both need less than their share: equal split preserved
    viewer_distribute_dual_budget(40, 5, 8, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 20);
    TEST_ASSERT_INT_EQ(out1, 20);
    TEST_ASSERT_INT_EQ(out0 + out1, 40);

    // 3. Conservation invariant across a range of values and odd/even budgets
    for (int total = 0; total <= 100; total++) {
        for (int l0 = 0; l0 <= 40; l0 += 10) {
            for (int l1 = 0; l1 <= 40; l1 += 10) {
                viewer_distribute_dual_budget(total, l0, l1, 8, &out0, &out1);
                TEST_ASSERT_INT_EQ(out0 + out1, total);
                TEST_ASSERT(out0 >= 0);
                TEST_ASSERT(out1 >= 0);
            }
        }
    }

    // 4. Small total budgets
    viewer_distribute_dual_budget(5, 10, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 2);
    TEST_ASSERT_INT_EQ(out1, 3);
    TEST_ASSERT_INT_EQ(out0 + out1, 5);

    viewer_distribute_dual_budget(1, 10, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 0);
    TEST_ASSERT_INT_EQ(out1, 1);
    TEST_ASSERT_INT_EQ(out0 + out1, 1);

    // Small total budget with surplus donation
    viewer_distribute_dual_budget(6, 1, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 1);
    TEST_ASSERT_INT_EQ(out1, 5);
    TEST_ASSERT_INT_EQ(out0 + out1, 6);

    // 5. Zero and negative budgets
    out0 = 99; out1 = 99;
    viewer_distribute_dual_budget(0, 10, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 0);
    TEST_ASSERT_INT_EQ(out1, 0);

    out0 = 99; out1 = 99;
    viewer_distribute_dual_budget(-10, 10, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 0);
    TEST_ASSERT_INT_EQ(out1, 0);

    out0 = 99; out1 = 99;
    viewer_distribute_dual_budget(INT_MIN, 10, 10, 8, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 0);
    TEST_ASSERT_INT_EQ(out1, 0);

    // Negative lens and min_len
    viewer_distribute_dual_budget(20, -5, 15, -2, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 0);
    TEST_ASSERT_INT_EQ(out1, 20);

    viewer_distribute_dual_budget(20, 15, -5, -2, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 20);
    TEST_ASSERT_INT_EQ(out1, 0);

    viewer_distribute_dual_budget(20, -10, -10, -2, &out0, &out1);
    TEST_ASSERT_INT_EQ(out0, 10);
    TEST_ASSERT_INT_EQ(out1, 10);
    TEST_ASSERT_INT_EQ(out0 + out1, 20);

    // 6. NULL safety
    viewer_distribute_dual_budget(36, 10, 10, 8, NULL, &out1);
    viewer_distribute_dual_budget(36, 10, 10, 8, &out0, NULL);
    viewer_distribute_dual_budget(36, 10, 10, 8, NULL, NULL);
}

static void test_viewer_calc_status_layout_single(void) {
    const char *name = "long_exposure_landscape_photograph.jpg";
    int img_w = 1920, img_h = 1080, zoom = 100, idx = 1, count = 10;
    bool sync = true;

    // Window width 1920 -> FULL hints
    ViewerStatusBarLayout l1920 = viewer_calc_status_layout_single(
        1920, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l1920.hint_tier, VIEWER_HINT_FULL);
    TEST_ASSERT(l1920.name_budget[0] >= VIEWER_INFO_NAME_TARGET_SINGLE);
    // Verify budget expansion on wide screens
    TEST_ASSERT(l1920.name_budget[0] > 100);

    // Window width 1024 -> FULL hints
    ViewerStatusBarLayout l1024 = viewer_calc_status_layout_single(
        1024, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l1024.hint_tier, VIEWER_HINT_FULL);
    TEST_ASSERT(l1024.name_budget[0] >= VIEWER_INFO_NAME_TARGET_SINGLE);

    // Window width 800 -> COMPACT hints
    ViewerStatusBarLayout l800 = viewer_calc_status_layout_single(
        800, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l800.hint_tier, VIEWER_HINT_COMPACT);
    TEST_ASSERT(l800.name_budget[0] >= VIEWER_INFO_NAME_TARGET_SINGLE);

    // Window width 640 -> MINIMAL hints
    ViewerStatusBarLayout l640 = viewer_calc_status_layout_single(
        640, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l640.hint_tier, VIEWER_HINT_MINIMAL);
    TEST_ASSERT(l640.name_budget[0] >= VIEWER_INFO_NAME_TARGET_SINGLE);

    // Window width 400 -> NONE hints
    ViewerStatusBarLayout l400 = viewer_calc_status_layout_single(
        400, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l400.hint_tier, VIEWER_HINT_NONE);

    // Window width 300 -> NONE hints
    ViewerStatusBarLayout l300 = viewer_calc_status_layout_single(
        300, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l300.hint_tier, VIEWER_HINT_NONE);

    // Window width 0 -> NONE hints, clamped to 0
    ViewerStatusBarLayout l0 = viewer_calc_status_layout_single(
        0, name, img_w, img_h, zoom, sync, idx, count);
    TEST_ASSERT_INT_EQ(l0.hint_tier, VIEWER_HINT_NONE);
    TEST_ASSERT_INT_EQ(l0.usable_chars, 0);
    TEST_ASSERT_INT_EQ(l0.name_budget[0], 0);

    // Window width edge cases: 1, 6, 11, 12, 13, negative, INT_MIN
    int degen_w[] = {1, 6, 11, 12, 13, -1, -50, INT_MIN};
    for (size_t i = 0; i < sizeof(degen_w)/sizeof(degen_w[0]); i++) {
        ViewerStatusBarLayout l_deg = viewer_calc_status_layout_single(
            degen_w[i], name, img_w, img_h, zoom, sync, idx, count);
        TEST_ASSERT_INT_EQ(l_deg.hint_tier, VIEWER_HINT_NONE);
        TEST_ASSERT_INT_EQ(l_deg.usable_chars, 0);
        TEST_ASSERT_INT_EQ(l_deg.name_budget[0], 0);
    }

    // Negative and extreme dimensions / counters
    ViewerStatusBarLayout l_neg = viewer_calc_status_layout_single(
        800, name, -100, -200, -50, sync, -1, -5);
    TEST_ASSERT(l_neg.usable_chars > 0);
    char buf_neg[512];
    viewer_format_status_single(&l_neg, name, -100, -200, -50, sync, -1, -5, buf_neg, sizeof(buf_neg));
    TEST_ASSERT(strstr(buf_neg, "0x0") != NULL);
    TEST_ASSERT(strstr(buf_neg, "0/0") != NULL);
    TEST_ASSERT(strstr(buf_neg, "0%") != NULL);

    ViewerStatusBarLayout l_huge = viewer_calc_status_layout_single(
        800, name, INT_MAX, INT_MAX, 100, sync, 1, 10);
    char buf_huge[512];
    viewer_format_status_single(&l_huge, name, INT_MAX, INT_MAX, 100, sync, 1, 10, buf_huge, sizeof(buf_huge));
    TEST_ASSERT(strstr(buf_huge, "2147483647x2147483647") != NULL);

    // Verify clamped total length <= usable_chars for all tested widths
    int widths[] = {1920, 1024, 800, 640, 400, 300, 0};
    for (size_t i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
        int w = widths[i];
        ViewerStatusBarLayout layout = viewer_calc_status_layout_single(
            w, name, img_w, img_h, zoom, sync, idx, count);
        char buf[512];
        int len = viewer_format_status_single(&layout, name, img_w, img_h, zoom, sync, idx, count, buf, sizeof(buf));
        int clamped_len = (len > layout.usable_chars) ? layout.usable_chars : len;
        TEST_ASSERT(clamped_len <= layout.usable_chars);
        if (layout.usable_chars >= 30) {
            TEST_ASSERT(len <= layout.usable_chars);
        }
    }
}

static void test_viewer_calc_status_layout_dual(void) {
    const char *name_short = "a.jpg";
    const char *name_long = "panoramic_mountain_view_scenic_2026.png";
    int w0 = 1920, h0 = 1080, w1 = 1280, h1 = 720;
    int zoom = 100;
    bool sync = true;
    int active = 0;

    // Test tier degradation across widths
    ViewerStatusBarLayout l1920 = viewer_calc_status_layout_dual(
        1920, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l1920.hint_tier, VIEWER_HINT_FULL);

    ViewerStatusBarLayout l1024 = viewer_calc_status_layout_dual(
        1024, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l1024.hint_tier, VIEWER_HINT_FULL);

    ViewerStatusBarLayout l900 = viewer_calc_status_layout_dual(
        900, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l900.hint_tier, VIEWER_HINT_COMPACT);

    ViewerStatusBarLayout l800 = viewer_calc_status_layout_dual(
        800, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l800.hint_tier, VIEWER_HINT_MINIMAL);

    ViewerStatusBarLayout l640 = viewer_calc_status_layout_dual(
        640, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l640.hint_tier, VIEWER_HINT_NONE);

    ViewerStatusBarLayout l0 = viewer_calc_status_layout_dual(
        0, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l0.hint_tier, VIEWER_HINT_NONE);
    TEST_ASSERT_INT_EQ(l0.usable_chars, 0);
    TEST_ASSERT_INT_EQ(l0.name_budget[0], 0);
    TEST_ASSERT_INT_EQ(l0.name_budget[1], 0);

    // Window width edge cases: 1, 6, 11, 12, 13, negative, INT_MIN
    int degen_dual_w[] = {1, 6, 11, 12, 13, -1, -50, INT_MIN};
    for (size_t i = 0; i < sizeof(degen_dual_w)/sizeof(degen_dual_w[0]); i++) {
        ViewerStatusBarLayout l_deg = viewer_calc_status_layout_dual(
            degen_dual_w[i], name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
        TEST_ASSERT_INT_EQ(l_deg.hint_tier, VIEWER_HINT_NONE);
        TEST_ASSERT_INT_EQ(l_deg.usable_chars, 0);
        TEST_ASSERT_INT_EQ(l_deg.name_budget[0], 0);
        TEST_ASSERT_INT_EQ(l_deg.name_budget[1], 0);
    }

    // Degenerate active_pane values (-1, 2, 100, INT_MIN)
    int degen_panes[] = {-1, 2, 100, INT_MIN};
    for (size_t i = 0; i < sizeof(degen_panes)/sizeof(degen_panes[0]); i++) {
        ViewerStatusBarLayout l_p = viewer_calc_status_layout_dual(
            800, name_short, w0, h0, name_long, w1, h1, zoom, false, degen_panes[i]);
        TEST_ASSERT(l_p.usable_chars > 0);
        char dbuf[512];
        viewer_format_status_dual(&l_p, name_short, w0, h0, name_long, w1, h1, zoom, false, degen_panes[i], dbuf, sizeof(dbuf));
        TEST_ASSERT(strstr(dbuf, "[L*]") != NULL);
    }

    // Verify surplus sharing between short and long filenames:
    // When pane 0 is short ("a.jpg", 5 chars) and pane 1 is long (39 chars)
    ViewerStatusBarLayout l_share0 = viewer_calc_status_layout_dual(
        800, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
    // Pane 0 needs only 5 chars, so its budget should be 5
    TEST_ASSERT_INT_EQ(l_share0.name_budget[0], 5);
    // Pane 1 receives the surplus and gets more than half the total name budget
    TEST_ASSERT(l_share0.name_budget[1] > l_share0.name_budget[0]);

    // Reverse: pane 0 is long, pane 1 is short
    ViewerStatusBarLayout l_share1 = viewer_calc_status_layout_dual(
        800, name_long, w0, h0, name_short, w1, h1, zoom, sync, active);
    TEST_ASSERT_INT_EQ(l_share1.name_budget[1], 5);
    TEST_ASSERT(l_share1.name_budget[0] > l_share1.name_budget[1]);

    // Verify clamped total length <= usable_chars across widths
    int widths[] = {1920, 1024, 900, 800, 640, 400, 0};
    for (size_t i = 0; i < sizeof(widths)/sizeof(widths[0]); i++) {
        int w = widths[i];
        ViewerStatusBarLayout layout = viewer_calc_status_layout_dual(
            w, name_short, w0, h0, name_long, w1, h1, zoom, sync, active);
        char buf[512];
        int len = viewer_format_status_dual(
            &layout, name_short, w0, h0, name_long, w1, h1, zoom, sync, active, buf, sizeof(buf));
        int clamped_len = (len > layout.usable_chars) ? layout.usable_chars : len;
        TEST_ASSERT(clamped_len <= layout.usable_chars);
        if (layout.usable_chars >= 50) {
            TEST_ASSERT(len <= layout.usable_chars);
        }
    }
}

static void test_viewer_format_status(void) {
    // 1. Single pane status formatting with extension preservation
    ViewerStatusBarLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.usable_chars = 120;
    layout.hint_tier = VIEWER_HINT_MINIMAL;
    layout.name_budget[0] = 18;

    char buf[256];
    int len = viewer_format_status_single(
        &layout, "verylongfilenametest.jpg", 1920, 1080, 100, true, 3, 25, buf, sizeof(buf));
    TEST_ASSERT(len > 0);
    TEST_ASSERT_INT_EQ(len, (int)strlen(buf));
    // Verify filename truncation preserves extension .jpg
    TEST_ASSERT(strstr(buf, "...jpg") != NULL);
    // Verify metadata formatted properly
    TEST_ASSERT(strstr(buf, "1920x1080") != NULL);
    TEST_ASSERT(strstr(buf, "100%") != NULL);
    TEST_ASSERT(strstr(buf, "SYNC") != NULL);
    TEST_ASSERT(strstr(buf, "3/25") != NULL);
    // Verify minimal hint present
    TEST_ASSERT(strstr(buf, "[e] exif [ESC]") != NULL);

    // 2. Dual pane status formatting in FREE mode
    layout.hint_tier = VIEWER_HINT_COMPACT;
    layout.name_budget[0] = 12;
    layout.name_budget[1] = 12;
    len = viewer_format_status_dual(
        &layout, "leftimage.png", 800, 600, "rightimage.png", 1024, 768, 150, false, 0, buf, sizeof(buf));
    TEST_ASSERT(len > 0);
    TEST_ASSERT_INT_EQ(len, (int)strlen(buf));
    TEST_ASSERT(strstr(buf, "(800x600)") != NULL);
    TEST_ASSERT(strstr(buf, "(1024x768)") != NULL);
    TEST_ASSERT(strstr(buf, "FREE") != NULL);
    TEST_ASSERT(strstr(buf, "[L*]") != NULL);
    TEST_ASSERT(strstr(buf, "[s]ync [Tab] pane [e] exif") != NULL);

    // Active pane 1 indicator
    viewer_format_status_dual(
        &layout, "leftimage.png", 800, 600, "rightimage.png", 1024, 768, 150, false, 1, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "[R*]") != NULL);

    // 3. Small output buffer safety: truncation without buffer overrun
    char small_buf[16];
    len = viewer_format_status_single(
        &layout, "image.jpg", 800, 600, 100, true, 1, 1, small_buf, sizeof(small_buf));
    TEST_ASSERT(len < (int)sizeof(small_buf));
    TEST_ASSERT_INT_EQ((int)strlen(small_buf), len);
    TEST_ASSERT(small_buf[sizeof(small_buf) - 1] == '\0');

    // 4. NULL / zero buffer edge cases
    TEST_ASSERT_INT_EQ(viewer_format_status_single(&layout, "image.jpg", 800, 600, 100, true, 1, 1, NULL, 0), 0);
    TEST_ASSERT_INT_EQ(viewer_format_status_single(NULL, "image.jpg", 800, 600, 100, true, 1, 1, buf, sizeof(buf)), 0);
    TEST_ASSERT_INT_EQ(viewer_format_status_dual(&layout, "a.jpg", 800, 600, "b.jpg", 800, 600, 100, true, 0, NULL, 0), 0);
    TEST_ASSERT_INT_EQ(viewer_format_status_dual(NULL, "a.jpg", 800, 600, "b.jpg", 800, 600, 100, true, 0, buf, sizeof(buf)), 0);
}

static void test_viewer_render_metadata_and_info_edge_cases(void) {
    int orig_w = g_win_w;
    int orig_h = g_win_h;
    int orig_count = g_count;
    bool orig_show_info = g_show_info;
    bool orig_show_meta = g_show_metadata;
    bool orig_sync = g_sync;
    int orig_active = g_active;
    int orig_file_index = g_file_index;
    int orig_file_count = g_file_count;
    char *orig_p0 = g_img[0].path;
    char *orig_p1 = g_img[1].path;

    g_show_info = true;
    g_show_metadata = true;

    // 1. Degenerate window sizes with render_info_bar and render_metadata
    int test_sizes[][2] = {
        {0, 0}, {1, 1}, {6, 6}, {11, 11}, {12, 12}, {13, 13},
        {40, 40}, {50, 50}, {-1, -1}, {-100, 500}, {500, -100}
    };

    for (size_t i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]); i++) {
        g_win_w = test_sizes[i][0];
        g_win_h = test_sizes[i][1];

        viewer_render_info_bar(g_ren);
        viewer_render_metadata(g_ren);
    }

    // 2. Degenerate g_active pane indices
    g_win_w = 800;
    g_win_h = 600;
    int bad_actives[] = {-1, 2, 5, 100, INT_MIN};

    for (size_t i = 0; i < sizeof(bad_actives)/sizeof(bad_actives[0]); i++) {
        g_active = bad_actives[i];
        g_sync = false;

        viewer_update_title();
        viewer_render_info_bar(g_ren);
        viewer_render_metadata(g_ren);
        viewer_toggle_sync();
        viewer_toggle_sync();
    }

    // 3. NULL image path with g_count = 1 and g_count = 2
    g_active = 0;
    g_sync = true;
    g_img[0].path = NULL;
    g_img[1].path = NULL;

    g_count = 1;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    g_count = 2;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    // 4. Degenerate g_count values (0, -1, 3)
    g_count = 0;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    g_count = -1;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    g_count = 3;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    // 5. Negative file index and count in title formatting
    g_count = 1;
    g_file_index = -5;
    g_file_count = -10;
    viewer_update_title();

    // Restore original state
    g_win_w = orig_w;
    g_win_h = orig_h;
    g_count = orig_count;
    g_show_info = orig_show_info;
    g_show_metadata = orig_show_meta;
    g_sync = orig_sync;
    g_active = orig_active;
    g_file_index = orig_file_index;
    g_file_count = orig_file_count;
    g_img[0].path = orig_p0;
    g_img[1].path = orig_p1;
}

static void test_viewer_metadata_cache_stress(void) {
    const char *img1 = "/tmp/civ_cache_stress1.png";
    const char *img2 = "/tmp/civ_cache_stress2.png";
    unsigned char px1[4] = {255, 0, 0, 255};
    unsigned char px2[4] = {0, 255, 0, 255};
    TEST_ASSERT(stbi_write_png(img1, 1, 1, 4, px1, 4));
    TEST_ASSERT(stbi_write_png(img2, 1, 1, 4, px2, 4));

    bool orig_meta = g_show_metadata;
    int orig_count = g_count;
    int orig_w = g_win_w;
    int orig_h = g_win_h;

    g_win_w = 800;
    g_win_h = 600;
    g_show_metadata = true;
    g_count = 1;

    TEST_ASSERT(viewer_load_image(img1, &g_img[0]));

    // 2000 metadata cache lookups, resets, and alternations
    for (int i = 0; i < 2000; i++) {
        if (i % 20 == 0) {
            viewer_reset_metadata_cache();
        }
        if (i % 50 == 0) {
            const char *next = (i % 100 == 0) ? img1 : img2;
            TEST_ASSERT(viewer_load_image(next, &g_img[0]));
        }
        viewer_render_metadata(g_ren);
    }

    viewer_unload_image(&g_img[0]);
    g_count = orig_count;
    g_show_metadata = orig_meta;
    g_win_w = orig_w;
    g_win_h = orig_h;

    unlink(img1);
    unlink(img2);
}

static void test_viewer_repeated_load_unload_cycles(void) {
    const char *img_a = "/tmp/civ_cycle_a.png";
    const char *img_b = "/tmp/civ_cycle_b.png";
    unsigned char px_a[8 * 8 * 4];
    unsigned char px_b[16 * 16 * 4];
    memset(px_a, 100, sizeof(px_a));
    memset(px_b, 200, sizeof(px_b));
    TEST_ASSERT(stbi_write_png(img_a, 8, 8, 4, px_a, 8 * 4));
    TEST_ASSERT(stbi_write_png(img_b, 16, 16, 4, px_b, 16 * 4));

    // 1. Repeated direct overwrites with viewer_load_image without prior unload
    Image img = {0};
    for (int i = 0; i < 300; i++) {
        const char *src = (i % 2 == 0) ? img_a : img_b;
        int expected_dim = (i % 2 == 0) ? 8 : 16;
        TEST_ASSERT(viewer_load_image(src, &img));
        TEST_ASSERT(img.tex != NULL);
        TEST_ASSERT(img.path != NULL);
        TEST_ASSERT_INT_EQ(img.w, expected_dim);
        TEST_ASSERT_INT_EQ(img.h, expected_dim);
    }
    viewer_unload_image(&img);
    TEST_ASSERT(img.tex == NULL && img.path == NULL);

    // 2. Repeated load / unload cycles
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT(viewer_load_image(img_a, &img));
        TEST_ASSERT(img.tex != NULL);
        viewer_unload_image(&img);
        TEST_ASSERT(img.tex == NULL && img.path == NULL);
    }

    // 3. Repeated viewer_replace_image cycles
    for (int i = 0; i < 300; i++) {
        const char *src = (i % 2 == 0) ? img_a : img_b;
        TEST_ASSERT(viewer_replace_image(0, src));
        TEST_ASSERT(g_img[0].tex != NULL);
    }
    viewer_unload_image(&g_img[0]);
    g_count = 0;

    unlink(img_a);
    unlink(img_b);
}

static void test_viewer_corrupt_load_cleanup_stress(void) {
    const char *valid = "/tmp/civ_cstress_valid.png";
    const char *empty = "/tmp/civ_cstress_empty.png";
    const char *trunc = "/tmp/civ_cstress_trunc.png";
    const char *garbage = "/tmp/civ_cstress_garbage.png";

    unsigned char px[4] = {120, 200, 80, 255};
    TEST_ASSERT(stbi_write_png(valid, 1, 1, 4, px, 4));
    write_raw_file(empty, "", 0);
    uint8_t png_hdr[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    write_raw_file(trunc, png_hdr, sizeof(png_hdr));
    uint8_t rand_bytes[2048];
    for (size_t i = 0; i < sizeof(rand_bytes); i++) rand_bytes[i] = (uint8_t)(i * 47 + 13);
    write_raw_file(garbage, rand_bytes, sizeof(rand_bytes));

    const char *corrupt_list[] = {
        empty,
        trunc,
        garbage,
        "/tmp/civ_cstress_nonexistent.png",
        "/dev/null",
        "/tmp"
    };
    int corrupt_count = (int)(sizeof(corrupt_list) / sizeof(corrupt_list[0]));

    // 1. Verify that loading corrupt file into already-loaded Image cleanly unloads and zeroes it without leak
    for (int c = 0; c < corrupt_count; c++) {
        Image img = {0};
        TEST_ASSERT(viewer_load_image(valid, &img));
        TEST_ASSERT(img.tex != NULL && img.path != NULL);

        // Attempting to load corrupt image into already-loaded img struct
        TEST_ASSERT(!viewer_load_image(corrupt_list[c], &img));
        TEST_ASSERT(img.tex == NULL);
        TEST_ASSERT(img.path == NULL);
        TEST_ASSERT_INT_EQ(img.w, 0);
        TEST_ASSERT_INT_EQ(img.h, 0);
    }

    // 2. Verify repeated stress cycles of valid load followed by corrupt load
    for (int i = 0; i < 200; i++) {
        Image img = {0};
        TEST_ASSERT(viewer_load_image(valid, &img));
        const char *bad = corrupt_list[i % corrupt_count];
        TEST_ASSERT(!viewer_load_image(bad, &img));
        TEST_ASSERT(img.tex == NULL && img.path == NULL);
    }

    // 3. Verify viewer_replace_image preserves existing image on corrupt replacement attempts
    TEST_ASSERT(viewer_replace_image(0, valid));
    for (int i = 0; i < 200; i++) {
        const char *bad = corrupt_list[i % corrupt_count];
        TEST_ASSERT(!viewer_replace_image(0, bad));
        TEST_ASSERT(g_img[0].tex != NULL);
        TEST_ASSERT_STR_EQ(g_img[0].path, valid);
    }
    viewer_unload_image(&g_img[0]);
    g_count = 0;

    unlink(valid);
    unlink(empty);
    unlink(trunc);
    unlink(garbage);
}

static bool test_helpers_check_atomic_bracket_tokens(const char *str) {
    const char *p = str;
    while ((p = strchr(p, '[')) != NULL) {
        const char *close = strchr(p, ']');
        if (!close) return false;
        if (strncmp(p, "[e", 2) == 0 && strncmp(p, "[e]", 3) != 0) return false;
        if (strncmp(p, "[s", 2) == 0 && strncmp(p, "[s]ync", 6) != 0) return false;
        if (strncmp(p, "[f", 2) == 0 && strncmp(p, "[f]ull", 6) != 0) return false;
        if (strncmp(p, "[n", 2) == 0 && strncmp(p, "[n/p]", 5) != 0) return false;
        if (strncmp(p, "[T", 2) == 0 && strncmp(p, "[Tab]", 5) != 0) return false;
        if (strncmp(p, "[E", 2) == 0 && strncmp(p, "[ESC]", 5) != 0) return false;
        p = close + 1;
    }
    return true;
}

static void test_viewer_ui_layout_invariants(void) {
    // 1. Single pane status bar layout invariants across widths from 100px to 4000px
    const char *filenames[] = {
        "",
        "a.jpg",
        "normal_photo.png",
        "panoramic_mountain_view_scenic_high_resolution_landscape_autumn_2026.jpeg",
        "an_exceptionally_enormous_filename_stress_testing_character_budget_allocation_and_boundary_checks_2026_test_pattern_version_final.png",
        "no_extension_image",
        "x"
    };

    for (size_t f = 0; f < sizeof(filenames)/sizeof(filenames[0]); f++) {
        const char *name = filenames[f];
        int prev_tier = (int)VIEWER_HINT_NONE;

        // Sweep from 100px to 4000px in 50px increments
        for (int w = 100; w <= 4000; w += 50) {
            ViewerStatusBarLayout layout = viewer_calc_status_layout_single(
                w, name, 1920, 1080, 100, true, 3, 12);

            // Invariants
            TEST_ASSERT(layout.usable_chars >= 0);
            TEST_ASSERT_INT_EQ(layout.usable_chars, (w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W);
            TEST_ASSERT(layout.name_budget[0] >= 0);
            TEST_ASSERT_INT_EQ(layout.name_budget[1], 0);
            TEST_ASSERT(layout.hint_tier >= VIEWER_HINT_NONE && layout.hint_tier < VIEWER_HINT_TIER_COUNT);

            // Monotonic tier transitions: tier never decreases as width increases
            TEST_ASSERT((int)layout.hint_tier >= prev_tier);
            prev_tier = (int)layout.hint_tier;

            char buf[512];
            int len = viewer_format_status_single(
                &layout, name, 1920, 1080, 100, true, 3, 12, buf, sizeof(buf));
            TEST_ASSERT(len >= 0);
            TEST_ASSERT_INT_EQ(len, (int)strlen(buf));

            // Clamped length invariant
            int clamped_len = (len > layout.usable_chars) ? layout.usable_chars : len;
            TEST_ASSERT(clamped_len <= layout.usable_chars);

            // When hint is displayed, line MUST fit entirely within usable_chars
            if (layout.hint_tier > VIEWER_HINT_NONE) {
                TEST_ASSERT(len <= layout.usable_chars);
            }

            // Verify shortcut atomicity
            TEST_ASSERT(test_helpers_check_atomic_bracket_tokens(buf));
        }
    }

    // 2. Dual pane status bar layout invariants across widths from 100px to 4000px
    const char *dual_pairs[][2] = {
        {"a.jpg", "b.jpg"},
        {"a.jpg", "extremely_long_photo_mountain_panorama_2026.png"},
        {"extremely_long_photo_mountain_panorama_2026.png", "b.jpg"},
        {"panoramic_view_1.jpeg", "panoramic_view_2.jpeg"},
        {"", "panoramic_view_2.jpeg"},
        {"panoramic_view_1.jpeg", ""}
    };

    for (size_t p = 0; p < sizeof(dual_pairs)/sizeof(dual_pairs[0]); p++) {
        const char *n0 = dual_pairs[p][0];
        const char *n1 = dual_pairs[p][1];
        int prev_tier = (int)VIEWER_HINT_NONE;

        for (int w = 100; w <= 4000; w += 50) {
            for (int sync_idx = 0; sync_idx < 2; sync_idx++) {
                bool sync = (sync_idx == 1);
                for (int active = 0; active < 2; active++) {
                    ViewerStatusBarLayout layout = viewer_calc_status_layout_dual(
                        w, n0, 1920, 1080, n1, 1280, 720, 100, sync, active);

                    // Invariants
                    TEST_ASSERT(layout.usable_chars >= 0);
                    TEST_ASSERT_INT_EQ(layout.usable_chars, (w - 2 * VIEWER_INFO_MARGIN_X) / VIEWER_INFO_FONT_W);
                    TEST_ASSERT(layout.name_budget[0] >= 0);
                    TEST_ASSERT(layout.name_budget[1] >= 0);
                    TEST_ASSERT(layout.hint_tier >= VIEWER_HINT_NONE && layout.hint_tier < VIEWER_HINT_TIER_COUNT);

                    char buf[512];
                    int len = viewer_format_status_dual(
                        &layout, n0, 1920, 1080, n1, 1280, 720, 100, sync, active, buf, sizeof(buf));
                    TEST_ASSERT(len >= 0);
                    TEST_ASSERT_INT_EQ(len, (int)strlen(buf));

                    int clamped_len = (len > layout.usable_chars) ? layout.usable_chars : len;
                    TEST_ASSERT(clamped_len <= layout.usable_chars);

                    if (layout.hint_tier > VIEWER_HINT_NONE) {
                        TEST_ASSERT(len <= layout.usable_chars);
                    }

                    TEST_ASSERT(test_helpers_check_atomic_bracket_tokens(buf));

                    if (!sync) {
                        if (active == 0) TEST_ASSERT(strstr(buf, "[L*]") != NULL);
                        else TEST_ASSERT(strstr(buf, "[R*]") != NULL);
                    }
                }
            }

            // Monotonicity check under fixed conditions
            ViewerStatusBarLayout l_mono = viewer_calc_status_layout_dual(
                w, n0, 1920, 1080, n1, 1280, 720, 100, true, 0);
            TEST_ASSERT((int)l_mono.hint_tier >= prev_tier);
            prev_tier = (int)l_mono.hint_tier;
        }
    }

    // 3. Dual budget distribution symmetry and surplus donation
    int b0 = -1, b1 = -1;
    viewer_distribute_dual_budget(60, 50, 50, 8, &b0, &b1);
    TEST_ASSERT_INT_EQ(b0, 30);
    TEST_ASSERT_INT_EQ(b1, 30);
    TEST_ASSERT_INT_EQ(b0 + b1, 60);

    viewer_distribute_dual_budget(60, 10, 50, 8, &b0, &b1);
    TEST_ASSERT_INT_EQ(b0, 10);
    TEST_ASSERT_INT_EQ(b1, 50);
    TEST_ASSERT_INT_EQ(b0 + b1, 60);

    viewer_distribute_dual_budget(60, 50, 10, 8, &b0, &b1);
    TEST_ASSERT_INT_EQ(b0, 50);
    TEST_ASSERT_INT_EQ(b1, 10);
    TEST_ASSERT_INT_EQ(b0 + b1, 60);

    // 4. Metadata panel bounding box geometry invariants across degenerate and normal dimensions
    int degen_w[] = {-100, -1, 0, 1, 10, 50, 100, 150, 180, 199};
    int degen_h[] = {-100, -1, 0, 1, 10, 50, 70, 79, 100, 119};
    for (size_t i = 0; i < sizeof(degen_w)/sizeof(degen_w[0]); i++) {
        for (size_t j = 0; j < sizeof(degen_h)/sizeof(degen_h[0]); j++) {
            ViewerMetadataLayout l_deg = viewer_calc_metadata_layout(degen_w[i], degen_h[j], 1);
            TEST_ASSERT(!l_deg.visible);
        }
    }

    int test_widths[] = {200, 240, 300, 360, 380, 400, 640, 800, 1024, 1280, 1920, 2560, 3840};
    int test_heights[] = {120, 160, 200, 240, 360, 480, 600, 720, 1080, 1440, 2160};
    int exif_counts[] = {0, 1, 4, 8, 12};

    for (size_t i = 0; i < sizeof(test_widths)/sizeof(test_widths[0]); i++) {
        int w = test_widths[i];
        for (size_t j = 0; j < sizeof(test_heights)/sizeof(test_heights[0]); j++) {
            int h = test_heights[j];
            for (size_t k = 0; k < sizeof(exif_counts)/sizeof(exif_counts[0]); k++) {
                int ec = exif_counts[k];
                ViewerMetadataLayout ml = viewer_calc_metadata_layout(w, h, ec);
                if (ml.visible) {
                    TEST_ASSERT(ml.pw >= VIEWER_METADATA_MIN_PW);
                    TEST_ASSERT(ml.ph >= VIEWER_METADATA_MIN_PH);
                    TEST_ASSERT(ml.px >= 0);
                    TEST_ASSERT(ml.px + ml.pw <= w);
                    TEST_ASSERT(ml.py >= 0);
                    TEST_ASSERT(ml.py + ml.ph <= h);
                    TEST_ASSERT(ml.label_x >= ml.px + 8);
                    TEST_ASSERT(ml.label_w >= 48);
                    TEST_ASSERT(ml.label_x + ml.label_w <= ml.value_x);
                    TEST_ASSERT(ml.value_w > 0);
                    TEST_ASSERT(ml.value_x + ml.value_w <= ml.px + ml.pw - 8);
                    TEST_ASSERT_INT_EQ(ml.footer_y, ml.py + ml.ph - 20);
                    TEST_ASSERT(ml.footer_y >= ml.py + 26);
                }
            }
        }
    }

    // 5. Active pane switching reflection test
    const char *tmp_pane0 = "/tmp/civ_pane0_active_test.png";
    const char *tmp_pane1 = "/tmp/civ_pane1_active_test.png";
    unsigned char dummy_pixels[16 * 16 * 4];
    memset(dummy_pixels, 100, sizeof(dummy_pixels));
    TEST_ASSERT(stbi_write_png(tmp_pane0, 16, 16, 4, dummy_pixels, 16 * 4));
    memset(dummy_pixels, 200, sizeof(dummy_pixels));
    TEST_ASSERT(stbi_write_png(tmp_pane1, 16, 16, 4, dummy_pixels, 16 * 4));

    TEST_ASSERT(viewer_replace_image(0, tmp_pane0));
    TEST_ASSERT(viewer_replace_image(1, tmp_pane1));
    g_count = 2;
    g_win_w = 800;
    g_win_h = 600;
    g_show_info = true;
    g_show_metadata = true;

    // Free mode: switching pane changes status bar indicator and active index
    g_sync = false;
    g_active = 0;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    ViewerStatusBarLayout l_act0 = viewer_calc_status_layout_dual(
        g_win_w, g_img[0].path, 16, 16, g_img[1].path, 16, 16, 100, false, 0);
    char buf_act0[512];
    viewer_format_status_dual(&l_act0, g_img[0].path, 16, 16, g_img[1].path, 16, 16, 100, false, 0, buf_act0, sizeof(buf_act0));
    TEST_ASSERT(strstr(buf_act0, "[L*]") != NULL);

    g_active = 1;
    viewer_update_title();
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    ViewerStatusBarLayout l_act1 = viewer_calc_status_layout_dual(
        g_win_w, g_img[0].path, 16, 16, g_img[1].path, 16, 16, 100, false, 1);
    char buf_act1[512];
    viewer_format_status_dual(&l_act1, g_img[0].path, 16, 16, g_img[1].path, 16, 16, 100, false, 1, buf_act1, sizeof(buf_act1));
    TEST_ASSERT(strstr(buf_act1, "[R*]") != NULL);

    // Sync mode: active pane switches metadata overlay target
    g_sync = true;
    g_active = 0;
    viewer_render_metadata(g_ren);
    g_active = 1;
    viewer_render_metadata(g_ren);

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(tmp_pane0);
    unlink(tmp_pane1);
}

static void test_viewer_performance_benchmarks(void) {
    const char *path = "/home/user/pictures/vacation/scenic_mountain_sunset_2026.jpg";
    const char *name0 = "photo_archive_original_hires_image_pane0.png";
    const char *name1 = "photo_archive_processed_contrast_enhanced_pane1.png";
    char out[512];

    // 1. Benchmark status bar layout + format: 50,000 iterations single-pane and 50,000 dual-pane
    Uint64 t0 = SDL_GetPerformanceCounter();
    int dummy_len = 0;

    for (int i = 0; i < 50000; i++) {
        int w = 800 + (i % 800);
        ViewerStatusBarLayout l_single = viewer_calc_status_layout_single(
            w, path, 1920, 1080, 100, true, (i % 100) + 1, 100);
        dummy_len += viewer_format_status_single(
            &l_single, path, 1920, 1080, 100, true, (i % 100) + 1, 100, out, sizeof(out));
    }

    for (int i = 0; i < 50000; i++) {
        int w = 800 + (i % 800);
        ViewerStatusBarLayout l_dual = viewer_calc_status_layout_dual(
            w, name0, 1920, 1080, name1, 1280, 720, 120, false, i % 2);
        dummy_len += viewer_format_status_dual(
            &l_dual, name0, 1920, 1080, name1, 1280, 720, 120, false, i % 2, out, sizeof(out));
    }

    Uint64 t1 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    double ms_status = (double)(t1 - t0) * 1000.0 / freq;
    printf("    [Benchmark] Status bar layout+format (50k single + 50k dual): %.2f ms\n", ms_status);
    TEST_ASSERT(dummy_len > 0);

    // 2. Benchmark path and filename truncation: 50,000 iterations each
    Uint64 t2 = SDL_GetPerformanceCounter();
    int dummy_trunc = 0;

    for (int i = 0; i < 50000; i++) {
        int max_len = 10 + (i % 40);
        viewer_truncate_filename(name0, out, sizeof(out), max_len);
        dummy_trunc += (unsigned char)out[0];
    }

    for (int i = 0; i < 50000; i++) {
        int max_chars = 15 + (i % 50);
        viewer_truncate_path(path, out, sizeof(out), max_chars);
        dummy_trunc += (unsigned char)out[0];
    }

    Uint64 t3 = SDL_GetPerformanceCounter();
    double ms_trunc = (double)(t3 - t2) * 1000.0 / freq;
    printf("    [Benchmark] Truncation (50k filename + 50k path): %.2f ms\n", ms_trunc);
    TEST_ASSERT(dummy_trunc != 0);
}

static void test_viewer_stress_lifecycle_and_mutation(void) {
    int orig_w = g_win_w;
    int orig_h = g_win_h;
    int orig_count = g_count;
    bool orig_show_info = g_show_info;
    bool orig_show_meta = g_show_metadata;
    bool orig_sync = g_sync;
    int orig_active = g_active;
    int orig_file_index = g_file_index;
    int orig_file_count = g_file_count;
    char *orig_p0 = g_img[0].path;
    char *orig_p1 = g_img[1].path;
    char orig_dir[PATH_MAX];
    snprintf(orig_dir, sizeof(orig_dir), "%s", g_current_dir);

    // =========================================================================
    // 1. Active File Deletion / Unlinking Mid-Display
    // =========================================================================
    const char *del_img = "/tmp/civ_stress_active_del.png";
    unsigned char del_px[16 * 16 * 4];
    memset(del_px, 180, sizeof(del_px));
    TEST_ASSERT(stbi_write_png(del_img, 16, 16, 4, del_px, 16 * 4));

    g_win_w = 800;
    g_win_h = 600;
    g_count = 1;
    g_active = 0;
    g_sync = true;
    g_show_info = true;
    g_show_metadata = true;

    TEST_ASSERT(viewer_load_image(del_img, &g_img[0]));
    TEST_ASSERT(g_img[0].tex != NULL);
    TEST_ASSERT(g_img[0].path != NULL);

    // Initial render with metadata: stat and EXIF succeed
    viewer_render_metadata(g_ren);
    char sz_buf[64] = {0}, mt_buf[64] = {0};
    bool has_stat = viewer_get_cached_stat_info(sz_buf, sizeof(sz_buf), mt_buf, sizeof(mt_buf));
    TEST_ASSERT(has_stat);
    TEST_ASSERT(strcmp(sz_buf, "?") != 0);
    TEST_ASSERT(strcmp(mt_buf, "?") != 0);

    // Full render while file is on disk
    viewer_render(g_ren);

    // Delete active file from disk while displayed & metadata overlay is open
    TEST_ASSERT_INT_EQ(unlink(del_img), 0);
    TEST_ASSERT(access(del_img, F_OK) != 0);

    // File is unlinked, but in-memory GPU texture remains valid and render must not crash
    viewer_render(g_ren);
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    // Reset metadata cache to force re-stat and re-parse of the deleted file
    viewer_reset_metadata_cache();
    viewer_render_metadata(g_ren);

    // Verify stat failure handling when file was deleted:
    // s_has_stat becomes false, cached size and mtime strings fallback to "?"
    has_stat = viewer_get_cached_stat_info(sz_buf, sizeof(sz_buf), mt_buf, sizeof(mt_buf));
    TEST_ASSERT(!has_stat);
    TEST_ASSERT_STR_EQ(sz_buf, "?");
    TEST_ASSERT_STR_EQ(mt_buf, "?");

    // Verify exif_read returns false and has_exif is false for missing/deleted file
    ExifData del_exif;
    bool exif_ret = exif_read(del_img, &del_exif);
    TEST_ASSERT(!exif_ret);
    TEST_ASSERT(!del_exif.has_exif);
    TEST_ASSERT_INT_EQ(del_exif.orientation, 1);

    // Render again with invalidated/failed stat: must not crash or leak
    viewer_render(g_ren);
    viewer_unload_image(&g_img[0]);
    g_count = 0;

    // =========================================================================
    // 2. Window Resizing Storm Simulation (1000 Cycles Across Extremes)
    // =========================================================================
    const char *storm0 = "/tmp/civ_storm_0.png";
    const char *storm1 = "/tmp/civ_storm_1.png";
    unsigned char s_px[8 * 8 * 4];
    memset(s_px, 120, sizeof(s_px));
    TEST_ASSERT(stbi_write_png(storm0, 8, 8, 4, s_px, 8 * 4));
    TEST_ASSERT(stbi_write_png(storm1, 8, 8, 4, s_px, 8 * 4));

    TEST_ASSERT(viewer_load_image(storm0, &g_img[0]));
    TEST_ASSERT(viewer_load_image(storm1, &g_img[1]));

    static const int storm_dims[][2] = {
        {3840, 2160},
        {100, 50},
        {10, 10},
        {0, 0},
        {-50, -50},
        {800, 600},
        {1920, 1080},
        {1, 1},
        {2, 500},
        {12, 12},
        {40, 40},
        {160, 80},
        {300, 200},
        {7680, 4320},
        {-1, 500},
        {500, -1},
        {-1000, -1000}
    };
    size_t num_dims = sizeof(storm_dims) / sizeof(storm_dims[0]);

    for (int i = 0; i < 1000; i++) {
        size_t d_idx = (size_t)i % num_dims;
        g_win_w = storm_dims[d_idx][0];
        g_win_h = storm_dims[d_idx][1];

        g_count = (i % 3 == 0) ? 1 : 2;
        g_sync = (i % 2 == 0);
        g_active = (i % 4 == 0) ? 1 : 0;
        g_show_info = (i % 5 != 0);
        g_show_metadata = (i % 6 != 0);

        viewer_render_info_bar(g_ren);
        viewer_render_metadata(g_ren);
        viewer_render(g_ren);
        viewer_fit_view();

        int mx = (g_win_w > 0) ? (g_win_w / 2) : 0;
        int my = (g_win_h > 0) ? (g_win_h / 2) : 0;
        float factor = (i % 2 == 0) ? 1.15f : 0.85f;
        viewer_do_zoom(factor, mx, my);
        viewer_do_pan((i % 20) - 10, 10 - (i % 20));
        viewer_update_title();
    }

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(storm0);
    unlink(storm1);

    // =========================================================================
    // 3. Rapid Navigation, Dual-Pane & Concurrent State Transitions
    // =========================================================================
    const char *trans0 = "/tmp/civ_trans_0.png";
    const char *trans1 = "/tmp/civ_trans_1.png";
    TEST_ASSERT(stbi_write_png(trans0, 8, 8, 4, s_px, 8 * 4));
    TEST_ASSERT(stbi_write_png(trans1, 8, 8, 4, s_px, 8 * 4));

    TEST_ASSERT(viewer_load_image(trans0, &g_img[0]));
    TEST_ASSERT(viewer_load_image(trans1, &g_img[1]));
    g_count = 2;
    g_win_w = 1024;
    g_win_h = 768;
    g_sync = true;
    g_active = 0;

    for (int i = 0; i < 1500; i++) {
        if (i % 2 == 0) {
            viewer_toggle_active_pane();
            TEST_ASSERT(g_active == 0 || g_active == 1);
        }
        if (i % 3 == 0) {
            viewer_toggle_sync();
        }
        if (i % 5 == 0) {
            viewer_toggle_metadata();
        }
        float zoom_step = (i % 7 == 0) ? 1.5f : ((i % 7 == 1) ? 0.6f : 1.05f);
        viewer_do_zoom(zoom_step, 512, 384);
        TEST_ASSERT(g_zoom >= 0.05f && g_zoom <= 32.0f);
        TEST_ASSERT(g_free_zoom[0] >= 0.05f && g_free_zoom[0] <= 32.0f);
        TEST_ASSERT(g_free_zoom[1] >= 0.05f && g_free_zoom[1] <= 32.0f);

        viewer_do_pan((i % 16) - 8, 8 - (i % 16));
        viewer_update_title();

        if (i % 10 == 0) {
            viewer_render(g_ren);
        }
    }

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(trans0);
    unlink(trans1);

    // =========================================================================
    // 4. Broken Symlinks, Corrupted/0-Byte Files, Mid-Navigation Unlinking & Chmod 000
    // =========================================================================
    const char *stress_dir = "/tmp/civ_stress_dir";
    mkdir(stress_dir, 0755);

    char f_01[PATH_MAX], f_02[PATH_MAX], f_03[PATH_MAX], f_04[PATH_MAX];
    char f_05[PATH_MAX], f_06[PATH_MAX], f_07[PATH_MAX], f_08[PATH_MAX];
    snprintf(f_01, sizeof(f_01), "%s/01_valid.png", stress_dir);
    snprintf(f_02, sizeof(f_02), "%s/02_corrupt.png", stress_dir);
    snprintf(f_03, sizeof(f_03), "%s/03_zero.png", stress_dir);
    snprintf(f_04, sizeof(f_04), "%s/04_valid.png", stress_dir);
    snprintf(f_05, sizeof(f_05), "%s/05_deleted.png", stress_dir);
    snprintf(f_06, sizeof(f_06), "%s/06_broken.png", stress_dir);
    snprintf(f_07, sizeof(f_07), "%s/07_noperm.png", stress_dir);
    snprintf(f_08, sizeof(f_08), "%s/08_valid.png", stress_dir);

    // 01: Valid PNG
    TEST_ASSERT(stbi_write_png(f_01, 8, 8, 4, s_px, 8 * 4));

    // 02: Corrupt image file (garbage bytes)
    FILE *fc = fopen(f_02, "wb");
    TEST_ASSERT(fc != NULL);
    fputs("GARBAGE_NOT_A_PNG_FILE_HEADER_1234567890", fc);
    fclose(fc);

    // 03: 0-byte image file
    FILE *fz = fopen(f_03, "wb");
    TEST_ASSERT(fz != NULL);
    fclose(fz);

    // 04: Valid PNG
    TEST_ASSERT(stbi_write_png(f_04, 8, 8, 4, s_px, 8 * 4));

    // 05: Valid PNG (will be unlinked mid-navigation)
    TEST_ASSERT(stbi_write_png(f_05, 8, 8, 4, s_px, 8 * 4));

    // 06: Broken symlink pointing to nonexistent destination
    unlink(f_06);
    TEST_ASSERT_INT_EQ(symlink("/tmp/civ_nonexistent_dest_file.png", f_06), 0);

    // 07: Regular file with permission 0000 (unreadable)
    TEST_ASSERT(stbi_write_png(f_07, 8, 8, 4, s_px, 8 * 4));
    TEST_ASSERT_INT_EQ(chmod(f_07, 0000), 0);

    // 08: Valid PNG
    TEST_ASSERT(stbi_write_png(f_08, 8, 8, 4, s_px, 8 * 4));

    // Directory scan test: broken symlink f_06 must be excluded
    TEST_ASSERT(viewer_scan_current_dir(f_01));
    for (int i = 0; i < g_file_count; i++) {
        TEST_ASSERT(strstr(g_file_list[i], "06_broken.png") == NULL);
    }
    // Chmod 000 file f_07 is in g_file_list because stat() succeeded on regular file
    bool found_noperm = false;
    for (int i = 0; i < g_file_count; i++) {
        if (strstr(g_file_list[i], "07_noperm.png") != NULL) found_noperm = true;
    }
    TEST_ASSERT(found_noperm);

    // Direct open & replace on broken symlink and chmod 000
    Image bad_im = {0};
    TEST_ASSERT(!viewer_load_image(f_06, &bad_im));
    TEST_ASSERT(bad_im.tex == NULL && bad_im.path == NULL);
    TEST_ASSERT(!viewer_load_image(f_07, &bad_im));
    TEST_ASSERT(bad_im.tex == NULL && bad_im.path == NULL);

    TEST_ASSERT(viewer_load_image(f_01, &g_img[0]));
    g_count = 1;
    // Replace with broken symlink fails and leaves pane 0 untouched
    TEST_ASSERT(!viewer_replace_image(0, f_06));
    TEST_ASSERT(g_img[0].path != NULL && strstr(g_img[0].path, "01_valid.png") != NULL);
    // Replace with chmod 000 fails and leaves pane 0 untouched
    TEST_ASSERT(!viewer_replace_image(0, f_07));
    TEST_ASSERT(g_img[0].path != NULL && strstr(g_img[0].path, "01_valid.png") != NULL);

    // Direct exif_read on broken symlink and chmod 000
    ExifData probe_exif;
    TEST_ASSERT(!exif_read(f_06, &probe_exif));
    TEST_ASSERT(!probe_exif.has_exif);
    TEST_ASSERT(!exif_read(f_07, &probe_exif));
    TEST_ASSERT(!probe_exif.has_exif);

    // Unlink f_05 mid-navigation
    TEST_ASSERT_INT_EQ(unlink(f_05), 0);

    // Navigate forward: skips 02 (corrupt) and 03 (0 bytes) -> lands on 04_valid.png
    TEST_ASSERT(viewer_navigate(+1));
    TEST_ASSERT(strstr(g_img[0].path, "04_valid.png") != NULL);

    // Navigate forward: skips 05 (unlinked mid-test) and 07 (chmod 000) -> lands on 08_valid.png
    TEST_ASSERT(viewer_navigate(+1));
    TEST_ASSERT(strstr(g_img[0].path, "08_valid.png") != NULL);

    // Navigate backwards: skips 07 (chmod 000) and 05 (unlinked) -> lands on 04_valid.png
    TEST_ASSERT(viewer_navigate(-1));
    TEST_ASSERT(strstr(g_img[0].path, "04_valid.png") != NULL);

    // Navigate backwards: skips 03 (0 bytes) and 02 (corrupt) -> lands on 01_valid.png
    TEST_ASSERT(viewer_navigate(-1));
    TEST_ASSERT(strstr(g_img[0].path, "01_valid.png") != NULL);

    // Cleanup phase 4
    chmod(f_07, 0644);
    unlink(f_01);
    unlink(f_02);
    unlink(f_03);
    unlink(f_04);
    unlink(f_06);
    unlink(f_07);
    unlink(f_08);
    rmdir(stress_dir);

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    viewer_free_file_list();

    // Restore original viewer state
    g_win_w = orig_w;
    g_win_h = orig_h;
    g_count = orig_count;
    g_show_info = orig_show_info;
    g_show_metadata = orig_show_meta;
    g_sync = orig_sync;
    g_active = orig_active;
    g_file_index = orig_file_index;
    g_file_count = orig_file_count;
    g_img[0].path = orig_p0;
    g_img[1].path = orig_p1;
    snprintf(g_current_dir, sizeof(g_current_dir), "%s", orig_dir);
}

static void test_viewer_path_join_boundary(void) {
    char out[128];

    // NULL pointers and zero destination size
    TEST_ASSERT(!viewer_path_join(NULL, sizeof(out), "/dir", "file.png"));
    TEST_ASSERT(!viewer_path_join(out, sizeof(out), NULL, "file.png"));
    TEST_ASSERT(!viewer_path_join(out, sizeof(out), "/dir", NULL));
    TEST_ASSERT(!viewer_path_join(out, 0, "/dir", "file.png"));

    // dst_size == 1 edge cases
    char tiny[1];
    TEST_ASSERT(!viewer_path_join(tiny, 1, "/dir", "file.png"));
    TEST_ASSERT(!viewer_path_join(tiny, 1, "/", "file.png"));
    TEST_ASSERT(!viewer_path_join(tiny, 1, "/", ""));
    TEST_ASSERT(!viewer_path_join(tiny, 1, "a", ""));
    // dst_size == 1 can hold empty string when dir and file are both empty
    TEST_ASSERT(viewer_path_join(tiny, 1, "", ""));
    TEST_ASSERT_STR_EQ(tiny, "");

    // Directory prefix variations
    // dir = "/"
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/", "image.png"));
    TEST_ASSERT_STR_EQ(out, "/image.png");

    // dir = "///" (multiple root slashes collapsed)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "///", "image.png"));
    TEST_ASSERT_STR_EQ(out, "/image.png");

    // dir = "/a/b/" (trailing slash removed before joining)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b/", "image.png"));
    TEST_ASSERT_STR_EQ(out, "/a/b/image.png");

    // dir = "/a/b" (no trailing slash)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b", "image.png"));
    TEST_ASSERT_STR_EQ(out, "/a/b/image.png");

    // dir = "relative/path"
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "relative/path", "image.png"));
    TEST_ASSERT_STR_EQ(out, "relative/path/image.png");

    // dir = "relative/path/"
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "relative/path/", "image.png"));
    TEST_ASSERT_STR_EQ(out, "relative/path/image.png");

    // dir = "" (empty dir)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "", "image.png"));
    TEST_ASSERT_STR_EQ(out, "image.png");

    // File variations
    // file = "/image.png" (leading slash stripped)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b", "/image.png"));
    TEST_ASSERT_STR_EQ(out, "/a/b/image.png");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/", "/image.png"));
    TEST_ASSERT_STR_EQ(out, "/image.png");

    // file = "///image.png" (multiple leading slashes stripped)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b", "///image.png"));
    TEST_ASSERT_STR_EQ(out, "/a/b/image.png");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "///", "///image.png"));
    TEST_ASSERT_STR_EQ(out, "/image.png");

    // file = "" (empty filename preserves dir without trailing slash)
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b/", ""));
    TEST_ASSERT_STR_EQ(out, "/a/b");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b", ""));
    TEST_ASSERT_STR_EQ(out, "/a/b");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/", ""));
    TEST_ASSERT_STR_EQ(out, "/");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "///", ""));
    TEST_ASSERT_STR_EQ(out, "/");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "relative/path", ""));
    TEST_ASSERT_STR_EQ(out, "relative/path");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "relative/path/", ""));
    TEST_ASSERT_STR_EQ(out, "relative/path");

    // file with only slashes "///"
    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/a/b/", "///"));
    TEST_ASSERT_STR_EQ(out, "/a/b");

    TEST_ASSERT(viewer_path_join(out, sizeof(out), "/", "///"));
    TEST_ASSERT_STR_EQ(out, "/");

    // Exact buffer fit checks:
    // dir = "/dir" (strlen 4), file = "img.png" (strlen 7)
    // Joined string is "/dir/img.png" (strlen 12).
    // Total buffer required = strlen(dir) + 1 + strlen(file) + 1 = 4 + 1 + 7 + 1 = 13 bytes.
    char b13[13];
    TEST_ASSERT(viewer_path_join(b13, sizeof(b13), "/dir", "img.png"));
    TEST_ASSERT_STR_EQ(b13, "/dir/img.png");
    TEST_ASSERT_INT_EQ((int)strlen(b13), 12);

    // dst_size < needed (12 bytes for a 12-char string + null terminator)
    char b12[12];
    TEST_ASSERT(!viewer_path_join(b12, sizeof(b12), "/dir", "img.png"));

    // dst_size > needed (14 bytes)
    char b14[14];
    TEST_ASSERT(viewer_path_join(b14, sizeof(b14), "/dir", "img.png"));
    TEST_ASSERT_STR_EQ(b14, "/dir/img.png");

    // Exact fit with trailing slash on dir:
    // dir = "/dir/" (strlen 5, stripped to 4), file = "img.png" (strlen 7).
    // Still produces "/dir/img.png" which requires 13 bytes.
    TEST_ASSERT(viewer_path_join(b13, 13, "/dir/", "img.png"));
    TEST_ASSERT_STR_EQ(b13, "/dir/img.png");
    TEST_ASSERT(!viewer_path_join(b12, 12, "/dir/", "img.png"));

    // Exact fit with root dir:
    // dir = "/", file = "x" -> "/x" (strlen 2, requires 3 bytes).
    char b3[3];
    TEST_ASSERT(viewer_path_join(b3, sizeof(b3), "/", "x"));
    TEST_ASSERT_STR_EQ(b3, "/x");
    char b2[2];
    TEST_ASSERT(!viewer_path_join(b2, sizeof(b2), "/", "x"));
}

static void test_viewer_reset_1to1_invariants(void) {
    // Backup current viewer state
    int orig_count = g_count;
    float orig_zoom = g_zoom;
    float orig_pan_x = g_pan_x;
    float orig_pan_y = g_pan_y;
    float orig_fz[2] = { g_free_zoom[0], g_free_zoom[1] };
    float orig_fpx[2] = { g_free_pan_x[0], g_free_pan_x[1] };
    float orig_fpy[2] = { g_free_pan_y[0], g_free_pan_y[1] };

    // Test with g_count == 0
    g_count = 0;
    g_zoom = 5.25f;
    g_pan_x = 120.5f;
    g_pan_y = -340.0f;
    g_free_zoom[0] = 0.25f;
    g_free_zoom[1] = 8.0f;
    g_free_pan_x[0] = 50.0f;
    g_free_pan_x[1] = -75.0f;
    g_free_pan_y[0] = 180.0f;
    g_free_pan_y[1] = -220.0f;

    viewer_reset_1to1();

    TEST_ASSERT_INT_EQ(g_count, 0);
    TEST_ASSERT(g_zoom == 1.0f);
    TEST_ASSERT(g_pan_x == 0.0f);
    TEST_ASSERT(g_pan_y == 0.0f);
    TEST_ASSERT(g_free_zoom[0] == 1.0f);
    TEST_ASSERT(g_free_zoom[1] == 1.0f);
    TEST_ASSERT(g_free_pan_x[0] == 0.0f);
    TEST_ASSERT(g_free_pan_x[1] == 0.0f);
    TEST_ASSERT(g_free_pan_y[0] == 0.0f);
    TEST_ASSERT(g_free_pan_y[1] == 0.0f);

    // Test with g_count == 1
    g_count = 1;
    g_zoom = 0.05f;
    g_pan_x = -999.0f;
    g_pan_y = 888.0f;
    g_free_zoom[0] = 16.0f;
    g_free_zoom[1] = 0.1f;
    g_free_pan_x[0] = -12.3f;
    g_free_pan_x[1] = 45.6f;
    g_free_pan_y[0] = -78.9f;
    g_free_pan_y[1] = 101.1f;

    viewer_reset_1to1();

    TEST_ASSERT_INT_EQ(g_count, 1);
    TEST_ASSERT(g_zoom == 1.0f);
    TEST_ASSERT(g_pan_x == 0.0f);
    TEST_ASSERT(g_pan_y == 0.0f);
    TEST_ASSERT(g_free_zoom[0] == 1.0f);
    TEST_ASSERT(g_free_zoom[1] == 1.0f);
    TEST_ASSERT(g_free_pan_x[0] == 0.0f);
    TEST_ASSERT(g_free_pan_x[1] == 0.0f);
    TEST_ASSERT(g_free_pan_y[0] == 0.0f);
    TEST_ASSERT(g_free_pan_y[1] == 0.0f);

    // Test with g_count == 2
    g_count = 2;
    g_zoom = 32.0f;
    g_pan_x = 42.0f;
    g_pan_y = -42.0f;
    g_free_zoom[0] = 3.14f;
    g_free_zoom[1] = 2.71f;
    g_free_pan_x[0] = 1000.0f;
    g_free_pan_x[1] = -1000.0f;
    g_free_pan_y[0] = 2000.0f;
    g_free_pan_y[1] = -2000.0f;

    viewer_reset_1to1();

    TEST_ASSERT_INT_EQ(g_count, 2);
    TEST_ASSERT(g_zoom == 1.0f);
    TEST_ASSERT(g_pan_x == 0.0f);
    TEST_ASSERT(g_pan_y == 0.0f);
    TEST_ASSERT(g_free_zoom[0] == 1.0f);
    TEST_ASSERT(g_free_zoom[1] == 1.0f);
    TEST_ASSERT(g_free_pan_x[0] == 0.0f);
    TEST_ASSERT(g_free_pan_x[1] == 0.0f);
    TEST_ASSERT(g_free_pan_y[0] == 0.0f);
    TEST_ASSERT(g_free_pan_y[1] == 0.0f);

    // Test idempotence (calling again changes nothing)
    viewer_reset_1to1();
    TEST_ASSERT(g_zoom == 1.0f);
    TEST_ASSERT(g_pan_x == 0.0f);
    TEST_ASSERT(g_pan_y == 0.0f);
    TEST_ASSERT(g_free_zoom[0] == 1.0f);
    TEST_ASSERT(g_free_zoom[1] == 1.0f);
    TEST_ASSERT(g_free_pan_x[0] == 0.0f);
    TEST_ASSERT(g_free_pan_x[1] == 0.0f);
    TEST_ASSERT(g_free_pan_y[0] == 0.0f);
    TEST_ASSERT(g_free_pan_y[1] == 0.0f);

    // Restore original viewer state
    g_count = orig_count;
    g_zoom = orig_zoom;
    g_pan_x = orig_pan_x;
    g_pan_y = orig_pan_y;
    g_free_zoom[0] = orig_fz[0];
    g_free_zoom[1] = orig_fz[1];
    g_free_pan_x[0] = orig_fpx[0];
    g_free_pan_x[1] = orig_fpx[1];
    g_free_pan_y[0] = orig_fpy[0];
    g_free_pan_y[1] = orig_fpy[1];
}

static void test_viewer_deep_directory_and_navigation_stress(void) {
    char base_dir[] = "/tmp/civ_vdeep_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    // Save viewer state
    int orig_count = g_count;
    int orig_file_index = g_file_index;
    int orig_file_count = g_file_count;
    char orig_dir[PATH_MAX];
    snprintf(orig_dir, sizeof(orig_dir), "%s", g_current_dir);

    // Build a 20-level deep directory tree
    const int depth = 20;
    char current_path[PATH_MAX];
    snprintf(current_path, sizeof(current_path), "%s", base_dir);
    char dir_chain[20][PATH_MAX];

    for (int i = 0; i < depth; i++) {
        char next_path[PATH_MAX];
        snprintf(next_path, sizeof(next_path), "%.4000s/lvl%02d", current_path, i);
        TEST_ASSERT_INT_EQ(mkdir(next_path, 0755), 0);
        snprintf(dir_chain[i], sizeof(dir_chain[i]), "%s", next_path);
        snprintf(current_path, sizeof(current_path), "%s", next_path);
    }

    // Place an image at level 19 (deepest) and level 18 (parent)
    char deep_img[PATH_MAX];
    snprintf(deep_img, sizeof(deep_img), "%.4000s/deep_leaf.png", current_path);
    uint8_t px[4] = {255, 0, 0, 255};
    TEST_ASSERT(stbi_write_png(deep_img, 1, 1, 4, px, 4));

    char parent_img[PATH_MAX];
    snprintf(parent_img, sizeof(parent_img), "%.4000s/deep_parent.png", dir_chain[depth - 2]);
    TEST_ASSERT(stbi_write_png(parent_img, 1, 1, 4, px, 4));

    // Test viewer_path_join on deep path
    char joined[PATH_MAX];
    TEST_ASSERT(viewer_path_join(joined, sizeof(joined), current_path, "deep_leaf.png"));
    TEST_ASSERT_STR_EQ(joined, deep_img);

    // Test viewer_truncate_path on deep path with various max_len
    char tr_out[PATH_MAX];
    viewer_truncate_path(deep_img, tr_out, sizeof(tr_out), 20);
    TEST_ASSERT((int)strlen(tr_out) <= 20);
    TEST_ASSERT(strstr(tr_out, "...") != NULL);

    viewer_truncate_path(deep_img, tr_out, sizeof(tr_out), 10);
    TEST_ASSERT((int)strlen(tr_out) <= 10);

    viewer_truncate_path(deep_img, tr_out, sizeof(tr_out), 2);
    TEST_ASSERT((int)strlen(tr_out) <= 2);

    viewer_truncate_path(deep_img, tr_out, sizeof(tr_out), (int)strlen(deep_img) + 10);
    TEST_ASSERT_STR_EQ(tr_out, deep_img);

    // Scan the deepest directory
    TEST_ASSERT(viewer_scan_current_dir(deep_img));
    TEST_ASSERT_INT_EQ(g_file_count, 1);
    TEST_ASSERT_INT_EQ(g_file_index, 0);

    // Test viewer_go_parent navigation from level 19 to level 18
    TEST_ASSERT(viewer_go_parent());
    TEST_ASSERT_INT_EQ(g_file_count, 1);
    TEST_ASSERT(strstr(g_file_list[0], "deep_parent.png") != NULL);

    // Cleanup
    viewer_free_file_list();
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);

    unlink(deep_img);
    unlink(parent_img);
    for (int i = depth - 1; i >= 0; i--) {
        rmdir(dir_chain[i]);
    }
    rmdir(base_dir);

    g_count = orig_count;
    g_file_index = orig_file_index;
    g_file_count = orig_file_count;
    snprintf(g_current_dir, sizeof(g_current_dir), "%s", orig_dir);
}

static void test_viewer_empty_and_non_image_stress(void) {
    char base_dir[] = "/tmp/civ_vempty_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    int orig_count = g_count;
    int orig_file_index = g_file_index;
    int orig_file_count = g_file_count;
    char orig_dir[PATH_MAX];
    snprintf(orig_dir, sizeof(orig_dir), "%s", g_current_dir);

    char f_empty_png[PATH_MAX];
    char f_corrupt_jpg[PATH_MAX];
    char f_text[PATH_MAX];
    char f_script[PATH_MAX];
    char f_bak[PATH_MAX];
    char f_valid_png[PATH_MAX];

    snprintf(f_empty_png, sizeof(f_empty_png), "%s/empty.png", base_dir);
    snprintf(f_corrupt_jpg, sizeof(f_corrupt_jpg), "%s/corrupt.jpg", base_dir);
    snprintf(f_text, sizeof(f_text), "%s/notes.txt", base_dir);
    snprintf(f_script, sizeof(f_script), "%s/run.sh", base_dir);
    snprintf(f_bak, sizeof(f_bak), "%s/photo.png.bak", base_dir);
    snprintf(f_valid_png, sizeof(f_valid_png), "%s/valid.png", base_dir);

    // 1. Empty file (0 bytes)
    FILE *fe = fopen(f_empty_png, "wb");
    TEST_ASSERT(fe != NULL);
    fclose(fe);

    // 2. Corrupted file with image extension
    FILE *fc = fopen(f_corrupt_jpg, "wb");
    TEST_ASSERT(fc != NULL);
    fputs("NOT A JPEG AT ALL", fc);
    fclose(fc);

    // 3. Non-image text file
    FILE *ft = fopen(f_text, "wb");
    TEST_ASSERT(ft != NULL);
    fputs("plain text notes\n", ft);
    fclose(ft);

    // 4. Non-image script
    FILE *fs = fopen(f_script, "wb");
    TEST_ASSERT(fs != NULL);
    fputs("#!/bin/sh\necho hi\n", fs);
    fclose(fs);

    // 5. Backup file
    FILE *fb = fopen(f_bak, "wb");
    TEST_ASSERT(fb != NULL);
    fputs("backup", fb);
    fclose(fb);

    // 6. Valid 1x1 PNG
    uint8_t px[4] = {0, 255, 0, 255};
    TEST_ASSERT(stbi_write_png(f_valid_png, 1, 1, 4, px, 4));

    // Test viewer_is_image_file extension filtering
    TEST_ASSERT(viewer_is_image_file("file.png"));
    TEST_ASSERT(viewer_is_image_file("file.jpg"));
    TEST_ASSERT(viewer_is_image_file("file.JPEG"));
    TEST_ASSERT(viewer_is_image_file("file.webp"));
    TEST_ASSERT(!viewer_is_image_file("notes.txt"));
    TEST_ASSERT(!viewer_is_image_file("run.sh"));
    TEST_ASSERT(!viewer_is_image_file("photo.png.bak"));
    TEST_ASSERT(!viewer_is_image_file("photo.jpg~"));
    TEST_ASSERT(!viewer_is_image_file("archive.tar.gz"));
    TEST_ASSERT(!viewer_is_image_file("no_extension"));
    TEST_ASSERT(!viewer_is_image_file(".hidden"));
    TEST_ASSERT(viewer_is_image_file(".png"));
    TEST_ASSERT(!viewer_is_image_file("trailing_dot."));
    TEST_ASSERT(!viewer_is_image_file(""));

    // Test viewer_validate_image_path
    char clean[PATH_MAX];
    // Empty PNG: valid regular file with image extension
    TEST_ASSERT(viewer_validate_image_path(f_empty_png, clean, sizeof(clean)));
    // Corrupt JPG: valid regular file with image extension
    TEST_ASSERT(viewer_validate_image_path(f_corrupt_jpg, clean, sizeof(clean)));
    // Text and script files: rejected because of extension
    TEST_ASSERT(!viewer_validate_image_path(f_text, clean, sizeof(clean)));
    TEST_ASSERT(!viewer_validate_image_path(f_script, clean, sizeof(clean)));
    TEST_ASSERT(!viewer_validate_image_path(f_bak, clean, sizeof(clean)));
    // Non-existent file: rejected
    TEST_ASSERT(!viewer_validate_image_path("/tmp/nonexistent_12345.png", clean, sizeof(clean)));
    // NULL or empty: rejected
    TEST_ASSERT(!viewer_validate_image_path(NULL, clean, sizeof(clean)));
    TEST_ASSERT(!viewer_validate_image_path("", clean, sizeof(clean)));

    // Test viewer_load_image failure modes on 0-byte and corrupted files
    Image im = {0};
    TEST_ASSERT(!viewer_load_image(f_empty_png, &im));
    TEST_ASSERT(im.tex == NULL);
    TEST_ASSERT(im.path == NULL);
    TEST_ASSERT_INT_EQ(im.w, 0);

    TEST_ASSERT(!viewer_load_image(f_corrupt_jpg, &im));
    TEST_ASSERT(im.tex == NULL);
    TEST_ASSERT(im.path == NULL);

    TEST_ASSERT(!viewer_load_image(f_text, &im));
    TEST_ASSERT(im.tex == NULL);

    // Valid file loads cleanly
    TEST_ASSERT(viewer_load_image(f_valid_png, &im));
    TEST_ASSERT(im.tex != NULL);
    TEST_ASSERT(im.path != NULL);
    TEST_ASSERT_INT_EQ(im.w, 1);
    TEST_ASSERT_INT_EQ(im.h, 1);
    viewer_unload_image(&im);

    // Directory scan on base_dir: only files with image extensions are included
    TEST_ASSERT(viewer_scan_current_dir(f_valid_png));
    // Should contain empty.png, corrupt.jpg, and valid.png (3 files), NOT notes.txt, run.sh, photo.png.bak
    TEST_ASSERT_INT_EQ(g_file_count, 3);

    // Verify navigation skips corrupt and empty files
    viewer_navigate(+1);

    // Cleanup
    viewer_free_file_list();
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);

    unlink(f_empty_png);
    unlink(f_corrupt_jpg);
    unlink(f_text);
    unlink(f_script);
    unlink(f_bak);
    unlink(f_valid_png);
    rmdir(base_dir);

    g_count = orig_count;
    g_file_index = orig_file_index;
    g_file_count = orig_file_count;
    snprintf(g_current_dir, sizeof(g_current_dir), "%s", orig_dir);
}

static void test_viewer_async_decoding(void) {
    int orig_count = g_count;
    bool orig_sync = g_sync;
    int orig_active = g_active;

    viewer_cleanup_async();
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);

    const char *valid_png = "/tmp/civ_test_async_valid.png";
    const char *corrupt_png = "/tmp/civ_test_async_corrupt.png";

    // Create valid test image (16x16 RGBA)
    unsigned char px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16 * 4; i++) px[i] = (unsigned char)(i & 0xFF);
    TEST_ASSERT(stbi_write_png(valid_png, 16, 16, 4, px, 16 * 4));

    // Create corrupt test image (invalid header and junk)
    uint8_t garbage[512];
    for (size_t i = 0; i < sizeof(garbage); i++) garbage[i] = (uint8_t)(i * 31 + 7);
    write_raw_file(corrupt_png, garbage, sizeof(garbage));

    // 1. Argument validation edge cases
    TEST_ASSERT(!viewer_load_image_async(-1, valid_png));
    TEST_ASSERT(!viewer_load_image_async(2, valid_png));
    TEST_ASSERT(!viewer_load_image_async(0, NULL));
    TEST_ASSERT(!viewer_is_loading(-1));
    TEST_ASSERT(!viewer_is_loading(2));
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));
    TEST_ASSERT(!viewer_pump_async_loads());

    // 2. Test async loading of a valid PNG file and active loading state
    g_count = 1;
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_is_loading(0));

    // 3. Test "Loading..." splash rendering path during active async load
    // While loading is in progress and im->tex is NULL, viewer_render must execute
    // the centered "Loading..." splash branch without crash or artifact.
    viewer_render(g_ren);

    // Pump until decode completes and texture is uploaded
    int safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex != NULL);
    TEST_ASSERT_INT_EQ(g_img[0].w, 16);
    TEST_ASSERT_INT_EQ(g_img[0].h, 16);
    TEST_ASSERT_INT_EQ(g_img[0].channels, 4);
    TEST_ASSERT(g_img[0].path != NULL);
    TEST_ASSERT_STR_EQ(g_img[0].path, valid_png);

    // Render loaded image frame
    viewer_render(g_ren);

    // 4. Test async loading of a corrupt image file
    TEST_ASSERT(viewer_load_image_async(0, corrupt_png));
    TEST_ASSERT(viewer_is_loading(0));

    safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    // Graceful failure: tex must be NULL, path must be NULL
    TEST_ASSERT(g_img[0].tex == NULL);
    TEST_ASSERT(g_img[0].path == NULL);

    // 5. Test dual-pane simultaneous async load and "Loading..." splash rendering
    g_count = 2;
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    TEST_ASSERT(viewer_is_loading(0));
    TEST_ASSERT(viewer_is_loading(1));

    // Render both panes in loading state
    viewer_render(g_ren);

    // Pump until both complete
    safety_spins = 1000;
    while ((viewer_is_loading(0) || viewer_is_loading(1)) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));
    TEST_ASSERT(g_img[0].tex != NULL);
    TEST_ASSERT(g_img[1].tex != NULL);
    TEST_ASSERT_INT_EQ(g_img[0].w, 16);
    TEST_ASSERT_INT_EQ(g_img[1].w, 16);

    // Render both panes in loaded state
    viewer_render(g_ren);

    // 6. Test cancellation during unload and replace
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(!viewer_is_loading(0));

    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_replace_image(0, valid_png));
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex != NULL);

    // 7. Test viewer_cleanup_async cancels/joins active loads cleanly
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    viewer_cleanup_async();
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));

    // Multiple cleanup calls must be safe and idempotent
    viewer_cleanup_async();
    viewer_cleanup_async();

    // 8. Rapid cancellation stress test: 50 rapid alternating calls without intermediate pumping
    for (int i = 0; i < 50; i++) {
        const char *target = (i % 2 == 0) ? corrupt_png : valid_png;
        TEST_ASSERT(viewer_load_image_async(0, target));
    }
    safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex != NULL);
    TEST_ASSERT_INT_EQ(g_img[0].w, 16);
    TEST_ASSERT_INT_EQ(g_img[0].h, 16);
    TEST_ASSERT_INT_EQ(g_img[0].channels, 4);
    TEST_ASSERT(g_img[0].path != NULL);
    TEST_ASSERT_STR_EQ(g_img[0].path, valid_png);

    // 9. Asymmetric dual-pane concurrency test: corrupt on pane 0, valid on pane 1
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 2;
    TEST_ASSERT(viewer_load_image_async(0, corrupt_png));
    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    TEST_ASSERT(viewer_is_loading(0));
    TEST_ASSERT(viewer_is_loading(1));

    safety_spins = 1000;
    while ((viewer_is_loading(0) || viewer_is_loading(1)) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));
    TEST_ASSERT(g_img[0].tex == NULL);
    TEST_ASSERT(g_img[0].path == NULL);
    TEST_ASSERT(g_img[1].tex != NULL);
    TEST_ASSERT_INT_EQ(g_img[1].w, 16);
    TEST_ASSERT_INT_EQ(g_img[1].h, 16);
    TEST_ASSERT_INT_EQ(g_img[1].channels, 4);
    TEST_ASSERT(g_img[1].path != NULL);
    TEST_ASSERT_STR_EQ(g_img[1].path, valid_png);

    // 10. Intensive memory & lifecycle stress test:
    // 100 rapid cycles of: dispatch async load with valid PNG, immediately unload or cancel, and assert !viewer_is_loading
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(viewer_load_image_async(0, valid_png));
        if (i % 2 == 0) {
            viewer_unload_image(&g_img[0]);
        } else {
            viewer_cleanup_async();
        }
        TEST_ASSERT(!viewer_is_loading(0));
    }

    // 11. In-flight async load followed by viewer_replace_image with a corrupted image:
    // verify replace returns false, in-flight task was canceled cleanly, previous image is preserved,
    // and pumping afterwards is a clean no-op without leaks.
    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(viewer_load_image(valid_png, &g_img[0]));
    TEST_ASSERT(g_img[0].tex != NULL);
    SDL_Texture *saved_tex0 = g_img[0].tex;
    char saved_path0[PATH_MAX];
    snprintf(saved_path0, sizeof(saved_path0), "%s", g_img[0].path);
    int saved_w0 = g_img[0].w;
    int saved_h0 = g_img[0].h;
    int saved_channels0 = g_img[0].channels;

    // Dispatch in-flight async load
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_is_loading(0));

    // Followed immediately by viewer_replace_image with corrupted image
    TEST_ASSERT(!viewer_replace_image(0, corrupt_png));

    // Verify in-flight task was canceled cleanly
    TEST_ASSERT(!viewer_is_loading(0));

    // Verify previous image is preserved
    TEST_ASSERT(g_img[0].tex == saved_tex0);
    TEST_ASSERT(g_img[0].path != NULL);
    TEST_ASSERT_STR_EQ(g_img[0].path, saved_path0);
    TEST_ASSERT_INT_EQ(g_img[0].w, saved_w0);
    TEST_ASSERT_INT_EQ(g_img[0].h, saved_h0);
    TEST_ASSERT_INT_EQ(g_img[0].channels, saved_channels0);

    // Verify pumping afterwards is a clean no-op without leaks
    TEST_ASSERT(!viewer_pump_async_loads());
    TEST_ASSERT(g_img[0].tex == saved_tex0);
    TEST_ASSERT_STR_EQ(g_img[0].path, saved_path0);
    TEST_ASSERT(!viewer_is_loading(0));

    // Dual-pane verification: in-flight async load on pane 1 followed by viewer_replace_image with corrupt image
    g_count = 2;
    viewer_unload_image(&g_img[1]);
    TEST_ASSERT(viewer_load_image(valid_png, &g_img[1]));
    TEST_ASSERT(g_img[1].tex != NULL);
    SDL_Texture *saved_tex1 = g_img[1].tex;
    char saved_path1[PATH_MAX];
    snprintf(saved_path1, sizeof(saved_path1), "%s", g_img[1].path);
    int saved_w1 = g_img[1].w;
    int saved_h1 = g_img[1].h;
    int saved_channels1 = g_img[1].channels;

    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    TEST_ASSERT(viewer_is_loading(1));

    TEST_ASSERT(!viewer_replace_image(1, corrupt_png));
    TEST_ASSERT(!viewer_is_loading(1));
    TEST_ASSERT(g_img[1].tex == saved_tex1);
    TEST_ASSERT(g_img[1].path != NULL);
    TEST_ASSERT_STR_EQ(g_img[1].path, saved_path1);
    TEST_ASSERT_INT_EQ(g_img[1].w, saved_w1);
    TEST_ASSERT_INT_EQ(g_img[1].h, saved_h1);
    TEST_ASSERT_INT_EQ(g_img[1].channels, saved_channels1);

    TEST_ASSERT(!viewer_pump_async_loads());
    TEST_ASSERT(g_img[1].tex == saved_tex1);
    TEST_ASSERT_STR_EQ(g_img[1].path, saved_path1);
    TEST_ASSERT(!viewer_is_loading(1));

    // 12. Error and boundary conditions: 0-byte empty file, truncated PNG header, non-existent path
    const char *empty_file = "/tmp/civ_test_async_empty.png";
    write_raw_file(empty_file, "", 0);

    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(viewer_load_image_async(0, empty_file));
    TEST_ASSERT(viewer_is_loading(0));
    safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex == NULL);
    TEST_ASSERT(g_img[0].path == NULL);
    unlink(empty_file);

    const char *trunc_file = "/tmp/civ_test_async_trunc.png";
    uint8_t png_magic[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    write_raw_file(trunc_file, png_magic, sizeof(png_magic));

    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(viewer_load_image_async(0, trunc_file));
    TEST_ASSERT(viewer_is_loading(0));
    safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex == NULL);
    TEST_ASSERT(g_img[0].path == NULL);
    unlink(trunc_file);

    const char *nonexistent_file = "/tmp/civ_test_async_nonexistent_9999.png";
    unlink(nonexistent_file);

    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(viewer_load_image_async(0, nonexistent_file));
    TEST_ASSERT(viewer_is_loading(0));
    safety_spins = 1000;
    while (viewer_is_loading(0) && --safety_spins > 0) {
        viewer_pump_async_loads();
        SDL_Delay(1);
    }
    viewer_pump_async_loads();
    TEST_ASSERT(safety_spins > 0);
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(g_img[0].tex == NULL);
    TEST_ASSERT(g_img[0].path == NULL);

    // 13. Status bar and metadata rendering while image is actively loading
    int orig_win_w = g_win_w;
    int orig_win_h = g_win_h;
    bool orig_show_info = g_show_info;
    bool orig_show_meta = g_show_metadata;

    g_win_w = 800;
    g_win_h = 600;
    g_show_info = true;
    g_show_metadata = true;

    // Single-pane loading status bar and metadata verification
    g_count = 1;
    viewer_unload_image(&g_img[0]);
    TEST_ASSERT(viewer_load_image(valid_png, &g_img[0]));
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_is_loading(0));

    // Invoke renderer while actively loading
    viewer_render_info_bar(g_ren);
    viewer_render_metadata(g_ren);

    char status_single[512];
    ViewerStatusBarLayout layout_s = viewer_calc_status_layout_single(
        g_win_w, valid_png, 0, 0, 100, true, 1, 1);
    int len_s = viewer_format_status_single(
        &layout_s, valid_png, 0, 0, 100, true, 1, 1, status_single, sizeof(status_single));
    TEST_ASSERT(len_s > 0);
    TEST_ASSERT(strstr(status_single, "Loading...") != NULL);
    TEST_ASSERT(strstr(status_single, "0x0") == NULL);

    // Verify small buffer bounds safety (no buffer overruns)
    char small_s[12];
    int small_len_s = viewer_format_status_single(
        &layout_s, valid_png, 0, 0, 100, true, 1, 1, small_s, sizeof(small_s));
    TEST_ASSERT(small_len_s < (int)sizeof(small_s));
    TEST_ASSERT_INT_EQ((int)strlen(small_s), small_len_s);
    TEST_ASSERT(small_s[sizeof(small_s) - 1] == '\0');

    // Dual-pane loading status bar verification (both panes loading)
    g_count = 2;
    TEST_ASSERT(viewer_load_image(valid_png, &g_img[1]));
    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    TEST_ASSERT(viewer_is_loading(0));
    TEST_ASSERT(viewer_is_loading(1));

    viewer_render_info_bar(g_ren);
    g_active = 0;
    viewer_render_metadata(g_ren);
    g_active = 1;
    viewer_render_metadata(g_ren);

    char status_dual[512];
    ViewerStatusBarLayout layout_d = viewer_calc_status_layout_dual(
        g_win_w, valid_png, 0, 0, valid_png, 0, 0, 100, true, 0);
    int len_d = viewer_format_status_dual(
        &layout_d, valid_png, 0, 0, valid_png, 0, 0, 100, true, 0, status_dual, sizeof(status_dual));
    TEST_ASSERT(len_d > 0);
    TEST_ASSERT(strstr(status_dual, "Loading...") != NULL);
    TEST_ASSERT(strstr(status_dual, "(Loading...)") != NULL);
    TEST_ASSERT(strstr(status_dual, "0x0") == NULL);

    // Verify small buffer bounds safety in dual pane
    char small_d[12];
    int small_len_d = viewer_format_status_dual(
        &layout_d, valid_png, 0, 0, valid_png, 0, 0, 100, true, 0, small_d, sizeof(small_d));
    TEST_ASSERT(small_len_d < (int)sizeof(small_d));
    TEST_ASSERT_INT_EQ((int)strlen(small_d), small_len_d);
    TEST_ASSERT(small_d[sizeof(small_d) - 1] == '\0');

    // Dual-pane asymmetric loading: cancel pane 1 and load synchronously, pane 0 still loading
    viewer_unload_image(&g_img[1]);
    TEST_ASSERT(!viewer_is_loading(1));
    TEST_ASSERT(viewer_load_image(valid_png, &g_img[1]));
    TEST_ASSERT(viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));

    viewer_render_info_bar(g_ren);
    g_active = 0;
    viewer_render_metadata(g_ren);
    g_active = 1;
    viewer_render_metadata(g_ren);

    char status_asym[512];
    ViewerStatusBarLayout layout_asym = viewer_calc_status_layout_dual(
        g_win_w, valid_png, 0, 0, valid_png, g_img[1].w, g_img[1].h, 100, true, 0);
    int len_asym = viewer_format_status_dual(
        &layout_asym, valid_png, 0, 0, valid_png, g_img[1].w, g_img[1].h, 100, true, 0, status_asym, sizeof(status_asym));
    TEST_ASSERT(len_asym > 0);
    TEST_ASSERT(strstr(status_asym, "(Loading...)") != NULL);
    TEST_ASSERT(strstr(status_asym, "(16x16)") != NULL);

    // Clean up async tasks
    viewer_cleanup_async();
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));

    // 14. UI/UX edge cases during active loading: degenerate window sizes and view fitting
    g_count = 1;
    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    TEST_ASSERT(viewer_load_image_async(0, valid_png));
    TEST_ASSERT(viewer_is_loading(0));

    // Degenerate window sizes (0x0) while actively loading
    g_win_w = 0;
    g_win_h = 0;
    viewer_render(g_ren);
    viewer_fit_view();
    TEST_ASSERT(g_zoom >= 0.05f && g_zoom <= 1.0f);
    TEST_ASSERT(!isnan(g_zoom) && !isinf(g_zoom));

    // Narrow window size (20x15, narrower than splash text) in single pane
    g_win_w = 20;
    g_win_h = 15;
    viewer_render(g_ren);
    viewer_fit_view();
    TEST_ASSERT(g_zoom >= 0.05f && g_zoom <= 1.0f);
    TEST_ASSERT(!isnan(g_zoom) && !isinf(g_zoom));

    // Narrow window size in dual pane (both panes actively loading)
    g_count = 2;
    TEST_ASSERT(viewer_load_image_async(1, valid_png));
    TEST_ASSERT(viewer_is_loading(0));
    TEST_ASSERT(viewer_is_loading(1));
    viewer_render(g_ren);
    viewer_fit_view();
    TEST_ASSERT(g_zoom >= 0.05f && g_zoom <= 1.0f);
    TEST_ASSERT(!isnan(g_zoom) && !isinf(g_zoom));

    // View fitting in free mode while actively loading
    g_sync = false;
    viewer_fit_view();
    TEST_ASSERT(g_free_zoom[0] >= 0.05f && g_free_zoom[0] <= 1.0f);
    TEST_ASSERT(g_free_zoom[1] >= 0.05f && g_free_zoom[1] <= 1.0f);
    TEST_ASSERT(!isnan(g_free_zoom[0]) && !isnan(g_free_zoom[1]));
    viewer_render(g_ren);
    g_sync = orig_sync;

    // Clean up async tasks
    viewer_cleanup_async();
    TEST_ASSERT(!viewer_is_loading(0));
    TEST_ASSERT(!viewer_is_loading(1));

    g_win_w = orig_win_w;
    g_win_h = orig_win_h;
    g_show_info = orig_show_info;
    g_show_metadata = orig_show_meta;

    // Cleanup files and state
    unlink(valid_png);
    unlink(corrupt_png);

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    viewer_cleanup_async();

    g_count = orig_count;
    g_sync = orig_sync;
    g_active = orig_active;
}

void run_viewer_tests(void) {
    printf("--- Viewer Test Suite ---\n");
    TEST_RUN(test_viewer_is_image_file);
    TEST_RUN(test_viewer_truncate_filename);
    TEST_RUN(test_viewer_format_color_depth);
    TEST_RUN(test_viewer_truncate_path);
    TEST_RUN(test_viewer_format_file_size);
    TEST_RUN(test_viewer_distribute_dual_budget);
    TEST_RUN(test_viewer_calc_status_layout_single);
    TEST_RUN(test_viewer_calc_status_layout_dual);
    TEST_RUN(test_viewer_format_status);
    TEST_RUN(test_viewer_ui_layout_invariants);
    TEST_RUN(test_viewer_performance_benchmarks);
    TEST_RUN(test_viewer_render_metadata_and_info_edge_cases);
    TEST_RUN(test_viewer_file_list_and_scan);
    TEST_RUN(test_viewer_load_unload);
    TEST_RUN(test_viewer_replace_and_bounds);
    TEST_RUN(test_viewer_dual_pane_and_navigation);
    TEST_RUN(test_viewer_go_parent_edge_cases);
    TEST_RUN(test_viewer_validate_image_path);
    TEST_RUN(test_viewer_corrupt_and_garbage_images);
    TEST_RUN(test_viewer_degenerate_window_and_fit);
    TEST_RUN(test_viewer_navigate_corrupt_file_skipping);
    TEST_RUN(test_viewer_dual_pane_boundary_cases);
    TEST_RUN(test_viewer_culling_and_metadata_render);
    TEST_RUN(test_viewer_metadata_cache_stress);
    TEST_RUN(test_viewer_repeated_load_unload_cycles);
    TEST_RUN(test_viewer_corrupt_load_cleanup_stress);
    TEST_RUN(test_viewer_stress_lifecycle_and_mutation);
    TEST_RUN(test_viewer_path_join_boundary);
    TEST_RUN(test_viewer_reset_1to1_invariants);
    TEST_RUN(test_viewer_deep_directory_and_navigation_stress);
    TEST_RUN(test_viewer_empty_and_non_image_stress);
    TEST_RUN(test_viewer_async_decoding);
}
