/**
 * @file src/webui_remember.cpp
 * @brief Definitions for browsers that may open the web UI without signing in.
 */
// this include
#include "webui_remember.h"

// standard includes
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <system_error>
#include <vector>

// lib includes
#include <boost/algorithm/string/trim.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "crypto.h"
#include "logging.h"
#include "utility.h"

using namespace std::literals;

namespace webui_remember {
  namespace {
    namespace fs = std::filesystem;

    /**
     * @brief A remembered browser as kept in the store.
     */
    struct browser_t {
      std::string token_hash;  ///< SHA-256 of the browser's token, in hex.
      std::int64_t expires;  ///< Expiry in seconds since the epoch.
    };

    std::mutex store_mutex;  ///< Serializes changes to stores. NOSONAR(cpp:S5421) - intentionally mutable global

    // 43 characters from a 62-letter alphabet carry 256 bits, and stay valid in a cookie unquoted.
    constexpr std::size_t token_length = 43;
    constexpr std::string_view token_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

    std::string hash_token(std::string_view token) {
      return util::hex(crypto::hash(token)).to_string();
    }

    std::int64_t to_seconds(std::chrono::system_clock::time_point time) {
      return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    /**
     * @brief Read the browsers in a store that have not expired.
     *
     * @param file Store to read.
     * @param now Current time.
     * @return The live browsers; none when the store is missing or unreadable.
     */
    std::vector<browser_t> load(const fs::path &file, std::chrono::system_clock::time_point now) {
      std::vector<browser_t> browsers;
      std::ifstream in {file};
      if (!in) {
        return browsers;
      }

      try {
        for (const auto &entry : nlohmann::json::parse(in).at("browsers")) {
          browser_t browser {entry.at("token_hash").get<std::string>(), entry.at("expires").get<std::int64_t>()};
          if (browser.expires > to_seconds(now)) {
            browsers.push_back(std::move(browser));
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Ignoring unreadable remembered browsers in "sv << file.string() << ": "sv << e.what();
        browsers.clear();
      }
      return browsers;
    }

    /**
     * @brief Replace a store's contents.
     *
     * @param file Store to write.
     * @param browsers Browsers to keep.
     * @return True when the store was written.
     */
    bool save(const fs::path &file, const std::vector<browser_t> &browsers) {
      auto tree = nlohmann::json::object();
      tree["browsers"] = nlohmann::json::array();
      for (const auto &browser : browsers) {
        tree["browsers"].push_back({{"token_hash", browser.token_hash}, {"expires", browser.expires}});
      }

      // Written aside and renamed into place, so a reader never sees half a file.
      auto temporary = file;
      temporary += ".tmp";
      {
        std::ofstream out {temporary, std::ios::binary | std::ios::trunc};
        if (!(out << tree.dump(2))) {
          BOOST_LOG(error) << "Couldn't write "sv << temporary.string();
          return false;
        }
      }

      std::error_code ec;
      fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
      fs::rename(temporary, file, ec);
      if (ec) {
        BOOST_LOG(error) << "Couldn't replace "sv << file.string() << ": "sv << ec.message();
        fs::remove(temporary, ec);
        return false;
      }
      return true;
    }
  }  // namespace

  std::filesystem::path store_file(const std::filesystem::path &credentials_file) {
    return credentials_file.parent_path() / "remembered_browsers.json";
  }

  std::string remember(const std::filesystem::path &file, std::chrono::system_clock::time_point now) {
    auto token = crypto::rand_alphabet(token_length, token_alphabet);

    std::scoped_lock lock {store_mutex};
    auto browsers = load(file, now);
    browsers.push_back({hash_token(token), to_seconds(now + lifetime)});
    if (browsers.size() > max_browsers) {
      std::ranges::sort(browsers, std::ranges::greater {}, &browser_t::expires);
      browsers.resize(max_browsers);
    }

    if (!save(file, browsers)) {
      return {};
    }
    return token;
  }

  bool is_remembered(const std::filesystem::path &file, std::string_view token, std::chrono::system_clock::time_point now) {
    if (token.empty()) {
      return false;
    }

    const auto token_hash = hash_token(token);
    std::scoped_lock lock {store_mutex};
    return std::ranges::any_of(load(file, now), [&token_hash](const browser_t &browser) {
      return browser.token_hash == token_hash;
    });
  }

  std::size_t count(const std::filesystem::path &file, std::chrono::system_clock::time_point now) {
    std::scoped_lock lock {store_mutex};
    return load(file, now).size();
  }

  bool forget_all(const std::filesystem::path &file) {
    std::scoped_lock lock {store_mutex};
    std::error_code ec;
    fs::remove(file, ec);
    return !fs::exists(file, ec);
  }

  std::optional<std::string> token_from_cookies(std::string_view cookie_header) {
    while (!cookie_header.empty()) {
      const auto end = cookie_header.find(';');
      auto pair = std::string {cookie_header.substr(0, end)};
      cookie_header = end == std::string_view::npos ? std::string_view {} : cookie_header.substr(end + 1);

      boost::algorithm::trim(pair);
      const auto equals = pair.find('=');
      if (equals == std::string::npos || std::string_view {pair}.substr(0, equals) != cookie_name) {
        continue;
      }
      if (auto value = pair.substr(equals + 1); !value.empty()) {
        return value;
      }
    }
    return std::nullopt;
  }

  std::string set_cookie(std::string_view token) {
    const auto max_age = token.empty() ? 0 : lifetime.count();
    // Strict keeps the cookie off requests that other sites start; typed addresses and bookmarks still send it.
    return std::string {cookie_name} + "=" + std::string {token} + "; Max-Age=" + std::to_string(max_age) +
           "; Path=/; Secure; HttpOnly; SameSite=Strict";
  }
}  // namespace webui_remember
