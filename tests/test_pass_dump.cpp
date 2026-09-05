// STANLI_DUMP_PASSES: one graph dump per lowering pass, so consecutive
// stages can be diffed. The dumps are a debugging aid, so the property that
// matters most is that turning them on does not change what is compiled.
#include "env_helpers.hpp"
#include <stanli/compile.hpp>
#include <stanli/graph_print.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define test_dup _dup
#define test_dup2 _dup2
#define test_close _close
static int test_open_for_write(const char* path) {
  return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IWRITE);
}
#else
#include <fcntl.h>
#include <unistd.h>
#define test_dup dup
#define test_dup2 dup2
#define test_close close
static int test_open_for_write(const char* path) {
  return ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}
#endif

static int failures = 0;
static void expect(const std::string& what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

static std::string slurp(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::vector<std::string> listing(const std::filesystem::path& dir) {
  std::vector<std::string> names;
  if (!std::filesystem::exists(dir)) return names;
  for (const auto& e : std::filesystem::directory_iterator(dir))
    names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

static const char* kExpected[] = {"00-mir.sexp",
                                  "01-log_prob-bind_data.txt",
                                  "02-log_prob-lower.txt",
                                  "03-log_prob-inplace.txt",
                                  "04-log_prob-store_forward.txt",
                                  "05-log_prob-constfold.txt",
                                  "06-log_prob-reroll.txt",
                                  "07-log_prob-post_reroll_inplace.txt",
                                  "08-log_prob-partition.txt",
                                  "09-log_prob-post_partition_inplace.txt",
                                  "10-log_prob-elide_stores.txt",
                                  "11-log_prob-cse.txt",
                                  "12-log_prob-island.txt",
                                  "13-log_prob-reduce.txt",
                                  "14-write_array-lower.txt",
                                  "15-write_array-inplace.txt",
                                  "16-write_array-store_forward.txt",
                                  "17-write_array-reroll.txt",
                                  "18-write_array-post_reroll_inplace.txt",
                                  "19-write_array-finalize.txt"};

int main() {
  using namespace stanli;

  // Constant folding removes nine of this model's sixteen ops, so the
  // store_forward and constfold dumps differ in the graph itself.
  const DataMap data =
      DataMap::from_json_file("tests/fixtures/arr2d_rowrange.json");
  const std::string mir = slurp("tests/fixtures/arr2d_rowrange.tmir.sexp");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "stanli_pass_dump_test";
  std::filesystem::remove_all(root);

  test_unsetenv("STANLI_DUMP_PASSES");
  CompiledModel plain = compile_model(mir, data);

  const std::filesystem::path on_dir = root / "on";
  test_setenv("STANLI_DUMP_PASSES", on_dir.string().c_str(), 1);
  CompiledModel dumped = compile_model(mir, data);
  test_unsetenv("STANLI_DUMP_PASSES");

  const std::vector<std::string> got = listing(on_dir);
  const std::vector<std::string> want(std::begin(kExpected),
                                      std::end(kExpected));
  expect("stage file names", got == want);
  if (got != want)
    for (const std::string& n : got) std::printf("  got %s\n", n.c_str());

  for (const std::string& n : got)
    expect("non-empty " + n, !slurp(on_dir / n).empty());

  expect("00-mir.sexp is the input mir",
         slurp(on_dir / "00-mir.sexp") == mir);

  const std::string before = slurp(on_dir / "04-log_prob-store_forward.txt");
  const std::string after = slurp(on_dir / "05-log_prob-constfold.txt");
  expect("constfold changes the graph",
         before.substr(0, before.find('\n')) !=
             after.substr(0, after.find('\n')));

  compile_model(mir, data);
  expect("no dumps once unset", listing(on_dir) == want);

  const auto listing_with = [&](const char* stages, const char* name) {
    const std::filesystem::path dir = root / name;
    test_setenv("STANLI_DUMP_PASSES", dir.string().c_str(), 1);
    test_setenv("STANLI_DUMP_STAGES", stages, 1);
    compile_model(mir, data);
    test_unsetenv("STANLI_DUMP_PASSES");
    test_unsetenv("STANLI_DUMP_STAGES");
    return listing(dir);
  };

  // Numbering counts the stages the filter dropped, so a documented
  // `diff 05 06` keeps naming the same two passes under any selection.
  expect("one stage keeps its unfiltered number",
         listing_with("constfold", "constfold") ==
             std::vector<std::string>{"05-log_prob-constfold.txt"});
  expect("bare stage name matches every graph",
         listing_with("reroll", "reroll") ==
             std::vector<std::string>{"06-log_prob-reroll.txt",
                                      "17-write_array-reroll.txt"});
  expect("qualified stage name matches one graph",
         listing_with("log_prob:reroll", "qualified") ==
             std::vector<std::string>{"06-log_prob-reroll.txt"});
  expect("mir is selectable",
         listing_with("mir,constfold", "mir") ==
             std::vector<std::string>{"00-mir.sexp",
                                      "05-log_prob-constfold.txt"});
  expect("all selects every stage", listing_with("all", "all") == want);

  const std::filesystem::path captured = root / "stdout.txt";
  std::fflush(stdout);
  const int saved = test_dup(1);
  const int sink = test_open_for_write(captured.string().c_str());
  test_dup2(sink, 1);
  test_setenv("STANLI_DUMP_PASSES", "-", 1);
  test_setenv("STANLI_DUMP_STAGES", "reroll", 1);
  compile_model(mir, data);
  test_unsetenv("STANLI_DUMP_PASSES");
  test_unsetenv("STANLI_DUMP_STAGES");
  std::fflush(stdout);
  test_dup2(saved, 1);
  test_close(sink);
  test_close(saved);

  std::vector<std::string> banners;
  std::istringstream lines(slurp(captured));
  for (std::string line; std::getline(lines, line);)
    if (line.rfind(";; ", 0) == 0) banners.push_back(line);
  expect("stdout banners are balanced",
         banners == std::vector<std::string>{
                        ";; log_prob:reroll", ";; end log_prob:reroll",
                        ";; write_array:reroll", ";; end write_array:reroll"});
  expect("stdout mode makes no directory", !std::filesystem::exists("-"));

  std::string a, b;
  print_graph(a, plain.graph);
  print_graph(b, dumped.graph);
  expect("dumping does not change the compiled graph", a == b && !a.empty());

  std::filesystem::remove_all(root);
  if (failures == 0) std::printf("test_pass_dump OK\n");
  return failures == 0 ? 0 : 1;
}
