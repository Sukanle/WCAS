#include "skl/wcas/error.h"
#define SKL_ERR_ENUM errc

namespace skl {
SKL_DERIVE_ERROR_CPP(wcas, SKL_WCAS_ERROR_CODES)
}   // namespace skl
