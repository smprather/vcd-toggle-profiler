use std::cmp::{max, min};
use std::collections::{HashMap, HashSet};
use std::env;
use std::fs::{self, File};
use std::hash::{BuildHasherDefault, Hasher};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdout, Command, Stdio};

#[derive(Clone, Debug)]
struct Options {
    input_path: String,
    outdir: String,
    title: String,
    title_explicit: bool,
    preamble: String,

    win_fs: u64,
    step_fs: u64,
    rate_unit_fs: u64,
    rate_unit_label: String,

    has_start_time: bool,
    has_stop_time: bool,
    start_fs: u64,
    snapped_start_fs: u64,
    stop_fs: u64,

    allow_top_window_overlap: bool,
    debug: bool,
    max_points: u64,

    uplot_js_path: String,
    uplot_css_path: String,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            input_path: String::new(),
            outdir: "output".to_string(),
            title: String::new(),
            title_explicit: false,
            preamble: String::new(),
            win_fs: 500_000,
            step_fs: 50_000,
            rate_unit_fs: 1_000_000,
            rate_unit_label: "ns".to_string(),
            has_start_time: false,
            has_stop_time: false,
            start_fs: 0,
            snapped_start_fs: 0,
            stop_fs: 0,
            allow_top_window_overlap: false,
            debug: false,
            max_points: 200_000,
            uplot_js_path: String::new(),
            uplot_css_path: String::new(),
        }
    }
}

#[derive(Clone, Debug)]
struct SignalState {
    output_name: String,
    leaf_name: String,
    depth: usize,

    width: u32,
    initialized: bool,
    value: Vec<u8>,

    total_toggles: u64,
}

#[derive(Clone, Debug)]
struct ParserStats {
    total_lines: u64,
    timestamp_updates: u64,
    parsed_value_changes: u64,
    unknown_id_changes: u64,
    alias_dedup_skipped: u64,
    found_timescale: bool,
    timescale_fs: u64,
}

impl Default for ParserStats {
    fn default() -> Self {
        Self {
            total_lines: 0,
            timestamp_updates: 0,
            parsed_value_changes: 0,
            unknown_id_changes: 0,
            alias_dedup_skipped: 0,
            found_timescale: false,
            timescale_fs: 1_000,
        }
    }
}

#[derive(Clone, Debug)]
struct SeriesPoint {
    right_fs: u64,
    left_fs: u64,
    effective_window_fs: u64,
    window_toggles: u64,
    cumulative_toggles: u64,
    rate: f64,
}

#[derive(Clone, Debug)]
struct TopWindow {
    left_fs: u64,
    right_fs: u64,
    effective_window_fs: u64,
    window_toggles: u64,
    rate: f64,
}

struct InputHandle {
    reader: Box<dyn BufRead>,
    child: Option<Child>,
}

impl InputHandle {
    fn close(mut self, parse_succeeded: bool) -> Result<(), String> {
        drop(self.reader);

        if let Some(mut child) = self.child.take() {
            let status = child
                .wait()
                .map_err(|e| format!("failed waiting for decompressor: {e}"))?;

            if !parse_succeeded {
                return Ok(());
            }

            if status.success() {
                return Ok(());
            }

            return Err("decompression failed".to_string());
        }

        Ok(())
    }
}

enum ParseOutcome {
    Help,
    Run(Options),
}

#[derive(Clone)]
struct FnvHasher(u64);

impl Default for FnvHasher {
    fn default() -> Self {
        Self(0xcbf29ce484222325)
    }
}

impl Hasher for FnvHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.0 ^= *byte as u64;
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

type FastMap<K, V> = HashMap<K, V, BuildHasherDefault<FnvHasher>>;
type FastSet<T> = HashSet<T, BuildHasherDefault<FnvHasher>>;

const INPUT_BUFFER_CAPACITY: usize = 1 << 20;

fn starts_with(s: &str, prefix: &str) -> bool {
    s.starts_with(prefix)
}

fn ends_with(s: &str, suffix: &str) -> bool {
    s.ends_with(suffix)
}

fn normalize_logic_byte(c: u8) -> u8 {
    match c {
        b'0' => b'0',
        b'1' => b'1',
        b'x' | b'X' => b'x',
        b'z' | b'Z' => b'z',
        _ => b'x',
    }
}

fn fill_normalized_value(dst: &mut [u8], raw: &[u8]) {
    let w = dst.len();
    if w == 0 {
        return;
    }

    if w == 1 {
        dst[0] = if raw.is_empty() {
            b'x'
        } else {
            normalize_logic_byte(raw[raw.len() - 1])
        };
        return;
    }

    if raw.len() >= w {
        let offset = raw.len() - w;
        for i in 0..w {
            dst[i] = normalize_logic_byte(raw[offset + i]);
        }
        return;
    }

    let fill = if raw.is_empty() {
        b'x'
    } else {
        normalize_logic_byte(raw[0])
    };
    let pad = w - raw.len();

    for x in &mut dst[..pad] {
        *x = fill;
    }

    for (i, b) in raw.iter().enumerate() {
        dst[pad + i] = normalize_logic_byte(*b);
    }
}

fn count_toggles_and_update(dst: &mut [u8], raw: &[u8]) -> u64 {
    let w = dst.len();
    if w == 0 {
        return 0;
    }

    let mut toggles = 0u64;

    if w == 1 {
        let normalized = if raw.is_empty() {
            b'x'
        } else {
            normalize_logic_byte(raw[raw.len() - 1])
        };
        if dst[0] != normalized {
            dst[0] = normalized;
            return 1;
        }
        return 0;
    }

    if raw.len() >= w {
        let offset = raw.len() - w;
        for i in 0..w {
            let normalized = normalize_logic_byte(raw[offset + i]);
            if dst[i] != normalized {
                dst[i] = normalized;
                toggles += 1;
            }
        }
        return toggles;
    }

    let fill = if raw.is_empty() {
        b'x'
    } else {
        normalize_logic_byte(raw[0])
    };
    let pad = w - raw.len();

    for x in &mut dst[..pad] {
        if *x != fill {
            *x = fill;
            toggles += 1;
        }
    }

    for (i, b) in raw.iter().enumerate() {
        let normalized = normalize_logic_byte(*b);
        let idx = pad + i;
        if dst[idx] != normalized {
            dst[idx] = normalized;
            toggles += 1;
        }
    }

    toggles
}

fn parse_u64(text: &str) -> Option<u64> {
    if text.is_empty() {
        return None;
    }
    text.parse::<u64>().ok()
}

fn saturating_u64(value: u128) -> u64 {
    if value > u64::MAX as u128 {
        u64::MAX
    } else {
        value as u64
    }
}

fn ceil_div_u64(num: u64, den: u64) -> u64 {
    if den == 0 {
        return u64::MAX;
    }
    (num / den) + u64::from(num % den != 0)
}

