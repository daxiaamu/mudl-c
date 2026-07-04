# MUDL-C

> **Nano CLI download manager for Windows** - a small, dependency-free C downloader built on Win32 and SChannel.

## Features

| Feature | Description |
|---------|-------------|
| Multi-thread downloads | Parallel HTTP range downloads, up to 32 connections |
| Strict resume | Resume state includes segment layout and CRC32 validation |
| Signed URLs | Handles long redirect URLs used by cloud/CDN signed links |
| Native HTTPS | Uses Windows SChannel, no bundled OpenSSL |
| UTF-8 paths | Supports non-ASCII output filenames through Windows wide APIs |
| Progress output | Console progress bar or JSON output for scripting |

## Quick Start

```bash
mudl "https://example.com/file.zip"
mudl -c 1 "https://example.com/file.zip"
mudl -c 16 -o output.zip "https://example.com/file.iso"
```

## Resume With New URL

```bash
# First run, press Ctrl+C to pause:
mudl -c 8 "https://example.com/file.zip"

# If the signed URL changes, pass the new URL:
mudl -c 8 "https://cdn.example.com/file.zip?token=abc"
```

Resume works when the new URL points to the same file size. Existing data is verified against the saved segment CRC32 before download continues.

## Windows 7 HTTPS Note

MUDL-C uses the system SChannel TLS stack. On Windows 7, modern HTTPS sites may require TLS 1.2 updates, TLS 1.2 enabled in the system, and up-to-date root certificates. If the system TLS stack is too old, HTTPS handshakes can fail before any download starts.

## Build From Source

Requires MinGW-w64, either cross-compiled from Linux/WSL or built from MSYS2/MinGW on Windows.

```bash
git clone https://github.com/daxiaamu/mudl-c.git
cd mudl-c
make
```

Manual build:

```bash
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -mconsole \
  -o mudl.exe main.c http.c file_io.c progress.c utils.c \
  segment.c thread_pool.c persist.c \
  -lws2_32 -lshlwapi -lsecur32
strip mudl.exe
```

## Options

```text
  -o, --output <FILE>       Output filename
  -d, --dir <DIR>           Output directory
  -c, --connections <N>     Threads (default 8, max 32)
  -q, --quiet               Silent mode
  -p, -progress <FORMAT>    Progress format: bar | json | none
  -ua, --user-agent <UA>    Custom User-Agent
      --referer <URL>       Custom Referer
      --header <K:V>        Custom HTTP header (repeatable)
      -timeout <SEC>        Connection timeout (default 30)
      -retries <N>          Max retries per segment (default 5)
  -h, --help                Show help
  -V, --version             Show version
```

## Project Structure

```text
main.c            Entry point, argument parsing, engine orchestration
http.c / .h       HTTP/HTTPS client using SChannel
file_io.c / .h    Thread-safe file I/O and UTF-8 path handling
segment.c / .h    Segment manager
thread_pool.c/.h  Worker thread pool
progress.c / .h   Progress bar and JSON output
persist.c / .h    Resume state and CRC32 validation
utils.c / .h      Formatting, tracing, and checksum helpers
```

## License

MIT
