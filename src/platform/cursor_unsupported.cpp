/**
 * @file src/platform/cursor_unsupported.cpp
 * @brief Pointer sync stub for hosts without a native pointer backend.
 */
// local includes
#include "src/cursor_sync.h"

namespace platf {
  std::unique_ptr<cursor_sync::backend_t> make_cursor() {
    return nullptr;
  }
}  // namespace platf