fn parse_unit_fs(unit: &str) -> Option<u64> {
    match unit.to_ascii_lowercase().as_str() {
        "fs" => Some(1),
        "ps" => Some(1_000),
        "ns" => Some(1_000_000),
        "us" => Some(1_000_000_000),
        "ms" => Some(1_000_000_000_000),
        "s" => Some(1_000_000_000_000_000),
        _ => None,
    }
}

fn parse_duration_to_fs(text: &str, allow_zero: bool) -> Result<u64, String> {
    let t = text.trim();
    if t.is_empty() {
        return Err("duration is empty".to_string());
    }

    let split = t
        .char_indices()
        .find(|(_, c)| !c.is_ascii_digit())
        .map(|(i, _)| i)
        .unwrap_or(t.len());

    if split == 0 || split == t.len() {
        return Err("duration must be in form <number><unit>, e.g. 500ps".to_string());
    }

    let mag = parse_u64(&t[..split]).ok_or_else(|| {
        if allow_zero {
            "duration magnitude must be a non-negative integer".to_string()
        } else {
            "duration magnitude must be a positive integer".to_string()
        }
    })?;

    if !allow_zero && mag == 0 {
        return Err("duration magnitude must be a positive integer".to_string());
    }

    let unit_fs = parse_unit_fs(&t[split..])
        .ok_or_else(|| "unsupported unit; use fs/ps/ns/us/ms/s".to_string())?;

    Ok(saturating_u64((mag as u128) * (unit_fs as u128)))
}

fn parse_timescale_fs(raw: &str) -> Result<u64, String> {
    let cleaned = raw.replace('$', " ");

    let mut number_tok = String::new();
    let mut unit_tok = String::new();

    for tok in cleaned.split_whitespace() {
        let lower = tok.to_ascii_lowercase();
        if lower == "timescale" || lower == "end" {
            continue;
        }

        if number_tok.is_empty() {
            number_tok = tok.to_string();
            continue;
        }

        if unit_tok.is_empty() {
            unit_tok = tok.to_string();
            break;
        }
    }

    if number_tok.is_empty() {
        return Err("missing numeric timescale token".to_string());
    }

    let split = number_tok
        .char_indices()
        .find(|(_, c)| !c.is_ascii_digit())
        .map(|(i, _)| i)
        .unwrap_or(number_tok.len());

    if split == 0 {
        return Err("invalid timescale number token".to_string());
    }

    let mag = parse_u64(&number_tok[..split])
        .ok_or_else(|| "timescale magnitude must be positive integer".to_string())?;

    if mag == 0 {
        return Err("timescale magnitude must be positive integer".to_string());
    }

    if split < number_tok.len() {
        unit_tok = number_tok[split..].to_string();
    }

    if unit_tok.is_empty() {
        return Err("missing timescale unit token".to_string());
    }

    let unit_fs =
        parse_unit_fs(&unit_tok).ok_or_else(|| "unsupported timescale unit".to_string())?;
    Ok(saturating_u64((mag as u128) * (unit_fs as u128)))
}

fn parse_bool(text: &str) -> Option<bool> {
    match text.trim().to_ascii_lowercase().as_str() {
        "true" => Some(true),
        "false" => Some(false),
        _ => None,
    }
}

fn html_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len());
    for c in input.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            _ => out.push(c),
        }
    }
    out
}

fn js_string_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len() + 8);
    for c in input.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '"' => out.push_str("\\\""),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            _ => out.push(c),
        }
    }
    out
}

fn resolve_asset_path(explicit_path: &str, relative_from_root: &str) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if !explicit_path.is_empty() {
        candidates.push(PathBuf::from(explicit_path));
    }

    candidates.push(PathBuf::from(relative_from_root));
    candidates.push(Path::new("..").join(relative_from_root));
    candidates.push(Path::new("../..").join(relative_from_root));

    for candidate in candidates {
        if candidate.is_file() {
            if let Ok(canon) = fs::canonicalize(&candidate) {
                return Some(canon);
            }
            return Some(candidate);
        }
    }

    None
}

fn read_text_file(path: &Path) -> Result<String, String> {
    let mut f =
        File::open(path).map_err(|e| format!("failed to open file '{}': {e}", path.display()))?;
    let mut s = String::new();
    f.read_to_string(&mut s)
        .map_err(|e| format!("failed to read file '{}': {e}", path.display()))?;
    Ok(s)
}

fn add_signed_delta(map: &mut FastMap<u64, i64>, key: u64, delta: i64) {
    let entry = map.entry(key).or_insert(0);
    *entry += delta;
    if *entry == 0 {
        map.remove(&key);
    }
}

fn add_unsigned_delta(map: &mut FastMap<u64, u64>, key: u64, delta: u64) {
    let entry = map.entry(key).or_insert(0);
    *entry = entry.saturating_add(delta);
}

#[derive(Clone, Debug)]
struct StepAccumulator {
    win_fs: u64,
    step_fs: u64,
    window_delta: FastMap<u64, i64>,
    cumulative_start_delta: FastMap<u64, u64>,
    max_step: u64,
    total_toggles: u64,
}

impl StepAccumulator {
    fn new(win_fs: u64, step_fs: u64) -> Self {
        Self {
            win_fs,
            step_fs,
            window_delta: FastMap::default(),
            cumulative_start_delta: FastMap::default(),
            max_step: 0,
            total_toggles: 0,
        }
    }

    fn add_event(&mut self, timestamp_fs: u64, toggles: u64) {
        if toggles == 0 {
            return;
        }

        let n_lo = ceil_div_u64(timestamp_fs, self.step_fs);
        let hi_num = (timestamp_fs as u128)
            .saturating_add(self.win_fs as u128)
            .saturating_sub(1);
        let n_hi = saturating_u64(hi_num / (self.step_fs as u128));

        if n_lo <= n_hi {
            add_signed_delta(&mut self.window_delta, n_lo, toggles as i64);
            if n_hi != u64::MAX {
                add_signed_delta(&mut self.window_delta, n_hi + 1, -(toggles as i64));
            }
            if n_hi > self.max_step {
                self.max_step = n_hi;
            }
        }

        add_unsigned_delta(&mut self.cumulative_start_delta, n_lo, toggles);
        if n_lo > self.max_step {
            self.max_step = n_lo;
        }

        self.total_toggles = self.total_toggles.saturating_add(toggles);
    }

