/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;  ///< GameStream base-port offset used for the video UDP stream.
  constexpr auto CONTROL_PORT = 10;  ///< GameStream base-port offset used for the control channel.
  constexpr auto AUDIO_STREAM_PORT = 11;  ///< GameStream base-port offset used for the audio UDP stream.

  struct session_t;

  /**
   * @brief Stream configuration shared by capture and network senders.
   */
  struct config_t {
    audio::config_t audio;  ///< Audio capture configuration for the stream.
    video::config_t monitor;  ///< Video capture and encoder configuration for the selected monitor.

    int packetsize;  ///< Maximum payload size for network packets.
    int minRequiredFecPackets;  ///< Minimum recovery packets required before FEC is emitted.
    int mlFeatureFlags;  ///< Moonlight feature flags negotiated for this session.
    int controlProtocolType;  ///< GameStream control protocol variant selected by the client.
    int audioQosType;  ///< Audio QoS type.
    int videoQosType;  ///< Video QoS type.

    uint32_t encryptionFlagsEnabled;  ///< Bitmask of GameStream encryption features enabled for the session.

    std::optional<int> gcmap;  ///< Optional game-controller mapping override from the launch request.
  };

  /**
   * @brief Token bucket that caps the sustained video send rate.
   *
   * Normal frames fit in the bucket and go out at once. Only the part of a large keyframe beyond
   * the bucket waits for tokens, so it can't overflow the queue of a slower link behind the host.
   */
  class send_pacer_t {
  public:
    /**
     * @brief Set the sustained rate and the bucket size.
     *
     * @param packets_per_ms Packets per millisecond the bucket refills with; 0 turns pacing off.
     * @param burst_packets Packets that may be sent back to back once the bucket is full.
     */
    void configure(double packets_per_ms, double burst_packets);

    /**
     * @brief Take tokens for packets that are about to be sent.
     *
     * @param packets Number of packets.
     * @param now Current time.
     * @return How long to wait before sending them.
     */
    std::chrono::nanoseconds reserve(std::size_t packets, std::chrono::steady_clock::time_point now);

  private:
    double rate = 0;  ///< Packets per millisecond the bucket refills with.
    double burst = 0;  ///< Bucket capacity in packets.
    double tokens = 0;  ///< Packets that may be sent now; negative while sends are owed.
    std::optional<std::chrono::steady_clock::time_point> last_refill;  ///< When tokens were last added.
  };

  /**
   * @brief Sustained video send rate for a stream.
   *
   * @param bitrate_kbps Video bitrate of the stream.
   * @param blocksize Bytes per packet.
   * @return Packets per millisecond.
   */
  double sustained_send_rate(int bitrate_kbps, std::size_t blocksize);

  /**
   * @brief How a frame is split into FEC blocks.
   */
  struct fec_plan_t {
    std::size_t blocks;  ///< FEC blocks the frame is split into, from 1 to 4.
    int percentage;  ///< FEC percentage for every block of the frame.
  };

  /**
   * @brief Plan the FEC blocks for a frame.
   *
   * A frame too large for the configured percentage keeps whatever parity still fits the
   * protocol's limit of 4 blocks of 255 shards, instead of losing FEC altogether.
   *
   * @param payload_size Size of the frame including packet headers, in bytes.
   * @param blocksize Bytes per packet.
   * @param fec_percentage Configured FEC percentage.
   * @param min_parity_shards Parity shards the client requires whenever FEC is on.
   * @return Blocks and FEC percentage for the frame.
   */
  fec_plan_t plan_fec(std::size_t payload_size, std::size_t blocksize, int fec_percentage, int min_parity_shards);

  namespace session {
    /**
     * @brief Enumerates supported state options.
     */
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    /**
     * @brief Allocate and initialize platform input state for a stream.
     *
     * @param config Configuration values to apply.
     * @param launch_session Launch session.
     * @return Allocated object or identifier, or an error value on failure.
     */
    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    /**
     * @brief Start a streaming session for the supplied peer address.
     *
     * @param session Active streaming or pairing session for the request.
     * @param addr_string Addr string.
     * @return Start status.
     */
    int start(session_t &session, const std::string &addr_string);
    /**
     * @brief Stop a streaming session and prevent more packets from being queued.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void stop(session_t &session);
    /**
     * @brief Wait for worker threads owned by the session to exit.
     *
     * @param session Active streaming or pairing session for the request.
     */
    void join(session_t &session);
    /**
     * @brief Platform handle returned from stream setup.
     *
     * @param session Active streaming or pairing session for the request.
     * @return Current lifecycle state for the stream session.
     */
    state_e state(session_t &session);
    /**
     * @brief Return the paired client certificate for a stream session.
     *
     * @param session Active streaming or pairing session for the request.
     * @return PEM certificate associated with the session's client.
     */
    const std::string &client_cert(session_t &session);
  }  // namespace session
}  // namespace stream
