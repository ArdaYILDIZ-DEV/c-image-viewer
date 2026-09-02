CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS := -lSDL2 -lm

TARGET  := viewer
SRC     := main.c

all: $(TARGET)

$(TARGET): $(SRC) stb_image.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
