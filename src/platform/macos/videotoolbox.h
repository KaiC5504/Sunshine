/**
 * @file src/platform/macos/videotoolbox.h
 * @brief Declarations for the native VideoToolbox encoder on macOS.
 *
 * Objective-C sources that include this header must first define AVMediaType as
 * AVMediaType_FFmpeg, because AVFoundation and libavutil both declare AVMediaType.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// platform includes
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  /**
   * @brief Map a Sunshine pixel format to the CoreVideo format VideoToolbox encodes from.
   *
   * @param pix_fmt Sunshine pixel format requested for the encoder input.
   * @return CoreVideo pixel format, or 0 when VideoToolbox input does not support the format.
   */
  OSType videotoolbox_pixel_format(pix_fmt_e pix_fmt);

  /**
   * @brief Encode device that hands captured CoreVideo pixel buffers to VideoToolbox without copying.
   */
  class videotoolbox_encode_device_t: public encode_device_t {
  public:
    /**
     * @brief Callback that asks the capture backend to deliver frames at the encode resolution.
     */
    using resolution_fn_t = std::function<void(int width, int height)>;

    /**
     * @brief Callback that asks the capture backend to deliver frames in a CoreVideo pixel format.
     */
    using pixel_format_fn_t = std::function<void(OSType pixel_format)>;

    ~videotoolbox_encode_device_t() override;

    /**
     * @brief Configure the capture backend for the requested encoder input format.
     *
     * @param pix_fmt Sunshine pixel format for the encoder input.
     * @param resolution_fn Callback that resizes the capture output.
     * @param pixel_format_fn Callback that selects the capture pixel format.
     * @return 0 on success, nonzero when the pixel format is unsupported.
     */
    int init(pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn);

    /**
     * @brief Retain the pixel buffer of a captured image as the next frame to encode.
     *
     * @param img Captured image; must be an av_img_t carrying a pixel buffer.
     * @return 0 on success, nonzero when the image has no pixel buffer.
     */
    int convert(img_t &img) override;

    /**
     * @brief Ask the capture backend to deliver frames at the encode resolution.
     *
     * @param width Encoded frame width in pixels.
     * @param height Encoded frame height in pixels.
     */
    void set_resolution(int width, int height) const;

    /**
     * @brief Return the most recently converted pixel buffer.
     *
     * @return Pixel buffer retained by this device, or null before the first convert().
     */
    CVPixelBufferRef current_frame() const;

  private:
    resolution_fn_t resolution_fn;  ///< Callback that resizes the capture output.
    CVPixelBufferRef current = nullptr;  ///< Retained pixel buffer of the next frame to encode.
  };
}  // namespace platf

namespace video {
  namespace vt {
    /**
     * @brief VideoToolbox clock rate used for presentation timestamps (the RTP video clock).
     */
    inline constexpr int32_t timescale = 90000;

    /**
     * @brief Pick the presentation timestamp for the next submitted frame.
     *
     * VideoToolbox requires strictly increasing timestamps. Captured frames use their capture time
     * relative to the session epoch; repeated frames without a capture time, and captures that
     * would not move time forward, advance from the previous timestamp by the wall-clock time
     * since the previous submission.
     *
     * @param capture Capture time of the frame, when known.
     * @param epoch Session time that maps to timestamp zero.
     * @param last_pts Timestamp of the previous frame, or kCMTimeInvalid for the first frame.
     * @param last_submit Wall-clock time of the previous submission.
     * @param now Current wall-clock time.
     * @return Timestamp at the 90 kHz timescale, strictly greater than last_pts when it is valid.
     */
    CMTime next_presentation_time(
      std::optional<std::chrono::steady_clock::time_point> capture,
      std::chrono::steady_clock::time_point epoch,
      CMTime last_pts,
      std::chrono::steady_clock::time_point last_submit,
      std::chrono::steady_clock::time_point now
    );

