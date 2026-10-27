// Frame instances (see structured_instance_run.inc). While a trip of the
// framed loop records normally, it also records its data plan: each data-only
// kernel, decision, copy and index position the recorder evaluated, with
// operands that relocate to a later trip.

// Operands, from refs: Kernel inputs, outputs, scratch; Index inputs, output;
// Decide a cell; Bounds lower, upper (a = trip count); Iterate and
// OuterIterate a cell (site = loop, a = iteration); Copy source, destination
// (a = length); Write, Gather and Cells the kernel inputs (a, b = versions,
// c = positional index); Flags a = version, b = length or source version;
// Memo the key operands (a = pc, b = program, c = chosen memo).
struct PlanOp {
  enum Kind : uint8_t {
    Kernel,
    Index,
    Decide,
    Bounds,
    Iterate,
    OuterIterate,
    Copy,
    Write,
    Gather,
    Cells,
    FlagsOnes,
    FlagsZeros,
    FlagsCopy,
    Memo
  };
  Kind kind;
  uint8_t mode = 0;
  bool expect = false;
  uint32_t site = 0;
  uint32_t refs = 0;
  uint32_t clock = 0;
  int64_t a = 0, b = 0, c = 0;
};

enum PlanMode : uint8_t {
  kWriteConst = 1,
  kWriteCells = 2,
  kWriteRhsConst = 4,
  kWriteRhsCells = 8,
  kWriteLocal = 16,
  kPrepared = 32
};

enum class Use { Read, Write, Touch };

struct Access {
  uint32_t first = 0, last = 0;
  Use use = Use::Read;
  bool rhs = false;
  int32_t ref = -1;
  int64_t len = 0;
};

// Base 0 is the workspace, 1 the new frame's values, 2 scratch for arena
// cells the frame does not keep; later bases are earlier frames or
// absolute addresses.
struct PlanRef {
  int32_t base = -1;
  int32_t offset = 0;
};

struct Positional {
  uint32_t offset = 0, count = 0;
  int64_t slot = -1, adj_slot = -1;
  bool fixed = false;
};

// Frames are tape indices. Local is the frame being built.
struct FrameLoc {
  static constexpr int64_t Workspace = -1, Null = -2, Absolute = -3, Local = -4,
                           Scratch = -5;
  int64_t frame = Null;
  int64_t offset = 0;
  bool operator==(const FrameLoc& b) const {
    return frame == b.frame && offset == b.offset;
  }
};

struct VersionImage {
  FrameLoc value;
  int64_t adjoint = -1;
  int32_t owner = -1;
  bool constant = false;
};

struct StateImage {
  std::vector<VersionImage> versions;
  std::vector<int64_t> bindings, node_version, node_version2, loop_version;
  std::vector<int64_t> flag_keys;
  std::vector<std::pair<int64_t, int64_t>> links;
  std::vector<uint8_t> generations;
  std::vector<int64_t> loop_generation, node_generation;
  size_t promotions = 0, targets = 0;
  int64_t adjoint_size = 0;
  uint64_t effects = 0;
};

struct TripCapture {
  std::vector<PlanOp> ops;
  std::vector<const double*> ptrs;
  std::vector<double> seen;
  std::vector<int64_t> positions;
  std::vector<Positional> positional;
  std::vector<uint8_t> key_flags;
  std::vector<std::pair<int64_t, int64_t>> promotions;
  std::vector<std::pair<const double*, size_t>> blocks;
  std::vector<int64_t> keys;
  StateImage pre;
  const Stream* stream = nullptr;
  bool refused = false;

