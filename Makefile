# MUDL Makefile
HOST ?= x86_64-w64-mingw32
CC = $(HOST)-gcc
ifeq ($(OS),Windows_NT)
RC = windres
else
RC = $(HOST)-windres
endif
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-stringop-truncation -Wno-implicit-fallthrough -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1 -mconsole
DEPFLAGS = -MMD -MP
LDFLAGS = -lws2_32 -lshlwapi -lsecur32 -lshell32 -ladvapi32
SRCS = main.c options.c engine.c http.c url.c oss_ticket.c source.c schannel.c file_io.c progress.c utils.c segment.c thread_pool.c persist.c checksum.c
OBJS = $(SRCS:.c=.o)
RES_OBJ = mudl_res.o
DEPS = $(OBJS:.o=.d)
TARGET = mudl.exe
ifeq ($(OS),Windows_NT)
RM = del /Q /F
RM_TARGETS = $(subst /,\,$(OBJS) $(RES_OBJ) $(DEPS) $(TARGET))
else
RM = rm -f
RM_TARGETS = $(OBJS) $(RES_OBJ) $(DEPS) $(TARGET)
endif

.PHONY: all clean test unit-test integration-test https-test

all: $(TARGET)

$(TARGET): $(OBJS) $(RES_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	strip $@

$(RES_OBJ): mudl.rc assets/mudl-experimental.ico
	$(RC) -O coff -i mudl.rc -o $@

.c.o:
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	-$(RM) $(RM_TARGETS)

unit-test:
	$(CC) $(CFLAGS) -I. -o tests/test_segment.exe tests/test_segment.c segment.c utils.c -lws2_32
	./tests/test_segment.exe
	$(CC) $(CFLAGS) -I. -o tests/test_options.exe tests/test_options.c options.c url.c utils.c -lws2_32
	./tests/test_options.exe
	$(CC) $(CFLAGS) -I. -o tests/test_url.exe tests/test_url.c url.c -lws2_32
	./tests/test_url.exe

integration-test: $(TARGET)
	python tests/test_integration.py

https-test: $(TARGET)
	python tests/test_https.py tests/tls-cert.pem tests/tls-key.pem

test: unit-test integration-test

-include $(DEPS)
