#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef VCD_PROFILER_BIN
#error "VCD_PROFILER_BIN must be defined by CMake"
#endif

#ifndef VCD_REPO_ROOT
#error "VCD_REPO_ROOT must be defined by CMake"
#endif

struct RunResult {
  int exit_code = -1;
  std::string output;
};

struct TopWindowRow {
  int rank = 0;
  double left = 0.0;
  double right = 0.0;
  uint64_t toggles = 0;
  double rate = 0.0;
};

std::string ShellQuote(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  out.push_back('\'');
  for (char c : in) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

RunResult RunProfiler(const std::vector<std::string>& args) {
  std::string cmd = ShellQuote(VCD_PROFILER_BIN);
  for (const std::string& arg : args) {
    cmd.push_back(' ');
    cmd += ShellQuote(arg);
  }
  cmd += " 2>&1";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    return RunResult{.exit_code = -1, .output = "failed to spawn profiler"};
  }

  std::array<char, 4096> buffer{};
  std::string output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

  const int status = pclose(pipe);
  int exit_code = -1;
  if (status != -1 && WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  }

  return RunResult{.exit_code = exit_code, .output = output};
}

class TempDir {
 public:
  explicit TempDir(const std::string& prefix) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = ++counter;
    const std::string leaf = prefix + "_" + std::to_string(::getpid()) + "_" + std::to_string(n);
    path_ = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

fs::path RepoRoot() {
  return fs::path(VCD_REPO_ROOT);
}

fs::path WriteTextFile(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to open file for write: " + path.string());
  }
  out << text;
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
  return path;
}

std::vector<std::string> ReadLines(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("failed to open file for read: " + path.string());
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> BaseArgs(const fs::path& input, const fs::path& outdir) {
  return {
      input.string(),
      "--outdir",
      outdir.string(),
      "--uplot-js",
      (RepoRoot() / "third_party/uplot/uPlot.iife.js").string(),
      "--uplot-css",
      (RepoRoot() / "third_party/uplot/uPlot.min.css").string(),
  };
}

std::vector<TopWindowRow> ParseTopWindows(const fs::path& path) {
  const std::vector<std::string> lines = ReadLines(path);
  std::vector<TopWindowRow> rows;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.empty()) {
      continue;
    }
    std::istringstream iss(line);
    TopWindowRow row;
    iss >> row.rank >> row.left >> row.right >> row.toggles >> row.rate;
    if (iss) {
      rows.push_back(row);
    }
  }
  return rows;
}

double MaxDebugRate(const fs::path& path) {
  const std::vector<std::string> lines = ReadLines(path);
  double max_rate = 0.0;
  for (size_t i = 1; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    const size_t c1 = line.find(',');
    if (c1 == std::string::npos) {
      continue;
    }
    const size_t c2 = line.find(',', c1 + 1);
    if (c2 == std::string::npos || c2 <= c1 + 1) {
      continue;
    }
    const double rate = std::stod(line.substr(c1 + 1, c2 - c1 - 1));
    if (rate > max_rate) {
      max_rate = rate;
    }
  }
  return max_rate;
}

bool OverlapOpenLeftClosedRight(const TopWindowRow& a, const TopWindowRow& b) {
  const double l = std::max(a.left, b.left);
  const double r = std::min(a.right, b.right);
  return (l + 1e-12) < r;
}

TEST(CliValidation, StepMustBeLessThanOrEqualToWindow) {
  TempDir temp("vcd_cli_step_gt_win");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! a $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n");

  std::vector<std::string> args = BaseArgs(vcd, temp.path() / "out");
  args.insert(args.end(), {"--win-size", "10ns", "--step-size", "11ns"});
  const RunResult res = RunProfiler(args);

  EXPECT_NE(res.exit_code, 0);
  EXPECT_NE(res.output.find("--step-size must be <= --win-size"), std::string::npos);
}

TEST(CliValidation, WindowMustBeDivisibleByStep) {
  TempDir temp("vcd_cli_divisible");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! a $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n");

  std::vector<std::string> args = BaseArgs(vcd, temp.path() / "out");
  args.insert(args.end(), {"--win-size", "10ns", "--step-size", "3ns"});
  const RunResult res = RunProfiler(args);

  EXPECT_NE(res.exit_code, 0);
  EXPECT_NE(res.output.find("--win-size must be evenly divisible by --step-size"), std::string::npos);
}

TEST(CliValidation, InvalidRateUnitRejected) {
  TempDir temp("vcd_cli_rate_unit");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! a $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n");

  std::vector<std::string> args = BaseArgs(vcd, temp.path() / "out");
  args.insert(args.end(), {"--rate-unit", "bad"});
  const RunResult res = RunProfiler(args);

  EXPECT_NE(res.exit_code, 0);
  EXPECT_NE(res.output.find("--rate-unit must be one of fs/ps/ns/us/ms/s"), std::string::npos);
}

