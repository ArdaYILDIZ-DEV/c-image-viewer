#include "test_common.h"
#include "viewer.h"
#include <SDL2/SDL.h>

int g_tests_run = 0;
int g_tests_failed = 0;

void run_exif_tests(void);
void run_text_tests(void);
void run_clipboard_tests(void);
void run_viewer_tests(void);
void run_browser_tests(void);

int main(void) {
    printf("========================================\n");
    printf(" c-image-viewer Test Suite\n");
    printf("========================================\n");

    // Headless SDL initialization
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "Fatal: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    g_win = SDL_CreateWindow("Test Window", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
    if (!g_win) {
        fprintf(stderr, "Fatal: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) {
        fprintf(stderr, "Fatal: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return 1;
    }

    // Run test suites
    run_exif_tests();
    run_text_tests();
    run_clipboard_tests();
    run_viewer_tests();
    run_browser_tests();

    // Cleanup
    SDL_DestroyRenderer(g_ren);
    g_ren = NULL;
    SDL_DestroyWindow(g_win);
    g_win = NULL;
    SDL_Quit();

    printf("========================================\n");
    printf(" Summary: %d run, %d failed, %d passed\n",
        g_tests_run, g_tests_failed, g_tests_run - g_tests_failed);
    printf("========================================\n");

    return g_tests_failed == 0 ? 0 : 1;
}