    /**
     * @brief Report whether an encoded sample is a sync (IDR) frame.
     *
     * @param sample Encoded sample buffer produced by VideoToolbox.
     * @return True unless the sample is marked as not a sync sample.
     */
    bool is_keyframe(CMSampleBufferRef sample);

    /**
     * @brief Convert an encoded sample into an Annex B byte stream.
     *
     * Keyframes are prefixed with the parameter sets (SPS/PPS, plus VPS for HEVC).
     *
     * @param sample Encoded H.264 or HEVC sample buffer.
     * @param keyframe Whether to prepend the parameter sets.
     * @param out Receives the Annex B bytes; cleared first.
     * @return True on success; false when the sample is malformed or uses another codec.
     */
    bool to_annex_b(CMSampleBufferRef sample, bool keyframe, std::vector<uint8_t> &out);

    /**
     * @brief Map Moonlight's video format to a VideoToolbox codec type.
     *
     * @param video_format 0 for H.264, 1 for HEVC, 2 for AV1.
     * @return CoreMedia codec type.
     */
    CMVideoCodecType codec_type(int video_format);

    /**
     * @brief Map a Sunshine colorspace to CoreVideo color primaries.
     *
     * @param colorspace Colorspace negotiated for the stream.
     * @return CoreVideo color primaries constant.
     */
    CFStringRef color_primaries(const sunshine_colorspace_t &colorspace);

    /**
     * @brief Map a Sunshine colorspace to a CoreVideo transfer function.
     *
     * @param colorspace Colorspace negotiated for the stream.
     * @return CoreVideo transfer function constant.
     */
    CFStringRef transfer_function(const sunshine_colorspace_t &colorspace);

    /**
     * @brief Pick the HEVC profile for a stream's bit depth and chroma sampling.
     *
     * @param bit_depth 8 or 10.
     * @param yuv444 Whether the stream uses 4:4:4 chroma sampling.
     * @return Value for kVTCompressionPropertyKey_ProfileLevel.
     */
    CFStringRef hevc_profile(int bit_depth, bool yuv444);

    /**
     * @brief Map a Sunshine colorspace to a CoreVideo YCbCr matrix.
     *
     * @param colorspace Colorspace negotiated for the stream.
     * @return CoreVideo YCbCr matrix constant.
     */
    CFStringRef ycbcr_matrix(const sunshine_colorspace_t &colorspace);
  }  // namespace vt

  /**
   * @brief Encode session that drives a VTCompressionSession directly.
   *
   * Each frame's packet is queued from VideoToolbox's output callback as soon as the hardware
   * finishes it. FFmpeg's VideoToolbox wrapper only collects output when the next frame is
   * submitted, which delayed every frame by at least one capture interval.
   */
  class videotoolbox_encode_session_t: public encode_session_t {
  public:
    /**
     * @brief Construct a session for a negotiated stream; call init() before encoding.
     *
     * @param config Stream configuration negotiated with the client.
     * @param device Encode device that supplies captured pixel buffers.
     */
    videotoolbox_encode_session_t(const config_t &config, std::unique_ptr<platf::videotoolbox_encode_device_t> device);

    ~videotoolbox_encode_session_t() override;

    videotoolbox_encode_session_t(const videotoolbox_encode_session_t &) = delete;
    videotoolbox_encode_session_t &operator=(const videotoolbox_encode_session_t &) = delete;

    /**
     * @brief Create, configure and prepare the compression session.
     *
     * @param codec Codec entry whose common options become VideoToolbox session properties.
     * @return True on success; the session must not be used otherwise.
     */
    bool init(const encoder_t::codec_t &codec);

    /**
     * @brief Retain a captured image as the next frame to encode.
     *
     * @param img Captured image supplied by the display backend.
     * @return 0 on success, nonzero on failure.
     */
    int convert(platf::img_t &img) override;

    /**
     * @brief Force the next submitted frame to be a keyframe.
     */
    void request_idr_frame() override;

    /**
     * @brief Clear a pending keyframe request.
     */
    void request_normal_frame() override;