TEST(OutputDirectory, RemovesCreatedEmptyDirOnFailure) {
  TempDir temp("vcd_outdir_cleanup");
  const fs::path outdir = temp.path() / "new_outdir";
  std::vector<std::string> args = BaseArgs(temp.path() / "missing.vcd", outdir);

  const RunResult res = RunProfiler(args);
  EXPECT_NE(res.exit_code, 0);
  EXPECT_FALSE(fs::exists(outdir));
}

TEST(ParserBehavior, AliasDedupAndPreambleFilter) {
  TempDir temp("vcd_alias_preamble");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$scope module keep $end\n"
      "$var wire 1 ! a $end\n"
      "$var wire 1 ! a_alias $end\n"
      "$upscope $end\n"
      "$scope module drop $end\n"
      "$var wire 1 \" b $end\n"
      "$upscope $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n"
      "0\"\n"
      "#10\n"
      "1!\n"
      "#20\n"
      "0!\n");

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(vcd, outdir);
  args.insert(args.end(), {"--preamble", "top.keep", "--win-size", "10ns", "--step-size", "5ns"});
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  EXPECT_NE(res.output.find("Signals retained: 1"), std::string::npos);
  EXPECT_NE(res.output.find("Alias vars skipped: 1"), std::string::npos);

  const std::vector<std::string> signals = ReadLines(outdir / "signals.txt");
  ASSERT_FALSE(signals.empty());
  EXPECT_EQ(signals[0], "a");
  EXPECT_EQ(signals.size(), 1u);

  const std::vector<std::string> csv = ReadLines(outdir / "signal_toggle_counts.csv");
  ASSERT_GE(csv.size(), 2u);
  EXPECT_EQ(csv[0], "signal_name,total_toggle_count");
  EXPECT_EQ(csv[1], "\"a\",2");
}

TEST(GlitchFilter, CountsAndPerSignalCsv) {
  TempDir temp("vcd_glitch_filter");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! s1 $end\n"
      "$var wire 1 \" s2 $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n"
      "0\"\n"
      "#10\n"
      "1!\n"
      "#12\n"
      "0!\n"
      "#20\n"
      "1\"\n"
      "#21\n"
      "0\"\n"
      "#30\n"
      "1\"\n"
      "#31\n"
      "0\"\n"
      "#40\n"
      "1!\n"
      "#50\n"
      "1\"\n");

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(vcd, outdir);
  args.insert(args.end(), {"--glitch-threshold", "5ns", "--win-size", "10ns", "--step-size", "1ns"});
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  EXPECT_NE(res.output.find("Total glitches filtered: 3"), std::string::npos);

  const std::vector<std::string> glitch_csv = ReadLines(outdir / "signal_glitch_counts.csv");
  ASSERT_GE(glitch_csv.size(), 3u);
  EXPECT_EQ(glitch_csv[0], "signal_name,glitch_count");
  EXPECT_EQ(glitch_csv[1], "\"top.s2\",2");
  EXPECT_EQ(glitch_csv[2], "\"top.s1\",1");

  const std::vector<std::string> toggle_csv = ReadLines(outdir / "signal_toggle_counts.csv");
  ASSERT_GE(toggle_csv.size(), 3u);
  EXPECT_EQ(toggle_csv[0], "signal_name,total_toggle_count");
  EXPECT_EQ(toggle_csv[1], "\"top.s1\",1");
  EXPECT_EQ(toggle_csv[2], "\"top.s2\",1");
}

TEST(TopWindows, WidthInvariantAndNoOverlapWhenDisabled) {
  TempDir temp("vcd_top_windows_no_overlap");
  std::ostringstream vcd;
  vcd << "$timescale 1ns $end\n"
      << "$scope module top $end\n"
      << "$var wire 1 ! s $end\n"
      << "$upscope $end\n"
      << "$enddefinitions $end\n"
      << "#0\n"
      << "0!\n";
  for (int t = 1; t <= 40; ++t) {
    vcd << "#" << t << "\n";
    vcd << ((t % 2) ? '1' : '0') << "!\n";
  }
  const fs::path in = WriteTextFile(temp.path() / "in.vcd", vcd.str());

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(in, outdir);
  args.insert(args.end(), {"--win-size", "10ns", "--step-size", "5ns", "--time-unit", "ns"});
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  const std::vector<TopWindowRow> rows = ParseTopWindows(outdir / "top_20_windows.txt");
  ASSERT_GE(rows.size(), 3u);

  for (const TopWindowRow& row : rows) {
    EXPECT_NEAR(row.right - row.left, 10.0, 1e-9);
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    for (size_t j = i + 1; j < rows.size(); ++j) {
      EXPECT_FALSE(OverlapOpenLeftClosedRight(rows[i], rows[j]));
    }
  }
}

