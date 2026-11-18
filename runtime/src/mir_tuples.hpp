// The tuple variables of a model's interface. The compiler splits every
// tuple variable into one variable per leaf before the runtime sees it, so
// procedure bodies hold none. What stays is the boundary with CmdStan's
// conventions: the JSON key each data leaf is read under, the element order
// CmdStan's reader stores an array of tuples in, and the names write_array
// gives a tuple parameter's columns. This is the one place that knows a
// leaf's structure; it comes from the typed program, never from a name.
#ifndef STANLI_MIR_TUPLES_HPP
#define STANLI_MIR_TUPLES_HPP

#include <stanli/mir.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stanli {
namespace mir {
namespace detail {

// A sized type that may be a tuple: `type` is the node itself (an STuple,
// an SArray of STuple, or any other type), `components` the tuple's
// component types.
struct TupleType {
  SizedType type;
  std::vector<TupleType> components;
};

inline bool is_tuple(const SizedType& t) {
  return t.base == "STuple" || (t.base == "SArray" && t.elem_base == "STuple");
}

// One step from a tuple into one of its components. `before` counts the
// array dimensions of the leaf that come before the step: the dimensions of
// every array of tuples enclosing it.
struct TupleSlot {
  size_t before = 0;
  int component = 0;  // from 1
};

// A variable the compiler made for one leaf of a tuple input or output
// variable `base`. Its type has the dimensions of every enclosing array of
// tuples, then its own: `p[i].2` of `array[N] tuple(real, vector[2]) p` is
// an `array[N] vector[2]`.
struct TupleLeaf {
  std::string variable;
  std::string base;
  std::vector<TupleSlot> path;
  SizedType type;
  // CmdStan's JSON key: the base name followed by each component, `p.2`.
  std::string key;
  // A parameter leaf's read: its transform and the dimensions FnReadParam
  // reads, from the log density's declaration of the leaf.
  std::optional<Transform> read_transform;
  std::vector<Expr> read_dims;
  // Whether an array of tuples encloses the leaf, so that CmdStan's
  // reader lays it out element by element.
  bool in_array() const {
    for (const TupleSlot& slot : path)
      if (slot.before > 0) return true;
    return false;
  }
};

struct TupleInterface {
  // Every leaf, by its variable's name.
  std::map<std::string, TupleLeaf> leaves;
  // Each tuple input and output variable's leaf variables, depth first.
  std::map<std::string, std::vector<std::string>> leaves_of;

