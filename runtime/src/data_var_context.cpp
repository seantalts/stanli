// Building a DataMap from a Stan var_context, for callers that already have
// one -- language bindings that map their host arrays onto stan::io rather
// than serializing to JSON first.
#include <stanli/data.hpp>

#include <stan/io/var_context.hpp>

namespace stanli {

DataMap DataMap::from_var_context(const stan::io::var_context& context) {
  DataMap d;
  // A var_context stores multidimensional values flat and column-major,
  // which is what an Entry wants, so both loops copy without reordering.
  // Scalars report no dims from either side.
  std::vector<std::string> names;
  context.names_r(names);
  for (const std::string& name : names) {
    // Contexts differ on whether an integer variable also shows up under
    // names_r (as promoted doubles). Let the integer loop below own those:
    // it is the one that fills Entry::i.
    if (context.contains_i(name)) continue;
    Entry e;
    e.r = context.vals_r(name);
    const std::vector<size_t> dims = context.dims_r(name);
    e.dims.assign(dims.begin(), dims.end());
    d.m_[name] = std::move(e);
  }

  context.names_i(names);
  for (const std::string& name : names) {
    Entry e;
    e.is_int = true;
    e.i = context.vals_i(name);
    e.r.assign(e.i.begin(), e.i.end());  // ints are also usable as reals
    const std::vector<size_t> dims = context.dims_i(name);
    e.dims.assign(dims.begin(), dims.end());
    d.m_[name] = std::move(e);
  }
  return d;
}

}  // namespace stanli
