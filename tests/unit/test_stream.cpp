/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include <src/network.h>
#include <src/rtsp.h>
#include <src/stream.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  bool test_receive_ping_endpoints(bool session_id_pings);
}  // namespace stream

TEST(StreamPingTests, KeepsLegacyAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(false)) << "Attempt " << attempt;
  }
}

TEST(StreamPingTests, KeepsSessionIdAudioAndVideoSenderEndpointsSeparate) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    ASSERT_TRUE(stream::test_receive_ping_endpoints(true)) << "Attempt " << attempt;
  }
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, b1.size() + b2.size() + 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  std::array<char, 2> b1 = {'a', 'b'};
  std::array<char, 3> b2 = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1.data(), b1.size()}, std::string_view {b2.data(), b2.size()});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}

using namespace std::chrono_literals;

TEST(SendPacerTests, LetsAFullBurstOutAtOnce) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  EXPECT_EQ(pacer.reserve(60, start), 0ns);
  EXPECT_EQ(pacer.reserve(40, start), 0ns);
}

TEST(SendPacerTests, MakesPacketsBeyondTheBurstWait) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  EXPECT_EQ(pacer.reserve(150, start), 5ms);
}

TEST(SendPacerTests, CountsWaitedForPacketsAsSent) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  ASSERT_EQ(pacer.reserve(150, start), 5ms);
  EXPECT_EQ(pacer.reserve(10, start + 5ms), 1ms);
}

TEST(SendPacerTests, RefillsAtTheSustainedRate) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  ASSERT_EQ(pacer.reserve(100, start), 0ns);
  EXPECT_EQ(pacer.reserve(50, start + 5ms), 0ns);
  EXPECT_EQ(pacer.reserve(10, start + 5ms), 1ms);
}

TEST(SendPacerTests, HoldsNoMoreThanTheBurstWhenIdle) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  ASSERT_EQ(pacer.reserve(1, start), 0ns);
  EXPECT_EQ(pacer.reserve(150, start + 1h), 5ms);
}

TEST(SendPacerTests, IgnoresAnEarlierTime) {
  stream::send_pacer_t pacer;
  pacer.configure(10, 100);
  const auto start = std::chrono::steady_clock::now();

  ASSERT_EQ(pacer.reserve(100, start), 0ns);
  EXPECT_EQ(pacer.reserve(10, start - 1ms), 1ms);
}

TEST(SendPacerTests, ZeroRateTurnsPacingOff) {
  stream::send_pacer_t pacer;
  const auto start = std::chrono::steady_clock::now();

  EXPECT_EQ(pacer.reserve(100000, start), 0ns);
  pacer.configure(0, 0);
  EXPECT_EQ(pacer.reserve(100000, start), 0ns);
}

TEST(SustainedSendRateTests, AllowsTwiceTheBitrate) {
  // 300 Mbps in 1408-byte packets
  EXPECT_NEAR(stream::sustained_send_rate(150000, 1408), 26.63, 0.01);
}

TEST(SustainedSendRateTests, NeverGoesBelow100Mbps) {
  EXPECT_NEAR(stream::sustained_send_rate(10000, 1408), 8.88, 0.01);
}

TEST(SustainedSendRateTests, NeverGoesAboveTheOldFixedRate) {
  EXPECT_NEAR(stream::sustained_send_rate(500000, 1408), 71.02, 0.01);
}

TEST(PlanFecTests, KeepsTheConfiguredPercentageWhenTheFrameFits) {
  const auto small = stream::plan_fec(100 * 1408, 1408, 20, 2);
  EXPECT_EQ(small.blocks, 1U);
  EXPECT_EQ(small.percentage, 20);

  const auto large = stream::plan_fec(800 * 1408, 1408, 20, 2);
  EXPECT_EQ(large.blocks, 4U);
  EXPECT_EQ(large.percentage, 20);
}

TEST(PlanFecTests, KeepsWhatParityFitsForAFrameTooLargeForThePercentage) {
  // 225 data shards per block leave room for 30 parity shards
  const auto plan = stream::plan_fec(900 * 1408, 1408, 20, 2);
  EXPECT_EQ(plan.blocks, 4U);
  EXPECT_EQ(plan.percentage, 13);
}

TEST(PlanFecTests, DropsFecWhenNoParityFits) {
  const auto plan = stream::plan_fec(1700 * 1408, 1408, 20, 2);
  EXPECT_EQ(plan.blocks, 4U);
  EXPECT_EQ(plan.percentage, 0);
}

TEST(PlanFecTests, DropsFecWhenTheClientsMinimumParityDoesNotFit) {
  // 254 data shards per block leave room for one parity shard
  const auto plan = stream::plan_fec(4 * 254 * 1408, 1408, 20, 2);
  EXPECT_EQ(plan.blocks, 4U);
  EXPECT_EQ(plan.percentage, 0);
}

TEST(PlanFecTests, PlannedParityAlwaysFitsInABlock) {
  for (std::size_t size = 800 * 1408; size < 1100 * 1408; size += 997) {
    const auto plan = stream::plan_fec(size, 1408, 20, 2);
    ASSERT_LE(plan.blocks, 4U);
    if (plan.percentage == 0) {
      continue;
    }

    const auto aligned = ((size / plan.blocks + 1407) / 1408) * 1408;
    const auto largest = std::max(aligned / 1408, (size - (plan.blocks - 1) * aligned + 1407) / 1408);
    const auto parity = std::max<std::size_t>(2, (largest * plan.percentage + 99) / 100);
    EXPECT_LE(largest + parity, 255U) << size;
  }
}
