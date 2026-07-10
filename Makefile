# MUDL Makefile
HOST ?= x86_64-w64-mingw32
CC = $(HOST)-gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-stringop-truncation -Wno-implicit-fallthrough -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1 -mconsole
LDFLAGS = -lws2_32 -lshlwapi -lsecur32 -lshell32 -ladvapi32
SRCS = main.c http.c file_io.c progress.c utils.c segment.c thread_pool.c persist.c checksum.c
OBJS = $(SRCS:.c=.o)
TARGET = mudl.exe
ifeq ($(OS),Windows_NT)
RM = del /Q /F
RM_TARGETS = $(subst /,\,$(OBJS) $(TARGET))
else
RM = rm -f
RM_TARGETS = $(OBJS) $(TARGET)
endif

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip $@

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	-$(RM) $(RM_TARGETS)

test: $(TARGET)
	python tests/test_integration.py
