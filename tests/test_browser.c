#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "test_common.h"
#include "browser.h"
#include "viewer.h"
#include "stb_image_write.h"

#include <SDL2/SDL.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

static void make_dummy_png(const char *path) {
    unsigned char p[4] = {100, 150, 200, 255};
    stbi_write_png(path, 1, 1, 4, p, 4);
}

static void test_browser_lifecycle(void) {
    browser_cleanup();
    TEST_ASSERT(!browser_is_open());

    browser_toggle();
    TEST_ASSERT(browser_is_open());

    browser_toggle();
    TEST_ASSERT(!browser_is_open());

    // Multiple cleanups
    browser_cleanup();
    browser_cleanup();
    TEST_ASSERT(!browser_is_open());
}

static void test_browser_tree_navigation(void) {
    // Create temporary folder hierarchy
    char base_dir[] = "/tmp/civ_btest_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    char sub1[PATH_MAX], sub2[PATH_MAX];
    snprintf(sub1, sizeof(sub1), "%s/folder_a", base_dir);
    snprintf(sub2, sizeof(sub2), "%s/folder_b", base_dir);
    mkdir(sub1, 0755);
    mkdir(sub2, 0755);

    char f1[PATH_MAX + 32], f2[PATH_MAX + 32], f3[PATH_MAX + 32];
    snprintf(f1, sizeof(f1), "%s/root_img.png", base_dir);
    snprintf(f2, sizeof(f2), "%s/sub1_img.png", sub1);
    snprintf(f3, sizeof(f3), "%s/sub2_img.jpg", sub2);

    make_dummy_png(f1);
    make_dummy_png(f2);
    make_dummy_png(f3);

    browser_set_root(base_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Navigation keys
    // Down
    browser_handle_key(SDLK_DOWN, 0);
    // Expand (Right)
    browser_handle_key(SDLK_RIGHT, 0);
    // Expand again / Down
    browser_handle_key(SDLK_DOWN, 0);
    // Collapse (Left)
    browser_handle_key(SDLK_LEFT, 0);

    // Filter test
    browser_handle_key(SDLK_f, KMOD_CTRL);
    browser_handle_key(SDLK_r, 0);
    browser_handle_key(SDLK_o, 0);
    browser_handle_key(SDLK_o, 0);
    browser_handle_key(SDLK_t, 0);

    // Backspace filter
    browser_handle_key(SDLK_BACKSPACE, 0);

    // Render while open
    browser_render(g_ren);

    // Test render with very small panel (verify no buffer underflows)
    int old_w = g_win_w;
    int old_h = g_win_h;
    g_win_w = 4;
    g_win_h = 4;
    browser_render(g_ren);
    g_win_w = old_w;
    g_win_h = old_h;

    // Clear filter via ESC
    browser_handle_key(SDLK_ESCAPE, 0);
    // Close browser via ESC
    browser_handle_key(SDLK_ESCAPE, 0);
    TEST_ASSERT(!browser_is_open());

    browser_cleanup();

    unlink(f1);
    unlink(f2);
    unlink(f3);
    rmdir(sub1);
    rmdir(sub2);
    rmdir(base_dir);
}

static void test_browser_expand_collapse_stress(void) {
    char base_dir[] = "/tmp/civ_bstress_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    // Create 35 folders to force array reallocations beyond initial capacity of 32
    char subdirs[35][PATH_MAX + 32];
    char imgs[35][PATH_MAX + 32];
    for (int i = 0; i < 35; i++) {
        snprintf(subdirs[i], sizeof(subdirs[i]), "%s/dir_%02d", base_dir, i);
        mkdir(subdirs[i], 0755);
        snprintf(imgs[i], sizeof(imgs[i]), "%s/img_%02d.png", subdirs[i], i);
        make_dummy_png(imgs[i]);
    }

    browser_set_root(base_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Expand all directories to trigger reallocation
    for (int i = 0; i < 35; i++) {
        browser_handle_key(SDLK_RIGHT, 0);
        browser_handle_key(SDLK_DOWN, 0);
    }

    // Render expanded tree
    browser_render(g_ren);

    // Collapse back
    for (int i = 0; i < 35; i++) {
        browser_handle_key(SDLK_LEFT, 0);
        browser_handle_key(SDLK_UP, 0);
    }

    browser_cleanup();

    for (int i = 0; i < 35; i++) {
        unlink(imgs[i]);
        rmdir(subdirs[i]);
    }
    rmdir(base_dir);
}

static void test_browser_filter_sanitization(void) {
    browser_cleanup();
    browser_clear_filter();
    TEST_ASSERT_STR_EQ(browser_get_filter(), "");

    // Non-printable control characters must be rejected
    TEST_ASSERT(!browser_filter_add_char(0));
    TEST_ASSERT(!browser_filter_add_char('\n'));
    TEST_ASSERT(!browser_filter_add_char('\r'));
    TEST_ASSERT(!browser_filter_add_char('\t'));
    TEST_ASSERT(!browser_filter_add_char(27)); // ESC
    TEST_ASSERT(!browser_filter_add_char(127)); // DEL
    TEST_ASSERT(!browser_filter_add_char((char)-1));
    TEST_ASSERT(!browser_filter_add_char((char)-100));

    // Filter buffer should still be empty
    TEST_ASSERT_STR_EQ(browser_get_filter(), "");

    // Printable ASCII characters should be accepted
    TEST_ASSERT(browser_filter_add_char('t'));
    TEST_ASSERT(browser_filter_add_char('e'));
    TEST_ASSERT(browser_filter_add_char('s'));
    TEST_ASSERT(browser_filter_add_char('t'));
    TEST_ASSERT_STR_EQ(browser_get_filter(), "test");

    // Clear filter
    browser_clear_filter();
    TEST_ASSERT_STR_EQ(browser_get_filter(), "");

    // Test buffer capacity enforcement (buffer is 256 bytes, max 255 chars)
    for (int i = 0; i < 255; i++) {
        TEST_ASSERT(browser_filter_add_char('x'));
    }
    TEST_ASSERT_INT_EQ((int)strlen(browser_get_filter()), 255);

    // 256th character must be rejected without overflow
    TEST_ASSERT(!browser_filter_add_char('y'));
    TEST_ASSERT_INT_EQ((int)strlen(browser_get_filter()), 255);

    browser_clear_filter();
}

static void test_browser_symlink_loop_safety(void) {
    char base_dir[] = "/tmp/civ_symloop_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/dir_loop", base_dir);
    mkdir(sub, 0755);

    char img[PATH_MAX + 32];
    snprintf(img, sizeof(img), "%s/sample.png", sub);
    make_dummy_png(img);

    // Create a circular symlink pointing back to the directory itself
    char loop_link[PATH_MAX + 32];
    snprintf(loop_link, sizeof(loop_link), "%s/circular", sub);
    symlink(sub, loop_link);

    browser_set_root(base_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Expand dir_loop
    browser_handle_key(SDLK_RIGHT, 0);

    // Move to the circular link inside dir_loop and attempt to expand it
    browser_handle_key(SDLK_DOWN, 0);
    browser_handle_key(SDLK_RIGHT, 0);
    browser_handle_key(SDLK_RIGHT, 0);

    // Render overlay to verify stability
    browser_render(g_ren);

    browser_toggle();
    browser_cleanup();

    unlink(loop_link);
    unlink(img);
    rmdir(sub);
    rmdir(base_dir);
}

static void test_browser_root_path_safety(void) {
    browser_set_root("/");
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Render root folder overlay to ensure no buffer underflow / overflow or double slashes
    browser_render(g_ren);

    // Press left / backspace at root: must safely no-op without going above root or crashing
    browser_handle_key(SDLK_LEFT, 0);
    browser_handle_key(SDLK_BACKSPACE, 0);

    browser_toggle();
    browser_cleanup();
}

static void test_browser_empty_and_unreadable_directory(void) {
    char empty_dir[] = "/tmp/civ_bempty_XXXXXX";
    char *ed = mkdtemp(empty_dir);
    TEST_ASSERT(ed != NULL);

    browser_set_root(empty_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Keyboard events on empty directory
    browser_handle_key(SDLK_UP, 0);
    browser_handle_key(SDLK_DOWN, 0);
    browser_handle_key(SDLK_PAGEUP, 0);
    browser_handle_key(SDLK_PAGEDOWN, 0);
    browser_handle_key(SDLK_HOME, 0);
    browser_handle_key(SDLK_END, 0);
    browser_handle_key(SDLK_RETURN, 0);
    browser_handle_key(SDLK_SPACE, 0);
    browser_handle_key(SDLK_RIGHT, 0);

    // Mouse wheel events on empty directory
    SDL_Event wheel_up = {0};
    wheel_up.type = SDL_MOUSEWHEEL;
    wheel_up.wheel.y = 1;
    browser_handle_event(&wheel_up);

    SDL_Event wheel_down = {0};
    wheel_down.type = SDL_MOUSEWHEEL;
    wheel_down.wheel.y = -1;
    browser_handle_event(&wheel_down);

    // Mouse click events on empty directory
    SDL_Event click_inside = {0};
    click_inside.type = SDL_MOUSEBUTTONDOWN;
    click_inside.button.button = SDL_BUTTON_LEFT;
    click_inside.button.x = g_win_w / 2;
    click_inside.button.y = 100;
    browser_handle_event(&click_inside);

    // Render empty browser overlay
    browser_render(g_ren);

    // Navigate to parent directory from empty directory via Left / Backspace
    browser_handle_key(SDLK_LEFT, 0);

    // Test unreadable directory (permission 0000)
    char unreadable_dir[] = "/tmp/civ_bunread_XXXXXX";
    char *ud = mkdtemp(unreadable_dir);
    TEST_ASSERT(ud != NULL);
    chmod(unreadable_dir, 0000);

    browser_set_root(unreadable_dir);
    browser_render(g_ren);
    browser_handle_key(SDLK_DOWN, 0);

    // Restore permissions for cleanup
    chmod(unreadable_dir, 0755);
    rmdir(unreadable_dir);

    browser_toggle();
    browser_cleanup();
    rmdir(empty_dir);
}

static void test_browser_deeply_nested_directory_depth_limit(void) {
    char base_dir[] = "/tmp/civ_bdeep_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    // Create 36 levels of nested directories
    char current_path[PATH_MAX + 128];
    snprintf(current_path, sizeof(current_path), "%s", base_dir);
    char dir_chain[36][PATH_MAX + 128];

    for (int i = 0; i < 36; i++) {
        char next_path[PATH_MAX + 128];
        snprintf(next_path, sizeof(next_path), "%.4000s/l%02d", current_path, i);
        mkdir(next_path, 0755);
        snprintf(dir_chain[i], sizeof(dir_chain[i]), "%.4000s", next_path);
        snprintf(current_path, sizeof(current_path), "%.4000s", next_path);
    }

    // Place an image at innermost directory
    char inner_img[PATH_MAX + 128];
    snprintf(inner_img, sizeof(inner_img), "%.4000s/deep.png", current_path);
    make_dummy_png(inner_img);

    browser_set_root(base_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    // Attempt to expand beyond max depth limit (32)
    for (int i = 0; i < 36; i++) {
        browser_handle_key(SDLK_RIGHT, 0);
        browser_handle_key(SDLK_DOWN, 0);
    }

    // Render deeply nested tree safely
    browser_render(g_ren);

    // Collapse back
    for (int i = 0; i < 36; i++) {
        browser_handle_key(SDLK_LEFT, 0);
    }

    browser_toggle();
    browser_cleanup();

    // Clean up from deepest to shallowest
    unlink(inner_img);
    for (int i = 35; i >= 0; i--) {
        rmdir(dir_chain[i]);
    }
    rmdir(base_dir);
}

static void test_browser_filtering_performance(void) {
    char base_dir[] = "/tmp/civ_bperf_XXXXXX";
    char *bd = mkdtemp(base_dir);
    TEST_ASSERT(bd != NULL);

    // Create 300 image files
    const int nfiles = 300;
    char path[PATH_MAX];
    for (int i = 0; i < nfiles; i++) {
        snprintf(path, sizeof(path), "%s/photo_%04d.jpg", base_dir, i);
        make_dummy_png(path);
    }

    browser_set_root(base_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    Uint32 t0 = SDL_GetTicks();

    // Type filter "photo_01"
    browser_filter_add_char('p');
    browser_filter_add_char('h');
    browser_filter_add_char('o');
    browser_filter_add_char('t');
    browser_filter_add_char('o');
    browser_filter_add_char('_');
    browser_filter_add_char('0');
    browser_filter_add_char('1');

    TEST_ASSERT_STR_EQ(browser_get_filter(), "photo_01");

    // Render multiple frames with navigation
    for (int f = 0; f < 20; f++) {
        browser_render(g_ren);
        browser_handle_key(SDLK_DOWN, 0);
    }

    // Backspace 3 chars ("_01") -> "photo"
    for (int b = 0; b < 3; b++) {
        browser_handle_key(SDLK_BACKSPACE, 0);
    }
    TEST_ASSERT_STR_EQ(browser_get_filter(), "photo");

    for (int f = 0; f < 20; f++) {
        browser_render(g_ren);
        browser_handle_key(SDLK_UP, 0);
    }

    browser_clear_filter();
    TEST_ASSERT_STR_EQ(browser_get_filter(), "");

    Uint32 elapsed = SDL_GetTicks() - t0;
    printf("    [Benchmark] Filter 300 entries + 40 renders + navigation: %u ms\n", elapsed);

    browser_toggle();
    browser_cleanup();

    for (int i = 0; i < nfiles; i++) {
        snprintf(path, sizeof(path), "%s/photo_%04d.jpg", base_dir, i);
        unlink(path);
    }
    rmdir(base_dir);

    // Multi-thousand entry benchmark (2,500 entries)
    char multi_dir[] = "/tmp/civ_bperf_multi_XXXXXX";
    char *md = mkdtemp(multi_dir);
    TEST_ASSERT(md != NULL);
    const int n_multi = 2500;
    for (int i = 0; i < n_multi; i++) {
        snprintf(path, sizeof(path), "%s/photo_archive_%04d.jpg", multi_dir, i);
        int fd = creat(path, 0644);
        if (fd >= 0) close(fd);
    }

    browser_set_root(multi_dir);
    browser_toggle();
    TEST_ASSERT(browser_is_open());

    Uint64 t_m0 = SDL_GetPerformanceCounter();
    double perf_freq = (double)SDL_GetPerformanceFrequency();

    // Type filter "photo_archive_01"
    const char *fstr = "photo_archive_01";
    for (const char *fp = fstr; *fp; fp++) {
        browser_filter_add_char(*fp);
    }

    for (int f = 0; f < 20; f++) {
        browser_render(g_ren);
        browser_handle_key(SDLK_DOWN, 0);
    }

    for (int b = 0; b < 3; b++) {
        browser_handle_key(SDLK_BACKSPACE, 0);
    }

    for (int f = 0; f < 20; f++) {
        browser_render(g_ren);
        browser_handle_key(SDLK_UP, 0);
    }

    browser_clear_filter();

    Uint64 t_m1 = SDL_GetPerformanceCounter();
    double ms_multi = (double)(t_m1 - t_m0) * 1000.0 / perf_freq;
    printf("    [Benchmark] Filter 2500 entries + 40 renders + navigation: %.2f ms\n", ms_multi);

    browser_toggle();
    browser_cleanup();

    for (int i = 0; i < n_multi; i++) {
        snprintf(path, sizeof(path), "%s/photo_archive_%04d.jpg", multi_dir, i);
        unlink(path);
    }
    rmdir(multi_dir);
}

void run_browser_tests(void) {
    printf("--- Browser Test Suite ---\n");
    TEST_RUN(test_browser_lifecycle);
    TEST_RUN(test_browser_tree_navigation);
    TEST_RUN(test_browser_expand_collapse_stress);
    TEST_RUN(test_browser_filter_sanitization);
    TEST_RUN(test_browser_symlink_loop_safety);
    TEST_RUN(test_browser_root_path_safety);
    TEST_RUN(test_browser_empty_and_unreadable_directory);
    TEST_RUN(test_browser_deeply_nested_directory_depth_limit);
    TEST_RUN(test_browser_filtering_performance);
}