    fn build_series(&self, rate_unit_fs: u64) -> Vec<SeriesPoint> {
        let mut window_deltas: Vec<(u64, i64)> =
            self.window_delta.iter().map(|(k, v)| (*k, *v)).collect();
        let mut cumulative_deltas: Vec<(u64, u64)> = self
            .cumulative_start_delta
            .iter()
            .map(|(k, v)| (*k, *v))
            .collect();

        window_deltas.sort_by_key(|x| x.0);
        cumulative_deltas.sort_by_key(|x| x.0);

        let mut out = Vec::new();
        out.reserve((self.max_step as usize).saturating_add(1));

        let mut wi = 0usize;
        let mut ci = 0usize;
        let mut running_window: i64 = 0;
        let mut running_cumulative: u64 = 0;

        let mut step = 0u64;
        loop {
            if step > self.max_step {
                break;
            }

            while wi < window_deltas.len() && window_deltas[wi].0 == step {
                running_window += window_deltas[wi].1;
                wi += 1;
            }

            while ci < cumulative_deltas.len() && cumulative_deltas[ci].0 == step {
                running_cumulative = running_cumulative.saturating_add(cumulative_deltas[ci].1);
                ci += 1;
            }

            let right_fs = saturating_u64((step as u128) * (self.step_fs as u128));
            let left_fs = if right_fs > self.win_fs {
                right_fs - self.win_fs
            } else {
                0
            };
            let effective_window_fs = right_fs.saturating_sub(left_fs);

            let window_toggles = if running_window > 0 {
                running_window as u64
            } else {
                0
            };

            let rate = if effective_window_fs > 0 && window_toggles > 0 {
                (window_toggles as f64) * (rate_unit_fs as f64) / (effective_window_fs as f64)
            } else {
                0.0
            };

            out.push(SeriesPoint {
                right_fs,
                left_fs,
                effective_window_fs,
                window_toggles,
                cumulative_toggles: running_cumulative,
                rate,
            });

            if step == u64::MAX {
                break;
            }
            step += 1;
        }

        out
    }
}

struct VcdParser {
    opts: Options,
    accumulator: StepAccumulator,
    stats: ParserStats,

    seen_enddefinitions: bool,
    timescale_fs: u64,
    current_timestamp: u64,

    scope_stack: Vec<String>,
    id_to_index: FastMap<String, usize>,
    known_ids: FastSet<String>,
    signals: Vec<SignalState>,
}

impl VcdParser {
    fn new(opts: &Options) -> Self {
        Self {
            opts: opts.clone(),
            accumulator: StepAccumulator::new(opts.win_fs, opts.step_fs),
            stats: ParserStats::default(),
            seen_enddefinitions: false,
            timescale_fs: 1_000,
            current_timestamp: 0,
            scope_stack: Vec::new(),
            id_to_index: FastMap::default(),
            known_ids: FastSet::default(),
            signals: Vec::new(),
        }
    }

    fn parse(&mut self, reader: &mut dyn BufRead, input_name: &str) -> Result<(), String> {
        let mut in_timescale = false;
        let mut timescale_accum = String::new();

        let mut line = String::with_capacity(1 << 20);
        loop {
            line.clear();
            let n = reader
                .read_line(&mut line)
                .map_err(|e| format!("read error: {e}"))?;
            if n == 0 {
                break;
            }

            self.stats.total_lines += 1;

            let line_trimmed = line.trim();
            if line_trimmed.is_empty() {
                continue;
            }

            if !self.seen_enddefinitions {
                if starts_with(line_trimmed, "$scope") {
                    self.parse_scope(line_trimmed);
                    continue;
                }
                if starts_with(line_trimmed, "$upscope") {
                    self.scope_stack.pop();
                    continue;
                }
                if starts_with(line_trimmed, "$var") {
                    self.parse_var(line_trimmed);
                    continue;
                }

                if line_trimmed.contains("$timescale") {
                    in_timescale = true;
                }

                if in_timescale {
                    timescale_accum.push_str(line_trimmed);
                    timescale_accum.push(' ');
                    if line_trimmed.contains("$end") {
                        match parse_timescale_fs(&timescale_accum) {
                            Ok(parsed_fs) => {
                                self.timescale_fs = parsed_fs;
                                self.stats.timescale_fs = parsed_fs;
                                self.stats.found_timescale = true;
                            }
                            Err(err) => {
                                eprintln!(
                                    "warning: failed to parse $timescale in '{}': {}; defaulting to 1ps",
                                    input_name, err
                                );
                            }
                        }
                        timescale_accum.clear();
                        in_timescale = false;
                    }
                    continue;
                }

                if line_trimmed.contains("$enddefinitions") {
                    self.seen_enddefinitions = true;
                }
                continue;
            }

            self.parse_data_line(line_trimmed);
        }

        Ok(())
    }

    fn parse_scope(&mut self, line: &str) {
        let mut it = line.split_whitespace();
        let _ = it.next();
        let _ = it.next();
        if let Some(name) = it.next() {
            self.scope_stack.push(name.to_string());
        }
    }

    fn hierarchy_depth(fqsn: &str) -> usize {
        if fqsn.is_empty() {
            return 0;
        }
        1 + fqsn.chars().filter(|c| *c == '.').count()
    }

    fn build_fqsn(&self, leaf: &str) -> String {
        if self.scope_stack.is_empty() {
            return leaf.to_string();
        }
        let mut out = self.scope_stack.join(".");
        out.push('.');
        out.push_str(leaf);
        out
    }

    fn apply_preamble(&self, fqsn: &str) -> Option<String> {
        if self.opts.preamble.is_empty() {
            return Some(fqsn.to_string());
        }

        if !fqsn.starts_with(&self.opts.preamble) {
            return None;
        }

        let mut trimmed = fqsn[self.opts.preamble.len()..].to_string();
        if trimmed.starts_with('.') {
            trimmed.remove(0);
        }

        if trimmed.is_empty() {
            Some(fqsn.to_string())
        } else {
            Some(trimmed)
        }
    }

    fn parse_var(&mut self, line: &str) {
        let toks: Vec<&str> = line.split_whitespace().collect();
        if toks.len() < 5 {
            return;
        }

        let width_tok = toks[2];
        let id = toks[3].to_string();
        self.known_ids.insert(id.clone());

        let mut leaf_tok = "unnamed";
        for tok in toks.iter().skip(4) {
            if *tok == "$end" {
                break;
            }
            leaf_tok = tok;
            break;
        }

        let mut width = parse_u64(width_tok).unwrap_or(1);
        if width == 0 {
            width = 1;
        }

        if let Some(existing_idx) = self.id_to_index.get(&id).copied() {
            self.stats.alias_dedup_skipped += 1;
            let existing = &mut self.signals[existing_idx];
            if width > existing.width as u64 {
                existing.width = min(width, u32::MAX as u64) as u32;
            }
            return;
        }

        let fqsn = self.build_fqsn(leaf_tok);
        let Some(filtered_name) = self.apply_preamble(&fqsn) else {
            return;
        };

        let signal = SignalState {
            output_name: filtered_name.clone(),
            leaf_name: leaf_tok.to_string(),
            depth: Self::hierarchy_depth(&filtered_name),
            width: min(width, u32::MAX as u64) as u32,
            initialized: false,
            value: Vec::new(),
            total_toggles: 0,
        };

        let idx = self.signals.len();
        self.signals.push(signal);
        self.id_to_index.insert(id, idx);
    }