    /**
     * @brief Recover from lost frames with a keyframe; VideoToolbox cannot invalidate references.
     *
     * @param first_frame First lost frame.
     * @param last_frame Last lost frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override;

    /**
     * @brief Report that packets arrive from VideoToolbox's callback, after encode_frame() returns.
     *
     * @return Always true.
     */
    bool emits_packets_asynchronously() const override;

    /**
     * @brief Submit the current frame; its packet is queued from the output callback.
     *
     * @param frame_nr Frame number assigned by the video pipeline.
     * @param packets Queue that receives the encoded packet.
     * @param channel_data Stream session the packet belongs to.
     * @param frame_timestamp Capture time of the frame, or nullopt for a repeated frame.
     * @return 0 when VideoToolbox accepted the frame, nonzero on failure.
     */
    int encode_frame(int64_t frame_nr, const safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp);

    /**
     * @brief Report whether VideoToolbox's low-latency rate control is active.
     *
     * @return True when the session was created with low-latency rate control.
     */
    bool low_latency_rate_control() const;

  private:
    /**
     * @brief Per-frame state carried from submission to the output callback.
     */
    struct frame_context_t {
      safe::mail_raw_t::queue_t<packet_t> packets;  ///< Queue that receives the packet.
      stream::session_t *channel_data;  ///< Stream session the packet belongs to.
      int64_t frame_nr;  ///< Frame number assigned by the video pipeline.
      std::optional<std::chrono::steady_clock::time_point> capture_time;  ///< Capture time, when known.
      std::chrono::steady_clock::time_point submit_time;  ///< When the frame was handed to VideoToolbox.
    };

    /**
     * @brief VTCompressionSession output callback; forwards to handle_output().
     *
     * @param output_ref The session that submitted the frame.
     * @param frame_ref Frame context allocated by encode_frame(); ownership passes to the callback.
     * @param status Encoding status.
     * @param flags Encoding information flags.
     * @param sample Encoded sample, or null when the frame was dropped or failed.
     */
    static void output_callback(void *output_ref, void *frame_ref, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);

    /**
     * @brief Package an encoded sample and queue it for the stream.
     *
     * @param frame Context of the encoded frame.
     * @param status Encoding status.
     * @param flags Encoding information flags.
     * @param sample Encoded sample, or null when the frame was dropped or failed.
     */
    void handle_output(std::unique_ptr<frame_context_t> frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);

    /**
     * @brief Create the compression session for the stream's codec and size.
     *
     * @param low_latency Whether to request low-latency rate control.
     * @return True when the session was created.
     */
    bool create_session(bool low_latency);

    /**
     * @brief Apply the codec table's options as session properties.
     *
     * @param codec Codec entry whose common options name VideoToolbox properties.
     */
    void apply_options(const encoder_t::codec_t &codec);

    config_t config;  ///< Stream configuration negotiated with the client.
    std::unique_ptr<platf::videotoolbox_encode_device_t> device;  ///< Source of captured pixel buffers.
    VTCompressionSessionRef session = nullptr;  ///< Compression session, once created.
    bool low_latency = false;  ///< Whether low-latency rate control is active.
    std::atomic<bool> force_idr {false};  ///< Whether the next submitted frame must be a keyframe.

    std::optional<std::chrono::steady_clock::time_point> epoch;  ///< Time that maps to timestamp zero, set by the first frame.
    CMTime last_pts = kCMTimeInvalid;  ///< Timestamp of the previously submitted frame.
    std::chrono::steady_clock::time_point last_submit;  ///< When the previous frame was submitted.

    std::mutex stats_mutex;  ///< Serializes latency statistics across output callbacks.
    logging::min_max_avg_periodic_logger<double> capture_latency_logger;  ///< Capture-to-submit latency statistics.
    logging::min_max_avg_periodic_logger<double> encode_latency_logger;  ///< Submit-to-packet latency statistics.
  };
}  // namespace video
