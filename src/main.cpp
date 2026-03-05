#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string input_path;
  std::string outdir = "output";
  std::string title;
  bool title_explicit = false;
  std::string preamble;

  uint64_t win_fs = 500000;      // 500ps
  uint64_t step_fs = 50000;      // 50ps
  uint64_t rate_unit_fs = 1000000;  // 1ns
  std::string rate_unit_label = "ns";
  bool has_start_time = false;
  bool has_stop_time = false;
  uint64_t start_fs = 0;
  uint64_t snapped_start_fs = 0;
  uint64_t stop_fs = 0;

  bool allow_top_window_overlap = false;
  bool debug = false;
  uint64_t max_points = 200000;

  std::string uplot_js_path;
  std::string uplot_css_path;
};

struct SignalState {
  std::string output_name;
  std::string leaf_name;
  size_t depth = 0;

  uint32_t width = 1;
  bool initialized = false;
  std::string value;

  uint64_t total_toggles = 0;
};

struct ParserStats {
  uint64_t total_lines = 0;
  uint64_t timestamp_updates = 0;
  uint64_t parsed_value_changes = 0;
  uint64_t unknown_id_changes = 0;
  uint64_t alias_dedup_skipped = 0;
  bool found_timescale = false;
  uint64_t timescale_fs = 1000;
};

struct SeriesPoint {
  uint64_t step_index = 0;
  uint64_t right_fs = 0;
  uint64_t left_fs = 0;
  uint64_t effective_window_fs = 0;
  uint64_t window_toggles = 0;
  uint64_t cumulative_toggles = 0;
  double rate = 0.0;
};

struct TopWindow {
  uint64_t left_fs = 0;
  uint64_t right_fs = 0;
  uint64_t effective_window_fs = 0;
  uint64_t window_toggles = 0;
  double rate = 0.0;
};

struct InputHandle {
  FILE* stream = nullptr;
  bool is_stdin = false;
  pid_t gzip_pid = -1;
};

constexpr size_t kInputBufferSize = 1 << 20;

struct FastU64Hash {
  size_t operator()(uint64_t x) const noexcept {
    x ^= (x >> 33);
    x *= 0xff51afd7ed558ccdULL;
    x ^= (x >> 33);
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= (x >> 33);
    return static_cast<size_t>(x);
  }
};

struct FastStringViewHash {
  size_t operator()(std::string_view sv) const noexcept {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : sv) {
      h ^= static_cast<unsigned char>(c);
      h *= 0x100000001b3ULL;
    }
    return static_cast<size_t>(h);
  }
};

