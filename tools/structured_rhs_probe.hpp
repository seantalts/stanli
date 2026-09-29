// Developer-only scalar callback adapter for the structured-loop experiment.
// The wrapper model has unconstrained parameters (t, vector[1] y, real theta)
// and target += rhs(t, y, ...)[1]. It must contain a retained OP_LOOP.
// This reuses production lowering, kernel history, and Executor reverse without
// installing a new callback path or changing the unrolling policy.
#ifndef STANLI_TOOLS_STRUCTURED_RHS_PROBE_HPP
#define STANLI_TOOLS_STRUCTURED_RHS_PROBE_HPP
#include <stanli/compile.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <map>
#include <set>
#include <functional>
#include <unordered_map>
#include "../tests/env_helpers.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

// Optional second experiment: use the existing straight-line segment compiler
// inside the existing loop history. Only immutable prepared plans are cloned;
// this never changes the normal lowerer or shared runtime plans.
inline void probe_segments(stanli::StructuredLoop& p) {
  using Node = stanli::StructuredLoop::Node;
  std::map<int, int> reads;
  std::set<int> writes;
  std::vector<char> active(p.body.slots.size(), 0);
  for (const auto& i : p.imports) active[i.slot] = i.active;
  auto account = [&](const Node& n, auto&& read) {
    if (n.kind == Node::KernelCall) {
      const auto& op = p.body.ops[n.op];
      for (int k = 0; k < op.n_in; ++k) read(op.in[k]);
    } else if (n.kind == Node::Alias || n.kind == Node::Target)
      read(n.src);
    else if (n.kind == Node::If || n.kind == Node::While)
      read(n.condition);
    else if (n.kind == Node::For) {
      read(n.lower);
      read(n.upper);
    }
  };
  std::function<void(const Node&)> scan = [&](const Node& n) {
    account(n, [&](int slot) { ++reads[slot]; });
    if (n.kind == Node::KernelCall) {
      const auto& op = p.body.ops[n.op];
      writes.insert(op.out);
      if (op.out2 >= 0) writes.insert(op.out2);
      if (n.active) {
        active[op.out] = 1;
        if (op.out2 >= 0) active[op.out2] = 1;
      }
    } else if (n.kind == Node::Alias)
      writes.insert(n.dst);
    else if (n.kind == Node::For)
      writes.insert(n.iterator);
    for (const auto& c : n.children) scan(c);
  };
  scan(p.root);
  for (int out : p.outputs) ++reads[out];
  bool changed = true;
  std::function<void(const Node&)> aliases = [&](const Node& n) {
    if (n.kind == Node::Alias && active[n.src] && !active[n.dst]) {
      active[n.dst] = 1;
      changed = true;
    }
    for (const auto& c : n.children) aliases(c);
  };
  while (changed) {
    changed = false;
    aliases(p.root);
  }
  std::unordered_map<int, const std::vector<double>*> constants;
  for (const auto& f : p.fills)
    if (!writes.count(f.first)) constants[f.first] = &f.second;
  const auto eligible = [&](const Node& c) {
    return c.kind == Node::Alias ||
           (c.kind == Node::KernelCall && c.storage != Node::InPlace &&
            p.body.ops[c.op].out2 < 0 &&
            stanli::segment_supports(p.body, p.body.ops[c.op]));
  };
  const auto segment_run = [&](const std::vector<Node>& nodes, size_t first,
                               size_t last, Node& replacement) {
    if (last - first < 2) return false;
    std::vector<stanli::SegmentItem> items;
    std::map<int, int> inside;
    std::set<int> produced;
    bool any_active = false;
    for (size_t i = first; i < last; ++i) {
      const auto& c = nodes[i];
      if (c.kind == Node::KernelCall) {
        items.push_back({c.op, -1, -1});
        produced.insert(p.body.ops[c.op].out);
        any_active |= c.active;
      } else {
        items.push_back({-1, c.dst, c.src});
        produced.insert(c.dst);
        any_active |= active[c.dst] != 0;
      }
      account(c, [&](int slot) { ++inside[slot]; });
    }
    std::vector<int> live_out;
    for (int slot : produced)
      if (reads[slot] > inside[slot]) live_out.push_back(slot);
    stanli::Segment segment;
    if (!stanli::compile_segment(p.body, items, constants, live_out, active,
                                 &segment))
      return false;
    replacement.kind = Node::Segment;
    replacement.active = any_active;
    replacement.segment = p.segments.size();
    p.segments.push_back(std::move(segment));
    return true;
  };
  std::function<void(Node&)> combine = [&](Node& n) {
    for (auto& c : n.children) combine(c);
    if (n.kind != Node::Sequence || n.children.size() < 2) return;
    std::vector<Node> next;
    for (size_t first = 0; first < n.children.size();) {
      if (!eligible(n.children[first])) {
        next.push_back(std::move(n.children[first++]));
        continue;
      }
      size_t last = first + 1;
      while (last < n.children.size() && eligible(n.children[last])) ++last;
      Node replacement;
      if (segment_run(n.children, first, last, replacement))
        next.push_back(std::move(replacement));
      else
        for (size_t i = first; i < last; ++i)
          next.push_back(std::move(n.children[i]));
      first = last;
    }
    n.children = std::move(next);
  };
  combine(p.root);
  p.site_count = 0;
  std::function<void(Node&)> number = [&](Node& n) {
    if (n.kind == Node::KernelCall) n.site = p.site_count++;
    for (auto& c : n.children) number(c);
  };
  number(p.root);
}

