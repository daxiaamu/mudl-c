# MUDL-C

[![CI](https://github.com/daxiaamu/mudl-c/actions/workflows/ci.yml/badge.svg)](https://github.com/daxiaamu/mudl-c/actions/workflows/ci.yml)

See [CHANGELOG.md](CHANGELOG.md) for release history.

Every CI run builds and tests the Windows executable. Successful runs provide
a `mudl-windows-x64` artifact, and CI can also be started manually from the
Actions page.

> **Nano CLI download manager for Windows** - a small, dependency-free C downloader built on Win32 and SChannel.

## Features

| Feature | Description |
|---------|-------------|
| Multi-thread downloads | Parallel HTTP range downloads, up to 32 connections |
| Strict resume | Validates segment CRC32 plus the remote ETag/Last-Modified identity |
| Signed URLs | Handles long redirect URLs used by cloud/CDN signed links |
| Native HTTPS | Uses Windows SChannel, no bundled OpenSSL |
| UTF-8 paths | Supports non-ASCII output filenames through Windows wide APIs |
| Progress output | Console progress bar, log-friendly lines, or JSON output |

## Quick Start

```bash
mudl "https://example.com/file.zip"
mudl -c 1 "https://example.com/file.zip"
mudl -c 16 -d downloads -o output.zip "https://example.com/file.iso"
mudl --progress line "https://example.com/file.zip"
```

`-d` and `-o` follow aria2-style output rules: `-d` selects the output directory, and `-o` selects the output filename only. Use `-d "D:\Downloads" -o file.zip` instead of passing a full path to `-o`.

## Resume With New URL

```bash
# First run, press Ctrl+C to pause:
mudl -c 8 "https://example.com/file.zip"

# If the signed URL changes, pass the new URL:
mudl -c 8 "https://cdn.example.com/file.zip?token=abc"
```

Resume works when the new URL returns the same file size and remote validator (`ETag`, or `Last-Modified` when no ETag is available). Existing bytes are also verified against each saved segment CRC32. If the server provides no stable validator, MUDL restarts instead of trusting a size-only match.

## HTTPS Notes

MUDL-C uses the system SChannel TLS stack. HTTPS support depends on the Windows TLS configuration, certificate store, and any local network interception software. On Windows 7, modern HTTPS sites may require TLS 1.2 updates, TLS 1.2 enabled in the system, and up-to-date root certificates.

`--timeout` controls each network connect/read attempt. If the initial range probe cannot get a response, MUDL stops immediately instead of waiting for a second full download attempt.

## Build From Source

Requires MinGW-w64, either cross-compiled from Linux/WSL or built from MSYS2/MinGW on Windows.

```bash
git clone https://github.com/daxiaamu/mudl-c.git
cd mudl-c
make
# Optional C unit tests and local HTTP integration tests (requires Python 3):
make test
```

Manual build:

```bash
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -mconsole \
  -o mudl.exe main.c options.c engine.c http.c url.c schannel.c file_io.c progress.c utils.c \
  segment.c thread_pool.c persist.c checksum.c \
  -lws2_32 -lshlwapi -lsecur32 -lshell32 -ladvapi32
strip mudl.exe
```

## Options

```text
  -o,  --output <FILE>      Output filename
  -d,  --dir <DIR>          Output directory
  -c,  --connections <N>    Threads (default 8, max 32)
  -q,  --quiet              Hide detail logs, keep progress
  -p,  --progress <FORMAT>  Progress format: bar | line | json | none
  -ua, --user-agent <UA>    Custom User-Agent
       --referer <URL>      Custom Referer
       --header <K:V>       Custom HTTP header (repeatable)
       --timeout <SEC>      Connection timeout (default 30)
       --retries <N>        Max retries per segment (default 5)
       --checksum <TYPE=DIGEST> Verify file checksum after download
       --proxy <PROXY>      Alias for --all-proxy
       --all-proxy <PROXY>  Proxy for all HTTP(S) downloads
       --http-proxy <PROXY> Proxy for HTTP downloads
       --https-proxy <PROXY> Proxy for HTTPS downloads
       --no-proxy <LIST>    Comma-separated hosts/domains/IP ranges
  -h,  --help               Show help
  -V,  --version            Show version
```

Use `-q`/`--quiet` for scripts that should hide detail logs while keeping progress visible. Use `--progress none` for fully silent downloads. Use `--progress line` for background jobs or log files. It prints periodic full lines instead of rewriting the same console line.

Custom headers may contain spaces when quoted by the shell, for example `--header "X-Test: hello world"`.

## Checksum

`--checksum` follows aria2 syntax: `--checksum=<TYPE>=<DIGEST>`.

```bash
mudl --checksum=sha-256=012345... "https://example.com/file.zip"
mudl --checksum sha-256=012345... "https://example.com/file.zip"
```

The space-separated form is also accepted for convenience, but the aria2-compatible form uses `=`.

Supported types: `md5`, `sha-1`/`sha1`, `sha-256`/`sha256`, `sha-384`/`sha384`, and `sha-512`/`sha512`.

Checksum verification runs after a complete download. A mismatch returns a non-zero exit code.

## Proxy

MUDL supports lightweight HTTP proxies without external dependencies:

```bash
mudl --proxy http://127.0.0.1:7890 "https://example.com/file.zip"
mudl --http-proxy http://127.0.0.1:8080 "http://example.com/file.zip"
mudl --https-proxy http://user:pass@127.0.0.1:7890 "https://example.com/file.zip"
```

Proxy options:

```text
       --proxy <PROXY>       Alias for --all-proxy
       --all-proxy <PROXY>   Proxy for all HTTP(S) downloads
       --http-proxy <PROXY>  Proxy for HTTP downloads
       --https-proxy <PROXY> Proxy for HTTPS downloads
       --no-proxy <LIST>     Comma-separated hosts/domains/IP ranges
```

`PROXY` uses aria2-style syntax: `[http://][USER:PASSWORD@]HOST[:PORT]`.

`--no-proxy` accepts exact hosts, leading-dot domains, and numeric IPv4 CIDR ranges. Examples: `localhost,127.0.0.1,.daxiaamu.com,192.168.0.0/16`.

HTTPS downloads through an HTTP proxy use `CONNECT` tunneling. TLS is still handled by Windows SChannel after the proxy tunnel is established.

## Project Structure

```text
main.c            Process entry, console setup, and signal forwarding
options.c / .h    Command-line parsing, option model, and defaults
engine.c / .h     Download probing, orchestration, and completion checks
url.c / .h        URL, proxy, and no-proxy parsing
http.c / .h       HTTP request, response, and transport orchestration
schannel.c / .h   Windows SChannel TLS handshake and encrypted I/O
file_io.c / .h    Thread-safe file I/O and UTF-8 path handling
segment.c / .h    Segment manager
thread_pool.c/.h  Worker thread pool
progress.c / .h   Progress bar, line, and JSON output
persist.c / .h    Resume state and CRC32 validation
checksum.c / .h   Download checksum verification
utils.c / .h      Formatting, tracing, and checksum helpers
tests/             C unit tests and HTTP integration tests
```

## License

[MIT](LICENSE)
