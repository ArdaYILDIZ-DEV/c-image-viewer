CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wpedantic -std=c11
INCLUDES := -Iinclude -Iexternal
LDFLAGS  ?=
LDLIBS   := -lSDL2 -lm

BUILD_DIR    ?= build
BIN_DIR      := $(BUILD_DIR)
OBJ_DIR      := $(BUILD_DIR)/src
TEST_OBJ_DIR := $(BUILD_DIR)/tests

TARGET      := $(BIN_DIR)/viewer
TEST_TARGET := $(BIN_DIR)/test_runner

SRCS    := src/main.c src/viewer.c src/browser.c src/text.c src/exif.c src/clipboard.c
OBJS    := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
HEADERS := include/viewer.h include/browser.h include/text.h include/exif.h include/clipboard.h \
           external/stb_image.h external/stb_image_write.h external/font8x8.h

TEST_SRCS     := tests/test_main.c tests/test_exif.c tests/test_text.c tests/test_clipboard.c \
                 tests/test_viewer.c tests/test_browser.c
TEST_OBJS     := $(patsubst tests/%.c,$(TEST_OBJ_DIR)/%.o,$(TEST_SRCS))
TEST_APP_SRCS := src/viewer.c src/browser.c src/text.c src/exif.c src/clipboard.c
TEST_APP_OBJS := $(patsubst src/%.c,$(TEST_OBJ_DIR)/app_%.o,$(TEST_APP_SRCS))
ALL_TEST_OBJS := $(TEST_OBJS) $(TEST_APP_OBJS)

TEST_CFLAGS  ?= $(CFLAGS) -fsanitize=address,undefined -g
TEST_LDFLAGS ?= $(LDFLAGS) -fsanitize=address,undefined

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
DESKTOPDIR ?= $(DATADIR)/applications
ICONDIR    ?= $(DATADIR)/icons/hicolor/48x48/apps

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@ln -sf $(TARGET) viewer

$(OBJ_DIR)/%.o: src/%.c $(HEADERS) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_OBJ_DIR)/%.o: tests/%.c $(HEADERS) tests/test_common.h | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -Itests -c $< -o $@

$(TEST_OBJ_DIR)/app_%.o: src/%.c $(HEADERS) | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_TARGET): $(ALL_TEST_OBJS) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(ALL_TEST_OBJS) -o $@ $(TEST_LDFLAGS) $(LDLIBS)
	@ln -sf $(TEST_TARGET) test_runner

$(BIN_DIR) $(OBJ_DIR) $(TEST_OBJ_DIR):
	mkdir -p $@

test: $(TEST_TARGET)
	SDL_VIDEODRIVER=dummy ./$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR) viewer test_runner

install: $(TARGET) assets/c-image-viewer.desktop
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/c-image-viewer
	install -Dm644 assets/c-image-viewer.desktop $(DESTDIR)$(DESKTOPDIR)/c-image-viewer.desktop
	install -d $(DESTDIR)$(ICONDIR)
	./$(TARGET) --dump-icon $(DESTDIR)$(ICONDIR)/c-image-viewer.png
	chmod 644 $(DESTDIR)$(ICONDIR)/c-image-viewer.png
	@echo "Installed to $(DESTDIR)$(PREFIX)"
	@echo "Run 'update-desktop-database $(DESTDIR)$(DATADIR)/applications' if needed"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/c-image-viewer
	rm -f $(DESTDIR)$(DESKTOPDIR)/c-image-viewer.desktop
	rm -f $(DESTDIR)$(ICONDIR)/c-image-viewer.png

.PHONY: all clean install uninstall test