TEST(TopWindows, OverlapCanBeEnabled) {
  TempDir temp("vcd_top_windows_overlap");
  std::ostringstream vcd;
  vcd << "$timescale 1ns $end\n"
      << "$scope module top $end\n"
      << "$var wire 1 ! s $end\n"
      << "$upscope $end\n"
      << "$enddefinitions $end\n"
      << "#0\n"
      << "0!\n";
  for (int t = 1; t <= 40; ++t) {
    vcd << "#" << t << "\n";
    vcd << ((t % 2) ? '1' : '0') << "!\n";
  }
  const fs::path in = WriteTextFile(temp.path() / "in.vcd", vcd.str());

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(in, outdir);
  args.insert(args.end(), {"--win-size", "10ns", "--step-size", "5ns", "--allow-top-window-overlap", "true"});
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  const std::vector<TopWindowRow> rows = ParseTopWindows(outdir / "top_20_windows.txt");
  ASSERT_GE(rows.size(), 2u);
  EXPECT_TRUE(OverlapOpenLeftClosedRight(rows[0], rows[1]));
}

TEST(Downsample, PreservesPeakToggleRate) {
  TempDir temp("vcd_downsample_peak");
  std::ostringstream vcd;
  vcd << "$timescale 1ns $end\n"
      << "$scope module top $end\n";
  for (int i = 0; i < 32; ++i) {
    vcd << "$var wire 1 id" << i << " s" << i << " $end\n";
  }
  vcd << "$upscope $end\n"
      << "$enddefinitions $end\n"
      << "#0\n";
  for (int i = 0; i < 32; ++i) {
    vcd << "0id" << i << "\n";
  }
  vcd << "#150\n";
  for (int i = 0; i < 32; ++i) {
    vcd << "1id" << i << "\n";
  }
  const fs::path in = WriteTextFile(temp.path() / "in.vcd", vcd.str());

  const fs::path out_full = temp.path() / "out_full";
  std::vector<std::string> args_full = BaseArgs(in, out_full);
  args_full.insert(args_full.end(), {"--win-size", "10ns", "--step-size", "1ns", "--debug", "--max-points", "0"});
  const RunResult full = RunProfiler(args_full);
  ASSERT_EQ(full.exit_code, 0) << full.output;

  const fs::path out_ds = temp.path() / "out_ds";
  std::vector<std::string> args_ds = BaseArgs(in, out_ds);
  args_ds.insert(args_ds.end(), {"--win-size", "10ns", "--step-size", "1ns", "--debug", "--max-points", "20"});
  const RunResult ds = RunProfiler(args_ds);
  ASSERT_EQ(ds.exit_code, 0) << ds.output;

  const double max_full = MaxDebugRate(out_full / "debug.csv");
  const double max_ds = MaxDebugRate(out_ds / "debug.csv");
  EXPECT_GT(max_full, 0.0);
  EXPECT_NEAR(max_ds, max_full, 1e-9);
}

TEST(SparseStress, LargeTimestampGapWithSmallStep) {
  TempDir temp("vcd_sparse_stress");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! s $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n"
      "#300000\n"
      "1!\n");

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(vcd, outdir);
  args.insert(args.end(), {"--win-size", "10ns", "--step-size", "1ns", "--max-points", "50"});
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  EXPECT_NE(res.output.find("Series points (full): 300010"), std::string::npos);
  EXPECT_NE(res.output.find("Rendered points: 50"), std::string::npos);
}

TEST(TimeRange, StartTimeSnapsDownToStepBoundary) {
  TempDir temp("vcd_start_snap");
  const fs::path vcd = WriteTextFile(
      temp.path() / "in.vcd",
      "$timescale 1ns $end\n"
      "$scope module top $end\n"
      "$var wire 1 ! s $end\n"
      "$upscope $end\n"
      "$enddefinitions $end\n"
      "#0\n"
      "0!\n"
      "#25\n"
      "1!\n");

  const fs::path outdir = temp.path() / "out";
  std::vector<std::string> args = BaseArgs(vcd, outdir);
  args.insert(args.end(), {
                           "--win-size",
                           "20ns",
                           "--step-size",
                           "10ns",
                           "--time-unit",
                           "ns",
                           "--start-time",
                           "23ns",
                           "--stop-time",
                           "50ns",
                       });
  const RunResult res = RunProfiler(args);

  ASSERT_EQ(res.exit_code, 0) << res.output;
  EXPECT_NE(res.output.find("Start time requested: 23 ns"), std::string::npos);
  EXPECT_NE(res.output.find("Start time snapped: 20 ns"), std::string::npos);
}

}  // namespace