  void push(PlanOp::Kind kind, uint32_t site = 0) {
    PlanOp op;
    op.kind = kind;
    op.site = site;
    op.refs = static_cast<uint32_t>(ptrs.size());
    op.clock =
        static_cast<uint32_t>(stream->program.size() - stream->targets.size());
    ops.push_back(op);
  }
  void operand(const double* ptr, bool scalar = false) {
    ptrs.push_back(ptr);
    seen.push_back(scalar && ptr ? *ptr : 0.0);
  }
  void inputs(const Op& op, const KernelCtx& c) {
    for (int k = 0; k < op.n_in; ++k) operand(c.in[k].data, c.in[k].len == 1);
  }
  template <class T>
  void place(const T* at, size_t count) {
    Positional entry;
    entry.offset = static_cast<uint32_t>(positions.size());
    entry.count = static_cast<uint32_t>(count);
    positions.insert(positions.end(), at, at + count);
    ops.back().c = static_cast<int64_t>(positional.size());
    positional.push_back(entry);
  }
  void kernel(const Node& n, const Op& op, const KernelCtx& c,
              bool prepared = false) {
    push(prepared ? PlanOp::Index : PlanOp::Kernel, n.site);
    inputs(op, c);
    operand(c.out.data);
    if (prepared) return;
    if (op.out2 >= 0) operand(c.out2.data);
    operand(c.scratch);
  }
  void decide(const double* cell, bool taken) {
    push(PlanOp::Decide);
    ops.back().expect = taken;
    operand(cell, true);
  }
  void bounds(int loop, const double* lower, const double* upper,
              int64_t count) {
    push(PlanOp::Bounds, static_cast<uint32_t>(loop));
    ops.back().a = count;
    operand(lower, true);
    operand(upper, true);
  }
  void iterate(const double* cell, int loop, int64_t iteration) {
    push(PlanOp::Iterate, static_cast<uint32_t>(loop));
    ops.back().a = iteration;
    operand(cell);
  }
  void outer_iterate(const double* cell) {
    push(PlanOp::OuterIterate);
    operand(cell);
  }
  void copy(const double* from, const double* to, int64_t len) {
    if (len <= 0) return;
    push(PlanOp::Copy);
    ops.back().a = len;
    operand(from);
    operand(to);
  }
  template <class T>
  void write(const Node& n, const Op& op, const KernelCtx& c, int64_t base,
             int64_t rhs, bool constant, bool cells, bool rhs_constant,
             bool rhs_cells, const T* at, size_t count) {
    push(PlanOp::Write, n.site);
    auto& w = ops.back();
    w.mode = (constant ? kWriteConst : 0) | (cells ? kWriteCells : 0) |
             (rhs_constant ? kWriteRhsConst : 0) |
             (rhs_cells ? kWriteRhsCells : 0);
    w.a = base;
    w.b = rhs;
    inputs(op, c);
    if (!constant) place(at, count);
  }
  void gather(const Node& n, const Op& op, const KernelCtx& c,
              const int64_t* at, size_t count) {
    push(PlanOp::Gather, n.site);
    inputs(op, c);
    place(at, count);
  }
  void cells(const Node& n, const Op& op, const KernelCtx& c, int64_t version,
             bool constant) {
    push(PlanOp::Cells, n.site);
    ops.back().a = version;
    ops.back().expect = constant;
    inputs(op, c);
  }
  void flags(PlanOp::Kind kind, int64_t version, int64_t other) {
    push(kind);
    ops.back().a = version;
    ops.back().b = other;
  }
  void promotion(int64_t index, int64_t adjoint) {
    promotions.emplace_back(index, adjoint);
  }
  void memo(const RecordingProgram& program, size_t pc,
            const DataLoopMemo* chosen, bool trace, bool disabled) {
    push(PlanOp::Memo);
    ops.back().a = static_cast<int64_t>(pc);
    ops.back().b = reinterpret_cast<intptr_t>(&program);
    ops.back().expect = disabled;
    ops.back().site = static_cast<uint32_t>(key_flags.size());
    chose(chosen, trace);
  }
  void key(const double* ptr, bool constant) {
    operand(ptr);
    key_flags.push_back(constant);
  }
  void chose(const DataLoopMemo* chosen, bool trace) {
    if (trace) refused = true;
    ops.back().c = reinterpret_cast<intptr_t>(chosen);
  }
};

struct FrameTemplate {
  const LoopFrame* frame = nullptr;
  size_t index = 0, instructions = 0;
  int flow = 0;
  std::vector<PlanOp> ops;
  std::vector<PlanRef> refs;
  std::vector<double> seen, values;
  std::vector<std::pair<uint32_t, double>> checks;
  std::vector<int64_t> positions;
  std::vector<Positional> positional;
  std::vector<uint8_t> key_flags;
  std::vector<FrameLoc> bases;
  size_t image_size = 0;
  std::vector<FrameLoc> bindings;
  std::vector<uint8_t> fixed_slots, fixed_adj_slots;
  StateImage pre, post;
  std::vector<std::pair<int64_t, int64_t>> flag_remap, promotion_writes;
  std::vector<std::pair<FrameLoc, FrameAdjoint>> targets;
  std::vector<uint32_t> ws_plan;
  // Ops that repeat the template's inputs need not run; see plan_schedule.
  std::vector<double> workspace;
  std::vector<uint32_t> schedule, frame_restore;
  std::vector<std::pair<uint32_t, double>> ws_restore;
  std::vector<int64_t> flag_restore;
  std::unordered_map<int64_t, std::vector<uint8_t>> final_flags;
  std::vector<uint8_t> varying;
  std::vector<int64_t> bounds, flag_units;
  std::vector<Access> accesses;
  std::vector<uint32_t> access_at, array_checks;
  std::unordered_map<uint32_t, std::pair<size_t, int64_t>> array_at;
  std::vector<double> array_values;
  bool reads_initial = false;
  size_t reschedules = 0;
  bool stale = false;
};

struct FrameInstanceRestart {};

struct MemoCounts {
  DataLoopCandidate* candidate;
  size_t hits, skipped, events;
  bool disabled;
};

struct FrameInstances {
  // Keyed by the programs of the previous frame and this frame.
  using Context = std::pair<const FrameCode*, const FrameCode*>;
  std::map<Context, std::unique_ptr<FrameTemplate>> templates;
  std::map<Context, const FrameCode*> successor;
  std::unique_ptr<TripCapture> capture;
  std::map<const double*, size_t> by_address;
  size_t indexed = 0;
  // Where commit placed each version, while no recording has changed them.
  std::vector<FrameLoc> placed;
  bool placed_valid = false;
  std::vector<double> image, registers, workspace;
  std::vector<int64_t> shifts, scratch;
  std::vector<double*> bases;
  std::unordered_map<int64_t, std::vector<uint8_t>> flags;
  std::vector<std::pair<double*, double>> undo, revert;
  std::vector<std::pair<uint32_t, double>> kept;
  std::vector<MemoCounts> memo_undo;
  std::vector<uint8_t> written;
  size_t instantiated = 0, fallback = 0, refused = 0, restarts = 0;
  bool enabled = true;
};
