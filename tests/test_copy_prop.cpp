#include <stanli/program.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}

namespace copy_prop_test {

using stanli::Program;
using Instr = Program::Instr;
using Seeded = std::vector<std::pair<int, int>>;

Program make_program(int n_regs, std::vector<Instr> code,
                     std::vector<int> out_regs) {
  Program p;
  p.n_regs = n_regs;
  p.code = std::move(code);
  p.out_regs = std::move(out_regs);
  p.pool = {2.0};
  return p;
}

int count_code(const Program& p, Program::Code code) {
  int n = 0;
  for (const Instr& I : p.code) n += I.code == code;
  return n;
}

void test_chain_is_forwarded_and_dropped() {
  Program p =
      make_program(4,
                   {Instr(Program::MOV, 1, 0), Instr(Program::MOV, 2, 1),
                    Instr(Program::ADD, 3, 2, 2)},
                   {3});
  check(stanli::propagate_program_copies(p, {{0, 1}}),
        "chain: program changed");
  check(p.code.size() == 1 && p.code[0].code == Program::ADD,
        "chain: both copies dropped");
  check(p.code[0].a == 0 && p.code[0].b == 0, "chain: reads point at the root");
}

void test_source_rewritten_on_one_path() {
  Program p =
      make_program(4,
                   {Instr(Program::MOV, 2, 0), Instr(Program::JZ, 3, 1),
                    Instr(Program::ADD, 0, 0, 1), Instr(Program::ADD, 3, 2, 2)},
                   {3});
  stanli::propagate_program_copies(p, {{0, 1}, {1, 1}});
  check(count_code(p, Program::MOV) == 1, "rewritten source: copy kept");
  check(p.code.back().a == 2 && p.code.back().b == 2,
        "rewritten source: join read keeps the destination");
  check(p.code[1].code == Program::JZ && p.code[1].dst == 3,
        "rewritten source: jump target intact");
}

void test_destination_written_on_two_paths() {
  Program p = make_program(4,
                           {Instr(Program::JZ, 3, 0), Instr(Program::MOV, 2, 0),
                            Instr(Program::JMP, 4), Instr(Program::MOV, 2, 1),
                            Instr(Program::ADD, 3, 2, 2)},
                           {3});
  stanli::propagate_program_copies(p, {{0, 1}, {1, 1}});
  check(count_code(p, Program::MOV) == 2, "joined writers: both copies kept");
  check(p.code.back().a == 2 && p.code.back().b == 2,
        "joined writers: read keeps the destination");
}

void test_branch_local_copy_is_forwarded() {
  Program p =
      make_program(4,
                   {Instr(Program::JZ, 4, 0), Instr(Program::MOV, 2, 1),
                    Instr(Program::ADD, 3, 2, 2), Instr(Program::JMP, 5),
                    Instr(Program::CONST, 3, 0)},
                   {3});
  check(stanli::propagate_program_copies(p, {{0, 1}, {1, 1}}),
        "branch local: program changed");
  check(count_code(p, Program::MOV) == 0, "branch local: copy dropped");
  for (const Instr& I : p.code)
    if (I.code == Program::ADD)
      check(I.a == 1 && I.b == 1, "branch local: read points at the source");
}

Program::Call range_call(int in, int len, int out) {
  Program::Call c;
  c.n_in = 1;
  c.in[0] = in;
  c.in_len[0] = len;
  c.out = out;
  c.out_len = 1;
  return c;
}

void test_call_input_range() {
  {
    Program p =
        make_program(7,
                     {Instr(Program::MOV, 4, 0), Instr(Program::MOV, 5, 2),
                      Instr(Program::CALL, 0, 0)},
                     {6});
    p.calls.push_back(range_call(4, 2, 6));
    stanli::propagate_program_copies(p, {{0, 1}, {2, 1}});
    check(count_code(p, Program::MOV) == 2, "split range: copies kept");
    check(p.calls[0].in[0] == 4, "split range: call input unchanged");
  }
  {
    Program p =
        make_program(7,
                     {Instr(Program::MOV, 4, 0), Instr(Program::MOV, 5, 1),
                      Instr(Program::CALL, 0, 0)},
                     {6});
    p.calls.push_back(range_call(4, 2, 6));
    stanli::propagate_program_copies(p, {{0, 2}});
    check(count_code(p, Program::MOV) == 0, "contiguous range: copies dropped");
    check(p.calls[0].in[0] == 0, "contiguous range: call reads the source run");
  }
}

void test_dyn_index_run_keeps_its_registers() {
  Program p =
      make_program(13,
                   {Instr(Program::MOV, 10, 0), Instr(Program::MOV, 11, 1),
                    Instr(Program::MOV, 12, 2), Instr(Program::MOV, 8, 3),
                    Instr(Program::DYN_INDEX, 5, 10, 8, 0, 3)},
                   {5});
  stanli::propagate_program_copies(p, {{0, 4}});
  check(count_code(p, Program::MOV) == 3, "dyn index: run copies kept");
  const Instr& I = p.code.back();
  check(I.code == Program::DYN_INDEX && I.a == 10 && I.c == 0 && I.len == 3,
        "dyn index: run operands unchanged");
  check(I.b == 3, "dyn index: index copy forwarded");
}

void test_seeded_and_live_out_writes_stay() {
  {
    Program p = make_program(
        3, {Instr(Program::MOV, 0, 1), Instr(Program::ADD, 2, 0, 0)}, {2});
    stanli::propagate_program_copies(p, {{0, 1}, {1, 1}});
    check(count_code(p, Program::MOV) == 1, "seeded: write to a live-in kept");
    check(p.code.back().a == 1 && p.code.back().b == 1,
          "seeded: later reads follow the source");
  }
  {
    Program p = make_program(
        4, {Instr(Program::MOV, 2, 0), Instr(Program::ADD, 3, 2, 2)}, {2, 3});
    stanli::propagate_program_copies(p, {{0, 1}});
    check(count_code(p, Program::MOV) == 1, "live-out: copy kept");
    check(p.code.back().a == 0, "live-out: reads still forwarded");
  }
}

void test_dead_constant_and_range_copy() {
  {
    Program p =
        make_program(4,
                     {Instr(Program::CONST, 2, 0), Instr(Program::MOV, 2, 0),
                      Instr(Program::ADD, 3, 2, 2)},
                     {3});
    stanli::propagate_program_copies(p, {{0, 1}});
    check(p.code.size() == 1 && p.code[0].code == Program::ADD,
          "dead constant: fill and copy both dropped");
  }
  {
    Program p = make_program(8,
                             {Instr(Program::MOVR, 4, 0, 0, 0, 2),
                              Instr(Program::LOG_RANGE, 6, 4, 0, 0, 2)},
                             {6, 7});
    stanli::propagate_program_copies(p, {{0, 2}});
    check(count_code(p, Program::MOVR) == 0, "range copy: dropped");
    check(p.code.size() == 1 && p.code[0].a == 0,
          "range copy: ranged read follows the source run");
  }
}

}  // namespace copy_prop_test

int main() {
  using namespace copy_prop_test;
  test_chain_is_forwarded_and_dropped();
  test_source_rewritten_on_one_path();
  test_destination_written_on_two_paths();
  test_branch_local_copy_is_forwarded();
  test_call_input_range();
  test_dyn_index_run_keeps_its_registers();
  test_seeded_and_live_out_writes_stay();
  test_dead_constant_and_range_copy();
  if (failures == 0) std::printf("test_copy_prop OK\n");
  return failures == 0 ? 0 : 1;
}
