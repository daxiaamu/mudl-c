# Changelog

All notable changes to MUDL-C are documented in this file.

## [Unreleased]

### Added

- Manual GitHub Actions runs through `workflow_dispatch`.
- A tested `mudl.exe` Actions artifact after successful CI runs.
- A dedicated command-line option module and direct parser unit tests.

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

[0.5.9]: https://github.com/daxiaamu/mudl-c/compare/v0.5.8...v0.5.9
[0.5.8]: https://github.com/daxiaamu/mudl-c/compare/v0.5.7...v0.5.8