    fn parse_data_line(&mut self, line: &str) {
        let bytes = line.as_bytes();
        if bytes.is_empty() {
            return;
        }
        let lead = bytes[0];

        if lead == b'#' {
            if let Some(ts) = parse_u64(line[1..].trim()) {
                self.current_timestamp = ts;
                self.stats.timestamp_updates += 1;
            }
            return;
        }

        if lead == b'$' {
            return;
        }

        let mut toggles_sum = 0u64;

        if matches!(lead, b'b' | b'B' | b'r' | b'R') {
            let rest = &line[1..];
            let mut it = rest.split_ascii_whitespace();
            if let (Some(value), Some(id)) = (it.next(), it.next()) {
                toggles_sum = self.apply_change_bytes(id, value.as_bytes());
            }
        } else if matches!(
            lead,
            b'0' | b'1'
                | b'x'
                | b'X'
                | b'z'
                | b'Z'
                | b'u'
                | b'U'
                | b'w'
                | b'W'
                | b'h'
                | b'H'
                | b'l'
                | b'L'
                | b'-'
        ) {
            let id = line[1..].trim();
            if !id.is_empty() {
                let scalar = [normalize_logic_byte(lead)];
                toggles_sum = self.apply_change_bytes(id, &scalar);
            }
        }

        if toggles_sum > 0 {
            self.stats.parsed_value_changes += 1;
            let timestamp_fs =
                saturating_u64((self.current_timestamp as u128) * (self.timescale_fs as u128));
            self.accumulator.add_event(timestamp_fs, toggles_sum);
        }
    }

    fn apply_change_bytes(&mut self, id: &str, raw_value: &[u8]) -> u64 {
        let Some(idx) = self.id_to_index.get(id).copied() else {
            if !self.known_ids.contains(id) {
                self.stats.unknown_id_changes += 1;
            }
            return 0;
        };

        let signal = &mut self.signals[idx];
        let width = max(signal.width, 1) as usize;
        if signal.value.len() != width {
            signal.value.resize(width, b'x');
        }

        if !signal.initialized {
            fill_normalized_value(&mut signal.value, raw_value);
            signal.initialized = true;
            return 0;
        }

        let toggles = count_toggles_and_update(&mut signal.value, raw_value);

        if toggles > 0 {
            signal.total_toggles = signal.total_toggles.saturating_add(toggles);
        }

        toggles
    }
}

fn intervals_overlap_open_left_closed_right(a: &TopWindow, b: &TopWindow) -> bool {
    let l = max(a.left_fs, b.left_fs);
    let r = min(a.right_fs, b.right_fs);
    l < r
}

fn select_top_windows(points: &[SeriesPoint], allow_overlap: bool, top_n: usize) -> Vec<TopWindow> {
    let mut candidates: Vec<TopWindow> = points
        .iter()
        .filter(|p| p.window_toggles > 0)
        .map(|p| TopWindow {
            left_fs: p.left_fs,
            right_fs: p.right_fs,
            effective_window_fs: p.effective_window_fs,
            window_toggles: p.window_toggles,
            rate: p.rate,
        })
        .collect();

    candidates.sort_by(|a, b| {
        b.window_toggles
            .cmp(&a.window_toggles)
            .then_with(|| a.right_fs.cmp(&b.right_fs))
    });

    let mut selected = Vec::with_capacity(top_n);
    for c in candidates {
        if selected.len() >= top_n {
            break;
        }
        if allow_overlap {
            selected.push(c);
            continue;
        }

        let mut overlaps = false;
        for s in &selected {
            if intervals_overlap_open_left_closed_right(&c, s) {
                overlaps = true;
                break;
            }
        }

        if !overlaps {
            selected.push(c);
        }
    }

    selected
}

fn filter_series_by_time_range(points: &[SeriesPoint], opts: &Options) -> Vec<SeriesPoint> {
    let start_fs = if opts.has_start_time {
        opts.snapped_start_fs
    } else {
        0
    };
    let use_stop = opts.has_stop_time;
    let stop_fs = opts.stop_fs;

    points
        .iter()
        .filter(|p| p.right_fs >= start_fs && (!use_stop || p.right_fs <= stop_fs))
        .cloned()
        .collect()
}

fn filter_series_to_full_window_only(points: &[SeriesPoint], win_fs: u64) -> Vec<SeriesPoint> {
    points
        .iter()
        .filter(|p| p.effective_window_fs == win_fs)
        .cloned()
        .collect()
}

#[derive(Clone, Debug)]
struct PlotData {
    x_values: Vec<f64>,
    y_rate: Vec<f64>,
    y_cumulative: Vec<f64>,
    x_unit_label: String,
    x_unit_fs: u64,
    cumulative_unit_label: String,
    cumulative_unit_scale: f64,
}

#[derive(Copy, Clone)]
struct TimeUnit {
    label: &'static str,
    fs: u64,
}

#[derive(Copy, Clone)]
struct CountUnit {
    label: &'static str,
    scale: f64,
}

fn choose_readable_time_unit(max_fs: u64) -> TimeUnit {
    const UNITS: [TimeUnit; 6] = [
        TimeUnit {
            label: "s",
            fs: 1_000_000_000_000_000,
        },
        TimeUnit {
            label: "ms",
            fs: 1_000_000_000_000,
        },
        TimeUnit {
            label: "us",
            fs: 1_000_000_000,
        },
        TimeUnit {
            label: "ns",
            fs: 1_000_000,
        },
        TimeUnit {
            label: "ps",
            fs: 1_000,
        },
        TimeUnit { label: "fs", fs: 1 },
    ];

    if max_fs == 0 {
        return TimeUnit {
            label: "ps",
            fs: 1_000,
        };
    }

    for unit in UNITS {
        if max_fs >= unit.fs {
            return unit;
        }
    }

    TimeUnit { label: "fs", fs: 1 }
}

fn choose_readable_count_unit(max_count: u64) -> CountUnit {
    const UNITS: [CountUnit; 6] = [
        CountUnit {
            label: "P toggles",
            scale: 1e15,
        },
        CountUnit {
            label: "T toggles",
            scale: 1e12,
        },
        CountUnit {
            label: "G toggles",
            scale: 1e9,
        },
        CountUnit {
            label: "M toggles",
            scale: 1e6,
        },
        CountUnit {
            label: "K toggles",
            scale: 1e3,
        },
        CountUnit {
            label: "toggles",
            scale: 1.0,
        },
    ];

    if max_count == 0 {
        return CountUnit {
            label: "toggles",
            scale: 1.0,
        };
    }

    for unit in UNITS {
        if (max_count as f64) >= unit.scale {
            return unit;
        }
    }

    CountUnit {
        label: "toggles",
        scale: 1.0,
    }
}

