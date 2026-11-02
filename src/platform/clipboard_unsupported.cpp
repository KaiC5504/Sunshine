/**
 * @file src/platform/clipboard_unsupported.cpp
 * @brief Clipboard sync stub for hosts without a native clipboard backend.
 */
// local includes
#include "src/clipboard_sync.h"

namespace platf {
  std::unique_ptr<clipboard_sync::backend_t> make_clipboard() {
    return nullptr;
  }

  std::filesystem::path clipboard_transfer_directory() {
    return {};
  }
}  // namespace platf
