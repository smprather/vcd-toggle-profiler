# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

VCD Toggle Profiler — profiles total toggle-count vs. time for signals in Value Change Dump (VCD) files. Outputs a static HTML file with an embedded [uPlot](https://github.com/leeoniya/uPlot) chart (no CDN), plus per-signal CSV, sorted signal list, and top-20 windows table.

## Status

Implemented and functional. Rust implementation in `src/main.rs`. Builds with Cargo and runs against included sample VCD files.

Recent performance work also tuned the C++ implementation (`src/main.cpp`) for large
trace throughput:
- In-place value normalization/update in hot loops (remove per-change allocations)
- `string_view`-based ID lookups with stable storage (remove per-change ID string construction)
- Faster ASCII token/whitespace checks in parser paths
- Fast hashers for high-frequency maps
- `pigz`-first decompression and larger stdio buffering
- Release flags: `-march=native -mtune=native -flto`

Benchmark command:

```bash
hyperfine --warmup 1 --max-runs 3 ./run_rust
hyperfine --warmup 1 --max-runs 3 ./run_cpp
```

Reference result on `vcd-samples/Briey/dump1.vcd.gz` (`--max-points 0 --win-size 10ns --step-size 1ns`):
- Rust: ~3.15s mean
- C++: ~2.98s mean

## Build

```bash
cargo build --release
# Binary: ./target/release/vcd-toggle-profiler
```

## Tech Stack

- **Rust** — implementation (`src/main.rs`)
- **Cargo** — build system (`Cargo.toml`)
- **uPlot** v1.6.16 — chart library inlined into HTML output (vendored in `third_party/uplot/`)
- **gzip/pigz** — runtime dependency for `.vcd.gz` decompression (pigz preferred when available)

## Project Structure

- `src/main.rs` — entire application (parser, profiler, HTML generator)
- `doc/architecture.md` — design spec and feature requirements
- `third_party/` — vendored uPlot assets (fully offline build)
- `vcd-samples/` — test VCD files at various scales
- `target/` — Cargo build directory (gitignored)
- `output/` — default output directory (gitignored)

## Architecture & Design Constraints

See `doc/architecture.md` for full details. Key requirements:

- **Performance**: Handle >200GB compressed VCD files in <2 hours
- **Memory**: Stream-process lines from disk; never load whole file; stay under 0.5 GB RAM
- **Parsing**: Minimize allocations in tight loops; use static buffers for line processing
- **Output**: Static HTML with embedded uPlot JS/CSS (no CDN references)
- **Offline**: All dependencies vendored; no network access needed to build or run

## Key Concepts

- **FQSN** — Fully Qualified Signal Name (dot-separated hierarchy, e.g. `i1.i2.foo`)
- **Window semantics** — half-open `(t_left, t_right]` where `t_left = max(0, t_right - win_size)`
- **Toggle rate** — `window_toggles / effective_window_size` at each step
- **Duration units** — `fs`, `ps`, `ns`, `us`, `ms`, `s` (internally all converted to femtoseconds)
- **Alias dedup** — multiple VCD signal names sharing the same identifier code are counted once

## Sample VCD Files

`vcd-samples/` contains test data of varying complexity:

- `random/` — tiny 8-bit counter (~3 KB), good for basic testing
- `jtag/` — small JTAG controller (~50 KB)
- `bgm434/` — pipelined pow-5 design (~1.5 MB)
- `swerv/` — RISC-V SweRV core (gzipped, ~14 MB uncompressed), good stress test
- `Briey/` — VexRiscv Briey SoC (gzipped)