fn build_plot_data(points: &[SeriesPoint]) -> PlotData {
    let mut max_right_fs = 0u64;
    let mut max_cumulative = 0u64;

    for p in points {
        if p.right_fs > max_right_fs {
            max_right_fs = p.right_fs;
        }
        if p.cumulative_toggles > max_cumulative {
            max_cumulative = p.cumulative_toggles;
        }
    }

    let x_unit = choose_readable_time_unit(max_right_fs);
    let count_unit = choose_readable_count_unit(max_cumulative);

    let mut out = PlotData {
        x_values: Vec::with_capacity(points.len()),
        y_rate: Vec::with_capacity(points.len()),
        y_cumulative: Vec::with_capacity(points.len()),
        x_unit_label: x_unit.label.to_string(),
        x_unit_fs: x_unit.fs,
        cumulative_unit_label: count_unit.label.to_string(),
        cumulative_unit_scale: count_unit.scale,
    };

    for p in points {
        out.x_values.push((p.right_fs as f64) / (x_unit.fs as f64));
        out.y_rate.push(p.rate);
        out.y_cumulative
            .push((p.cumulative_toggles as f64) / count_unit.scale);
    }

    if out.x_values.is_empty() {
        out.x_values.push(0.0);
        out.y_rate.push(0.0);
        out.y_cumulative.push(0.0);
    }

    out
}

fn downsample_plot_data(input: &PlotData, max_points: u64) -> PlotData {
    if max_points == 0 || (input.x_values.len() as u64) <= max_points {
        return input.clone();
    }

    let bucket =
        ((input.x_values.len() + (max_points as usize) - 1) / (max_points as usize)).max(1);

    let mut out = PlotData {
        x_values: Vec::with_capacity(max_points as usize),
        y_rate: Vec::with_capacity(max_points as usize),
        y_cumulative: Vec::with_capacity(max_points as usize),
        x_unit_label: input.x_unit_label.clone(),
        x_unit_fs: input.x_unit_fs,
        cumulative_unit_label: input.cumulative_unit_label.clone(),
        cumulative_unit_scale: input.cumulative_unit_scale,
    };

    let mut i = 0usize;
    while i < input.x_values.len() {
        let end = min(input.x_values.len(), i + bucket);

        let mut max_rate_idx = i;
        for j in (i + 1)..end {
            if input.y_rate[j] > input.y_rate[max_rate_idx] {
                max_rate_idx = j;
            }
        }
        let last_idx = end - 1;

        out.x_values.push(input.x_values[last_idx]);
        out.y_rate.push(input.y_rate[max_rate_idx]);
        out.y_cumulative.push(input.y_cumulative[last_idx]);

        i += bucket;
    }

    out
}

fn write_signal_list(path: &Path, signals: &[SignalState]) -> Result<(), String> {
    let mut sorted: Vec<&SignalState> = signals.iter().collect();
    sorted.sort_by(|a, b| {
        a.depth
            .cmp(&b.depth)
            .then_with(|| a.leaf_name.len().cmp(&b.leaf_name.len()))
            .then_with(|| a.output_name.cmp(&b.output_name))
    });

    let mut out = File::create(path)
        .map_err(|e| format!("failed to write signal list '{}': {e}", path.display()))?;

    for s in sorted {
        writeln!(out, "{}", s.output_name)
            .map_err(|e| format!("failed to write signal list '{}': {e}", path.display()))?;
    }

    Ok(())
}

fn write_signal_csv(path: &Path, signals: &[SignalState]) -> Result<(), String> {
    let mut sorted: Vec<&SignalState> = signals.iter().collect();
    sorted.sort_by(|a, b| {
        b.total_toggles
            .cmp(&a.total_toggles)
            .then_with(|| a.output_name.cmp(&b.output_name))
    });

    let mut out = File::create(path)
        .map_err(|e| format!("failed to write signal CSV '{}': {e}", path.display()))?;

    writeln!(out, "signal_name,total_toggle_count")
        .map_err(|e| format!("failed to write signal CSV '{}': {e}", path.display()))?;

    for s in sorted {
        writeln!(out, "\"{}\",{}", s.output_name, s.total_toggles)
            .map_err(|e| format!("failed to write signal CSV '{}': {e}", path.display()))?;
    }

    Ok(())
}

fn fmt_trimmed(value: f64) -> String {
    let mut s = format!("{:.6}", value);
    while s.ends_with('0') {
        s.pop();
    }
    if s.ends_with('.') {
        s.pop();
    }
    if s.is_empty() {
        "0".to_string()
    } else {
        s
    }
}

fn write_top_windows(path: &Path, windows: &[TopWindow]) -> Result<(), String> {
    let mut out = File::create(path)
        .map_err(|e| format!("failed to write top window file '{}': {e}", path.display()))?;

    writeln!(
        out,
        "{:<6} {:<16} {:<16} {:<16} {}",
        "rank", "left_ps", "right_ps", "total_toggles", "toggle_rate_per_ns"
    )
    .map_err(|e| format!("failed to write top window file '{}': {e}", path.display()))?;

    for (i, w) in windows.iter().enumerate() {
        let left_ps = (w.left_fs as f64) / 1000.0;
        let right_ps = (w.right_fs as f64) / 1000.0;
        let rate_per_ns = if w.effective_window_fs > 0 {
            (w.window_toggles as f64) * 1_000_000.0 / (w.effective_window_fs as f64)
        } else {
            0.0
        };

        writeln!(
            out,
            "{:<6} {:<16} {:<16} {:<16} {}",
            i + 1,
            fmt_trimmed(left_ps),
            fmt_trimmed(right_ps),
            w.window_toggles,
            fmt_trimmed(rate_per_ns)
        )
        .map_err(|e| format!("failed to write top window file '{}': {e}", path.display()))?;
    }

    Ok(())
}

fn write_debug_csv(path: &Path, plot: &PlotData, opts: &Options) -> Result<(), String> {
    let mut out = File::create(path)
        .map_err(|e| format!("failed to write debug CSV '{}': {e}", path.display()))?;

    writeln!(
        out,
        "time({}),toggle_rate(toggles/{}),cumulative_toggle_count",
        plot.x_unit_label, opts.rate_unit_label
    )
    .map_err(|e| format!("failed to write debug CSV '{}': {e}", path.display()))?;

    for i in 0..plot.x_values.len() {
        let cumulative_count =
            ((plot.y_cumulative[i] * plot.cumulative_unit_scale).round() as i128).max(0) as u64;
        writeln!(
            out,
            "{:.9},{:.9},{}",
            plot.x_values[i], plot.y_rate[i], cumulative_count
        )
        .map_err(|e| format!("failed to write debug CSV '{}': {e}", path.display()))?;
    }

    Ok(())
}

