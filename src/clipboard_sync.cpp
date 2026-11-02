/**
 * @file src/clipboard_sync.cpp
 * @brief Definitions for sharing the host clipboard with a streaming Moonlight client.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>

// local includes
#include "clipboard_sync.h"
#include "logging.h"

using namespace std::literals;

namespace clipboard_sync {
  namespace {
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
    return std::nullopt;
  }

  reply_t handle_get(backend_t &backend, std::optional<std::int64_t> since) {
    reply_t reply;
    reply.sequence = backend.sequence();
    if (since && *since == *reply.sequence) {
      reply.status = 204;
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

  reply_t handle_post(backend_t &backend, std::string_view content_type, std::string body) {
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

    if (!backend.write({*mime_type, std::move(body)})) {
      reply.status = 500;
      return reply;
    }
    reply.sequence = backend.sequence();
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
