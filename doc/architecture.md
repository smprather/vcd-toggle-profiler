# VCD Toggle Count Profiler

## Definitions

- Signal name: The name at the end of a hierarchy, without the included hierarchy.
  Example: In i1.i2.foo, the signal name is "foo".
- Fully qualified signal name: The full name of a signal, including the dot-separated
  hierarchy. Abbreviation: FQSN. Ex: i1.i2.foo is the fully qualified signal name.

## Miscellaneous Requirements

- Include snapshotted code for any external dependencies. The repo must be compilable
  offline without requiring any other downloads.
- Provide an overview of the tool in the help message.

## Features

- Use uPlot in a static html file to display the results in a web browser.
  Download and embed the uPlot javascript into the html file (don't
  use a CDN reference).
- Add a CLI option to set the window size for determining the toggles/time.
  --win-size `[si_prefix]s`. default=500ps.
- Add a CLI option for toggle rate unit, --rate-unit `[si_prefix]s`, default=ns.
  The primary y-axis unit should be toggles/<rate_unit>. Put the unit in parenthesis
  in the y-axis label.
- Add a secondary y-axis that shows the cumulative total toggle count plot.
- CLI option for an output directory, --outdir. The default is "output".
- Print the list of all signals to a file. Sort the file first by hierarchy depth,
  and second by length of the signal name at the end of the hierarchy.
- Print a csv file containing the columns "signal_name" and "total_toggle_count".
  Reverse sort the file by total toggle count.
- Support gzipped files. You can rely on .gz suffix for recognition.
- An option --preamble <str>. If a FQSN starts with the preamble string, then
  the preamble is removed, and the signal is retained for toggle counting. If the
  start of the FQSN does not match the preamble, then the signal is ignored.
- Support a separate step size for the toggle-counting `[si_prefix]s` window. The value of toggles/time
  at each integer multiple of the step value (N*step) is the number of toggles in a window formed
  by the right edge at the window at t_right=N*step and t_left=t_right-win_size. The floor value
  of t_left is 0. Option to set step size --step-size `[si_prefix]s`. Default=50ps. The step size must be <=
  window_size The window_size must be evenly divisible by the step size.
- Write the top 20 total-toggle windows to a file.
  - Columnized tabular with space-character separation (no tabs)
  - Columns: rank, left_ps, right_ps, total_toggles, toggle_rate_per_ns
  - Remove any '.00000...' from the numbers.
- Add an option --allow-top-window-overlap (true|false). Default=false. If false, none of the top 20
  windows printed to the top-20 file are allowed to have overlap.
- Make the title of the plot the basename of the vcd file.
- Auto-set the x-axis unit for maximum human readability.
- Auto-set the secondary y-axis unit (cumulative toggle count) for maximum human readability.
- In the information printed in the 2nd division of the html file, put the data in a table.
- Add a 3rd html division with the 20 top toggle count windows in a table.
- Make the first line of the html page "Double-click LMB to zoom-full".
- Create command line options for start time and stop time. Snap the start time to
  the first integer multiple of the step size <= the specified start time.
  --start-time `time_value[si_prefix]s`
  --stop-time `time_value[si_prefix]s`

## Implementation Notes

- Make the input file a positional argument.

## Priorities

- Performance
  - Needs to handle >200GB compressed VCD files in reasonable time.
  - Reasonable is defined as < 2hrs.
  - Consider multi-threading possibilities. Can file be split up and
    processed in multiple threads?
  - Achieve performance by absolutely minimizing mallocs in the tight
    line processing loops.
  - Use static buffers to read lines off disk and process them.
- Memory
  - Don't read the whole file into memory.
  - Process one block at a time read from disk. Make sure to use and reuse
    static blocks of memory to prevent costly mallocs.
  - Maximum RAM usage should be < 500MB.

## Tech Stack

- C++
- [uPlot](https://github.com/leeoniya/uPlot)
- CLI11 for CLI implementation
