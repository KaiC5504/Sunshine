/**
 * @file tests/unit/test_clipboard_sync.cpp
 * @brief Test the platform-independent clipboard sync request handling.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/clipboard_sync.h>

namespace {
  /**
   * @brief In-memory clipboard that counts changes like NSPasteboard.
   */
  class fake_clipboard_t final: public clipboard_sync::backend_t {
  public:
    std::int64_t sequence() override {
      return changes;
    }

    std::optional<clipboard_sync::item_t> read() override {
      return contents;
    }

    bool write(const clipboard_sync::item_t &item) override {
      writes.push_back(item);
      if (fail_writes) {
        return false;
      }
      contents = item;
      copied_files.clear();
      ++changes;
      return true;
    }

    std::vector<std::filesystem::path> files() override {
      return copied_files;
    }

    bool write_files(const std::vector<std::filesystem::path> &paths) override {
      if (fail_writes) {
        return false;
      }
      copied_files = paths;
      contents.reset();
      ++changes;
      return true;
    }

    std::int64_t changes = 7;  ///< Current change count.
    std::optional<clipboard_sync::item_t> contents;  ///< Current clipboard item.
    std::vector<std::filesystem::path> copied_files;  ///< Files currently on the clipboard.
    std::vector<clipboard_sync::item_t> writes;  ///< Every item the handlers tried to write.
    bool fail_writes = false;  ///< Whether writes should fail.
  };

  /**
   * @brief Create a scratch directory for file-transfer tests.
   */
  class ClipboardFilesTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      root = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "clipboard_files";
      std::filesystem::remove_all(root);
      std::filesystem::create_directories(root / "source");
      transfers = root / "transfers";
    }

    void TearDown() override {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
      BaseTest::TearDown();
    }

    /**
     * @brief Write a file below the scratch directory.
     *
     * @param relative Path below the scratch directory.
     * @param contents File contents.
     * @return Path of the written file.
     */
    std::filesystem::path make_file(const std::filesystem::path &relative, const std::string &contents) const {
      const auto path = root / relative;
      std::filesystem::create_directories(path.parent_path());
      std::ofstream {path, std::ios::binary} << contents;
      return path;
    }

    std::filesystem::path root;  ///< Scratch directory removed after each test.
    std::filesystem::path transfers;  ///< Transfer directory used by uploads.
  };
}  // namespace

TEST(ClipboardSyncTest, NormalizesSupportedContentTypes) {
  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/plain"), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type(" Text/Plain ; charset=UTF-8"), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/plain; format=flowed; charset=\"utf-8\""), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("image/png"), clipboard_sync::png_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("application/x-moonlight-files+json; charset=utf-8"), clipboard_sync::files_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("IMAGE/PNG;"), clipboard_sync::png_mime_type);

  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/plain; charset=utf-16"), std::nullopt);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("image/png; charset=utf-8"), std::nullopt);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/html"), std::nullopt);
  EXPECT_EQ(clipboard_sync::normalize_mime_type(""), std::nullopt);
}

TEST(ClipboardSyncTest, ParsesSequenceNumbers) {
  EXPECT_EQ(clipboard_sync::parse_sequence("0"), 0);
  EXPECT_EQ(clipboard_sync::parse_sequence("1234567890123"), 1234567890123);
  EXPECT_EQ(clipboard_sync::parse_sequence("-3"), -3);
  EXPECT_EQ(clipboard_sync::parse_sequence(""), std::nullopt);
  EXPECT_EQ(clipboard_sync::parse_sequence("12a"), std::nullopt);
  EXPECT_EQ(clipboard_sync::parse_sequence(" 12"), std::nullopt);
}

TEST(ClipboardSyncTest, GetReturnsContentsUntilTheClientHasSeenThem) {
  fake_clipboard_t clipboard;
  clipboard.contents = clipboard_sync::item_t {std::string {clipboard_sync::text_mime_type}, "hello"};

  auto reply = clipboard_sync::handle_get(clipboard, std::nullopt);
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.content_type, clipboard_sync::text_mime_type);
  EXPECT_EQ(reply.body, "hello");
  EXPECT_EQ(reply.sequence, 7);

  reply = clipboard_sync::handle_get(clipboard, 6);
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.body, "hello");

  reply = clipboard_sync::handle_get(clipboard, 7);
  EXPECT_EQ(reply.status, 204);
  EXPECT_TRUE(reply.body.empty());
  EXPECT_TRUE(reply.content_type.empty());
  EXPECT_EQ(reply.sequence, 7);
}

TEST(ClipboardSyncTest, GetReportsNothingForEmptyOrOversizedClipboards) {
  fake_clipboard_t clipboard;

  auto reply = clipboard_sync::handle_get(clipboard, std::nullopt);
  EXPECT_EQ(reply.status, 204);
  EXPECT_EQ(reply.sequence, 7);

  clipboard.contents = clipboard_sync::item_t {std::string {clipboard_sync::png_mime_type}, std::string(clipboard_sync::max_item_size + 1, 'x')};
  reply = clipboard_sync::handle_get(clipboard, std::nullopt);
  EXPECT_EQ(reply.status, 204);
  EXPECT_TRUE(reply.body.empty());
}

