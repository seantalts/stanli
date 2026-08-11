// Half the densities models lean on. The other half is
// densities_common_b.cpp; hand-written shapes live in their own shards.
//
// One of the density shards: see densities_impl.hpp for why they
// are split and what they share.
#include "densities_impl.hpp"

namespace stanli {
namespace dens {

STANLI_SCALAR_DENSITY_LIST_COMMON_A(STANLI_DEFINE_DENSITY_FWD)

}  // namespace dens
}  // namespace stanli
