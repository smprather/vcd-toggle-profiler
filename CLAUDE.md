# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

VCD Toggle Profiler — profiles total toggle-count vs. time for signals in Value Change Dump (VCD) files. Outputs a static HTML file with an embedded [uPlot](https://github.com/leeoniya/uPlot) chart (no CDN). Lots of other data processing and output statistics.

## Status

Pre-implementation. The repository contains architecture docs (`doc/architecture.md`) and sample VCD files (`vcd-samples/`) but no source code yet.

## Tech Stack

- **C++** for the core parser/profiler
- **uPlot** (JavaScript) embedded in generated HTML output
- `.gitignore` is currently Rust/Cargo-based (likely needs updating for C++)

## Architecture & Design Constraints

See `doc/architecture.md` for full details. Key requirements:

- **Performance**: Handle >200GB compressed VCD files in <2 hours
- **Memory**: Stream-process blocks from disk; never load whole file; stay under 0.5GB RAM
- **Parsing**: Minimize allocations in tight loops; use static buffers for line processing
- **Output**: Static HTML with embedded uPlot JS (no CDN references)
- **Windowing**: 100ps windows for toggle counting

## Sample VCD Files

`vcd-samples/` contains test data of varying complexity:

- `random/` — tiny counter (~3KB), good for basic testing
- `jtag/` — small JTAG controller
- `swerv/` — RISC-V SweRV core (~14MB), good stress test
- `Briey/` and `bgm434/` — compressed with Brotli (`.vcd.br`)
