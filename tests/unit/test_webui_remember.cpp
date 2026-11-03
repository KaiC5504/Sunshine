/**
 * @file tests/unit/test_webui_remember.cpp
 * @brief Test src/webui_remember.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// local includes
#include <src/httpcommon.h>
#include <src/webui_remember.h>

using namespace std::literals;

namespace {
  const auto now = std::chrono::system_clock::now();

  std::string read_text(const std::filesystem::path &path) {
    std::ifstream in {path, std::ios::binary};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }

  class WebUIRememberTest: public BaseTest {
  protected:
    std::filesystem::path dir;
    std::filesystem::path store;

    void SetUp() override {
      BaseTest::SetUp();
      dir = std::filesystem::temp_directory_path() / "sunshine_test_webui_remember";  // NOSONAR(cpp:S5443): safe for tests
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);
      store = webui_remember::store_file(dir / "sunshine_state.json");
    }

    void TearDown() override {
      std::filesystem::remove_all(dir);
      BaseTest::TearDown();
    }
  };
}  // namespace

TEST_F(WebUIRememberTest, StoreSitsNextToTheCredentials) {
  EXPECT_EQ(store, dir / "remembered_browsers.json");
}

TEST_F(WebUIRememberTest, RememberedTokenIsAccepted) {
  const auto token = webui_remember::remember(store, now);

  EXPECT_EQ(token.size(), 43u);
  EXPECT_TRUE(webui_remember::is_remembered(store, token, now));
  EXPECT_EQ(webui_remember::count(store, now), 1u);
}

TEST_F(WebUIRememberTest, OtherTokensAreRejected) {
  const auto token = webui_remember::remember(store, now);

  EXPECT_FALSE(webui_remember::is_remembered(store, "", now));
  EXPECT_FALSE(webui_remember::is_remembered(store, token + "x", now));
  EXPECT_FALSE(webui_remember::is_remembered(store, token.substr(1), now));
}

TEST_F(WebUIRememberTest, NothingIsRememberedWithoutAStore) {
  EXPECT_FALSE(webui_remember::is_remembered(store, "token", now));
  EXPECT_EQ(webui_remember::count(store, now), 0u);
}

TEST_F(WebUIRememberTest, StoreKeepsOnlyTheTokenHash) {
  const auto token = webui_remember::remember(store, now);

  const auto contents = read_text(store);
  EXPECT_NE(contents.find("token_hash"), std::string::npos);
  EXPECT_EQ(contents.find(token), std::string::npos);
}

TEST_F(WebUIRememberTest, BrowsersExpireAfterAYear) {
  const auto token = webui_remember::remember(store, now);

  EXPECT_TRUE(webui_remember::is_remembered(store, token, now + webui_remember::lifetime - 1min));
  EXPECT_FALSE(webui_remember::is_remembered(store, token, now + webui_remember::lifetime));
  EXPECT_EQ(webui_remember::count(store, now + webui_remember::lifetime), 0u);
}

TEST_F(WebUIRememberTest, EachBrowserGetsItsOwnToken) {
  const auto first = webui_remember::remember(store, now);
  const auto second = webui_remember::remember(store, now);

  EXPECT_NE(first, second);
  EXPECT_TRUE(webui_remember::is_remembered(store, first, now));
  EXPECT_TRUE(webui_remember::is_remembered(store, second, now));
  EXPECT_EQ(webui_remember::count(store, now), 2u);
}

TEST_F(WebUIRememberTest, TheBrowserThatExpiresFirstMakesRoom) {
  const auto oldest = webui_remember::remember(store, now);
  for (std::size_t i = 1; i <= webui_remember::max_browsers; ++i) {
    webui_remember::remember(store, now + std::chrono::minutes(i));
  }

  EXPECT_EQ(webui_remember::count(store, now), webui_remember::max_browsers);
  EXPECT_FALSE(webui_remember::is_remembered(store, oldest, now));
}

TEST_F(WebUIRememberTest, ForgetAllSignsEveryBrowserOut) {
  const auto token = webui_remember::remember(store, now);

  EXPECT_TRUE(webui_remember::forget_all(store));
  EXPECT_FALSE(webui_remember::is_remembered(store, token, now));
  EXPECT_FALSE(std::filesystem::exists(store));
  EXPECT_TRUE(webui_remember::forget_all(store));
}

TEST_F(WebUIRememberTest, DamagedStoreIsTreatedAsEmptyAndReplaced) {
  std::ofstream {store} << "{not json";

  EXPECT_EQ(webui_remember::count(store, now), 0u);
  const auto token = webui_remember::remember(store, now);
  EXPECT_TRUE(webui_remember::is_remembered(store, token, now));
  EXPECT_EQ(webui_remember::count(store, now), 1u);
}

TEST_F(WebUIRememberTest, RememberFailsWhenTheStoreCannotBeWritten) {
  EXPECT_EQ(webui_remember::remember(dir / "missing" / "remembered_browsers.json", now), "");
}

TEST_F(WebUIRememberTest, ChangingTheCredentialsForgetsEveryBrowser) {
  const auto credentials = dir / "sunshine_state.json";
  const auto token = webui_remember::remember(store, now);

  ASSERT_EQ(http::save_user_creds(credentials.string(), "user", "password"), 0);
  EXPECT_FALSE(webui_remember::is_remembered(store, token, now));
}

TEST(WebUIRememberCookieTest, TokenIsFoundAmongOtherCookies) {
  EXPECT_EQ(webui_remember::token_from_cookies("sunshine_remember=abc"), "abc");
  EXPECT_EQ(webui_remember::token_from_cookies("theme=dark; sunshine_remember=abc; other=1"), "abc");
  EXPECT_EQ(webui_remember::token_from_cookies("theme=dark;sunshine_remember=abc"), "abc");
}

TEST(WebUIRememberCookieTest, MissingOrEmptyTokenIsNotFound) {
  EXPECT_FALSE(webui_remember::token_from_cookies(""));
  EXPECT_FALSE(webui_remember::token_from_cookies("theme=dark"));
  EXPECT_FALSE(webui_remember::token_from_cookies("sunshine_remember="));
  EXPECT_FALSE(webui_remember::token_from_cookies("sunshine_remember"));
  EXPECT_FALSE(webui_remember::token_from_cookies("xsunshine_remember=abc"));
}

TEST(WebUIRememberCookieTest, CookieIsKeptFromScriptsAndOtherSites) {
  EXPECT_EQ(webui_remember::set_cookie("abc"), "sunshine_remember=abc; Max-Age=31536000; Path=/; Secure; HttpOnly; SameSite=Strict");
}

TEST(WebUIRememberCookieTest, EmptyTokenClearsTheCookie) {
  EXPECT_EQ(webui_remember::set_cookie(""), "sunshine_remember=; Max-Age=0; Path=/; Secure; HttpOnly; SameSite=Strict");
}
