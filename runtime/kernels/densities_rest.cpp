// The long tail of densities: one instantiation each, no propto family.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_DENSITY_LIST_REST(STANLI_DEFINE_DENSITY_FWD)

}  // namespace dens
}  // namespace stanli
