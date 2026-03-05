# vcd-toggle-profiler

Profiles total VCD toggle-count vs. time and writes a static HTML report with embedded uPlot (offline, no CDN).

## Features

- Streaming VCD parser (`$scope`, `$var`, scalar/vector value changes)
- Sliding-window toggle profile with separate window and step sizes
- Window semantics: `(t_left, t_right]` with `t_left = max(0, t_right - win_size)`
- Auto-selected x-axis time unit (`fs/ps/ns/us/ms/s`) for readability
- Primary plot: toggle rate = `toggles_in_window / effective_window_size_at_step`
- Secondary plot: cumulative total toggle count with auto-selected readable count units
- HTML info section rendered as a table
- HTML third section with top-20 toggle windows table
- `.vcd` and `.vcd.gz` input support (`.gz` recognized by suffix)
- Signal preamble filter (`--preamble`) with case-sensitive prefix match
- Alias deduplication by VCD identifier (same id counted once in totals and per-signal outputs)
- Additional output files: sorted signal list, per-signal toggle CSV, top-20 windows text table
- Static HTML report with vendored `uPlot` assets

## Build (Offline)

Prerequisite: C++17 compiler, CMake, and `gzip` runtime command for `.gz` input.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Binary:

```bash
./build/vcd-toggle-profiler
```

## Usage

```bash
./build/vcd-toggle-profiler \
  vcd-samples/random/random.vcd \
  --outdir output \
  --win-size 500ps \
  --step-size 50ps \
  --start-time 1ns \
  --stop-time 200ns \
  --rate-unit ns
```

## CLI Options

- `<input>` required positional input path (`file.vcd`, `file.vcd.gz`, or `-` for stdin)
- `--outdir <dir>` output directory (default: `output`)
- `--win-size <duration>` sliding window size (default: `500ps`)
- `--step-size <duration>` step size (default: `50ps`)
- `--start-time <duration>` optional time-range start for plotted/selected windows; snapped down to the nearest step boundary
- `--stop-time <duration>` optional time-range stop for plotted/selected windows
- `--rate-unit <unit>` rate denominator unit for y-axis label/rate calculation (default: `ns`)
- `--preamble <prefix>` retain only signals whose FQSN starts with this case-sensitive prefix; prefix is removed in outputs
- `--allow-top-window-overlap <true|false>` overlap policy for top-20 windows (default: `false`)
- `--title <text>` report title
- `--max-points <n>` max rendered plot points in HTML (default: `200000`, `0` disables downsampling)
- `--uplot-js <path>` override vendored uPlot JS path
- `--uplot-css <path>` override vendored uPlot CSS path

Duration units accepted for `--win-size`, `--step-size`, `--start-time`, and `--stop-time`: `fs`, `ps`, `ns`, `us`, `ms`, `s`.
Units accepted for `--rate-unit`: `fs`, `ps`, `ns`, `us`, `ms`, `s`.

Constraints:

- `step_size <= win_size`
- `win_size % step_size == 0`
- `stop_time >= snapped_start_time` when both are set

## Output Files

Written under `--outdir`:

- `toggle_profile.html`
- `signals.txt` (sorted by hierarchy depth, then leaf-name length, then lexicographic)
- `signal_toggle_counts.csv` (`signal_name,total_toggle_count`, reverse-sorted by total toggle count)
- `top_20_windows.txt` (space-separated table: `rank left_ps right_ps total_toggles toggle_rate_per_ns`)

## Vendored dependencies

- `third_party/uplot/uPlot.iife.js`
- `third_party/uplot/uPlot.min.css`
- `third_party/uplot/LICENSE-uPlot.txt`
- `third_party/CLI11/include/CLI/*.hpp`
- `third_party/CLI11/LICENSE-CLI11.txt`