class StructuredRhsProbe {
 public:
  StructuredRhsProbe(const std::string& mir, const stanli::DataMap& data) {
    struct ScopedPolicy {
      std::string old;
      bool present;
      ScopedPolicy()
          : present(std::getenv("STANLI_STRUCTURED_LOOPS") != nullptr) {
        if (present) old = std::getenv("STANLI_STRUCTURED_LOOPS");
        test_setenv("STANLI_STRUCTURED_LOOPS", "force");
      }
      ~ScopedPolicy() {
        if (present)
          test_setenv("STANLI_STRUCTURED_LOOPS", old.c_str());
        else
          test_unsetenv("STANLI_STRUCTURED_LOOPS");
      }
    } policy;
    auto model = stanli::compile_model(mir, data);
    if (model.unc_params.size() != 3 || model.n_unconstrained != 3)
      throw std::runtime_error(
          "probe wrapper requires three scalar-width inputs");
    for (const auto& p : model.unc_params)
      if (p.len != 1 || p.transform != stanli::mir::Transform::Identity)
        throw std::runtime_error("probe wrapper inputs must be unconstrained");
    size_t loops = 0;
    for (const auto& op : model.graph.ops)
      loops += op.opcode == stanli::OP_LOOP;
    if (!loops)
      throw std::runtime_error("probe wrapper has no structured loop");
    if (std::getenv("STANLI_CALLBACK_PROBE_SEGMENTS")) {
      size_t segments = 0;
      for (auto& op : model.graph.ops)
        if (op.opcode == stanli::OP_LOOP) {
          auto plan = std::make_shared<stanli::StructuredLoop>(
              *static_cast<const stanli::StructuredLoop*>(op.udata));
          probe_segments(*plan);
          segments += plan->segments.size();
          op.udata = plan.get();
          model.graph.udata_pool.push_back(std::move(plan));
        }
      if (!segments) throw std::runtime_error("no eligible probe segments");
    }
    executor_ = std::make_unique<stanli::Executor>(std::move(model.graph));
    model.bind(*executor_);
  }
  StructuredRhsProbe(const StructuredRhsProbe& other)
      : executor_(std::make_unique<stanli::Executor>(*other.executor_)) {}
  void seed(double t, double y, double theta) {
    auto* p = executor_->params_data();
    p[0] = t;
    p[1] = y;
    p[2] = theta;
  }
  double forward(double t, double y, double theta) {
    seed(t, y, theta);
    return executor_->forward_value_only();
  }
  double gradient(double t, double y, double theta, double* grad,
                  double weight = 1) {
    seed(t, y, theta);
    const double value = executor_->forward();
    executor_->reverse(grad, weight);
    return value;
  }
  const stanli::Executor& executor() const { return *executor_; }
  void profile(bool enabled) { executor_->set_profile(enabled); }

 private:
  std::unique_ptr<stanli::Executor> executor_;
};
#endif
