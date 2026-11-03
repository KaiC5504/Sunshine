/**
 * @file tests/unit/test_cursor_sync.cpp
 * @brief Test the platform-independent pointer sync.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// local includes
#include <src/cursor_sync.h>

using namespace std::chrono_literals;

namespace {
  /**
   * @brief Pointer whose image and visibility the test sets.
   */
  class fake_cursor_t final: public cursor_sync::backend_t {
  public:
    std::uint64_t image_seed() override {
      return seed;
    }

    bool visible() override {
      return shown;
    }

    std::optional<cursor_sync::image_t> image(int stream_width) override {
      ++renders;
      return cursor_sync::image_t {"png@" + std::to_string(stream_width) + "#" + std::to_string(seed), 2, 3};
    }

    std::atomic<std::uint64_t> seed {1};  ///< Image seed reported to the hub.
    std::atomic<bool> shown {true};  ///< Visibility reported to the hub.
    std::atomic<int> renders {0};  ///< Number of image() calls.
  };

  /**
   * @brief Hub over a fake pointer, with the replies it sent.
   */
  struct hub_fixture_t {
    hub_fixture_t():
        cursor(new fake_cursor_t),
        hub(std::unique_ptr<cursor_sync::backend_t>(cursor)) {
    }

    /**
     * @brief Poll the hub, recording the reply.
     *
     * @param since Sequence number the client last saw.
     * @param local Whether the client draws the pointer.
     * @param width Stream width.
     */
    void poll(std::optional<std::int64_t> since, bool local = true, int width = 2560) {
      const auto record = [this](const cursor_sync::reply_t &reply) {
        replies.push_back(reply);
      };
      hub.poll(since, width, local, record, start);
    }

    fake_cursor_t *cursor;  ///< Pointer owned by `hub`.
    cursor_sync::hub_t hub;  ///< Hub under test.
    std::vector<cursor_sync::reply_t> replies;  ///< Replies sent so far.
    cursor_sync::hub_t::clock::time_point start = cursor_sync::hub_t::clock::now();  ///< Time of the first poll.
  };
}  // namespace

TEST(CursorSyncTest, FirstPollGetsThePointerAtOnce) {
  hub_fixture_t fixture;
  fixture.poll(std::nullopt);

  ASSERT_EQ(fixture.replies.size(), 1U);
  const auto &reply = fixture.replies[0];
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.sequence, 1);
  EXPECT_TRUE(reply.visible);
  EXPECT_EQ(reply.png, "png@2560#1");
  EXPECT_EQ(reply.hotspot_x, 2);
  EXPECT_EQ(reply.hotspot_y, 3);
}

TEST(CursorSyncTest, PollWaitsUntilThePointerChanges) {
  hub_fixture_t fixture;
  fixture.poll(1);
  fixture.hub.update(fixture.start + 1ms);
  EXPECT_TRUE(fixture.replies.empty());

  fixture.cursor->seed = 7;
  fixture.hub.update(fixture.start + 2ms);
  ASSERT_EQ(fixture.replies.size(), 1U);
  EXPECT_EQ(fixture.replies[0].status, 200);
  EXPECT_EQ(fixture.replies[0].sequence, 2);
  EXPECT_EQ(fixture.replies[0].png, "png@2560#7");
}

TEST(CursorSyncTest, PollWithAnOldSequenceGetsThePointerAtOnce) {
  hub_fixture_t fixture;
  fixture.poll(std::nullopt);
  fixture.cursor->seed = 7;
  fixture.poll(1);

  ASSERT_EQ(fixture.replies.size(), 2U);
  EXPECT_EQ(fixture.replies[1].sequence, 2);
  EXPECT_EQ(fixture.replies[1].png, "png@2560#7");
}

TEST(CursorSyncTest, WaitingPollTimesOutWithNoContent) {
  hub_fixture_t fixture;
  fixture.poll(1);
  fixture.hub.update(fixture.start + cursor_sync::poll_timeout - 1ms);
  EXPECT_TRUE(fixture.replies.empty());

  fixture.hub.update(fixture.start + cursor_sync::poll_timeout);
  ASSERT_EQ(fixture.replies.size(), 1U);
  EXPECT_EQ(fixture.replies[0].status, 204);
  EXPECT_EQ(fixture.replies[0].sequence, 1);
  EXPECT_TRUE(fixture.replies[0].png.empty());
}

