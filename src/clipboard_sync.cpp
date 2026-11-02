/**
 * @file src/clipboard_sync.cpp
 * @brief Definitions for sharing the host clipboard with a streaming Moonlight client.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "clipboard_sync.h"
#include "logging.h"

using namespace std::literals;

namespace fs = std::filesystem;

namespace clipboard_sync {
  namespace {
    /**
     * @brief Convert a path to UTF-8 text.
     *
     * @param path Path to convert.
     * @return UTF-8 encoded path.
     */
    std::string to_utf8(const fs::path &path) {
      const auto text = path.u8string();
      return {text.begin(), text.end()};
    }

    /**
     * @brief Convert UTF-8 text to a path.
     *
     * @param text UTF-8 encoded path.
     * @return Path for `text`.
     */
    fs::path from_utf8(std::string_view text) {
      return fs::path {std::u8string {text.begin(), text.end()}};
    }

    /**
     * @brief Keep the regular files from a clipboard file list.
     *
     * @param paths Paths found on the clipboard.
     * @return Paths that name regular files, in their original order.
     */
    std::vector<fs::path> regular_files(const std::vector<fs::path> &paths) {
      std::vector<fs::path> files;
      for (const auto &path : paths) {
        if (std::error_code ec; fs::is_regular_file(path, ec)) {
          files.push_back(path);
        }
      }
      return files;
    }

    /**
     * @brief Check that a path names an existing regular file inside a directory.
     *
     * @param directory Directory that must contain the file.
     * @param path Path to check.
     * @return `true` when `path` resolves to a regular file below `directory`.
     */
    bool is_file_inside(const fs::path &directory, const fs::path &path) {
      std::error_code ec;
      const auto root = fs::weakly_canonical(directory, ec);
      if (ec || root.empty()) {
        return false;
      }
      const auto file = fs::weakly_canonical(path, ec);
      if (ec || !fs::is_regular_file(file, ec)) {
        return false;
      }

      const auto [root_end, file_rest] = std::mismatch(root.begin(), root.end(), file.begin(), file.end());
      return root_end == root.end() && file_rest != file.end();
    }

    /**
     * @brief Strip leading and trailing whitespace.
     *
     * @param value Text to trim.
     * @return View of `value` without surrounding whitespace.
     */
    std::string_view trim(std::string_view value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      return value;
    }

    /**
     * @brief Lowercase ASCII text.
     *
     * @param value Text to lowercase.
     * @return Lowercased copy of `value`.
     */
    std::string to_lower(std::string_view value) {
      std::string lowered {value};
      std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return lowered;
    }
  }  // namespace

  std::optional<std::string> normalize_mime_type(std::string_view content_type) {
    const auto separator = content_type.find(';');
    const auto type = to_lower(trim(content_type.substr(0, separator)));
    std::string charset;
    if (separator != std::string_view::npos) {
      auto parameters = content_type.substr(separator + 1);
      while (!parameters.empty()) {
        const auto next = parameters.find(';');
        const auto parameter = trim(parameters.substr(0, next));
        if (const auto equals = parameter.find('='); equals != std::string_view::npos && to_lower(trim(parameter.substr(0, equals))) == "charset"sv) {
          charset = to_lower(trim(parameter.substr(equals + 1)));
          std::erase(charset, '"');
        }
        parameters = next == std::string_view::npos ? std::string_view {} : parameters.substr(next + 1);
      }
    }

    if (type == "text/plain"sv && (charset.empty() || charset == "utf-8"sv)) {
      return std::string {text_mime_type};
    }
    if (type == png_mime_type && charset.empty()) {
      return std::string {png_mime_type};
    }
    if (type == files_mime_type && (charset.empty() || charset == "utf-8"sv)) {
      return std::string {files_mime_type};
    }
    return std::nullopt;
  }

  reply_t handle_get(backend_t &backend, std::optional<std::int64_t> since) {
    reply_t reply;
    reply.sequence = backend.sequence();
    if (since && *since == *reply.sequence) {
      reply.status = 204;
      return reply;
    }

    if (const auto files = regular_files(backend.files()); !files.empty()) {
      nlohmann::json list = nlohmann::json::array();
      std::uint64_t total_size = 0;
      for (std::size_t index = 0; index < files.size(); ++index) {
        std::error_code ec;
        const auto size = fs::file_size(files[index], ec);
        total_size += ec ? 0 : size;
        list.push_back({{"index", index}, {"name", to_utf8(files[index].filename())}, {"size", ec ? 0 : size}});
      }
      if (total_size > max_transfer_size) {
        BOOST_LOG(info) << "Copied files are too large to share ("sv << total_size << " bytes)"sv;
        reply.status = 204;
        return reply;
      }

      reply.content_type = files_mime_type;
      reply.body = list.dump();
      return reply;
    }

    auto item = backend.read();
    if (!item) {
      reply.status = 204;
      return reply;
    }
    if (item->data.size() > max_item_size) {
      BOOST_LOG(info) << "Clipboard item is too large to share ("sv << item->data.size() << " bytes)"sv;
      reply.status = 204;
      return reply;
    }

    reply.content_type = std::move(item->mime_type);
    reply.body = std::move(item->data);
    return reply;
  }

  reply_t handle_post(backend_t &backend, std::string_view content_type, std::string body, const fs::path &transfer_directory) {
    reply_t reply;
    const auto mime_type = normalize_mime_type(content_type);
    if (!mime_type) {
      reply.status = 415;
      return reply;
    }
    if (body.size() > max_item_size) {
      reply.status = 413;
      return reply;
    }
    if (body.empty()) {
      reply.status = 400;
      return reply;
    }

    if (*mime_type == files_mime_type) {
      std::vector<fs::path> paths;
      try {
        for (const auto &entry : nlohmann::json::parse(body)) {
          paths.push_back(from_utf8(entry.get<std::string>()));
        }
      } catch (const nlohmann::json::exception &) {
        reply.status = 400;
        return reply;
      }

      // Only files this host saved for a client may be put on the clipboard.
      if (paths.empty() || !std::ranges::all_of(paths, [&transfer_directory](const auto &path) {
            return is_file_inside(transfer_directory, path);
          })) {
        reply.status = 403;
        return reply;
      }
      if (!backend.write_files(paths)) {
        reply.status = 500;
        return reply;
      }
    } else if (!backend.write({*mime_type, std::move(body)})) {
      reply.status = 500;
      return reply;
    }
    reply.sequence = backend.sequence();
    return reply;
  }

  file_lookup_t find_file(backend_t &backend, std::optional<std::int64_t> sequence, std::optional<std::int64_t> index) {
    if (!sequence || !index || *index < 0) {
      return {400, {}};
    }
    if (*sequence != backend.sequence()) {
      return {409, {}};
    }

    const auto files = regular_files(backend.files());
    if (static_cast<std::uint64_t>(*index) >= files.size()) {
      return {404, {}};
    }
    return {200, files[static_cast<std::size_t>(*index)]};
  }

  reply_t save_upload(const fs::path &transfer_directory, std::string_view name, const std::string &body) {
    reply_t reply;

    // Treat both separators as directories so a Windows path cannot escape the transfer directory.
    std::string clean_name {name};
    std::ranges::replace(clean_name, '\\', '/');
    std::erase_if(clean_name, [](char c) {
      return static_cast<unsigned char>(c) < 0x20;
    });
    const auto file_name = from_utf8(clean_name).filename();
    if (file_name.empty() || file_name == "." || file_name == "..") {
      reply.status = 400;
      return reply;
    }
    if (body.size() > max_transfer_size) {
      reply.status = 413;
      return reply;
    }

    std::error_code ec;
    fs::create_directories(transfer_directory, ec);
    auto target = transfer_directory / file_name;
    for (int copy = 2; fs::exists(target, ec); ++copy) {
      target = transfer_directory / (file_name.stem().native() + fs::path(" (" + std::to_string(copy) + ")").native() + file_name.extension().native());
    }

    std::ofstream file {target, std::ios::binary};
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();
    if (!file) {
      fs::remove(target, ec);
      reply.status = 500;
      return reply;
    }

    reply.content_type = text_mime_type;
    reply.body = to_utf8(target);
    return reply;
  }

  std::optional<std::int64_t> parse_sequence(std::string_view value) {
    std::int64_t sequence {};
    const auto *const end = value.data() + value.size();
    if (const auto [ptr, ec] = std::from_chars(value.data(), end, sequence); value.empty() || ec != std::errc {} || ptr != end) {
      return std::nullopt;
    }
    return sequence;
  }
}  // namespace clipboard_sync
