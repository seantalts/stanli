// Why reroll or partition left a group of matching lanes unfused. Both passes
// record one entry per group they decline, and STANLI_LANE_REPORT=1 prints
// the entries as LANES lines, so a corpus sweep can rank the reasons
// (tools/lane_declines.py).
#ifndef STANLI_LANE_DECLINE_HPP
#define STANLI_LANE_DECLINE_HPP

#include <cstdint>
#include <vector>

namespace stanli {

#define STANLI_LANE_DECLINES(X)                                            \
  X(kVaryingSlot, "lanes read different non-constant slots")               \
  X(kRigidInputs, "a plain op reads different slots in the first lanes")   \
  X(kEscapes, "a lane value is read outside its lane")                     \
  X(kWrittenBetween, "a slot the lanes read is written between them")      \
  X(kEffectBetween, "a print, reject or check sits between the lanes")     \
  X(kMixedTerms, "some lanes are target terms and some are not")           \
  X(kTermAlsoInput, "a target term is also an op input")                   \
  X(kIdenticalLanes, "every lane computes the same term")                  \
  X(kIndexShape, "an indexed read is out of range or has a varying base")  \
  X(kRowShape, "a row read or gather does not cover a packable range")     \
  X(kStoreShape, "a store does not march through one private vector")      \
  X(kWidth, "lane widths do not agree")                                    \
  X(kDensityForm, "the density has no fused form for these operands")      \
  X(kOpcode, "an op has no lane rule")                                     \
  X(kSharedAdjoint, "an active scalar is read at two positions")           \
  X(kNothingToFuse, "no density, store or target term to fuse")            \
  X(kDelimiter, "the fused form would not end each lane at its delimiter") \
  X(kCost, "the cost model prefers the scalar form")                       \
  X(kSplitCap, "the graph already used its split budget")

enum class LaneDecline : uint8_t {
#define STANLI_LANE_DECLINE_ENUM(name, text) name,
  STANLI_LANE_DECLINES(STANLI_LANE_DECLINE_ENUM)
#undef STANLI_LANE_DECLINE_ENUM
};

inline const char* lane_decline_text(LaneDecline why) {
  switch (why) {
#define STANLI_LANE_DECLINE_TEXT(name, text) \
  case LaneDecline::name:                    \
    return text;
    STANLI_LANE_DECLINES(STANLI_LANE_DECLINE_TEXT)
#undef STANLI_LANE_DECLINE_TEXT
  }
  return "";
}

struct LaneDeclineRecord {
  LaneDecline why = LaneDecline::kCost;
  int64_t op = 0;       // first lane's first op, in the pass's input graph
  int64_t lanes = 0;    // lanes in the declined group
  int64_t period = 0;   // reroll: ops per lane; partition: 0
  uint16_t opcode = 0;  // the op the reason is about
};

// One LANES line per record through the diagnostic sink.
void print_lane_declines(const std::vector<LaneDeclineRecord>& declines,
                         const char* pass, const char* graph);

}  // namespace stanli

#endif
