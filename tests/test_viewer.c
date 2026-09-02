#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "test_common.h"
#include "viewer.h"
#include "stb_image_write.h"

#include <SDL2/SDL.h>
#include <unistd.h>
#include <sys/stat.h>

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
    TEST_ASSERT(img.path != NULL);

    viewer_unload_image(&img);
    TEST_ASSERT(img.tex == NULL);
    TEST_ASSERT(img.path == NULL);
    TEST_ASSERT_INT_EQ(img.w, 0);

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

    viewer_unload_image(&g_img[0]);
    viewer_unload_image(&g_img[1]);
    g_count = 0;
    unlink(tmp);
}

void run_viewer_tests(void) {
    printf("--- Viewer Test Suite ---\n");
    TEST_RUN(test_viewer_is_image_file);
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
}
