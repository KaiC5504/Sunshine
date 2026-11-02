/**
 * @file tests/unit/test_clipboard_sync.cpp
 * @brief Test the platform-independent clipboard sync request handling.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <optional>
#include <string>
#include <vector>

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
      ++changes;
      return true;
    }

    std::int64_t changes = 7;  ///< Current change count.
    std::optional<clipboard_sync::item_t> contents;  ///< Current clipboard item.
    std::vector<clipboard_sync::item_t> writes;  ///< Every item the handlers tried to write.
    bool fail_writes = false;  ///< Whether writes should fail.
  };
}  // namespace

TEST(ClipboardSyncTest, NormalizesSupportedContentTypes) {
  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/plain"), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type(" Text/Plain ; charset=UTF-8"), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("text/plain; format=flowed; charset=\"utf-8\""), clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard_sync::normalize_mime_type("image/png"), clipboard_sync::png_mime_type);
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

  auto reply = clipboard_sync::handle_post(clipboard, "text/plain; charset=utf-8", "copied");
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.sequence, 8);
  ASSERT_TRUE(clipboard.contents);
  EXPECT_EQ(clipboard.contents->mime_type, clipboard_sync::text_mime_type);
  EXPECT_EQ(clipboard.contents->data, "copied");

  // The client skips the item it just sent when it next polls with the reported sequence.
  EXPECT_EQ(clipboard_sync::handle_get(clipboard, reply.sequence).status, 204);

  reply = clipboard_sync::handle_post(clipboard, "image/png", "png bytes");
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(clipboard.contents->mime_type, clipboard_sync::png_mime_type);
}

TEST(ClipboardSyncTest, PostRejectsInvalidRequestsWithoutWriting) {
  fake_clipboard_t clipboard;

  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "text/html", "<b>no</b>").status, 415);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "", "text").status, 415);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "text/plain", "").status, 400);
  EXPECT_EQ(clipboard_sync::handle_post(clipboard, "image/png", std::string(clipboard_sync::max_item_size + 1, 'x')).status, 413);
  EXPECT_TRUE(clipboard.writes.empty());

  clipboard.fail_writes = true;
  const auto reply = clipboard_sync::handle_post(clipboard, "text/plain", "text");
  EXPECT_EQ(reply.status, 500);
  EXPECT_EQ(reply.sequence, std::nullopt);
  EXPECT_EQ(clipboard.writes.size(), 1);
}
