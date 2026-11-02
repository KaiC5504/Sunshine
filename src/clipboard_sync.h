/**
 * @file src/clipboard_sync.h
 * @brief Declarations for sharing the host clipboard with a streaming Moonlight client.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clipboard_sync {
  constexpr std::string_view text_mime_type = "text/plain;charset=utf-8";  ///< MIME type of UTF-8 text items.
  constexpr std::string_view png_mime_type = "image/png";  ///< MIME type of PNG image items.
  constexpr std::string_view files_mime_type = "application/x-moonlight-files+json";  ///< MIME type of a JSON list of copied files.
  constexpr std::string_view sequence_header = "X-Clipboard-Seq";  ///< Response header carrying the host clipboard sequence number.
  constexpr std::size_t max_item_size = 32 * 1024 * 1024;  ///< Largest clipboard item accepted or served.
  constexpr std::uint64_t max_transfer_size = 512ULL * 1024 * 1024;  ///< Largest total size of the files shared by one copy.

  /**
   * @brief One clipboard representation exchanged with a client.
   */
  struct item_t {
    std::string mime_type;  ///< `text_mime_type` or `png_mime_type`.
    std::string data;  ///< Raw bytes in `mime_type` format.
  };

  /**
   * @brief Native clipboard used by the sync endpoints.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    /**
     * @brief Return a number that changes whenever the clipboard contents change.
     *
     * @return Current clipboard sequence number.
     */
    virtual std::int64_t sequence() = 0;

    /**
     * @brief Read the clipboard, preferring text over an image.
     *
     * @return The current item, or `std::nullopt` when the clipboard holds nothing shareable.
     */
    virtual std::optional<item_t> read() = 0;

    /**
     * @brief Replace the clipboard contents.
     *
     * @param item Item to place on the clipboard.
     * @return `true` when the clipboard was updated.
     */
    virtual bool write(const item_t &item) = 0;

    /**
     * @brief List the files copied to the clipboard.
     *
     * @return Paths of the copied files, empty when the clipboard holds no files.
     */
    virtual std::vector<std::filesystem::path> files() = 0;

    /**
     * @brief Put files on the clipboard so they can be pasted into a folder.
     *
     * @param paths Files to place on the clipboard.
     * @return `true` when the clipboard was updated.
     */
    virtual bool write_files(const std::vector<std::filesystem::path> &paths) = 0;
  };

  /**
   * @brief Platform-independent HTTP reply produced by the sync handlers.
   */
  struct reply_t {
    int status = 200;  ///< HTTP status code.
    std::string content_type;  ///< Content type of `body`, empty when there is no body.
    std::string body;  ///< Response body.
    std::optional<std::int64_t> sequence;  ///< Host clipboard sequence number to report, if known.
  };

  /**
   * @brief Result of looking up a file offered by a clipboard poll.
   */
  struct file_lookup_t {
    int status = 200;  ///< HTTP status code.
    std::filesystem::path path;  ///< File to send when `status` is 200.
  };

  /**
   * @brief Map a request content type onto a supported clipboard MIME type.
   *
   * @param content_type Value of the request's `Content-Type` header.
   * @return The canonical MIME type, or `std::nullopt` when it is not supported.
   */
  std::optional<std::string> normalize_mime_type(std::string_view content_type);

  /**
   * @brief Answer a client's clipboard poll.
   *
   * Copied files are offered as a `files_mime_type` list ahead of any text or image.
   *
   * @param backend Native clipboard.
   * @param since Sequence number the client last saw, if any.
   * @return 204 when nothing new is shareable, otherwise 200 with the clipboard item.
   */
  reply_t handle_get(backend_t &backend, std::optional<std::int64_t> since);

  /**
   * @brief Apply clipboard contents sent by a client.
   *
   * A `files_mime_type` body lists files previously saved by `save_upload()`; anything outside
   * `transfer_directory` is refused.
   *
   * @param backend Native clipboard.
   * @param content_type Value of the request's `Content-Type` header.
   * @param body Raw request body.
   * @param transfer_directory Directory that holds uploaded files.
   * @return 200 with the new sequence number, or an error status.
   */
  reply_t handle_post(backend_t &backend, std::string_view content_type, std::string body, const std::filesystem::path &transfer_directory);

  /**
   * @brief Find a file listed by the clipboard poll that reported `sequence`.
   *
   * @param backend Native clipboard.
   * @param sequence Sequence number of the poll that listed the file.
   * @param index Position of the file in that list.
   * @return 200 with the file, 409 when the clipboard changed since, or 404 for an unknown file.
   */
  file_lookup_t find_file(backend_t &backend, std::optional<std::int64_t> sequence, std::optional<std::int64_t> index);

  /**
   * @brief Save a file uploaded by a client without overwriting existing files.
   *
   * @param transfer_directory Directory that receives uploads.
   * @param name File name suggested by the client; any directory part is ignored.
   * @param body File contents.
   * @return 200 with the saved path as the body, or an error status.
   */
  reply_t save_upload(const std::filesystem::path &transfer_directory, std::string_view name, const std::string &body);

  /**
   * @brief Parse the `since` query parameter of a poll.
   *
   * @param value Raw parameter value.
   * @return Parsed sequence number, or `std::nullopt` when absent or malformed.
   */
  std::optional<std::int64_t> parse_sequence(std::string_view value);
}  // namespace clipboard_sync

namespace platf {
  /**
   * @brief Open the native clipboard.
   *
   * @return Clipboard backend, or `nullptr` on platforms without clipboard sync.
   */
  std::unique_ptr<clipboard_sync::backend_t> make_clipboard();

  /**
   * @brief Directory that receives files sent by clients.
   *
   * @return Transfer directory, which may not exist yet.
   */
  std::filesystem::path clipboard_transfer_directory();
}  // namespace platf
