# MUDM Makefile
HOST ?= x86_64-w64-mingw32
CC = $(HOST)-gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-stringop-truncation -Wno-implicit-fallthrough -DWIN32_LEAN_AND_MEAN -mconsole
LDFLAGS = -lws2_32 -lshlwapi -lsecur32
SRCS = main.c http.c file_io.c progress.c utils.c segment.c thread_pool.c
OBJS = $(SRCS:.c=.o)
TARGET = mudm.exe

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
