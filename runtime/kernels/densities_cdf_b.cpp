// Distribution functions, second half, and the integer ones.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_CDF_LIST_B(STANLI_DEFINE_CDF_FWD)

STANLI_INT_CDF_LIST(STANLI_DEFINE_INT_CDF_FWD)

}  // namespace dens
}  // namespace stanli
