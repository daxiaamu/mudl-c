# MUDL-C

[![CI](https://github.com/daxiaamu/mudl-c/actions/workflows/ci.yml/badge.svg)](https://github.com/daxiaamu/mudl-c/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/daxiaamu/mudl-c?display_name=tag&sort=semver)](https://github.com/daxiaamu/mudl-c/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/daxiaamu/mudl-c/total)](https://github.com/daxiaamu/mudl-c/releases)
[![License](https://img.shields.io/github/license/daxiaamu/mudl-c)](LICENSE)
[![Stars](https://img.shields.io/github/stars/daxiaamu/mudl-c?style=flat)](https://github.com/daxiaamu/mudl-c/stargazers)
[![Forks](https://img.shields.io/github/forks/daxiaamu/mudl-c?style=flat)](https://github.com/daxiaamu/mudl-c/forks)
[![Issues](https://img.shields.io/github/issues/daxiaamu/mudl-c)](https://github.com/daxiaamu/mudl-c/issues)
[![Last Commit](https://img.shields.io/github/last-commit/daxiaamu/mudl-c)](https://github.com/daxiaamu/mudl-c/commits/main)
[![Contributors](https://img.shields.io/github/contributors/daxiaamu/mudl-c)](https://github.com/daxiaamu/mudl-c/graphs/contributors)
[![Repo Size](https://img.shields.io/github/repo-size/daxiaamu/mudl-c)](https://github.com/daxiaamu/mudl-c)

![C](https://img.shields.io/badge/language-C11-00599C?logo=c&logoColor=white)
![Windows](https://img.shields.io/badge/platform-Windows-0078D6?logo=windows&logoColor=white)
![Windows 7+](https://img.shields.io/badge/compatibility-Windows%207%2B-0078D6)
![Architecture](https://img.shields.io/badge/architecture-x86%20%7C%20x64-555555)
![SChannel](https://img.shields.io/badge/TLS-SChannel-2F6DB0)
![HTTP](https://img.shields.io/badge/protocol-HTTP%20%7C%20HTTPS-2EA44F)
![Connections](https://img.shields.io/badge/connections-up%20to%2032-FF8C00)
![Mirrors](https://img.shields.io/badge/mirror%20sources-up%20to%2016-8A2BE2)
![Dependencies](https://img.shields.io/badge/runtime%20dependencies-none-success)
![CLI](https://img.shields.io/badge/interface-CLI-black)

See [CHANGELOG.md](CHANGELOG.md) for release history.

Every CI run builds and tests the Windows executable. Successful runs provide
a `mudl-windows-x64` artifact, and CI can also be started manually from the
Actions page.

> **Nano CLI download manager for Windows** - a small, dependency-free C downloader built on Win32 and SChannel.

[【机场推荐：白月光，稳定高速】](https://www.sibker.com/register?invite_code=2XQR1UUz)

## Features

| Feature | Description |
|---------|-------------|
| Multi-thread downloads | Parallel HTTP range downloads, up to 32 connections |
| Multiple sources | Downloads one file from up to 16 mirror URLs with automatic failover |
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
mudl -o file.zip "https://host/file.zip" "https://mirror/file.zip"
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
  -o mudl.exe main.c options.c engine.c http.c url.c source.c schannel.c file_io.c progress.c utils.c \
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
       --retries <N>        Retries after failure (default 5, max 100)
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

`--retries N` allows up to `N` additional attempts after the initial attempt;
`0` disables retries. The accepted range is 0-100. Range downloads retain
verified progress. If a server does not support ranges, a failed transfer is
restarted from byte 0 so separate responses are never joined into a corrupt
file.

## Multiple Sources

Like aria2, multiple URL arguments are treated as sources for one file:

```bash
mudl -c 16 -o firmware.zip \
  "https://server.example/firmware.zip" \
  "https://mirror.example/firmware.zip"
```

MUDL checks every source before downloading. Sources must support byte ranges
and must have the same file size plus matching 64 KiB samples from the start
and end of the file. The first usable URL establishes the expected identity;
unavailable or mismatched sources are skipped. Workers
rotate across accepted sources; if one expires, disconnects, or returns an
invalid range, the same segment is continued from another source before its
normal retry count is consumed.

Up to 16 source URLs are accepted. This syntax does not create separate
download tasks. For high-value files, use `--checksum` as the final
whole-file integrity check.

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
source.c / .h     Mirror probing, identity sampling, and source state
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

Copyright (C) 2026 daxiaamu.

Licensed under the [GNU General Public License v3.0 only](LICENSE)
(`GPL-3.0-only`).
