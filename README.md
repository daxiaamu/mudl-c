# MUDL-C — Multi-threaded Universal Downloader in C

**The world's smallest downloader core.** Single 113KB static .exe, zero external dependencies.

## Features

- **Multi-thread** — Parallel segmented downloads with dynamic thread adjustment
- **Segment RollBack** — Work-stealing load balancing (fast threads steal from slow ones)
- **Resume support** — Both single-thread and multi-thread, survives URL changes
- **SChannel SSL** — Native Windows TLS, no OpenSSL dependency
- **OSS signed URLs** — Compatible with Alibaba Cloud OSS / CDN time-limited URLs
- **Single-file exe** — Statically linked MinGW-w64, runs on any Windows

## Build

Requires MinGW-w64 cross-compiler (Linux/WSL) or MSYS2.

```bash
make
# or manually:
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -mconsole \
  -o mudl.exe main.c http.c file_io.c progress.c utils.c \
  segment.c thread_pool.c persist.c \
  -lws2_32 -lshlwapi -lsecur32
strip mudl.exe
```

## Usage

```bash
mudl "https://example.com/file.zip"              # 8 threads (default)
mudl -c 16 "https://example.com/file.iso"        # 16 threads
mudl -c 1 "https://example.com/small.bin"        # single thread
mudl -o output.zip "https://example.com/file"    # custom filename
mudl -q -p json "https://example.com/file"       # JSON progress
```

### Resume with new URL

```bash
# First run (Ctrl+C to pause):
mudl -c 8 "https://old-signed-url"

# Get new URL, same file, resume automatically:
mudl -c 8 "https://new-signed-url"
```

## Architecture

```
┌─ NeatDownloadEngine ─────────────────────┐
│  ┌─ NeatSegmentManager ───────────────┐  │
│  │  ┌─ Segment[0] ─── ThreadPool ──┐  │  │
│  │  ├─ Segment[1] ─── Worker 1 ────┤  │  │
│  │  ├─ Segment[2] ─── Worker 2 ────┤  │  │
│  │  └─ Segment[N] ─── Worker N ────┘  │  │
│  └────────────────────────────────────┘  │
│  RollBack: fast workers steal from slow  │
└──────────────────────────────────────────┘
```

## Size comparison

| Downloader | Size | Notes |
|-----------|------|-------|
| **MUDL** | **113 KB** | Single exe, no deps |
| NDM | 1.7 MB | .NET + DLLs |
| IDM | 15 MB | + browser extensions |
| aria2c | 5 MB | + lib dependencies |
| curl | 3 MB | + OpenSSL DLL |

## Credits

Based on reverse engineering of Neat Download Manager (NDM) v1.4.24's dynamic multi-threading algorithm.