  const TupleLeaf* leaf(const std::string& variable) const {
    const auto found = leaves.find(variable);
    return found == leaves.end() ? nullptr : &found->second;
  }
};

inline const TupleLeaf* tuple_leaf(const Program& program,
                                   const std::string& variable) {
  return program.tuples ? program.tuples->leaf(variable) : nullptr;
}

// The key data for `name`, an input variable, is read under.
inline const std::string& data_key(const Program& program,
                                   const std::string& name) {
  const TupleLeaf* leaf = tuple_leaf(program, name);
  return leaf ? leaf->key : name;
}

// The wire's description of a leaf variable, before it is checked against
// the interface types.
struct WireTupleLeaf {
  std::string variable;
  std::string base;
  std::vector<TupleSlot> path;
};

// Check the leaf list against the tuple types of the input and output
// variables, replace each tuple input by its leaf variables in
// `program.input_vars`, and record the interface on `program.tuples`.
// `tuple_types` holds the full type of every tuple input and output.
void attach_tuple_interface(
    Program& program, std::map<std::string, TupleType> tuple_types,
    std::vector<std::pair<size_t, std::string>> tuple_inputs,
    std::vector<WireTupleLeaf> leaves);

// For a leaf of `dims` laid out as CmdStan's JSON reader stores an array of
// tuples -- element by element of the outermost tuple array, the first
// index fastest, each element's values whole -- the position of each value
// of the leaf variable, which is first index fastest across all its
// dimensions. Empty when the two orders agree.
std::vector<size_t> reader_order(const TupleLeaf& leaf,
                                 const std::vector<int64_t>& dims);

// The CSV column count of each leaf of tuple output `var`, in order.
template <class Extent>
std::vector<int64_t> output_parts(const TupleInterface* tuples,
                                  const OutputVar& var, const Extent& extent) {
  std::vector<int64_t> parts;
  if (!tuples) return parts;
  const auto found = tuples->leaves_of.find(var.name);
  if (found == tuples->leaves_of.end()) return parts;
  for (const std::string& name : found->second) {
    int64_t columns = 1;
    for (const Expr& dim : tuples->leaves.at(name).type.dims)
      columns *= extent(dim);
    parts.push_back(columns);
  }
  return parts;
}

// The tuple variables with no array of tuples among their leaves, for
// OutputCursor.
inline std::set<std::string> whole_leaf_tuples(const TupleInterface* tuples) {
  std::set<std::string> names;
  if (!tuples) return names;
  for (const auto& [name, leaves] : tuples->leaves_of)
    if (std::none_of(leaves.begin(), leaves.end(),
                     [&](const std::string& leaf) {
                       return tuples->leaves.at(leaf).in_array();
                     }))
      names.insert(name);
  return names;
}

// Which leaf of its tuple variable `leaf` is, in the order of
// output_parts. Empty for a null leaf.
inline std::optional<size_t> leaf_index(const TupleInterface* tuples,
                                        const TupleLeaf* leaf) {
  if (!tuples || !leaf) return std::nullopt;
  const auto found = tuples->leaves_of.find(leaf->base);
  if (found == tuples->leaves_of.end()) return std::nullopt;
  for (size_t k = 0; k < found->second.size(); ++k)
    if (found->second[k] == leaf->variable) return k;
  return std::nullopt;
}

// The leaf of tuple output `var` that a write the cursor placed in `part`
// is of. Null when there is no such part, or when an array of tuples holds
// the leaves: its writes go element by element, so they do not take the
// leaves in order.
inline const TupleLeaf* leaf_of_part(const TupleInterface* tuples,
                                     const OutputVar& var,
                                     std::optional<size_t> part) {
  if (!tuples || !part) return nullptr;
  const auto found = tuples->leaves_of.find(var.name);
  if (found == tuples->leaves_of.end() || *part >= found->second.size())
    return nullptr;
  for (const std::string& name : found->second)
    if (tuples->leaves.at(name).in_array()) return nullptr;
  return &tuples->leaves.at(found->second[*part]);
}

// The leaf of output `owner` a write is of, given the leaf variable the
// write reads (null when it reads none) and the part the cursor placed it
// in. The optimizer may have turned the write into a literal or into a read
// of another leaf, so the position decides where it can; inside an array of
// tuples only the leaf read can. Null when `owner` is not a tuple or the
// leaf cannot be told.
inline const TupleLeaf* output_leaf(const TupleInterface* tuples,
                                    const OutputVar& owner,
                                    const TupleLeaf* read,
                                    std::optional<size_t> part) {
  if (!is_tuple(owner.type)) return nullptr;
  if (const TupleLeaf* at = leaf_of_part(tuples, owner, part)) return at;
  return read && read->base == owner.name ? read : nullptr;
}

// CmdStan's name for the values one write produces: the output variable,
// then each array index and tuple slot of the write (`p.3:2`). `variable` is
// the part naming one CSV variable, the name up to the last tuple slot, and
// `index` the indices after it. Empty when the write is not of one element
// of each array holding a tuple.
struct OutputName {
  std::string name;
  std::string variable;
  std::vector<int64_t> index;
};

inline std::optional<OutputName> output_name(
    const std::string& owner, const TupleLeaf* leaf,
    const std::vector<int64_t>& indices) {
  OutputName out{owner, owner, {}};
  size_t used = 0;
  if (leaf)
    for (const TupleSlot& slot : leaf->path) {
      if (slot.before > indices.size()) return std::nullopt;
      for (; used < slot.before; ++used)
        out.name += "." + std::to_string(indices[used]);
      out.name += ":" + std::to_string(slot.component);
      out.variable = out.name;
    }
  for (; used < indices.size(); ++used) {
    out.name += "." + std::to_string(indices[used]);
    out.index.push_back(indices[used]);
  }
  return out;
}

// Give each MultiAssign target the view and adlevel of its declaration.
void finish_multi_assigns(Program& program);

// Copy each tuple parameter leaf's read, as the finalized log density
// declares it, into the interface.
void record_tuple_reads(Program& program);

}  // namespace detail
}  // namespace mir
}  // namespace stanli

#endif