TEST(ClipboardSyncTest, PostWritesSupportedItemsAndReportsTheNewSequence) {
  fake_clipboard_t clipboard;

  auto reply = clipboard_sync::handle_post(clipboard, "text/plain; charset=utf-8", "copied", {});
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.sequence, 8);
  ASSERT_TRUE(clipboard.contents);
  EXPECT_EQ(clipboard.contents->mime_type, clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard.contents->data, "copied");

  // The client skips the item it just sent when it next polls with the reported sequence.
  EXPECT_EQ(clipboard_sync::handle_get(clipboard, reply.sequence).status, 204);

  reply = clipboard_sync::handle_post(clipboard, "image/png", "png bytes", {});
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(clipboard.contents->mime_type, clipboard_sync::png_mime_type);
}

TEST(ClipboardSyncTest, PostRejectsInvalidRequestsWithoutWriting) {
  fake_clipboard_t clipboard;

  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "text/html", "<b>no</b>", {}).status, 415);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "", "text", {}).status, 415);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "text/plain", "", {}).status, 400);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "image/png", std::string(clipboard_sync::max_item_size + 1, 'x'), {}).status, 413);
  EXPECT_TRUE(clipboard.writes.empty());

  clipboard.fail_writes = true;
  const auto reply = clipboard_sync::handle_post(clipboard, "text/plain", "text", {});
  EXPECT_EQ(reply.status, 500);
  EXPECT_EQ(reply.sequence, std::nullopt);
  EXPECT_EQ(clipboard.writes.size(), 1);
}

TEST_F(ClipboardFilesTest, GetListsCopiedFilesAheadOfText) {
  fake_clipboard_t clipboard;
  clipboard.contents = clipboard_sync::item_t {std::string {clipboard_sync::text_mime_type}, "report.txt"};
  clipboard.copied_files = {make_file("source/report.txt", "12345"), root / "source", make_file("source/photo.png", "xy")};

  const auto reply = clipboard_sync::handle_get(clipboard, std::nullopt);
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(reply.content_type, clipboard_sync::files_mime_type);
  EXPECT_EQ(nlohmann::json::parse(reply.body), nlohmann::json::parse(R"([
    {"index": 0, "name": "report.txt", "size": 5},
    {"index": 1, "name": "photo.png", "size": 2}
  ])"));

  auto lookup = clipboard_sync::find_file(clipboard, reply.sequence, 1);
  EXPECT_EQ(lookup.status, 200);
  EXPECT_EQ(lookup.path, root / "source" / "photo.png");

  EXPECT_EQ(clipboard_sync::find_file(clipboard, reply.sequence, 2).status, 404);
  EXPECT_EQ(clipboard_sync::find_file(clipboard, reply.sequence, -1).status, 400);
  EXPECT_EQ(clipboard_sync::find_file(clipboard, std::nullopt, 0).status, 400);
  EXPECT_EQ(clipboard_sync::find_file(clipboard, *reply.sequence - 1, 0).status, 409);
}

TEST_F(ClipboardFilesTest, GetSkipsFileListsThatAreTooLarge) {
  fake_clipboard_t clipboard;
  const auto big = make_file("source/big.bin", "");
  std::filesystem::resize_file(big, clipboard_sync::max_transfer_size + 1);
  clipboard.copied_files = {big};

  EXPECT_EQ(clipboard_sync::handle_get(clipboard, std::nullopt).status, 204);
}

TEST_F(ClipboardFilesTest, UploadsAreSavedWithoutOverwritingOrEscaping) {
  auto reply = clipboard_sync::save_upload(transfers, "notes.txt", "first");
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(std::filesystem::path {reply.body}, transfers / "notes.txt");

  reply = clipboard_sync::save_upload(transfers, "C:\\Users\\me\\notes.txt", "second");
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(std::filesystem::path {reply.body}, transfers / "notes (2).txt");

  reply = clipboard_sync::save_upload(transfers, "../../escape.txt", "third");
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(std::filesystem::path {reply.body}, transfers / "escape.txt");

  EXPECT_EQ(clipboard_sync::save_upload(transfers, "", "x").status, 400);
  EXPECT_EQ(clipboard_sync::save_upload(transfers, "folder/..", "x").status, 400);

  std::ifstream saved {transfers / "notes (2).txt", std::ios::binary};
  EXPECT_EQ(std::string(std::istreambuf_iterator<char> {saved}, {}), "second");
}

TEST_F(ClipboardFilesTest, PostPutsOnlyUploadedFilesOnTheClipboard) {
  fake_clipboard_t clipboard;
  const auto uploaded = clipboard_sync::save_upload(transfers, "a.txt", "a");
  ASSERT_EQ(uploaded.status, 200);

  const nlohmann::json list = {uploaded.body};
  auto reply = clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, list.dump(), transfers);
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.sequence, 8);
  ASSERT_EQ(clipboard.copied_files.size(), 1);
  EXPECT_EQ(clipboard.copied_files.front(), transfers / "a.txt");

  const auto outside = make_file("source/secret.txt", "s");
  const nlohmann::json escaping = {uploaded.body, outside.string()};
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, escaping.dump(), transfers).status, 403);
  const nlohmann::json missing = {(transfers / "missing.txt").string()};
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, missing.dump(), transfers).status, 403);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, "[]", transfers).status, 403);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, "not json", transfers).status, 400);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, list.dump(), {}).status, 403);

  clipboard.fail_writes = true;
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, clipboard_sync::files_mime_type, list.dump(), transfers).status, 500);
}
