#!/bin/bash
set -e
HOST=x86_64-w64-mingw32
CC=$HOST-gcc
DST=/mnt/e/Hermes/MUDM
cd /tmp
mkdir -p mudm_build && cd mudm_build
cp "$DST"/*.c "$DST"/*.h .
$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-stringop-truncation -Wno-implicit-fallthrough -DWIN32_LEAN_AND_MEAN -mconsole \
    -o mudm.exe main.c http.c file_io.c progress.c utils.c segment.c thread_pool.c \
    -lws2_32 -lshlwapi -lsecur32
cp mudm.exe "$DST"/
echo "Build OK: $DST/mudm.exe"
