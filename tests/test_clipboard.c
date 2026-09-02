#include "test_common.h"
#include "clipboard.h"
#include "stb_image_write.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static void test_clipboard_guards(void) {
    // Calling with NULL must return false without crash
    TEST_ASSERT(!clipboard_copy_text(NULL));
    TEST_ASSERT(!clipboard_copy_path(NULL));
    TEST_ASSERT(!clipboard_copy_rgba(NULL, 0, 0, NULL));
    TEST_ASSERT(!clipboard_copy_rgba(NULL, 10, 10, NULL));

    unsigned char dummy[16] = {0};
    TEST_ASSERT(!clipboard_copy_rgba(dummy, 0, 4, NULL));
    TEST_ASSERT(!clipboard_copy_rgba(dummy, 4, 0, NULL));
    TEST_ASSERT(!clipboard_copy_rgba(dummy, -1, 4, NULL));
}

static void test_clipboard_nonexistent_path(void) {
    // Non-existent path should fail gracefully or fallback to text safely
    clipboard_copy_path("/tmp/civ_test_nonexistent_clip.png");
}

static void test_clipboard_has_tool(void) {
    // Should return a valid boolean without crash or leak
    bool has_tool = clipboard_has_image_tool();
    (void)has_tool;
}

static void test_clipboard_paste_leak_safety(void) {
    // In headless test environment without display, paste might return NULL or a valid temp path.
    // If it returns a string, caller is responsible for freeing and unlinking.
    char *p = clipboard_paste_to_temp();
    if (p) {
        unlink(p);
        free(p);
    }
}

static void test_clipboard_command_injection_safety(void) {
    const char *canary = "/tmp/civ_inj_canary_test1";
    unlink(canary);

    // Array of malicious paths and text payloads designed to execute commands if passed to a shell
    const char *payloads[] = {
        "/tmp/civ_test;touch /tmp/civ_inj_canary_test1;.png",
        "/tmp/civ_test'$(touch /tmp/civ_inj_canary_test1)'\".png",
        "/tmp/civ_test`touch /tmp/civ_inj_canary_test1`.png",
        "/tmp/civ_test && touch /tmp/civ_inj_canary_test1 && echo .png",
        "/tmp/civ_test | touch /tmp/civ_inj_canary_test1 | cat.png",
        "/tmp/civ_test\ntouch /tmp/civ_inj_canary_test1\n.png",
        "foo'bar$(whoami).png",
        "test;rm -rf /;",
        NULL
    };

    for (int i = 0; payloads[i]; i++) {
        clipboard_copy_path(payloads[i]);
        TEST_ASSERT(access(canary, F_OK) != 0);
    }

    const char *text_payloads[] = {
        "'; touch /tmp/civ_inj_canary_test1; echo '",
        "$(touch /tmp/civ_inj_canary_test1)",
        "`touch /tmp/civ_inj_canary_test1`",
        "\ntouch /tmp/civ_inj_canary_test1\n",
        "test;touch /tmp/civ_inj_canary_test1;",
        NULL
    };

    for (int i = 0; text_payloads[i]; i++) {
        clipboard_copy_text(text_payloads[i]);
        TEST_ASSERT(access(canary, F_OK) != 0);
    }

    unlink(canary);
}

static void test_clipboard_copy_real_image_with_shell_chars(void) {
    const char *canary = "civ_inj_canary2";
    unlink(canary);

    // Create an actual image file with dangerous shell characters in its name
    // If passed to a shell inside single quotes like 'path'$(cmd)'path', the shell would execute $(cmd)
    const char *evil_img = "/tmp/civ_test_evil_'$(touch civ_inj_canary2)';foo.png";
    unsigned char pixel[4] = {255, 128, 0, 255};
    TEST_ASSERT(stbi_write_png(evil_img, 1, 1, 4, pixel, 4));

    // Attempt to copy image to clipboard
    clipboard_copy_path(evil_img);

    // Verify the injected command was NEVER executed
    TEST_ASSERT(access(canary, F_OK) != 0);

    unlink(evil_img);
    unlink(canary);
}

static void test_clipboard_copy_rgba_execution(void) {
    unsigned char pixels[16 * 16 * 4];
    for (size_t i = 0; i < sizeof(pixels); i++) pixels[i] = (unsigned char)(i & 0xFF);

    // Valid RGBA copy
    clipboard_copy_rgba(pixels, 16, 16, NULL);

    // Fallback path check with NULL pixels
    clipboard_copy_rgba(NULL, 0, 0, "/tmp/civ_nonexistent_fallback.png");
}

void run_clipboard_tests(void) {
    printf("--- Clipboard Test Suite ---\n");
    TEST_RUN(test_clipboard_guards);
    TEST_RUN(test_clipboard_nonexistent_path);
    TEST_RUN(test_clipboard_has_tool);
    TEST_RUN(test_clipboard_paste_leak_safety);
    TEST_RUN(test_clipboard_command_injection_safety);
    TEST_RUN(test_clipboard_copy_real_image_with_shell_chars);
    TEST_RUN(test_clipboard_copy_rgba_execution);
}
