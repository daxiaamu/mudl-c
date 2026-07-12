#!/bin/bash
set -e
HOST=x86_64-w64-mingw32
CC=$HOST-gcc
DST=${DST:-/mnt/e/Hermes/MUDL}
cd /tmp
mkdir -p mudl_build && cd mudl_build
cp "$DST"/*.c "$DST"/*.h .
$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-stringop-truncation -Wno-implicit-fallthrough -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0600 -D__USE_MINGW_ANSI_STDIO=1 -mconsole \
    -o mudl.exe main.c options.c engine.c http.c file_io.c progress.c utils.c segment.c thread_pool.c persist.c checksum.c \
    -lws2_32 -lshlwapi -lsecur32 -lshell32 -ladvapi32
cp mudl.exe "$DST"/
echo "Build OK: $DST/mudl.exe"
