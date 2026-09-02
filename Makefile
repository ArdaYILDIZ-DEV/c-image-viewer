CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS := -lSDL2 -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
DESKTOPDIR ?= $(DATADIR)/applications

TARGET  := viewer
SRC     := main.c viewer.c browser.c text.c
OBJ     := $(SRC:.c=.o)
HEADERS := viewer.h browser.h text.h stb_image.h font8x8.h

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

install: $(TARGET) c-image-viewer.desktop
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/c-image-viewer
	install -Dm644 c-image-viewer.desktop $(DESTDIR)$(DESKTOPDIR)/c-image-viewer.desktop
	@echo "Installed to $(DESTDIR)$(PREFIX)"
	@echo "Run 'update-desktop-database $(DESTDIR)$(DATADIR)/applications' if needed"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/c-image-viewer
	rm -f $(DESTDIR)$(DESKTOPDIR)/c-image-viewer.desktop

.PHONY: all clean install uninstall
