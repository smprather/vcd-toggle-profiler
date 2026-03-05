# VCD Toggle Profiler

A high-performance command-line tool that profiles signal toggle activity in
[Value Change Dump (VCD)](https://en.wikipedia.org/wiki/Value_change_dump) files
and produces a self-contained HTML report with interactive charts.

Feed it a VCD from your RTL simulation; get back a plot of toggle rate vs. time,
a cumulative toggle count curve, per-signal statistics, and the hottest toggle
windows — all in a single offline HTML file powered by
[uPlot](https://github.com/leeoniya/uPlot).

## Features

- **Streaming VCD parser** — processes files line-by-line; never loads the whole
  file into memory. Targets < 0.5 GB RAM even on massive inputs.
- **Gzip support** — reads `.vcd.gz` files transparently (pipes through `gzip -d`).
- **Stdin support** — pass `-` as the input to read from a pipe.
- **Sliding-window toggle rate** — configurable window and step sizes with
  half-open `(t_left, t_right]` semantics.
- **Dual-axis HTML report** — primary axis shows toggle rate
  (toggles / user-chosen time unit); secondary axis shows cumulative toggle count.
  Auto-selected human-readable units on both axes.
- **Top-20 hottest windows** — with optional non-overlapping constraint.
- **Signal filtering** — `--preamble` retains only signals whose fully qualified
  name starts with a given prefix (prefix is stripped in output).
- **Alias deduplication** — VCD identifiers that map to the same signal are
  counted once.
- **Downsampling** — large traces are downsampled to `--max-points` for fast
  rendering while preserving peaks on the rate series.
- **Fully offline** — all dependencies (uPlot JS/CSS, CLI11) are vendored.
  No network access required to build or run.

## Build

Requirements: a C++17 compiler, CMake >= 3.16, and `gzip` on `$PATH` for `.gz`
input.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is written to `./build/vcd-toggle-profiler`.

## Quick start

```bash
# Basic run — writes report to output/toggle_profile.html
./build/vcd-toggle-profiler vcd-samples/random/random.vcd

# Custom window, step, time range, and output directory
./build/vcd-toggle-profiler vcd-samples/swerv/swerv1.vcd \
  --outdir swerv-out \
  --win-size 500ps \
  --step-size 50ps \
  --start-time 1ns \
  --stop-time 200ns \
  --rate-unit ns

# Gzipped input
./build/vcd-toggle-profiler vcd-samples/Briey/dump1.vcd.gz

# Pipe from another tool
zcat huge.vcd.gz | ./build/vcd-toggle-profiler - --title "Huge design"
```

## CLI reference

```
USAGE:  vcd-toggle-profiler <input> [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `<input>` | Input VCD path (`.vcd`, `.vcd.gz`, or `-` for stdin) | *required* |
| `--outdir <dir>` | Output directory | `output` |
| `--win-size <dur>` | Sliding window size | `500ps` |
| `--step-size <dur>` | Step size between successive windows | `50ps` |
| `--start-time <dur>` | Start of the analysis range (snapped down to step boundary) | *beginning of trace* |
| `--stop-time <dur>` | End of the analysis range | *end of trace* |
| `--rate-unit <unit>` | Time unit for the rate denominator (y-axis) | `ns` |
| `--preamble <prefix>` | Keep only signals whose FQSN starts with this prefix (prefix is removed in output) | *none* |
| `--allow-top-window-overlap <true\|false>` | Allow overlapping windows in the top-20 table | `false` |
| `--title <text>` | Report title (defaults to input filename) | |
| `--max-points <n>` | Max rendered data points in the HTML chart (`0` = unlimited) | `200000` |
| `--debug` | Write a `debug.csv` with per-step rate and cumulative count | |
| `--uplot-js <path>` | Override vendored uPlot JS | |
| `--uplot-css <path>` | Override vendored uPlot CSS | |

**Duration format:** a number followed by a unit — `fs`, `ps`, `ns`, `us`, `ms`, or `s`.
Examples: `100ps`, `1ns`, `500us`.

**Constraints:**
- `step_size` must be ≤ `win_size`
- `win_size` must be evenly divisible by `step_size`
- When both are set, `stop_time` must be ≥ the snapped `start_time`

## Output files

All files are written under the `--outdir` directory:

| File | Contents |
|------|----------|
| `toggle_profile.html` | Self-contained interactive report (dual-axis chart, info table, top-20 windows table) |
| `signals.txt` | All matched signals, sorted by hierarchy depth then leaf-name length then lexicographic |
| `signal_toggle_counts.csv` | `signal_name,total_toggle_count` — reverse-sorted by count |
| `top_20_windows.txt` | Space-aligned table: `rank left_ps right_ps total_toggles toggle_rate_per_ns` |
| `debug.csv` | *(only with `--debug`)* `time(<unit>),toggle_rate(toggles/<rate_unit>),cumulative_toggle_count` |

## Sample VCD files

The `vcd-samples/` directory contains test data at various scales:

| Directory | Description | Size |
|-----------|-------------|------|
| `random/` | Tiny 8-bit counter | ~3 KB |
| `jtag/` | Small JTAG controller | ~50 KB |
| `bgm434/` | Pipelined pow-5 design | ~1.5 MB |
| `swerv/` | RISC-V SweRV core | ~14 MB |
| `Briey/` | VexRiscv Briey SoC (gzipped) | ~5 MB compressed |

## How it works

1. **Header parse** — reads VCD `$scope` / `$var` / `$upscope` declarations to
   build a signal table keyed by VCD identifier code. Applies `--preamble`
   filtering and deduplicates aliases.
2. **Streaming value-change scan** — reads the VCD body line by line, tracking
   the current simulation time. On each value change, compares the new value
   against the signal's previous value to count bit-level toggles.
3. **Windowed aggregation** — maintains a circular buffer of per-step toggle
   counts. At each step boundary, computes the toggle rate
   (`window_toggles / effective_window_size`) and the running cumulative count.
4. **Top-window selection** — sorts all windows by total toggles; greedily
   selects the top 20 (optionally enforcing non-overlap).
5. **Report generation** — emits a static HTML file with vendored uPlot JS/CSS
   inlined, two y-axis series, an info table, and the top-20 windows table.
   Large series are downsampled (with peak preservation on the rate axis) to
   stay within `--max-points`.

## Vendored dependencies

All third-party code is checked into the repository so the project builds
fully offline:

- **[uPlot](https://github.com/leeoniya/uPlot)** v1.6.16 — `third_party/uplot/`
- **[CLI11](https://github.com/CLIUtils/CLI11)** — `third_party/CLI11/`

## License

[MIT](LICENSE) — Copyright (c) 2026 Myles Prather
