/**
 * @file src/webui_remember.h
 * @brief Declarations for browsers that may open the web UI without signing in.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

/**
 * @brief Browsers the user chose to remember, so the web UI skips the sign-in prompt on them.
 *
 * A remembered browser holds a random token in a cookie; the store keeps only the token's hash. The
 * store sits next to the credentials file and is cleared whenever the credentials change. It is read on
 * every check rather than cached, so changes made by another Sunshine process apply at once.
 */
namespace webui_remember {
  /**
   * @brief Name of the cookie that holds a remembered browser's token.
   */
  inline constexpr std::string_view cookie_name = "sunshine_remember";

  /**
   * @brief How long a browser stays remembered.
   */
  inline constexpr std::chrono::seconds lifetime = std::chrono::days(365);

  /**
   * @brief Most browsers remembered at once; remembering another drops the one that expires first.
   */
  inline constexpr std::size_t max_browsers = 32;

  /**
   * @brief Locate the store that belongs to a credentials file.
   *
   * @param credentials_file Path of the web UI credentials file.
   * @return Path of the remembered-browsers store in the same directory.
   */
  std::filesystem::path store_file(const std::filesystem::path &credentials_file);

  /**
   * @brief Remember a new browser.
   *
   * @param file Store to add the browser to.
   * @param now Current time, which sets the expiry.
   * @return The token to give the browser, or an empty string when the store can't be written.
   */
  std::string remember(const std::filesystem::path &file, std::chrono::system_clock::time_point now);

  /**
   * @brief Check whether a token belongs to a remembered browser.
   *
   * @param file Store to look in.
   * @param token Token from the browser's cookie.
   * @param now Current time; expired browsers don't count.
   * @return True when the token is remembered and has not expired.
   */
  bool is_remembered(const std::filesystem::path &file, std::string_view token, std::chrono::system_clock::time_point now);

  /**
   * @brief Count the browsers that are remembered and have not expired.
   *
   * @param file Store to count.
   * @param now Current time.
   * @return Number of remembered browsers.
   */
  std::size_t count(const std::filesystem::path &file, std::chrono::system_clock::time_point now);

  /**
   * @brief Forget every remembered browser.
   *
   * @param file Store to clear.
   * @return True when the store is gone afterwards.
   */
  bool forget_all(const std::filesystem::path &file);

  /**
   * @brief Find the remembered-browser token in a Cookie header.
   *
   * @param cookie_header Value of the request's Cookie header.
   * @return The token, or nothing when the cookie is missing or empty.
   */
  std::optional<std::string> token_from_cookies(std::string_view cookie_header);

  /**
   * @brief Build the Set-Cookie value that stores or clears a token.
   *
   * @param token Token to store; an empty token clears the cookie.
   * @return Value for a Set-Cookie header.
   */
  std::string set_cookie(std::string_view token);
}  // namespace webui_remember