struct FastStringViewEq {
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

using U64SignedMap = std::unordered_map<uint64_t, int64_t, FastU64Hash>;
using U64Map = std::unordered_map<uint64_t, uint64_t, FastU64Hash>;
using StringViewIndexMap =
    std::unordered_map<std::string_view, size_t, FastStringViewHash, FastStringViewEq>;
using StringViewSet =
    std::unordered_set<std::string_view, FastStringViewHash, FastStringViewEq>;

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

inline bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && IsAsciiSpace(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && IsAsciiSpace(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

void SkipWs(const char*& p) {
  while (*p != '\0' && IsAsciiSpace(*p)) {
    ++p;
  }
}

std::string_view ReadToken(const char*& p) {
  SkipWs(p);
  const char* begin = p;
  while (*p != '\0' && !IsAsciiSpace(*p)) {
    ++p;
  }
  return std::string_view(begin, static_cast<size_t>(p - begin));
}

char NormalizeLogicChar(char c) {
  switch (c) {
    case '0':
      return '0';
    case '1':
      return '1';
    case 'x':
    case 'X':
      return 'x';
    case 'z':
    case 'Z':
      return 'z';
    default:
      return 'x';
  }
}

void FillNormalizedValue(std::string* dst, std::string_view raw) {
  const size_t width = dst->size();
  if (width == 0) {
    return;
  }

  char* out = dst->data();

  if (width == 1) {
    out[0] = raw.empty() ? 'x' : NormalizeLogicChar(raw.back());
    return;
  }

  if (raw.size() >= width) {
    const size_t offset = raw.size() - width;
    for (size_t i = 0; i < width; ++i) {
      out[i] = NormalizeLogicChar(raw[offset + i]);
    }
    return;
  }

  const char fill = raw.empty() ? 'x' : NormalizeLogicChar(raw.front());
  const size_t pad = width - raw.size();
  std::fill(out, out + static_cast<std::ptrdiff_t>(pad), fill);
  for (size_t i = 0; i < raw.size(); ++i) {
    out[pad + i] = NormalizeLogicChar(raw[i]);
  }
}

uint64_t CountTogglesAndUpdate(std::string* dst, std::string_view raw) {
  const size_t width = dst->size();
  if (width == 0) {
    return 0;
  }

  char* cur = dst->data();
  uint64_t toggles = 0;

  if (width == 1) {
    const char next = raw.empty() ? 'x' : NormalizeLogicChar(raw.back());
    if (cur[0] != next) {
      cur[0] = next;
      return 1;
    }
    return 0;
  }

  if (raw.size() >= width) {
    const size_t offset = raw.size() - width;
    for (size_t i = 0; i < width; ++i) {
      const char next = NormalizeLogicChar(raw[offset + i]);
      if (cur[i] != next) {
        cur[i] = next;
        ++toggles;
      }
    }
    return toggles;
  }

  const char fill = raw.empty() ? 'x' : NormalizeLogicChar(raw.front());
  const size_t pad = width - raw.size();
  for (size_t i = 0; i < pad; ++i) {
    if (cur[i] != fill) {
      cur[i] = fill;
      ++toggles;
    }
  }
  for (size_t i = 0; i < raw.size(); ++i) {
    const char next = NormalizeLogicChar(raw[i]);
    const size_t idx = pad + i;
    if (cur[idx] != next) {
      cur[idx] = next;
      ++toggles;
    }
  }
  return toggles;
}

bool ParseUint64(std::string_view token, uint64_t* out) {
  if (token.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (char c : token) {
    if (!IsAsciiDigit(c)) {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

uint64_t SaturatingU64(unsigned __int128 value) {
  constexpr unsigned __int128 kMax = static_cast<unsigned __int128>(std::numeric_limits<uint64_t>::max());
  if (value > kMax) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(value);
}

uint64_t CeilDivU64(uint64_t num, uint64_t den) {
  if (den == 0) {
    return std::numeric_limits<uint64_t>::max();
  }
  return (num / den) + ((num % den) ? 1ULL : 0ULL);
}

std::string ToLowerCopy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

bool ParseUnitFs(std::string_view unit, uint64_t* unit_fs) {
  const std::string lower = ToLowerCopy(unit);
  if (lower == "fs") {
    *unit_fs = 1ULL;
    return true;
  }
  if (lower == "ps") {
    *unit_fs = 1000ULL;
    return true;
  }
  if (lower == "ns") {
    *unit_fs = 1000000ULL;
    return true;
  }
  if (lower == "us") {
    *unit_fs = 1000000000ULL;
    return true;
  }
  if (lower == "ms") {
    *unit_fs = 1000000000000ULL;
    return true;
  }
  if (lower == "s") {
    *unit_fs = 1000000000000000ULL;
    return true;
  }
  return false;
}

bool ParseDurationToFs(std::string_view text, uint64_t* fs_value, std::string* error) {
  text = Trim(text);
  if (text.empty()) {
    *error = "duration is empty";
    return false;
  }

  size_t split = 0;
  while (split < text.size() && IsAsciiDigit(text[split])) {
    ++split;
  }

  if (split == 0 || split == text.size()) {
    *error = "duration must be in form <number><unit>, e.g. 500ps";
    return false;
  }

  uint64_t magnitude = 0;
  if (!ParseUint64(text.substr(0, split), &magnitude) || magnitude == 0) {
    *error = "duration magnitude must be a positive integer";
    return false;
  }

  uint64_t unit_fs = 0;
  if (!ParseUnitFs(text.substr(split), &unit_fs)) {
    *error = "unsupported unit; use fs/ps/ns/us/ms/s";
    return false;
  }

  *fs_value = SaturatingU64(static_cast<unsigned __int128>(magnitude) * unit_fs);
  return true;
}

bool ParseNonNegativeDurationToFs(std::string_view text, uint64_t* fs_value, std::string* error) {
  text = Trim(text);
  if (text.empty()) {
    *error = "duration is empty";
    return false;
  }

  size_t split = 0;
  while (split < text.size() && IsAsciiDigit(text[split])) {
    ++split;
  }

  if (split == 0 || split == text.size()) {
    *error = "duration must be in form <number><unit>, e.g. 500ps";
    return false;
  }

  uint64_t magnitude = 0;
  if (!ParseUint64(text.substr(0, split), &magnitude)) {
    *error = "duration magnitude must be a non-negative integer";
    return false;
  }

  uint64_t unit_fs = 0;
  if (!ParseUnitFs(text.substr(split), &unit_fs)) {
    *error = "unsupported unit; use fs/ps/ns/us/ms/s";
    return false;
  }

  *fs_value = SaturatingU64(static_cast<unsigned __int128>(magnitude) * unit_fs);
  return true;
}

bool ParseTimescaleFs(const std::string& raw, uint64_t* out_fs, std::string* error) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (char c : raw) {
    cleaned.push_back(c == '$' ? ' ' : c);
  }

  std::istringstream iss(cleaned);
  std::string tok;
  std::string number_tok;
  std::string unit_tok;

  while (iss >> tok) {
    const std::string lower = ToLowerCopy(tok);
    if (lower == "timescale" || lower == "end") {
      continue;
    }
    if (number_tok.empty()) {
      number_tok = tok;
      continue;
    }
    if (unit_tok.empty()) {
      unit_tok = tok;
      break;
    }
  }

  if (number_tok.empty()) {
    *error = "missing numeric timescale token";
    return false;
  }

  size_t split = 0;
  while (split < number_tok.size() && IsAsciiDigit(number_tok[split])) {
    ++split;
  }

  if (split == 0) {
    *error = "invalid timescale number token";
    return false;
  }

  uint64_t magnitude = 0;
  if (!ParseUint64(std::string_view(number_tok).substr(0, split), &magnitude) || magnitude == 0) {
    *error = "timescale magnitude must be positive integer";
    return false;
  }

  if (split < number_tok.size()) {
    unit_tok = number_tok.substr(split);
  }

  if (unit_tok.empty()) {
    *error = "missing timescale unit token";
    return false;
  }

  uint64_t unit_fs = 0;
  if (!ParseUnitFs(unit_tok, &unit_fs)) {
    *error = "unsupported timescale unit";
    return false;
  }

  *out_fs = SaturatingU64(static_cast<unsigned __int128>(magnitude) * unit_fs);
  return true;
}

std::string HtmlEscape(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (char c : input) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '\"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string JsStringEscape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::optional<fs::path> ResolveAssetPath(const std::string& explicit_path, const fs::path& relative_from_root) {
  std::vector<fs::path> candidates;
  if (!explicit_path.empty()) {
    candidates.emplace_back(explicit_path);
  }
  candidates.emplace_back(relative_from_root);
  candidates.emplace_back(fs::path("..") / relative_from_root);
  candidates.emplace_back(fs::path("../..") / relative_from_root);

  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec && fs::is_regular_file(candidate, ec)) {
      return fs::canonical(candidate, ec);
    }
  }
  return std::nullopt;
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file: " + path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void AddSignedDelta(U64SignedMap* m, uint64_t key, int64_t delta) {
  auto [it, inserted] = m->try_emplace(key, delta);
  if (inserted) {
    return;
  }
  it->second += delta;
  if (it->second == 0) {
    m->erase(it);
  }
}

void AddUnsignedDelta(U64Map* m, uint64_t key, uint64_t delta) {
  auto [it, inserted] = m->try_emplace(key, delta);
  if (inserted) {
    return;
  }
  it->second += delta;
}

class StepAccumulator {
 public:
  StepAccumulator(uint64_t win_fs, uint64_t step_fs)
      : win_fs_(win_fs), step_fs_(step_fs) {}

  void AddEvent(uint64_t timestamp_fs, uint64_t toggles) {
    if (toggles == 0) {
      return;
    }

    const uint64_t n_lo = CeilDivU64(timestamp_fs, step_fs_);

    const unsigned __int128 hi_num = static_cast<unsigned __int128>(timestamp_fs) +
                                     static_cast<unsigned __int128>(win_fs_) - 1;
    const uint64_t n_hi = SaturatingU64(hi_num / step_fs_);

    if (n_lo <= n_hi) {
      AddSignedDelta(&window_delta_, n_lo, static_cast<int64_t>(toggles));
      if (n_hi != std::numeric_limits<uint64_t>::max()) {
        AddSignedDelta(&window_delta_, n_hi + 1, -static_cast<int64_t>(toggles));
      }
      if (n_hi > max_step_) {
        max_step_ = n_hi;
      }
    }

    AddUnsignedDelta(&cumulative_start_delta_, n_lo, toggles);
    if (n_lo > max_step_) {
      max_step_ = n_lo;
    }

    total_toggles_ += toggles;
  }

  uint64_t max_step() const { return max_step_; }
  uint64_t total_toggles() const { return total_toggles_; }

  std::vector<SeriesPoint> BuildSeries(uint64_t rate_unit_fs) const {
    std::vector<std::pair<uint64_t, int64_t>> window_deltas(window_delta_.begin(), window_delta_.end());
    std::vector<std::pair<uint64_t, uint64_t>> cumulative_deltas(cumulative_start_delta_.begin(), cumulative_start_delta_.end());

    std::sort(window_deltas.begin(), window_deltas.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(cumulative_deltas.begin(), cumulative_deltas.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<SeriesPoint> out;
    if (max_step_ > std::numeric_limits<size_t>::max() - 1) {
      throw std::runtime_error("step count too large for this build");
    }
    out.reserve(static_cast<size_t>(max_step_) + 1);

    size_t wi = 0;
    size_t ci = 0;
    int64_t running_window = 0;
    uint64_t running_cumulative = 0;

    for (uint64_t step = 0; step <= max_step_; ++step) {
      while (wi < window_deltas.size() && window_deltas[wi].first == step) {
        running_window += window_deltas[wi].second;
        ++wi;
      }
      while (ci < cumulative_deltas.size() && cumulative_deltas[ci].first == step) {
        running_cumulative += cumulative_deltas[ci].second;
        ++ci;
      }

      const unsigned __int128 right128 = static_cast<unsigned __int128>(step) * step_fs_;
      const uint64_t right_fs = SaturatingU64(right128);
      const uint64_t left_fs = (right_fs > win_fs_) ? (right_fs - win_fs_) : 0;
      const uint64_t effective_window_fs = right_fs - left_fs;

      uint64_t window_toggles = 0;
      if (running_window > 0) {
        window_toggles = static_cast<uint64_t>(running_window);
      }

      double rate = 0.0;
      if (effective_window_fs > 0 && window_toggles > 0) {
        rate = static_cast<double>(window_toggles) * static_cast<double>(rate_unit_fs) /
               static_cast<double>(effective_window_fs);
      }

      out.push_back(SeriesPoint{
          .step_index = step,
          .right_fs = right_fs,
          .left_fs = left_fs,
          .effective_window_fs = effective_window_fs,
          .window_toggles = window_toggles,
          .cumulative_toggles = running_cumulative,
          .rate = rate,
      });

      if (step == std::numeric_limits<uint64_t>::max()) {
        break;
      }
    }

    return out;
  }

 private:
  uint64_t win_fs_;
  uint64_t step_fs_;

  U64SignedMap window_delta_;
  U64Map cumulative_start_delta_;

  uint64_t max_step_ = 0;
  uint64_t total_toggles_ = 0;
};

class VcdParser {
 public:
  explicit VcdParser(const Options& opts)
      : opts_(opts), accumulator_(opts.win_fs, opts.step_fs) {}

  void Parse(FILE* file, const std::string& input_name) {
    std::vector<char> line_buf(kInputBufferSize);

    bool in_timescale = false;
    std::string timescale_accum;

    while (std::fgets(line_buf.data(), static_cast<int>(line_buf.size()), file) != nullptr) {
      ++stats_.total_lines;

      std::string_view line = Trim(line_buf.data());
      if (line.empty()) {
        continue;
      }

      if (!seen_enddefinitions_) {
        if (StartsWith(line, "$scope")) {
          ParseScope(line);
          continue;
        }
        if (StartsWith(line, "$upscope")) {
          if (!scope_stack_.empty()) {
            scope_stack_.pop_back();
          }
          continue;
        }
        if (StartsWith(line, "$var")) {
          ParseVar(line);
          continue;
        }

        if (line.find("$timescale") != std::string_view::npos) {
          in_timescale = true;
        }

        if (in_timescale) {
          timescale_accum.append(line.data(), line.size());
          timescale_accum.push_back(' ');
          if (line.find("$end") != std::string_view::npos) {
            uint64_t parsed_fs = 0;
            std::string err;
            if (ParseTimescaleFs(timescale_accum, &parsed_fs, &err)) {
              timescale_fs_ = parsed_fs;
              stats_.timescale_fs = parsed_fs;
              stats_.found_timescale = true;
            } else {
              std::cerr << "warning: failed to parse $timescale in '" << input_name
                        << "': " << err << "; defaulting to 1ps\n";
            }
            timescale_accum.clear();
            in_timescale = false;
          }
          continue;
        }

        if (line.find("$enddefinitions") != std::string_view::npos) {
          seen_enddefinitions_ = true;
        }
        continue;
      }

      ParseDataLine(line);
    }
  }

  const ParserStats& stats() const { return stats_; }
  const StepAccumulator& accumulator() const { return accumulator_; }
  const std::vector<SignalState>& signals() const { return signals_; }

 private:
  void ParseScope(std::string_view line) {
    const char* p = line.data();
    (void)ReadToken(p);  // $scope
    (void)ReadToken(p);  // scope type
    const std::string_view name_tok = ReadToken(p);
    if (!name_tok.empty()) {
      scope_stack_.emplace_back(name_tok);
    }
  }

  static size_t HierarchyDepth(std::string_view fqsn) {
    if (fqsn.empty()) {
      return 0;
    }
    size_t depth = 1;
    for (char c : fqsn) {
      if (c == '.') {
        ++depth;
      }
    }
    return depth;
  }

  std::string BuildFqsn(std::string_view leaf) const {
    std::string out;
    size_t total = leaf.size();
    for (const std::string& scope : scope_stack_) {
      total += scope.size() + 1;
    }
    out.reserve(total);

    bool first = true;
    for (const std::string& scope : scope_stack_) {
      if (!first) {
        out.push_back('.');
      }
      out += scope;
      first = false;
    }

    if (!out.empty()) {
      out.push_back('.');
    }
    out.append(leaf.data(), leaf.size());
    return out;
  }

  std::optional<std::string> ApplyPreamble(std::string_view fqsn) const {
    if (opts_.preamble.empty()) {
      return std::string(fqsn);
    }
    if (!StartsWith(fqsn, opts_.preamble)) {
      return std::nullopt;
    }
    std::string trimmed(fqsn.substr(opts_.preamble.size()));
    if (!trimmed.empty() && trimmed.front() == '.') {
      trimmed.erase(trimmed.begin());
    }
    if (trimmed.empty()) {
      return std::string(fqsn);
    }
    return trimmed;
  }

  void ParseVar(std::string_view line) {
    const char* p = line.data();
    (void)ReadToken(p);               // $var
    (void)ReadToken(p);               // type
    const std::string_view width_tok = ReadToken(p);
    const std::string_view id_tok = ReadToken(p);

    if (id_tok.empty()) {
      return;
    }
    std::string_view id_key = id_tok;
    auto known_it = known_ids_.find(id_tok);
    if (known_it == known_ids_.end()) {
      id_storage_.emplace_back(id_tok);
      id_key = id_storage_.back();
      known_ids_.insert(id_key);
    } else {
      id_key = *known_it;
    }

    std::string_view leaf_tok;
    for (;;) {
      std::string_view tok = ReadToken(p);
      if (tok.empty() || tok == "$end") {
        break;
      }
      if (leaf_tok.empty()) {
        leaf_tok = tok;
      }
    }

    if (leaf_tok.empty()) {
      leaf_tok = "unnamed";
    }

    uint64_t width = 1;
    if (!ParseUint64(width_tok, &width) || width == 0) {
      width = 1;
    }

    auto existing_it = id_to_index_.find(id_key);
    if (existing_it != id_to_index_.end()) {
      ++stats_.alias_dedup_skipped;
      SignalState& existing = signals_[existing_it->second];
      if (width > existing.width) {
        existing.width = static_cast<uint32_t>(std::min<uint64_t>(width, std::numeric_limits<uint32_t>::max()));
      }
      return;
    }

    const std::string fqsn = BuildFqsn(leaf_tok);
    const std::optional<std::string> filtered_name = ApplyPreamble(fqsn);
    if (!filtered_name) {
      return;
    }

    SignalState signal;
    signal.output_name = *filtered_name;
    signal.leaf_name = std::string(leaf_tok);
    signal.depth = HierarchyDepth(signal.output_name);
    signal.width = static_cast<uint32_t>(std::min<uint64_t>(width, std::numeric_limits<uint32_t>::max()));
    signal.value.clear();

    const size_t idx = signals_.size();
    signals_.push_back(std::move(signal));
    id_to_index_.emplace(id_key, idx);
  }

  void ParseDataLine(std::string_view line) {
    const char lead = line.front();

    if (lead == '#') {
      uint64_t timestamp = 0;
      if (ParseUint64(Trim(line.substr(1)), &timestamp)) {
        current_timestamp_ = timestamp;
        ++stats_.timestamp_updates;
      }
      return;
    }

    if (lead == '$') {
      return;
    }

    uint64_t toggles_sum = 0;

    if (lead == 'b' || lead == 'B' || lead == 'r' || lead == 'R') {
      const char* p = line.data() + 1;
      const std::string_view value = ReadToken(p);
      const std::string_view id = ReadToken(p);
      if (!value.empty() && !id.empty()) {
        toggles_sum = ApplyChange(id, value);
      }
    } else if (lead == '0' || lead == '1' || lead == 'x' || lead == 'X' || lead == 'z' || lead == 'Z' ||
               lead == 'u' || lead == 'U' || lead == 'w' || lead == 'W' || lead == 'h' || lead == 'H' ||
               lead == 'l' || lead == 'L' || lead == '-') {
      std::string_view id = Trim(line.substr(1));
      if (!id.empty()) {
        const char c = NormalizeLogicChar(lead);
        const char buf[2] = {c, '\0'};
        toggles_sum = ApplyChange(id, std::string_view(buf, 1));
      }
    }

    if (toggles_sum > 0) {
      ++stats_.parsed_value_changes;
      const uint64_t timestamp_fs = SaturatingU64(static_cast<unsigned __int128>(current_timestamp_) * timescale_fs_);
      accumulator_.AddEvent(timestamp_fs, toggles_sum);
    }
  }

  uint64_t ApplyChange(std::string_view id_sv, std::string_view raw_value) {
    auto it = id_to_index_.find(id_sv);
    if (it == id_to_index_.end()) {
      if (known_ids_.find(id_sv) == known_ids_.end()) {
        ++stats_.unknown_id_changes;
      }
      return 0;
    }

    SignalState& signal = signals_[it->second];
    const size_t width = std::max<uint32_t>(signal.width, 1U);
    if (signal.value.size() != width) {
      signal.value.assign(width, 'x');
      if (signal.initialized) {
        FillNormalizedValue(&signal.value, raw_value);
        return 0;
      }
    }

    if (!signal.initialized) {
      signal.initialized = true;
      FillNormalizedValue(&signal.value, raw_value);
      return 0;
    }
    uint64_t toggles = CountTogglesAndUpdate(&signal.value, raw_value);

    if (toggles > 0) {
      signal.total_toggles += toggles;
    }

    return toggles;
  }

  const Options& opts_;
  StepAccumulator accumulator_;
  ParserStats stats_;

  bool seen_enddefinitions_ = false;
  uint64_t timescale_fs_ = 1000;
  uint64_t current_timestamp_ = 0;

  std::vector<std::string> scope_stack_;
  StringViewIndexMap id_to_index_;
  StringViewSet known_ids_;
  std::deque<std::string> id_storage_;
  std::vector<SignalState> signals_;
};

bool ParseBool(std::string_view s, bool* out) {
  const std::string lower = ToLowerCopy(Trim(s));
  if (lower == "true") {
    *out = true;
    return true;
  }
  if (lower == "false") {
    *out = false;
    return true;
  }
  return false;
}

int ParseOptions(int argc, char** argv, Options* opts) {
  CLI::App app{
      "Profiles VCD signal toggles over time with a sliding window and generates an offline static HTML report "
      "with embedded uPlot, plus signal/toggle summary files."};
  app.name("vcd-toggle-profiler");

  std::string win_size_text = "500ps";
  std::string step_size_text = "50ps";
  std::string rate_unit_text = opts->rate_unit_label;
  std::string overlap_text = opts->allow_top_window_overlap ? "true" : "false";
  std::string start_time_text;
  std::string stop_time_text;

  app.add_option("input", opts->input_path, "Input path (.vcd, .vcd.gz, or - for stdin)")
      ->required();
  app.add_option("--outdir", opts->outdir, "Output directory")->default_val(opts->outdir);
  app.add_option("--win-size", win_size_text, "Sliding window size (e.g. 500ps)")
      ->default_val(win_size_text);
  app.add_option("--step-size", step_size_text, "Step size (e.g. 50ps)")
      ->default_val(step_size_text);
  app.add_option("--start-time",
                 start_time_text,
                 "Start time for plotted/selected windows (snapped down to a step boundary)");
  app.add_option("--stop-time",
                 stop_time_text,
                 "Stop time for plotted/selected windows");
  app.add_option("--rate-unit", rate_unit_text, "Rate unit: fs/ps/ns/us/ms/s")
      ->default_val(rate_unit_text);
  app.add_option("--preamble", opts->preamble, "Case-sensitive FQSN prefix filter");
  app.add_option("--allow-top-window-overlap",
                 overlap_text,
                 "Allow overlap among selected top windows (true|false)")
      ->default_val(overlap_text);
  app.add_flag("--debug", opts->debug, "Write debug CSV with time,toggle_rate,cumulative_toggle_count");
  app.add_option("--title", opts->title, "Report title (default: input basename)");
  app.add_option("--max-points", opts->max_points, "Max rendered plot points (0 disables downsampling)")
      ->default_val(opts->max_points);
  app.add_option("--uplot-js", opts->uplot_js_path, "Override uPlot JS asset path");
  app.add_option("--uplot-css", opts->uplot_css_path, "Override uPlot CSS asset path");

  try {
    app.parse(argc, argv);
  } catch (const CLI::CallForHelp& e) {
    app.exit(e);
    return -1;
  } catch (const CLI::CallForAllHelp& e) {
    app.exit(e);
    return -1;
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  opts->title_explicit = (app.count("--title") > 0);

  std::string err;
  if (!ParseDurationToFs(win_size_text, &opts->win_fs, &err)) {
    std::cerr << "error: --win-size " << err << "\n";
    return 1;
  }
  if (!ParseDurationToFs(step_size_text, &opts->step_fs, &err)) {
    std::cerr << "error: --step-size " << err << "\n";
    return 1;
  }

  uint64_t unit_fs = 0;
  if (!ParseUnitFs(rate_unit_text, &unit_fs)) {
    std::cerr << "error: --rate-unit must be one of fs/ps/ns/us/ms/s\n";
    return 1;
  }
  opts->rate_unit_fs = unit_fs;
  opts->rate_unit_label = ToLowerCopy(rate_unit_text);

  bool overlap = false;
  if (!ParseBool(overlap_text, &overlap)) {
    std::cerr << "error: --allow-top-window-overlap must be true or false\n";
    return 1;
  }
  opts->allow_top_window_overlap = overlap;

  if (opts->step_fs > opts->win_fs) {
    std::cerr << "error: --step-size must be <= --win-size\n";
    return 1;
  }
  if ((opts->win_fs % opts->step_fs) != 0) {
    std::cerr << "error: --win-size must be evenly divisible by --step-size\n";
    return 1;
  }

  if (!start_time_text.empty()) {
    if (!ParseNonNegativeDurationToFs(start_time_text, &opts->start_fs, &err)) {
      std::cerr << "error: --start-time " << err << "\n";
      return 1;
    }
    opts->has_start_time = true;
  }

  if (!stop_time_text.empty()) {
    if (!ParseNonNegativeDurationToFs(stop_time_text, &opts->stop_fs, &err)) {
      std::cerr << "error: --stop-time " << err << "\n";
      return 1;
    }
    opts->has_stop_time = true;
  }

  if (opts->has_start_time) {
    opts->snapped_start_fs = (opts->start_fs / opts->step_fs) * opts->step_fs;
  }

  if (opts->has_stop_time) {
    const uint64_t effective_start = opts->has_start_time ? opts->snapped_start_fs : 0ULL;
    if (opts->stop_fs < effective_start) {
      std::cerr << "error: --stop-time must be >= snapped --start-time\n";
      return 1;
    }
  }

  return 0;
}

std::string DefaultTitleFromInput(std::string_view input_path) {
  if (input_path == "-") {
    return "stdin";
  }

  fs::path p(input_path);
  std::string base = p.filename().string();
  if (base.empty()) {
    base = std::string(input_path);
  }

  if (EndsWith(base, ".gz")) {
    base.resize(base.size() - 3);
  }

  return base.empty() ? std::string(input_path) : base;
}

bool OpenInput(const Options& opts, InputHandle* handle, std::string* error) {
  if (opts.input_path == "-") {
    handle->stream = stdin;
    handle->is_stdin = true;
    std::setvbuf(handle->stream, nullptr, _IOFBF, kInputBufferSize);
    return true;
  }

  if (EndsWith(opts.input_path, ".gz")) {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
      *error = "failed to create gzip pipe: ";
      *error += std::strerror(errno);
      return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
      close(fds[0]);
      close(fds[1]);
      *error = "failed to fork gzip process: ";
      *error += std::strerror(errno);
      return false;
    }

    if (pid == 0) {
      close(fds[0]);
      if (dup2(fds[1], STDOUT_FILENO) < 0) {
        _exit(127);
      }
      close(fds[1]);
      execlp("pigz", "pigz", "-dc", "--", opts.input_path.c_str(), static_cast<char*>(nullptr));
      execlp("gzip", "gzip", "-dc", "--", opts.input_path.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }

    close(fds[1]);
    FILE* stream = fdopen(fds[0], "rb");
    if (stream == nullptr) {
      close(fds[0]);
      int status = 0;
      waitpid(pid, &status, 0);
      *error = "failed to open gzip output stream: ";
      *error += std::strerror(errno);
      return false;
    }

    handle->stream = stream;
    handle->gzip_pid = pid;
    std::setvbuf(handle->stream, nullptr, _IOFBF, kInputBufferSize);
    return true;
  }

  FILE* input = std::fopen(opts.input_path.c_str(), "rb");
  if (input == nullptr) {
    *error = "failed to open input '" + opts.input_path + "': " + std::strerror(errno);
    return false;
  }
  handle->stream = input;
  std::setvbuf(handle->stream, nullptr, _IOFBF, kInputBufferSize);
  return true;
}

bool CloseInput(InputHandle* handle, bool parse_succeeded, std::string* error) {
  if (handle->stream != nullptr && !handle->is_stdin) {
    std::fclose(handle->stream);
    handle->stream = nullptr;
  }

  if (handle->gzip_pid > 0) {
    int status = 0;
    if (waitpid(handle->gzip_pid, &status, 0) < 0) {
      *error = "failed waiting for gzip process: ";
      *error += std::strerror(errno);
      return false;
    }

    handle->gzip_pid = -1;

    if (!parse_succeeded) {
      return true;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      return true;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
      *error = "gzip command failed to execute. Install gzip or provide uncompressed input.";
      return false;
    }

    *error = "gzip decompression failed";
    return false;
  }

  return true;
}

bool IntervalsOverlapOpenLeftClosedRight(const TopWindow& a, const TopWindow& b) {
  const uint64_t l = std::max(a.left_fs, b.left_fs);
  const uint64_t r = std::min(a.right_fs, b.right_fs);
  return l < r;
}

std::vector<TopWindow> SelectTopWindows(const std::vector<SeriesPoint>& points,
                                        bool allow_overlap,
                                        size_t top_n) {
  std::vector<TopWindow> candidates;
  candidates.reserve(points.size());

  for (const SeriesPoint& p : points) {
    if (p.window_toggles == 0) {
      continue;
    }
    candidates.push_back(TopWindow{
        .left_fs = p.left_fs,
        .right_fs = p.right_fs,
        .effective_window_fs = p.effective_window_fs,
        .window_toggles = p.window_toggles,
        .rate = p.rate,
    });
  }

  std::sort(candidates.begin(), candidates.end(), [](const TopWindow& a, const TopWindow& b) {
    if (a.window_toggles != b.window_toggles) {
      return a.window_toggles > b.window_toggles;
    }
    return a.right_fs < b.right_fs;
  });

  std::vector<TopWindow> selected;
  selected.reserve(top_n);

  for (const TopWindow& c : candidates) {
    if (selected.size() >= top_n) {
      break;
    }
    if (allow_overlap) {
      selected.push_back(c);
      continue;
    }

    bool overlaps = false;
    for (const TopWindow& s : selected) {
      if (IntervalsOverlapOpenLeftClosedRight(c, s)) {
        overlaps = true;
        break;
      }
    }

    if (!overlaps) {
      selected.push_back(c);
    }
  }

  return selected;
}

std::vector<SeriesPoint> FilterSeriesByTimeRange(const std::vector<SeriesPoint>& points, const Options& opts) {
  const uint64_t start_fs = opts.has_start_time ? opts.snapped_start_fs : 0ULL;
  const bool use_stop = opts.has_stop_time;
  const uint64_t stop_fs = opts.stop_fs;

  std::vector<SeriesPoint> filtered;
  filtered.reserve(points.size());

  for (const SeriesPoint& p : points) {
    if (p.right_fs < start_fs) {
      continue;
    }
    if (use_stop && p.right_fs > stop_fs) {
      continue;
    }
    filtered.push_back(p);
  }

  return filtered;
}

std::vector<SeriesPoint> FilterSeriesToFullWindowOnly(const std::vector<SeriesPoint>& points, uint64_t win_fs) {
  std::vector<SeriesPoint> filtered;
  filtered.reserve(points.size());

  for (const SeriesPoint& p : points) {
    if (p.effective_window_fs == win_fs) {
      filtered.push_back(p);
    }
  }

  return filtered;
}

struct PlotData {
  std::vector<double> x_values;
  std::vector<double> y_rate;
  std::vector<double> y_cumulative;
  std::string x_unit_label = "ps";
  uint64_t x_unit_fs = 1000;
  std::string cumulative_unit_label = "toggles";
  double cumulative_unit_scale = 1.0;
};

struct TimeUnit {
  const char* label;
  uint64_t fs;
};

struct CountUnit {
  const char* label;
  double scale;
};

TimeUnit ChooseReadableTimeUnit(uint64_t max_fs) {
  static constexpr TimeUnit kUnits[] = {
      {"s", 1000000000000000ULL},
      {"ms", 1000000000000ULL},
      {"us", 1000000000ULL},
      {"ns", 1000000ULL},
      {"ps", 1000ULL},
      {"fs", 1ULL},
  };

  if (max_fs == 0) {
    return {"ps", 1000ULL};
  }

  for (const TimeUnit& unit : kUnits) {
    if (max_fs >= unit.fs) {
      return unit;
    }
  }

  return {"fs", 1ULL};
}

CountUnit ChooseReadableCountUnit(uint64_t max_count) {
  static constexpr CountUnit kUnits[] = {
      {"P toggles", 1e15},
      {"T toggles", 1e12},
      {"G toggles", 1e9},
      {"M toggles", 1e6},
      {"K toggles", 1e3},
      {"toggles", 1.0},
  };

  if (max_count == 0) {
    return {"toggles", 1.0};
  }

  for (const CountUnit& unit : kUnits) {
    if (static_cast<double>(max_count) >= unit.scale) {
      return unit;
    }
  }

  return {"toggles", 1.0};
}

PlotData BuildPlotData(const std::vector<SeriesPoint>& points) {
  uint64_t max_right_fs = 0;
  uint64_t max_cumulative = 0;
  for (const SeriesPoint& p : points) {
    if (p.right_fs > max_right_fs) {
      max_right_fs = p.right_fs;
    }
    if (p.cumulative_toggles > max_cumulative) {
      max_cumulative = p.cumulative_toggles;
    }
  }

  const TimeUnit x_unit = ChooseReadableTimeUnit(max_right_fs);
  const CountUnit count_unit = ChooseReadableCountUnit(max_cumulative);

  PlotData out;
  out.x_unit_label = x_unit.label;
  out.x_unit_fs = x_unit.fs;
  out.cumulative_unit_label = count_unit.label;
  out.cumulative_unit_scale = count_unit.scale;
  out.x_values.reserve(points.size());
  out.y_rate.reserve(points.size());
  out.y_cumulative.reserve(points.size());

  for (const SeriesPoint& p : points) {
    out.x_values.push_back(static_cast<double>(p.right_fs) / static_cast<double>(x_unit.fs));
    out.y_rate.push_back(p.rate);
    out.y_cumulative.push_back(static_cast<double>(p.cumulative_toggles) / count_unit.scale);
  }

  if (out.x_values.empty()) {
    out.x_values.push_back(0.0);
    out.y_rate.push_back(0.0);
    out.y_cumulative.push_back(0.0);
  }

  return out;
}

PlotData DownsamplePlotData(const PlotData& input, uint64_t max_points) {
  if (max_points == 0 || input.x_values.size() <= max_points) {
    return input;
  }

  const size_t bucket = static_cast<size_t>((input.x_values.size() + max_points - 1) / max_points);

  PlotData out;
  out.x_values.reserve(max_points);
  out.y_rate.reserve(max_points);
  out.y_cumulative.reserve(max_points);
  out.x_unit_label = input.x_unit_label;
  out.x_unit_fs = input.x_unit_fs;
  out.cumulative_unit_label = input.cumulative_unit_label;
  out.cumulative_unit_scale = input.cumulative_unit_scale;

  for (size_t i = 0; i < input.x_values.size(); i += bucket) {
    const size_t end = std::min(input.x_values.size(), i + bucket);
    size_t max_rate_idx = i;
    for (size_t j = i + 1; j < end; ++j) {
      if (input.y_rate[j] > input.y_rate[max_rate_idx]) {
        max_rate_idx = j;
      }
    }
    const size_t last_idx = end - 1;

    out.x_values.push_back(input.x_values[last_idx]);
    out.y_rate.push_back(input.y_rate[max_rate_idx]);
    out.y_cumulative.push_back(input.y_cumulative[last_idx]);
  }

  return out;
}

void WriteDebugCsv(const fs::path& path, const PlotData& plot, const Options& opts) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write debug CSV: " + path.string());
  }

  out << "time(" << plot.x_unit_label << "),"
      << "toggle_rate(toggles/" << opts.rate_unit_label << "),"
      << "cumulative_toggle_count\n";
  for (size_t i = 0; i < plot.x_values.size(); ++i) {
    const uint64_t cumulative_count = static_cast<uint64_t>(
        std::llround(plot.y_cumulative[i] * plot.cumulative_unit_scale));

    out << std::fixed << std::setprecision(9) << plot.x_values[i] << ','
        << std::fixed << std::setprecision(9) << plot.y_rate[i] << ','
        << cumulative_count << '\n';
  }
}

void WriteSignalList(const fs::path& path, const std::vector<SignalState>& signals) {
  std::vector<const SignalState*> sorted;
  sorted.reserve(signals.size());
  for (const SignalState& s : signals) {
    sorted.push_back(&s);
  }

  std::sort(sorted.begin(), sorted.end(), [](const SignalState* a, const SignalState* b) {
    if (a->depth != b->depth) {
      return a->depth < b->depth;
    }
    if (a->leaf_name.size() != b->leaf_name.size()) {
      return a->leaf_name.size() < b->leaf_name.size();
    }
    return a->output_name < b->output_name;
  });

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write signal list: " + path.string());
  }

  for (const SignalState* s : sorted) {
    out << s->output_name << '\n';
  }
}

void WriteSignalCsv(const fs::path& path, const std::vector<SignalState>& signals) {
  std::vector<const SignalState*> sorted;
  sorted.reserve(signals.size());
  for (const SignalState& s : signals) {
    sorted.push_back(&s);
  }

  std::sort(sorted.begin(), sorted.end(), [](const SignalState* a, const SignalState* b) {
    if (a->total_toggles != b->total_toggles) {
      return a->total_toggles > b->total_toggles;
    }
    return a->output_name < b->output_name;
  });

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write signal CSV: " + path.string());
  }

  out << "signal_name,total_toggle_count\n";
  for (const SignalState* s : sorted) {
    out << '"' << s->output_name << '"' << ',' << s->total_toggles << '\n';
  }
}

void WriteTopWindows(const fs::path& path,
                     const std::vector<TopWindow>& windows) {
  auto fmt_trimmed = [](double value) -> std::string {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << value;
    std::string s = ss.str();
    while (!s.empty() && s.back() == '0') {
      s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
      s.pop_back();
    }
    if (s.empty()) {
      s = "0";
    }
    return s;
  };

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to write top window file: " + path.string());
  }

  out << std::left << std::setw(6) << "rank" << ' '
      << std::setw(16) << "left_ps" << ' '
      << std::setw(16) << "right_ps" << ' '
      << std::setw(16) << "total_toggles" << ' '
      << "toggle_rate_per_ns"
      << '\n';

  for (size_t i = 0; i < windows.size(); ++i) {
    const TopWindow& w = windows[i];
    const double left_ps = static_cast<double>(w.left_fs) / 1000.0;
    const double right_ps = static_cast<double>(w.right_fs) / 1000.0;
    const double rate_per_ns = (w.effective_window_fs > 0)
                                   ? (static_cast<double>(w.window_toggles) * 1000000.0 /
                                      static_cast<double>(w.effective_window_fs))
                                   : 0.0;

    out << std::left << std::setw(6) << (i + 1) << ' '
        << std::setw(16) << fmt_trimmed(left_ps) << ' '
        << std::setw(16) << fmt_trimmed(right_ps) << ' '
        << std::setw(16) << w.window_toggles << ' '
        << fmt_trimmed(rate_per_ns) << '\n';
  }
}

void WriteHtmlReport(const fs::path& out_path,
                     const Options& opts,
                     const ParserStats& stats,
                     uint64_t signal_count,
                     uint64_t total_toggles,
                     const PlotData& plot,
                     const std::vector<TopWindow>& top_windows,
                     const std::string& uplot_js,
                     const std::string& uplot_css) {
  std::ofstream out(out_path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open output HTML: " + out_path.string());
  }

  out << "<!doctype html>\n"
      << "<html lang=\"en\">\n"
      << "<head>\n"
      << "  <meta charset=\"utf-8\">\n"
      << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      << "  <title>" << HtmlEscape(opts.title) << "</title>\n"
      << "  <style>\n"
      << "    body{margin:0;padding:18px;font-family:ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;background:#f8fafc;color:#0f172a;}\n"
      << "    #chart-wrap{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:10px;overflow:auto;}\n"
      << "    #chart{min-width:640px;}\n"
      << "    .panel{margin-top:12px;padding:12px;background:#fff;border:1px solid #e2e8f0;border-radius:8px;}\n"
      << "    .panel h2{font-size:0.95rem;margin:0 0 8px;color:#334155;}\n"
      << "    table{border-collapse:collapse;width:100%;font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;}\n"
      << "    th,td{border:1px solid #e2e8f0;padding:6px 8px;text-align:left;vertical-align:top;}\n"
      << "    th{background:#f8fafc;color:#334155;}\n"
      << "  </style>\n"
      << "  <style>\n"
      << uplot_css << "\n"
      << "  </style>\n"
      << "  <style>\n"
      << "    .uplot .u-legend{margin:8px 0 0 0;text-align:left;}\n"
      << "    .uplot .u-legend.u-inline tr{display:table-row;margin-right:0;}\n"
      << "    .uplot .u-legend.u-inline th,.uplot .u-legend.u-inline td{display:table-cell;}\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "  <div>Double-click LMB to zoom-full</div>\n"
      << "  <div id=\"chart-wrap\"><div id=\"chart\"></div></div>\n"
      << "  <div class=\"panel\" id=\"info-panel\">\n"
      << "    <h2>Run Information</h2>\n"
      << "    <table>\n"
      << "      <tbody>\n"
      << "        <tr><th>Window size</th><td>" << std::fixed << std::setprecision(3)
      << (static_cast<double>(opts.win_fs) / 1000.0) << " ps</td></tr>\n"
      << "        <tr><th>Step size</th><td>" << std::fixed << std::setprecision(3)
      << (static_cast<double>(opts.step_fs) / 1000.0) << " ps</td></tr>\n";

  if (opts.has_start_time) {
    out << "        <tr><th>Start time requested</th><td>" << opts.start_fs << " fs</td></tr>\n"
        << "        <tr><th>Start time snapped</th><td>" << opts.snapped_start_fs << " fs</td></tr>\n";
  }
  if (opts.has_stop_time) {
    out << "        <tr><th>Stop time</th><td>" << opts.stop_fs << " fs</td></tr>\n";
  }

  out << "        <tr><th>X-axis unit</th><td>" << HtmlEscape(plot.x_unit_label) << "</td></tr>\n"
      << "        <tr><th>Cumulative y-axis unit</th><td>" << HtmlEscape(plot.cumulative_unit_label) << "</td></tr>\n"
      << "        <tr><th>Rate unit</th><td>" << HtmlEscape(opts.rate_unit_label) << "</td></tr>\n"
      << "        <tr><th>Signals retained</th><td>" << signal_count << "</td></tr>\n"
      << "        <tr><th>Alias vars skipped</th><td>" << stats.alias_dedup_skipped << "</td></tr>\n"
      << "        <tr><th>Total toggles</th><td>" << total_toggles << "</td></tr>\n"
      << "        <tr><th>Rendered points</th><td>" << plot.x_values.size() << "</td></tr>\n"
      << "        <tr><th>Input lines</th><td>" << stats.total_lines << "</td></tr>\n"
      << "        <tr><th>Timestamp lines</th><td>" << stats.timestamp_updates << "</td></tr>\n"
      << "        <tr><th>Value changes parsed</th><td>" << stats.parsed_value_changes << "</td></tr>\n"
      << "        <tr><th>Unknown id changes</th><td>" << stats.unknown_id_changes << "</td></tr>\n"
      << "        <tr><th>Timescale</th><td>" << stats.timescale_fs << " fs</td></tr>\n"
      << "      </tbody>\n"
      << "    </table>\n"
      << "  </div>\n"
      << "  <div class=\"panel\" id=\"top-windows-panel\">\n"
      << "    <h2>Top 20 Toggle Windows</h2>\n"
      << "    <table>\n"
      << "      <thead>\n"
      << "        <tr>\n"
      << "          <th>Rank</th>\n"
      << "          <th>Left (" << HtmlEscape(plot.x_unit_label) << ")</th>\n"
      << "          <th>Right (" << HtmlEscape(plot.x_unit_label) << ")</th>\n"
      << "          <th>Total Toggles</th>\n"
      << "          <th>Rate (toggles/" << HtmlEscape(opts.rate_unit_label) << ")</th>\n"
      << "        </tr>\n"
      << "      </thead>\n"
      << "      <tbody>\n";

  for (size_t i = 0; i < top_windows.size(); ++i) {
    const TopWindow& w = top_windows[i];
    const double left = static_cast<double>(w.left_fs) / static_cast<double>(plot.x_unit_fs);
    const double right = static_cast<double>(w.right_fs) / static_cast<double>(plot.x_unit_fs);

    out << "        <tr>"
        << "<td>" << (i + 1) << "</td>"
        << "<td>" << std::fixed << std::setprecision(6) << left << "</td>"
        << "<td>" << std::fixed << std::setprecision(6) << right << "</td>"
        << "<td>" << w.window_toggles << "</td>"
        << "<td>" << std::fixed << std::setprecision(6) << w.rate << "</td>"
        << "</tr>\n";
  }

  if (top_windows.empty()) {
    out << "        <tr><td colspan=\"5\">No windows with toggles found.</td></tr>\n";
  }

  out << "      </tbody>\n"
      << "    </table>\n"
      << "  </div>\n"
      << "  <script>\n"
      << uplot_js << "\n"
      << "  </script>\n"
      << "  <script>\n";

  out << "    const x = [";
  for (size_t i = 0; i < plot.x_values.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << std::fixed << std::setprecision(6) << plot.x_values[i];
  }
  out << "];\n";

  out << "    const yRate = [";
  for (size_t i = 0; i < plot.y_rate.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << std::fixed << std::setprecision(9) << plot.y_rate[i];
  }
  out << "];\n";

  out << "    const yCum = [";
  for (size_t i = 0; i < plot.y_cumulative.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << std::fixed << std::setprecision(9) << plot.y_cumulative[i];
  }
  out << "];\n";

  out << "    const data = [x, yRate, yCum];\n"
      << "    const title = \"" << JsStringEscape(opts.title) << "\";\n"
      << "    const optsPlot = {\n"
      << "      title,\n"
      << "      width: Math.max(760, Math.min(1700, window.innerWidth - 70)),\n"
      << "      height: 500,\n"
      << "      scales: { x: {time:false}, rate: {auto:true}, cum: {auto:true} },\n"
      << "      axes: [\n"
      << "        { scale: 'x', label: 'Time (" << JsStringEscape(plot.x_unit_label) << ")' },\n"
      << "        { scale: 'rate', label: 'Toggle Rate (toggles/" << JsStringEscape(opts.rate_unit_label) << ")' },\n"
      << "        { scale: 'cum', side: 1, grid: {show:false}, label: 'Cumulative Toggle Count ("
      << JsStringEscape(plot.cumulative_unit_label) << ")' }\n"
      << "      ],\n"
      << "      series: [\n"
      << "        {},\n"
      << "        { label: 'Toggle rate', scale: 'rate', stroke: '#0f766e', width: 1, points: {show: false} },\n"
      << "        { label: 'Cumulative', scale: 'cum', stroke: '#b45309', width: 1, points: {show: false} }\n"
      << "      ]\n"
      << "    };\n"
      << "\n"
      << "    const plot = new uPlot(optsPlot, data, document.getElementById('chart'));\n"
      << "    window.addEventListener('resize', () => {\n"
      << "      const w = Math.max(760, Math.min(1700, window.innerWidth - 70));\n"
      << "      plot.setSize({width: w, height: 500});\n"
      << "    });\n"
      << "  </script>\n"
      << "</body>\n"
      << "</html>\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  const int parse_status = ParseOptions(argc, argv, &opts);
  if (parse_status == -1) {
    return 0;
  }
  if (parse_status != 0) {
    return parse_status;
  }

  if (!opts.title_explicit) {
    opts.title = DefaultTitleFromInput(opts.input_path);
  }

  try {
    std::error_code ec;
    fs::create_directories(opts.outdir, ec);
    if (ec) {
      throw std::runtime_error("failed to create output directory: " + opts.outdir);
    }

    InputHandle input;
    std::string input_error;
    if (!OpenInput(opts, &input, &input_error)) {
      throw std::runtime_error(input_error);
    }

    bool parse_ok = false;
    VcdParser parser(opts);
    try {
      parser.Parse(input.stream, opts.input_path);
      parse_ok = true;
    } catch (...) {
      std::string close_err;
      (void)CloseInput(&input, false, &close_err);
      throw;
    }

    std::string close_err;
    if (!CloseInput(&input, parse_ok, &close_err)) {
      throw std::runtime_error(close_err);
    }

    const auto series_all = parser.accumulator().BuildSeries(opts.rate_unit_fs);
    const auto series = FilterSeriesByTimeRange(series_all, opts);
    const auto top_series = FilterSeriesToFullWindowOnly(series, opts.win_fs);
    const auto top_windows = SelectTopWindows(top_series, opts.allow_top_window_overlap, 20);
    const auto plot_full = BuildPlotData(series);
    const auto plot = DownsamplePlotData(plot_full, opts.max_points);

    const fs::path outdir(opts.outdir);
    const fs::path html_path = outdir / "toggle_profile.html";
    const fs::path signal_list_path = outdir / "signals.txt";
    const fs::path signal_csv_path = outdir / "signal_toggle_counts.csv";
    const fs::path top_windows_path = outdir / "top_20_windows.txt";
    const fs::path debug_csv_path = outdir / "debug.csv";

    WriteSignalList(signal_list_path, parser.signals());
    WriteSignalCsv(signal_csv_path, parser.signals());
    WriteTopWindows(top_windows_path, top_windows);
    if (opts.debug) {
      WriteDebugCsv(debug_csv_path, plot, opts);
    }

    const auto js_path = ResolveAssetPath(opts.uplot_js_path, fs::path("third_party/uplot/uPlot.iife.js"));
    const auto css_path = ResolveAssetPath(opts.uplot_css_path, fs::path("third_party/uplot/uPlot.min.css"));
    if (!js_path || !css_path) {
      throw std::runtime_error(
          "could not resolve uPlot assets. Expected third_party/uplot/uPlot.iife.js and uPlot.min.css");
    }

    const std::string uplot_js = ReadTextFile(*js_path);
    const std::string uplot_css = ReadTextFile(*css_path);

    WriteHtmlReport(html_path,
                    opts,
                    parser.stats(),
                    static_cast<uint64_t>(parser.signals().size()),
                    parser.accumulator().total_toggles(),
                    plot,
                    top_windows,
                    uplot_js,
                    uplot_css);

    std::cout << "Input: " << opts.input_path << "\n"
              << "Outdir: " << outdir.string() << "\n"
              << "Signals retained: " << parser.signals().size() << "\n"
              << "Alias vars skipped: " << parser.stats().alias_dedup_skipped << "\n"
              << "Total toggles: " << parser.accumulator().total_toggles() << "\n"
              << "Series points (full): " << series_all.size() << "\n"
              << "Series points (time-filtered): " << series.size() << "\n"
              << "Rendered points: " << plot.x_values.size() << "\n"
              << "HTML: " << html_path.string() << "\n"
              << "Signals list: " << signal_list_path.string() << "\n"
              << "Signal CSV: " << signal_csv_path.string() << "\n"
              << "Top windows: " << top_windows_path.string() << "\n";
    if (opts.debug) {
      std::cout << "Debug CSV: " << debug_csv_path.string() << "\n";
    }

    if (opts.has_start_time) {
      std::cout << "Start time requested: " << opts.start_fs << " fs\n"
                << "Start time snapped: " << opts.snapped_start_fs << " fs\n";
    }
    if (opts.has_stop_time) {
      std::cout << "Stop time: " << opts.stop_fs << " fs\n";
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
