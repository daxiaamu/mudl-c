# Changelog

All notable changes to MUDL-C are documented in this file.

## [0.5.16-experimental] - 2026-07-14

### Added

- Added experimental renewable download tickets for supported OPPO, OnePlus,
  ColorOS, allawntech, and allawnos OTA endpoints.
- Added CRC32 combination so verified contiguous resume ranges can be compacted
  without reading the downloaded data a second time.
- Added direct tests for retry concurrency limits, CRC32 combination, and resume
  range compaction before repartitioning.

### Fixed

- Prevented resume metadata from repeatedly accumulating completed segment
  boundaries after failed downloads and restarts.
- Enforced the configured connection count as a hard limit when retrying or
  reacquiring pending ranges, including the maximum of 32 connections.
- Distinguished internal resume ranges from worker threads in status messages.
- Displayed progress threads as active/configured, such as `T:1/32`.

## [0.5.11] - 2026-07-13

### Changed

- Moved URL, proxy, and no-proxy parsing into a dedicated module with direct
  C unit tests.
- Added Windows SChannel integration coverage for a trusted public HTTPS
  download and self-signed certificate rejection.
- Moved the SChannel handshake and encrypted I/O state machine into a dedicated
  module with an opaque TLS context owned by the HTTP client.
- Added automatic Makefile header dependency tracking for reliable incremental
  rebuilds after `.h` changes.

## [0.5.10] - 2026-07-12

### Added

- Manual GitHub Actions runs through `workflow_dispatch`.
- A tested `mudl.exe` Actions artifact after successful CI runs.
- A dedicated command-line option module and direct parser unit tests.
- A dedicated download engine module, leaving `main.c` focused on process setup.
- Engine integration coverage for redirect loops, empty responses, malformed
  status lines, checksum failures, and invalid absolute `--output` paths.

## [0.5.9] - 2026-07-12

### Added

- Resume segment expansion when restarting with a higher connection limit.
- Segmented single-connection resume, preserving all verified ranges when
  lowering `--connections` to 1.
- C unit tests for segment initialization, expansion, queue coverage, and
  downloaded-byte accounting.
- GitHub Actions CI for MinGW builds, C unit tests, and HTTP integration tests.
- More specific WinSock and SChannel error diagnostics.

### Fixed

- Completed resume segments being restored as pending work, which could cause
  duplicate downloads, incorrect percentages, or totals above the file size.
- Previously downloaded ranges being ignored when a multi-connection download
  was resumed with one connection.

## [0.5.8] - 2026-07-12

### Added

- Strict resume validation using remote validators and per-segment CRC32.
- HTTP integration coverage for Range responses, chunked transfers, retries,
  changed resources, and invalid `Content-Range` values.

### Fixed

- Retry queue growth under simultaneous high-concurrency failures.
- Resume speed reporting and final summaries to follow aria2-style semantics.
- Early EOF, disk short-write, and malformed partial-response handling.

[0.5.16-experimental]: https://github.com/daxiaamu/mudl-c/compare/v0.5.11...v0.5.16-experimental
[0.5.11]: https://github.com/daxiaamu/mudl-c/compare/v0.5.10...v0.5.11
[0.5.10]: https://github.com/daxiaamu/mudl-c/compare/v0.5.9...v0.5.10
[0.5.9]: https://github.com/daxiaamu/mudl-c/compare/v0.5.8...v0.5.9
[0.5.8]: https://github.com/daxiaamu/mudl-c/compare/v0.5.7...v0.5.8
