# MUDL-C

> **Multi-threaded Universal Downloader in C** — 113KB single .exe, zero external dependencies.

Based on reverse engineering of [Neat Download Manager](https://www.neatdownloadmanager.com/) v1.4.24's dynamic multi-threading algorithm.

## Features

| Feature | Description |
|---------|-------------|
| ⚡ **Multi-thread** | Parallel segmented downloads, up to 32 connections |
| 🔄 **Segment RollBack** | Work-stealing: fast threads auto-steal from slow ones |
| ▶️ **Resume** | Both single & multi-thread. Survives URL changes (e.g. expired OSS tokens) |
| 🔒 **SChannel SSL** | Native Windows TLS (TLS 1.2/1.3), no OpenSSL needed |
| ☁️ **OSS signed URLs** | Compatible with Alibaba Cloud OSS / CDN time-limited signed URLs |
| 📦 **Single binary** | Statically linked MinGW-w64, runs on any Windows (Vista+) |
| 📊 **Progress** | Bar mode with ETA, or JSON output for scripting |

## Quick start

```bash
mudl "https://example.com/file.zip"                    # 8 threads (default)
mudl -c 1 "https://example.com/file.zip"                # single thread
mudl -c 16 -o output.zip "https://example.com/file.iso" # 16 threads + rename
```

## Resume with new URL

```bash
# First run (Ctrl+C to pause):
mudl -c 8 "https://oss-signed-url?token=abc"

# URL expired? Get a new one, resume automatically:
mudl -c 8 "https://oss-signed-url?token=xyz"
```

Works as long as the new URL points to the **same file** (same Content-Length). Progress saved every 3 seconds.

## Build from source

Requires MinGW-w64 (cross-compiler on Linux/WSL, or MSYS2 on Windows).

```bash
git clone https://github.com/daxiaamu/mudl-c.git
cd mudl-c
make
# output: mudl.exe (113KB)
```

Or manually:

```bash
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -mconsole \
  -o mudl.exe main.c http.c file_io.c progress.c utils.c \
  segment.c thread_pool.c persist.c \
  -lws2_32 -lshlwapi -lsecur32
strip mudl.exe
```

## Options

```
  -o, --output <FILE>       Output filename
  -d, --dir <DIR>           Output directory
  -c, --connections <N>     Threads (default 8, max 32)
  -q, --quiet               Silent mode (no progress)
  -p, --progress <FORMAT>   Progress format: bar | json | none
  -U, --user-agent <UA>     Custom User-Agent
      --referer <URL>       Custom Referer
      --header <K:V>        Custom HTTP header (repeatable)
      --timeout <SEC>       Connection timeout (default 30)
      --retry <N>           Max retries per segment (default 5)
  -h, --help                Show this help
  -V, --version             Show version
```

## Size comparison

| Downloader | Size | Dependencies |
|-----------|------|-------------|
| **MUDL** | **113 KB** | **None — single .exe** |
| NDM | 1.7 MB | .NET Runtime + DLLs |
| IDM | 15 MB | Browser extensions |
| aria2c | 5 MB | libcrypto, libssl, etc. |
| curl | 3 MB | OpenSSL DLL |

## Architecture

```
┌─ Download Engine ───────────────────────────┐
│  ┌─ Segment Manager ─────────────────────┐  │
│  │  ┌─ Segment[0] ←── Worker Thread 0 ─┐ │  │
│  │  ├─ Segment[1] ←── Worker Thread 1 ─┤ │  │
│  │  ├─ Segment[2] ←── Worker Thread 2 ─┤ │  │
│  │  └─ Segment[N] ←── Worker Thread N ─┘ │  │
│  │  RollBack: fast workers steal work     │  │
│  │  from slow workers (NDM algorithm)     │  │
│  └────────────────────────────────────────┘  │
│  Persist: segments.bin for resume support    │
└──────────────────────────────────────────────┘
```

## Project structure

```
main.c           Entry point, argument parsing, engine orchestration
http.c / .h      HTTP/HTTPS client (SChannel SSL)
file_io.c / .h   Thread-safe file I/O (OVERLAPPED)
segment.c / .h   Segment manager with RollBack (work-stealing)
thread_pool.c/h  Worker thread pool
progress.c / .h  Progress bar (ANSI) & JSON output
persist.c / .h   Resume state persistence
utils.c / .h     Formatting helpers (bytes, speed, time)
```

## License

MIT
