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
  file into memory.
- **Sliding-window toggle rate** — configurable window and step sizes with
  half-open `(t_left, t_right]` semantics.
- **Dual-axis HTML report** — primary axis shows toggle rate
  (toggles/time)
- Secondary axis shows cumulative toggle count.
- **Auto-selected human-readable** units on both axes.
- **Top-20 hottest windows** — with optional non-overlapping constraint.
- **Signal filtering** — `--preamble` retains only signals whose fully qualified
  name starts with a given prefix (prefix is stripped in output).
- **Alias deduplication** — VCD identifiers that map to the same signal are
  counted once.
- **Sparse series** — internal data structures scale with the number of
  value-change events, not the trace duration divided by step size. Arbitrarily
  small `--step-size` values work without blowing up memory.
- **Downsampling** — large traces are downsampled to `--max-points` for fast
  rendering while preserving peaks on the rate series. Consecutive points with
  unchanged y-values are deduplicated before downsampling.
- **Fully offline** — third-party dependencies are vendored in-repo. No network
  access is required to build or run.
- **Gzip support** — reads `.vcd.gz` files transparently (pipes through `gzip -d`).
- **Stdin support** — pass `-` as the input to read from a pipe.

## Build

### C++

Requirements: a C++17 compiler, CMake >= 3.16, and `gzip` on `$PATH` for `.gz`
input (`pigz` is used when available). Release builds enable `-march=native`,
`-mtune=native`, and LTO for maximum throughput.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is written to `./build/vcd-toggle-profiler`.

### C++ tests (optional)

GoogleTest is vendored in-repo (`third_party/googletest`) and can be enabled
without network access:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVCD_TOGGLE_PROFILER_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

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

## Benchmark Quick Check

Use the helper scripts in repo root:

```bash
hyperfine --warmup 1 --max-runs 3 ./run_cpp
```

Current reference run (Briey sample, `--max-points 0 --win-size 10ns --step-size 1ns`):

- `run_cpp`: ~2.98s mean

Recent C++ speedups were focused on minimizing parser-loop allocations and reducing
hash/tokenization overhead for better scaling to very large VCD files.

## CLI reference

```text
USAGE:  vcd-toggle-profiler <input> [OPTIONS]
```

| Option | Description | Default |
|--------|-------------|---------|
| `<input>` | Input VCD path (`.vcd`, `.vcd.gz`, or `-` for stdin) | *required* |
| `--outdir <dir>` | Output directory | `output` |
| `--win-size <dur>` | Sliding window size | `500ps` |
| `--step-size <dur>` | Step size between successive windows | `50ps` |
| `--time-unit <unit>` | Time unit used in ASCII output and HTML tables (`fs/ps/ns/us/ms/s`) | `ns` |
| `--glitch-threshold <dur>` | Ignore back-to-back transitions on the same signal when the interval is less than this threshold (both transitions are dropped) | `0fs` |
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
| `signal_glitch_counts.csv` | *(only when `--glitch-threshold > 0`)* `signal_name,glitch_count` — reverse-sorted by glitch count, only signals with count > 0 |
| `top_20_windows.txt` | Space-aligned table: `rank left_<time_unit> right_<time_unit> total_toggles toggle_rate_per_ns` |
| `debug.csv` | *(only with `--debug`)* `time(<unit>),toggle_rate(toggles/<rate_unit>),cumulative_toggle_count` |

## Sample VCD files

The `vcd-samples/` directory contains test data at various scales:

| Directory | Description | Size |
|-----------|-------------|------|
| `random/` | Tiny 8-bit counter | ~3 KB |
| `jtag/` | Small JTAG controller | ~50 KB |
| `bgm434/` | Pipelined pow-5 design | ~1.5 MB |
| `swerv/` | RISC-V SweRV core | ~14 MB |
| `Briey/` | VexRiscv Briey SoC | ~270 MB |

## How it works

1. **Header parse** — reads VCD `$scope` / `$var` / `$upscope` declarations to
   build a signal table keyed by VCD identifier code. Applies `--preamble`
   filtering and deduplicates aliases.
2. **Streaming value-change scan** — reads the VCD body line by line, tracking
   the current simulation time. On each value change, compares the new value
   against the signal's previous value to count bit-level toggles.
3. **Windowed aggregation** — records signed deltas into sparse maps keyed by
   step index. Series points are only materialized at steps where a delta
   exists, so memory scales with event count regardless of step size.
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
- **[CLI11](https://github.com/CLIUtils/CLI11)** — `third_party/CLI11/` (C++ build only)
- **[GoogleTest](https://github.com/google/googletest)** — `third_party/googletest/` (C++ tests)

## License

[MIT](LICENSE) — Copyright (c) 2026 Myles Prather