TEST(CursorSyncTest, HiddenPointerIsReportedWithoutAnImage) {
  hub_fixture_t fixture;
  fixture.poll(1);
  fixture.cursor->shown = false;
  fixture.hub.update(fixture.start + 1ms);

  ASSERT_EQ(fixture.replies.size(), 1U);
  EXPECT_EQ(fixture.replies[0].sequence, 2);
  EXPECT_FALSE(fixture.replies[0].visible);
  EXPECT_TRUE(fixture.replies[0].png.empty());
}

TEST(CursorSyncTest, RendersOncePerChangeAndStreamWidth) {
  hub_fixture_t fixture;
  fixture.poll(std::nullopt);
  fixture.poll(std::nullopt);
  EXPECT_EQ(fixture.cursor->renders, 1);

  fixture.poll(std::nullopt, true, 1920);
  EXPECT_EQ(fixture.cursor->renders, 2);
  EXPECT_EQ(fixture.replies.back().png, "png@1920#1");

  fixture.cursor->seed = 2;
  fixture.poll(std::nullopt);
  EXPECT_EQ(fixture.cursor->renders, 3);
}

TEST(CursorSyncTest, LocalPollsLeasePointerDrawingToTheClient) {
  hub_fixture_t fixture;
  EXPECT_FALSE(fixture.hub.client_draws_pointer(fixture.start));

  fixture.poll(std::nullopt);
  EXPECT_TRUE(fixture.hub.client_draws_pointer(fixture.start));
  EXPECT_TRUE(fixture.hub.client_draws_pointer(fixture.start + cursor_sync::lease - 1ms));
  EXPECT_FALSE(fixture.hub.client_draws_pointer(fixture.start + cursor_sync::lease));
}

TEST(CursorSyncTest, NonLocalPollEndsTheLeaseAndIsAnsweredAtOnce) {
  hub_fixture_t fixture;
  fixture.poll(std::nullopt);
  fixture.poll(1, false);

  ASSERT_EQ(fixture.replies.size(), 2U);
  EXPECT_EQ(fixture.replies[1].sequence, 1);
  EXPECT_FALSE(fixture.hub.client_draws_pointer(fixture.start));
}

TEST(CursorSyncTest, DestroyingTheHubAnswersWaitingPolls) {
  std::vector<cursor_sync::reply_t> replies;
  {
    cursor_sync::hub_t hub {std::make_unique<fake_cursor_t>()};
    const auto record = [&replies](const cursor_sync::reply_t &reply) {
      replies.push_back(reply);
    };
    hub.poll(1, 2560, true, record, cursor_sync::hub_t::clock::now());
    EXPECT_TRUE(replies.empty());
  }

  ASSERT_EQ(replies.size(), 1U);
  EXPECT_EQ(replies[0].status, 204);
}

TEST(CursorSyncTest, WatcherAnswersWhenThePointerChanges) {
  auto *cursor = new fake_cursor_t;
  cursor_sync::hub_t hub {std::unique_ptr<cursor_sync::backend_t>(cursor)};
  hub.start_watching();

  std::promise<cursor_sync::reply_t> answered;
  auto reply = answered.get_future();
  const auto answer = [&answered](const cursor_sync::reply_t &sent) {
    answered.set_value(sent);
  };
  hub.poll(1, 2560, true, answer, cursor_sync::hub_t::clock::now());

  cursor->seed = 9;
  ASSERT_EQ(reply.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(reply.get().png, "png@2560#9");
}

TEST(CursorSyncTest, ParsesNumbers) {
  EXPECT_EQ(cursor_sync::parse_number("12"), 12);
  EXPECT_EQ(cursor_sync::parse_number("-3"), -3);
  EXPECT_EQ(cursor_sync::parse_number(""), std::nullopt);
  EXPECT_EQ(cursor_sync::parse_number("1x"), std::nullopt);
  EXPECT_EQ(cursor_sync::parse_number("x"), std::nullopt);
}