fn write_f64_array(
    out: &mut File,
    name: &str,
    data: &[f64],
    precision: usize,
) -> Result<(), String> {
    write!(out, "    const {} = [", name).map_err(|e| format!("failed to write HTML: {e}"))?;
    for (i, v) in data.iter().enumerate() {
        if i > 0 {
            write!(out, ",").map_err(|e| format!("failed to write HTML: {e}"))?;
        }
        write!(out, "{:.p$}", v, p = precision)
            .map_err(|e| format!("failed to write HTML: {e}"))?;
    }
    writeln!(out, "];\n").map_err(|e| format!("failed to write HTML: {e}"))?;
    Ok(())
}

fn write_html_report(
    out_path: &Path,
    opts: &Options,
    stats: &ParserStats,
    signal_count: u64,
    total_toggles: u64,
    plot: &PlotData,
    top_windows: &[TopWindow],
    uplot_js: &str,
    uplot_css: &str,
) -> Result<(), String> {
    let mut out = File::create(out_path)
        .map_err(|e| format!("failed to open output HTML '{}': {e}", out_path.display()))?;

    writeln!(out, "<!doctype html>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "<html lang=\"en\">").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "<head>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  <meta charset=\"utf-8\">")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  <title>{}</title>", html_escape(&opts.title))
        .map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <style>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    body{{margin:0;padding:18px;font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:#f8fafc;color:#0f172a;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    #chart-wrap{{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:10px;overflow:auto;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    #chart{{min-width:640px;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    .panel{{margin-top:12px;padding:12px;background:#fff;border:1px solid #e2e8f0;border-radius:8px;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    .panel h2{{font-size:0.95rem;margin:0 0 8px;color:#334155;}}"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    table{{border-collapse:collapse;width:100%;font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    th,td{{border:1px solid #e2e8f0;padding:6px 8px;text-align:left;vertical-align:top;}}"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    th{{background:#f8fafc;color:#334155;}}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </style>").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <style>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "{}", uplot_css).map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </style>").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <style>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    .uplot .u-legend{{margin:8px 0 0 0;text-align:left;}}"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    .uplot .u-legend.u-inline tr{{display:table-row;margin-right:0;}}"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    .uplot .u-legend.u-inline th,.uplot .u-legend.u-inline td{{display:table-cell;}}"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </style>").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "</head>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "<body>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  <div>Double-click LMB to zoom-full</div>")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "  <div id=\"chart-wrap\"><div id=\"chart\"></div></div>"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <div class=\"panel\" id=\"info-panel\">")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    <h2>Run Information</h2>")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    <table>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      <tbody>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Window size</th><td>{:.3} ps</td></tr>",
        (opts.win_fs as f64) / 1000.0
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Step size</th><td>{:.3} ps</td></tr>",
        (opts.step_fs as f64) / 1000.0
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;

    if opts.has_start_time {
        writeln!(
            out,
            "        <tr><th>Start time requested</th><td>{} fs</td></tr>",
            opts.start_fs
        )
        .map_err(|e| format!("failed to write HTML: {e}"))?;
        writeln!(
            out,
            "        <tr><th>Start time snapped</th><td>{} fs</td></tr>",
            opts.snapped_start_fs
        )
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    }
    if opts.has_stop_time {
        writeln!(
            out,
            "        <tr><th>Stop time</th><td>{} fs</td></tr>",
            opts.stop_fs
        )
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    }

    writeln!(
        out,
        "        <tr><th>X-axis unit</th><td>{}</td></tr>",
        html_escape(&plot.x_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Cumulative y-axis unit</th><td>{}</td></tr>",
        html_escape(&plot.cumulative_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Rate unit</th><td>{}</td></tr>",
        html_escape(&opts.rate_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Signals retained</th><td>{}</td></tr>",
        signal_count
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Alias vars skipped</th><td>{}</td></tr>",
        stats.alias_dedup_skipped
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Total toggles</th><td>{}</td></tr>",
        total_toggles
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Rendered points</th><td>{}</td></tr>",
        plot.x_values.len()
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Input lines</th><td>{}</td></tr>",
        stats.total_lines
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Timestamp lines</th><td>{}</td></tr>",
        stats.timestamp_updates
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Value changes parsed</th><td>{}</td></tr>",
        stats.parsed_value_changes
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Unknown id changes</th><td>{}</td></tr>",
        stats.unknown_id_changes
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        <tr><th>Timescale</th><td>{} fs</td></tr>",
        stats.timescale_fs
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      </tbody>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    </table>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </div>").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <div class=\"panel\" id=\"top-windows-panel\">")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    <h2>Top 20 Toggle Windows</h2>")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    <table>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      <thead>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "        <tr>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "          <th>Rank</th>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "          <th>Left ({})</th>",
        html_escape(&plot.x_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "          <th>Right ({})</th>",
        html_escape(&plot.x_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "          <th>Total Toggles</th>")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "          <th>Rate (toggles/{})</th>",
        html_escape(&opts.rate_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "        </tr>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      </thead>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      <tbody>").map_err(|e| format!("failed to write HTML: {e}"))?;

    for (i, w) in top_windows.iter().enumerate() {
        let left = (w.left_fs as f64) / (plot.x_unit_fs as f64);
        let right = (w.right_fs as f64) / (plot.x_unit_fs as f64);

        writeln!(
            out,
            "        <tr><td>{}</td><td>{:.6}</td><td>{:.6}</td><td>{}</td><td>{:.6}</td></tr>",
            i + 1,
            left,
            right,
            w.window_toggles,
            w.rate
        )
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    }

    if top_windows.is_empty() {
        writeln!(
            out,
            "        <tr><td colspan=\"5\">No windows with toggles found.</td></tr>"
        )
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    }

    writeln!(out, "      </tbody>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    </table>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </div>").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  <script>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "{}", uplot_js).map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  </script>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "  <script>").map_err(|e| format!("failed to write HTML: {e}"))?;

    write_f64_array(&mut out, "x", &plot.x_values, 6)?;
    write_f64_array(&mut out, "yRate", &plot.y_rate, 9)?;
    write_f64_array(&mut out, "yCum", &plot.y_cumulative, 9)?;

    writeln!(out, "    const data = [x, yRate, yCum];")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "    const title = \"{}\";",
        js_string_escape(&opts.title)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    const optsPlot = {{").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      title,").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "      width: Math.max(760, Math.min(1700, window.innerWidth - 70)),"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      height: 500,").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "      scales: {{ x: {{time:false}}, rate: {{auto:true}}, cum: {{auto:true}} }},"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      axes: [").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        {{ scale: 'x', label: 'Time ({})' }},",
        js_string_escape(&plot.x_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        {{ scale: 'rate', label: 'Toggle Rate (toggles/{})' }},",
        js_string_escape(&opts.rate_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "        {{ scale: 'cum', side: 1, grid: {{show:false}}, label: 'Cumulative Toggle Count ({})' }}",
        js_string_escape(&plot.cumulative_unit_label)
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      ],").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      series: [").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "        {{}},").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "        {{ label: 'Toggle rate', scale: 'rate', stroke: '#0f766e', width: 1, points: {{show: false}} }},")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "        {{ label: 'Cumulative', scale: 'cum', stroke: '#b45309', width: 1, points: {{show: false}} }}")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      ]").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    }};").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(
        out,
        "    const plot = new uPlot(optsPlot, data, document.getElementById('chart'));"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    window.addEventListener('resize', () => {{")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(
        out,
        "      const w = Math.max(760, Math.min(1700, window.innerWidth - 70));"
    )
    .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "      plot.setSize({{width: w, height: 500}});")
        .map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "    }});").map_err(|e| format!("failed to write HTML: {e}"))?;

    writeln!(out, "  </script>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "</body>").map_err(|e| format!("failed to write HTML: {e}"))?;
    writeln!(out, "</html>").map_err(|e| format!("failed to write HTML: {e}"))?;

    Ok(())
}

fn default_title_from_input(input_path: &str) -> String {
    if input_path == "-" {
        return "stdin".to_string();
    }

    let p = Path::new(input_path);
    let mut base = p
        .file_name()
        .map(|x| x.to_string_lossy().to_string())
        .unwrap_or_else(|| input_path.to_string());

    if ends_with(&base, ".gz") {
        let new_len = base.len().saturating_sub(3);
        base.truncate(new_len);
    }

    if base.is_empty() {
        input_path.to_string()
    } else {
        base
    }
}

fn open_input(opts: &Options) -> Result<InputHandle, String> {
    if opts.input_path == "-" {
        return Ok(InputHandle {
            reader: Box::new(BufReader::with_capacity(INPUT_BUFFER_CAPACITY, io::stdin())),
            child: None,
        });
    }

    if ends_with(&opts.input_path, ".gz") {
        let mut child = match Command::new("pigz")
            .args(["-dc", "--", &opts.input_path])
            .stdout(Stdio::piped())
            .spawn()
        {
            Ok(c) => c,
            Err(_) => Command::new("gzip")
                .args(["-dc", "--", &opts.input_path])
                .stdout(Stdio::piped())
                .spawn()
                .map_err(|e| format!("failed to start decompressor: {e}"))?,
        };

        let stdout: ChildStdout = child
            .stdout
            .take()
            .ok_or_else(|| "failed to capture decompressor stdout".to_string())?;

        return Ok(InputHandle {
            reader: Box::new(BufReader::with_capacity(INPUT_BUFFER_CAPACITY, stdout)),
            child: Some(child),
        });
    }

    let f = File::open(&opts.input_path)
        .map_err(|e| format!("failed to open input '{}': {e}", opts.input_path))?;

    Ok(InputHandle {
        reader: Box::new(BufReader::with_capacity(INPUT_BUFFER_CAPACITY, f)),
        child: None,
    })
}

fn print_help(program: &str) {
    println!("Profiles VCD signal toggles over time with a sliding window and generates an offline static HTML report with embedded uPlot, plus signal/toggle summary files.\n");
    println!("{} [OPTIONS] input\n", program);
    println!("POSITIONALS:");
    println!("  input                       Input path (.vcd, .vcd.gz, or - for stdin)\n");
    println!("OPTIONS:");
    println!("  -h, --help                 Print this help message and exit");
    println!("      --outdir <dir>         Output directory [default: output]");
    println!("      --win-size <dur>       Sliding window size [default: 500ps]");
    println!("      --step-size <dur>      Step size [default: 50ps]");
    println!("      --start-time <dur>     Start time for plotted/selected windows (snapped down to a step boundary)");
    println!("      --stop-time <dur>      Stop time for plotted/selected windows");
    println!("      --rate-unit <unit>     Rate unit: fs/ps/ns/us/ms/s [default: ns]");
    println!("      --preamble <prefix>    Case-sensitive FQSN prefix filter");
    println!("      --allow-top-window-overlap <true|false>  Allow overlap among selected top windows [default: false]");
    println!("      --debug                Write debug CSV with time(<x_unit>),toggle_rate(toggles/<rate_unit>),cumulative_toggle_count");
    println!("      --title <text>         Report title (default: input basename)");
    println!("      --max-points <n>       Max rendered plot points (0 disables downsampling) [default: 200000]");
    println!("      --uplot-js <path>      Override uPlot JS asset path");
    println!("      --uplot-css <path>     Override uPlot CSS asset path");
}

fn parse_options() -> Result<ParseOutcome, String> {
    let mut opts = Options::default();

    let argv: Vec<String> = env::args().collect();
    let program = argv
        .first()
        .map(|s| {
            Path::new(s)
                .file_name()
                .map(|x| x.to_string_lossy().to_string())
                .unwrap_or_else(|| s.clone())
        })
        .unwrap_or_else(|| "vcd-toggle-profiler".to_string());

    let mut i = 1usize;
    while i < argv.len() {
        let arg = &argv[i];

        if arg == "-h" || arg == "--help" {
            print_help(&program);
            return Ok(ParseOutcome::Help);
        }

        if arg.starts_with("--") {
            let take_value =
                |idx: &mut usize, argv: &[String], name: &str| -> Result<String, String> {
                    *idx += 1;
                    if *idx >= argv.len() {
                        return Err(format!("missing value for {}", name));
                    }
                    Ok(argv[*idx].clone())
                };

            match arg.as_str() {
                "--outdir" => opts.outdir = take_value(&mut i, &argv, "--outdir")?,
                "--win-size" => {
                    let v = take_value(&mut i, &argv, "--win-size")?;
                    opts.win_fs =
                        parse_duration_to_fs(&v, false).map_err(|e| format!("--win-size {}", e))?;
                }
                "--step-size" => {
                    let v = take_value(&mut i, &argv, "--step-size")?;
                    opts.step_fs = parse_duration_to_fs(&v, false)
                        .map_err(|e| format!("--step-size {}", e))?;
                }
                "--start-time" => {
                    let v = take_value(&mut i, &argv, "--start-time")?;
                    opts.start_fs = parse_duration_to_fs(&v, true)
                        .map_err(|e| format!("--start-time {}", e))?;
                    opts.has_start_time = true;
                }
                "--stop-time" => {
                    let v = take_value(&mut i, &argv, "--stop-time")?;
                    opts.stop_fs =
                        parse_duration_to_fs(&v, true).map_err(|e| format!("--stop-time {}", e))?;
                    opts.has_stop_time = true;
                }
                "--rate-unit" => {
                    let v = take_value(&mut i, &argv, "--rate-unit")?;
                    let unit_fs = parse_unit_fs(&v)
                        .ok_or_else(|| "--rate-unit must be one of fs/ps/ns/us/ms/s".to_string())?;
                    opts.rate_unit_fs = unit_fs;
                    opts.rate_unit_label = v.to_ascii_lowercase();
                }
                "--preamble" => opts.preamble = take_value(&mut i, &argv, "--preamble")?,
                "--allow-top-window-overlap" => {
                    let v = take_value(&mut i, &argv, "--allow-top-window-overlap")?;
                    opts.allow_top_window_overlap = parse_bool(&v).ok_or_else(|| {
                        "--allow-top-window-overlap must be true or false".to_string()
                    })?;
                }
                "--debug" => opts.debug = true,
                "--title" => {
                    opts.title = take_value(&mut i, &argv, "--title")?;
                    opts.title_explicit = true;
                }
                "--max-points" => {
                    let v = take_value(&mut i, &argv, "--max-points")?;
                    opts.max_points = v
                        .parse::<u64>()
                        .map_err(|_| "--max-points must be an unsigned integer".to_string())?;
                }
                "--uplot-js" => opts.uplot_js_path = take_value(&mut i, &argv, "--uplot-js")?,
                "--uplot-css" => opts.uplot_css_path = take_value(&mut i, &argv, "--uplot-css")?,
                _ => return Err(format!("The following argument was not expected: {}", arg)),
            }

            i += 1;
            continue;
        }

        if arg.starts_with('-') && arg != "-" {
            return Err(format!("The following argument was not expected: {}", arg));
        }

        if opts.input_path.is_empty() {
            opts.input_path = arg.clone();
        } else {
            return Err(format!("unexpected positional argument: {}", arg));
        }

        i += 1;
    }

    if opts.input_path.is_empty() {
        return Err("input is required".to_string());
    }

    if opts.step_fs > opts.win_fs {
        return Err("--step-size must be <= --win-size".to_string());
    }
    if opts.win_fs % opts.step_fs != 0 {
        return Err("--win-size must be evenly divisible by --step-size".to_string());
    }

    if opts.has_start_time {
        opts.snapped_start_fs = (opts.start_fs / opts.step_fs) * opts.step_fs;
    }

    if opts.has_stop_time {
        let effective_start = if opts.has_start_time {
            opts.snapped_start_fs
        } else {
            0
        };
        if opts.stop_fs < effective_start {
            return Err("--stop-time must be >= snapped --start-time".to_string());
        }
    }

    Ok(ParseOutcome::Run(opts))
}

fn main() {
    let mut opts = match parse_options() {
        Ok(ParseOutcome::Help) => return,
        Ok(ParseOutcome::Run(opts)) => opts,
        Err(e) => {
            eprintln!("{}", e);
            eprintln!("Run with --help for more information.");
            std::process::exit(1);
        }
    };

    if !opts.title_explicit {
        opts.title = default_title_from_input(&opts.input_path);
    }

    if let Err(e) = run(opts) {
        eprintln!("error: {}", e);
        std::process::exit(1);
    }
}

fn run(opts: Options) -> Result<(), String> {
    fs::create_dir_all(&opts.outdir)
        .map_err(|e| format!("failed to create output directory '{}': {e}", opts.outdir))?;

    let mut input = open_input(&opts)?;

    let mut parser = VcdParser::new(&opts);
    let parse_ok = parser.parse(&mut input.reader, &opts.input_path).is_ok();

    if !parse_ok {
        let _ = input.close(false);
        return Err("failed while parsing input".to_string());
    }

    input.close(true)?;

    let series_all = parser.accumulator.build_series(opts.rate_unit_fs);
    let series = filter_series_by_time_range(&series_all, &opts);
    let top_series = filter_series_to_full_window_only(&series, opts.win_fs);
    let top_windows = select_top_windows(&top_series, opts.allow_top_window_overlap, 20);
    let plot_full = build_plot_data(&series);
    let plot = downsample_plot_data(&plot_full, opts.max_points);

    let outdir = PathBuf::from(&opts.outdir);
    let html_path = outdir.join("toggle_profile.html");
    let signal_list_path = outdir.join("signals.txt");
    let signal_csv_path = outdir.join("signal_toggle_counts.csv");
    let top_windows_path = outdir.join("top_20_windows.txt");
    let debug_csv_path = outdir.join("debug.csv");

    write_signal_list(&signal_list_path, &parser.signals)?;
    write_signal_csv(&signal_csv_path, &parser.signals)?;
    write_top_windows(&top_windows_path, &top_windows)?;
    if opts.debug {
        write_debug_csv(&debug_csv_path, &plot, &opts)?;
    }

    let js_path = resolve_asset_path(&opts.uplot_js_path, "third_party/uplot/uPlot.iife.js")
        .ok_or_else(|| {
            "could not resolve uPlot JS asset. Expected third_party/uplot/uPlot.iife.js".to_string()
        })?;
    let css_path = resolve_asset_path(&opts.uplot_css_path, "third_party/uplot/uPlot.min.css")
        .ok_or_else(|| {
            "could not resolve uPlot CSS asset. Expected third_party/uplot/uPlot.min.css"
                .to_string()
        })?;

    let uplot_js = read_text_file(&js_path)?;
    let uplot_css = read_text_file(&css_path)?;

    write_html_report(
        &html_path,
        &opts,
        &parser.stats,
        parser.signals.len() as u64,
        parser.accumulator.total_toggles,
        &plot,
        &top_windows,
        &uplot_js,
        &uplot_css,
    )?;

    println!("Input: {}", opts.input_path);
    println!("Outdir: {}", outdir.display());
    println!("Signals retained: {}", parser.signals.len());
    println!("Alias vars skipped: {}", parser.stats.alias_dedup_skipped);
    println!("Total toggles: {}", parser.accumulator.total_toggles);
    println!("Series points (full): {}", series_all.len());
    println!("Series points (time-filtered): {}", series.len());
    println!("Rendered points: {}", plot.x_values.len());
    println!("HTML: {}", html_path.display());
    println!("Signals list: {}", signal_list_path.display());
    println!("Signal CSV: {}", signal_csv_path.display());
    println!("Top windows: {}", top_windows_path.display());
    if opts.debug {
        println!("Debug CSV: {}", debug_csv_path.display());
    }
    if opts.has_start_time {
        println!("Start time requested: {} fs", opts.start_fs);
        println!("Start time snapped: {} fs", opts.snapped_start_fs);
    }
    if opts.has_stop_time {
        println!("Stop time: {} fs", opts.stop_fs);
    }

    Ok(())
}
