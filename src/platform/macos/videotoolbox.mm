/**
 * @file src/platform/macos/videotoolbox.mm
 * @brief Definitions for the native VideoToolbox encoder on macOS.
 *
 * Adapted from the native encoder in LizardByte/Sunshine#5817.
 */
// standard includes
#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

// local includes
#include "src/config.h"
#include "src/platform/macos/av_img_t.h"
#include "src/platform/macos/coreaudio_helpers.h"
#include "src/utility.h"

// Avoid conflict between AVFoundation and libavutil both defining AVMediaType
/**
 * @def AVMediaType
 * @brief Macro for AV media type.
 */
#define AVMediaType AVMediaType_FFmpeg
#include "src/platform/macos/videotoolbox.h"
#undef AVMediaType

using namespace std::literals;

namespace {
  constexpr uint8_t annex_b_start_code[] = {0x00, 0x00, 0x00, 0x01};

  /**
   * @brief Read a codec's parameter-set count and NAL length-prefix size, optionally appending the sets.
   *
   * @param format Format description of the encoded stream.
   * @param out When non-null, receives each parameter set prefixed with a start code.
   * @param nal_length_size Receives the size of the NAL length prefixes in the samples.
   * @return True on success.
   */
  bool read_parameter_sets(CMFormatDescriptionRef format, std::vector<uint8_t> *out, int *nal_length_size) {
    const auto codec = CMFormatDescriptionGetMediaSubType(format);
    const bool h264 = codec == kCMVideoCodecType_H264;
    if (!h264 && codec != kCMVideoCodecType_HEVC) {
      return false;
    }

    auto get_parameter_set = [format, h264](size_t index, const uint8_t **data, size_t *size, size_t *count, int *length_size) {
      return h264 ?
               CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, index, data, size, count, length_size) :
               CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, index, data, size, count, length_size);
    };

    size_t count = 0;
    if (get_parameter_set(0, nullptr, nullptr, &count, nal_length_size) != noErr) {
      return false;
    }

    if (!out) {
      return true;
    }

    for (size_t i = 0; i < count; ++i) {
      const uint8_t *data = nullptr;
      size_t size = 0;
      if (get_parameter_set(i, &data, &size, nullptr, nullptr) != noErr || !data) {
        return false;
      }
      out->insert(out->end(), std::begin(annex_b_start_code), std::end(annex_b_start_code));
      out->insert(out->end(), data, data + size);
    }

    return true;
  }

  /**
   * @brief How an encoder option maps onto a VideoToolbox session property.
   */
  enum class property_kind_e {
    boolean,  ///< CFBoolean property; nonzero option values mean true
    number,  ///< CFNumber property
    entropy  ///< H.264 entropy mode taken from the vt_coder setting
  };

  /**
   * @brief VideoToolbox session property targeted by an encoder option.
   */
  struct property_t {
    CFStringRef key;  ///< VideoToolbox property key.
    property_kind_e kind;  ///< Value type of the property.
  };

  const std::unordered_map<std::string, property_t> &session_properties() {
    static const std::unordered_map<std::string, property_t> properties {
      {"AllowFrameReordering", {kVTCompressionPropertyKey_AllowFrameReordering, property_kind_e::boolean}},
      {"AllowOpenGOP", {kVTCompressionPropertyKey_AllowOpenGOP, property_kind_e::boolean}},
      {"AllowTemporalCompression", {kVTCompressionPropertyKey_AllowTemporalCompression, property_kind_e::boolean}},
      {"H264EntropyMode", {kVTCompressionPropertyKey_H264EntropyMode, property_kind_e::entropy}},
      {"MaxKeyFrameInterval", {kVTCompressionPropertyKey_MaxKeyFrameInterval, property_kind_e::number}},
      {"MaxKeyFrameIntervalDuration", {kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, property_kind_e::number}},
      {"PrioritizeEncodingSpeedOverQuality", {kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, property_kind_e::boolean}},
      {"RealTime", {kVTCompressionPropertyKey_RealTime, property_kind_e::boolean}},
      {"ReferenceBufferCount", {kVTCompressionPropertyKey_ReferenceBufferCount, property_kind_e::number}},
    };
    return properties;
  }

  /**
   * @brief Resolve an encoder option to an integer, if it has an integer form.
   *
   * @param option Encoder option from the codec table.
   * @return Integer value, or nullopt for string-valued or unset options.
   */
  std::optional<int> option_int_value(const video::encoder_t::option_t &option) {
    return std::visit(
      util::overloaded {
        [](int value) -> std::optional<int> {
          return value;
        },
        [](int *value) -> std::optional<int> {
          return *value;
        },
        [](std::optional<int> *value) -> std::optional<int> {
          return *value;
        },
        [](const std::function<int()> &value) -> std::optional<int> {
          return value();
        },
        [](const auto &) -> std::optional<int> {
          return std::nullopt;
        }
      },
      option.value
    );
  }

  std::string to_string(CFStringRef string) {
    if (const char *direct = CFStringGetCStringPtr(string, kCFStringEncodingUTF8)) {
      return direct;
    }

    char buffer[256];
    return CFStringGetCString(string, buffer, sizeof(buffer), kCFStringEncodingUTF8) ? buffer : std::string {};
  }

  bool set_property(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value) {
    const auto status = VTSessionSetProperty(session, key, value);
    if (status != noErr) {
      BOOST_LOG(debug) << "VideoToolbox: property "sv << to_string(key) << " not applied: "sv << ca::Status(status);
    }
    return status == noErr;
  }

  bool set_number_property(VTCompressionSessionRef session, CFStringRef key, int64_t value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    const bool applied = set_property(session, key, number);
    CFRelease(number);
    return applied;
  }

  bool set_number_property(VTCompressionSessionRef session, CFStringRef key, double value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    const bool applied = set_property(session, key, number);
    CFRelease(number);
    return applied;
  }
}  // namespace

namespace platf {
  OSType videotoolbox_pixel_format(pix_fmt_e pix_fmt) {
    switch (pix_fmt) {
      case pix_fmt_e::nv12:
        return kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
      case pix_fmt_e::p010:
        return kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
      case pix_fmt_e::yuv444p:
        // VideoToolbox converts RGB to 4:4:4 itself; capture has no 4:4:4 YCbCr format.
        return kCVPixelFormatType_32BGRA;
      default:
        return 0;
    }
  }

  videotoolbox_encode_device_t::~videotoolbox_encode_device_t() {
    if (current) {
      CVPixelBufferRelease(current);
    }
  }

  int videotoolbox_encode_device_t::init(pix_fmt_e pix_fmt, resolution_fn_t resolution_fn, const pixel_format_fn_t &pixel_format_fn) {
    const auto pixel_format = videotoolbox_pixel_format(pix_fmt);
    if (!pixel_format) {
      // Unknown is how the encoder table marks a mode it doesn't offer, which every startup probes.
      BOOST_LOG(pix_fmt == pix_fmt_e::unknown ? debug : error) << "VideoToolbox: unsupported pixel format "sv << from_pix_fmt(pix_fmt);
      return -1;
    }

    if (pixel_format_fn) {
      pixel_format_fn(pixel_format);
    }
    this->resolution_fn = std::move(resolution_fn);
    return 0;
  }

  int videotoolbox_encode_device_t::convert(img_t &img) {
    const auto *av_img = static_cast<av_img_t *>(&img);
    if (!av_img->pixel_buffer || !av_img->pixel_buffer->buf) {
      return -1;
    }

    CVPixelBufferRef next = CVPixelBufferRetain(av_img->pixel_buffer->buf);
    if (current) {
      CVPixelBufferRelease(current);
    }
    current = next;
    return 0;
  }

  void videotoolbox_encode_device_t::set_resolution(int width, int height) const {
    if (resolution_fn) {
      resolution_fn(width, height);
    }
  }

  CVPixelBufferRef videotoolbox_encode_device_t::current_frame() const {
    return current;
  }
}  // namespace platf

namespace video {
  namespace vt {
    CMTime next_presentation_time(
      std::optional<std::chrono::steady_clock::time_point> capture,
      std::chrono::steady_clock::time_point epoch,
      CMTime last_pts,
      std::chrono::steady_clock::time_point last_submit,
      std::chrono::steady_clock::time_point now
    ) {
      using tick = std::chrono::duration<int64_t, std::ratio<1, timescale>>;

      CMTime pts = kCMTimeInvalid;
      if (capture) {
        pts = CMTimeMake(std::chrono::round<tick>(*capture - epoch).count(), timescale);
      }

      if (!CMTIME_IS_VALID(last_pts)) {
        return CMTIME_IS_VALID(pts) ? pts : CMTimeMake(0, timescale);
      }

      if (!CMTIME_IS_VALID(pts) || CMTimeCompare(pts, last_pts) <= 0) {
        const auto elapsed = std::max<int64_t>(std::chrono::round<tick>(now - last_submit).count(), 1);
        pts = CMTimeAdd(last_pts, CMTimeMake(elapsed, timescale));
      }

      return pts;
    }

    bool is_keyframe(CMSampleBufferRef sample) {
      CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
      if (!attachments || CFArrayGetCount(attachments) == 0) {
        return true;
      }

      auto attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
      return CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
    }

    bool to_annex_b(CMSampleBufferRef sample, bool keyframe, std::vector<uint8_t> &out) {
      out.clear();

      CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
      CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
      if (!format || !block) {
        return false;
      }

      int nal_length_size = 0;
      if (!read_parameter_sets(format, nullptr, &nal_length_size) || nal_length_size < 1 || nal_length_size > 4) {
        return false;
      }

      CMBlockBufferRef contiguous = nullptr;
      if (!CMBlockBufferIsRangeContiguous(block, 0, 0)) {
        if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, block, kCFAllocatorDefault, nullptr, 0, 0, 0, &contiguous) != noErr) {
          return false;
        }
        block = contiguous;
      }
      auto release_contiguous = util::fail_guard([contiguous]() {
        if (contiguous) {
          CFRelease(contiguous);
        }
      });

      size_t total_length = 0;
      char *data = nullptr;
      if (CMBlockBufferGetDataPointer(block, 0, nullptr, &total_length, &data) != noErr || !data) {
        return false;
      }

      out.reserve(total_length + 256);
      if (keyframe && !read_parameter_sets(format, &out, &nal_length_size)) {
        out.clear();
        return false;
      }

      const auto *bytes = reinterpret_cast<const uint8_t *>(data);
      size_t offset = 0;
      while (offset < total_length) {
        if (offset + nal_length_size > total_length) {
          out.clear();
          return false;
        }

        size_t nal_length = 0;
        for (int i = 0; i < nal_length_size; ++i) {
          nal_length = (nal_length << 8) | bytes[offset + i];
        }
        offset += nal_length_size;

        if (nal_length == 0 || offset + nal_length > total_length) {
          out.clear();
          return false;
        }

        out.insert(out.end(), std::begin(annex_b_start_code), std::end(annex_b_start_code));
        out.insert(out.end(), bytes + offset, bytes + offset + nal_length);
        offset += nal_length;
      }

      return !out.empty();
    }

    CMVideoCodecType codec_type(int video_format) {
      switch (video_format) {
        case 1:
          return kCMVideoCodecType_HEVC;
        case 2:
          return kCMVideoCodecType_AV1;
        default:
          return kCMVideoCodecType_H264;
      }
    }

    CFStringRef color_primaries(const sunshine_colorspace_t &colorspace) {
      switch (colorspace.colorspace) {
        case colorspace_e::rec601:
          return kCVImageBufferColorPrimaries_SMPTE_C;
        case colorspace_e::bt2020:
        case colorspace_e::bt2020sdr:
          return kCVImageBufferColorPrimaries_ITU_R_2020;
        default:
          return kCVImageBufferColorPrimaries_ITU_R_709_2;
      }
    }

    CFStringRef transfer_function(const sunshine_colorspace_t &colorspace) {
      switch (colorspace.colorspace) {
        case colorspace_e::bt2020:
          return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
        default:
          return kCVImageBufferTransferFunction_ITU_R_709_2;
      }
    }

    CFStringRef hevc_profile(int bit_depth, bool yuv444) {
      // The SDK has no constants for the 4:4:4 profiles, which Apple Silicon encoders accept by name.
      if (yuv444) {
        return bit_depth == 10 ? CFSTR("HEVC_Main44410_AutoLevel") : CFSTR("HEVC_Main444_AutoLevel");
      }
      return bit_depth == 10 ? kVTProfileLevel_HEVC_Main10_AutoLevel : kVTProfileLevel_HEVC_Main_AutoLevel;
    }

    CFStringRef ycbcr_matrix(const sunshine_colorspace_t &colorspace) {
      switch (colorspace.colorspace) {
        case colorspace_e::rec601:
          return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
        case colorspace_e::bt2020:
        case colorspace_e::bt2020sdr:
          return kCVImageBufferYCbCrMatrix_ITU_R_2020;
        default:
          return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
      }
    }
  }  // namespace vt

  videotoolbox_encode_session_t::videotoolbox_encode_session_t(const config_t &config, std::unique_ptr<platf::videotoolbox_encode_device_t> device):
      config(config),
      device(std::move(device)),
      capture_latency_logger(debug, "VideoToolbox capture-to-submit latency", "ms"),
      encode_latency_logger(debug, "VideoToolbox submit-to-packet latency", "ms") {
  }

  videotoolbox_encode_session_t::~videotoolbox_encode_session_t() {
    if (session) {
      // Runs every pending output callback, so no frame context outlives the session.
      VTCompressionSessionCompleteFrames(session, kCMTimeIndefinite);
      VTCompressionSessionInvalidate(session);
      CFRelease(session);
    }
  }

  bool videotoolbox_encode_session_t::create_session(bool low_latency) {
    const bool require_software = config::video.vt.vt_require_sw;
    const bool allow_software = config::video.vt.vt_allow_sw;

    CFTypeRef keys[] {
      kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
      kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
      kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
    };
    CFTypeRef values[] {
      low_latency ? kCFBooleanTrue : kCFBooleanFalse,
      require_software ? kCFBooleanFalse : kCFBooleanTrue,
      (allow_software || require_software) ? kCFBooleanFalse : kCFBooleanTrue,
    };
    CFDictionaryRef specification = CFDictionaryCreate(kCFAllocatorDefault, keys, values, std::size(keys), &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    const auto status = VTCompressionSessionCreate(
      kCFAllocatorDefault,
      config.width,
      config.height,
      vt::codec_type(config.videoFormat),
      specification,
      nullptr,
      kCFAllocatorDefault,
      &videotoolbox_encode_session_t::output_callback,
      this,
      &session
    );
    CFRelease(specification);

    if (status != noErr || !session) {
      BOOST_LOG(debug) << "VideoToolbox: session creation failed"sv << (low_latency ? " with low-latency rate control: "sv : ": "sv) << ca::Status(status);
      session = nullptr;
      return false;
    }

    this->low_latency = low_latency;
    return true;
  }

  void videotoolbox_encode_session_t::apply_options(const encoder_t::codec_t &codec) {
    for (const auto &option : codec.common_options) {
      const auto property = session_properties().find(option.name);
      if (property == session_properties().end()) {
        BOOST_LOG(error) << "VideoToolbox: unknown session property "sv << option.name;
        continue;
      }

      const auto value = option_int_value(option);
      if (!value) {
        BOOST_LOG(error) << "VideoToolbox: session property "sv << option.name << " needs an integer value"sv;
        continue;
      }

      switch (property->second.kind) {
        case property_kind_e::boolean:
          set_property(session, property->second.key, *value ? kCFBooleanTrue : kCFBooleanFalse);
          break;
        case property_kind_e::number:
          set_number_property(session, property->second.key, static_cast<int64_t>(*value));
          break;
        case property_kind_e::entropy:
          // 1 is CABAC and 2 is CAVLC in the vt_coder setting; anything else leaves VideoToolbox's choice.
          if (*value == 1) {
            set_property(session, property->second.key, kVTH264EntropyMode_CABAC);
          } else if (*value == 2) {
            set_property(session, property->second.key, kVTH264EntropyMode_CAVLC);
          }
          break;
      }
    }
  }

  bool videotoolbox_encode_session_t::init(const encoder_t::codec_t &codec) {
    if (!device) {
      return false;
    }

    const bool yuv444 = config.chromaSamplingType == 1;
    if (yuv444 && config.videoFormat != 1) {
      BOOST_LOG(debug) << "VideoToolbox: 4:4:4 is only available with HEVC"sv;
      return false;
    }

    device->set_resolution(config.width, config.height);

    // Low-latency rate control held the bitrate at 7.5 ms per frame for 1440p at 120 fps, against
    // 9-15 ms and overshoot without it, but with it Apple Silicon silently encodes 4:2:0 instead
    // of 4:4:4. It is also not available for every codec and OS version.
    if (!(!yuv444 && create_session(true)) && !create_session(false)) {
      BOOST_LOG(error) << "VideoToolbox: couldn't create a "sv << codec.name << " session"sv;
      return false;
    }

    apply_options(codec);

    const auto bitrate_kbps = config::video.max_bitrate > 0 ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate;
    set_number_property(session, kVTCompressionPropertyKey_AverageBitRate, static_cast<int64_t>(bitrate_kbps) * 1000);
    set_number_property(session, kVTCompressionPropertyKey_ExpectedFrameRate, config.framerateX100 > 0 ? config.framerateX100 / 100.0 : static_cast<double>(config.framerate));

    // Without the 4:4:4 profile the encoder quietly produces 4:2:0, so the stream must not start.
    if (config.videoFormat == 1 && !set_property(session, kVTCompressionPropertyKey_ProfileLevel, vt::hevc_profile(device->colorspace.bit_depth, yuv444)) && yuv444) {
      BOOST_LOG(error) << "VideoToolbox: this Mac can't encode HEVC 4:4:4"sv;
      return false;
    }

    // Spatial adaptive QP made 1440p 4:4:4 at 120 fps take 15 ms per frame (p95 31 ms) on an M6,
    // against 11.6 ms (p95 19 ms) without it. Low-latency rate control already turns it off.
    if (!low_latency) {
      set_number_property(session, kVTCompressionPropertyKey_SpatialAdaptiveQPLevel, static_cast<int64_t>(kVTQPModulationLevel_Disable));
    }

    set_property(session, kVTCompressionPropertyKey_ColorPrimaries, vt::color_primaries(device->colorspace));
    set_property(session, kVTCompressionPropertyKey_TransferFunction, vt::transfer_function(device->colorspace));
    set_property(session, kVTCompressionPropertyKey_YCbCrMatrix, vt::ycbcr_matrix(device->colorspace));

    const auto status = VTCompressionSessionPrepareToEncodeFrames(session);
    if (status != noErr) {
      BOOST_LOG(error) << "VideoToolbox: couldn't prepare the "sv << codec.name << " session: "sv << ca::Status(status);
      return false;
    }

    BOOST_LOG(info) << "VideoToolbox: native "sv << codec.name << " session ready at "sv << config.width << 'x' << config.height
                    << (yuv444 ? " 4:4:4"sv : " 4:2:0"sv) << ", low-latency rate control "sv << (low_latency ? "on"sv : "off"sv);
    return true;
  }

  int videotoolbox_encode_session_t::convert(platf::img_t &img) {
    return device ? device->convert(img) : -1;
  }

  void videotoolbox_encode_session_t::request_idr_frame() {
    force_idr = true;
  }

  void videotoolbox_encode_session_t::request_normal_frame() {
    force_idr = false;
  }

  void videotoolbox_encode_session_t::invalidate_ref_frames(int64_t, int64_t) {
    force_idr = true;
  }

  bool videotoolbox_encode_session_t::emits_packets_asynchronously() const {
    return true;
  }

  bool videotoolbox_encode_session_t::low_latency_rate_control() const {
    return low_latency;
  }

  int videotoolbox_encode_session_t::encode_frame(int64_t frame_nr, const safe::mail_raw_t::queue_t<packet_t> &packets, stream::session_t *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (!session || !device || !device->current_frame()) {
      return -1;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!epoch) {
      epoch = frame_timestamp.value_or(now);
    }
    const auto pts = vt::next_presentation_time(frame_timestamp, *epoch, last_pts, last_submit, now);
    last_pts = pts;
    last_submit = now;

    CFDictionaryRef frame_properties = nullptr;
    if (force_idr.exchange(false)) {
      const void *keys[] {kVTEncodeFrameOptionKey_ForceKeyFrame};
      const void *values[] {kCFBooleanTrue};
      frame_properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }

    // The output callback owns the context once VideoToolbox accepts the frame, and may run
    // before VTCompressionSessionEncodeFrame returns.
    auto *frame = new frame_context_t {packets, channel_data, frame_nr, frame_timestamp, now};
    const auto status = VTCompressionSessionEncodeFrame(session, device->current_frame(), pts, kCMTimeInvalid, frame_properties, frame, nullptr);
    if (frame_properties) {
      CFRelease(frame_properties);
    }

    if (status != noErr) {
      delete frame;
      BOOST_LOG(error) << "VideoToolbox: couldn't submit frame "sv << frame_nr << ": "sv << ca::Status(status);
      return -1;
    }

    return 0;
  }

  void videotoolbox_encode_session_t::output_callback(void *output_ref, void *frame_ref, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    std::unique_ptr<frame_context_t> frame {static_cast<frame_context_t *>(frame_ref)};
    if (output_ref && frame) {
      static_cast<videotoolbox_encode_session_t *>(output_ref)->handle_output(std::move(frame), status, flags, sample);
    }
  }

  void videotoolbox_encode_session_t::handle_output(std::unique_ptr<frame_context_t> frame, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    if (status != noErr) {
      BOOST_LOG(error) << "VideoToolbox: encoding frame "sv << frame->frame_nr << " failed: "sv << ca::Status(status);
      return;
    }

    if (flags & kVTEncodeInfo_FrameDropped) {
      BOOST_LOG(warning) << "VideoToolbox: encoder dropped frame "sv << frame->frame_nr;
      return;
    }

    if (!sample || !CMSampleBufferDataIsReady(sample)) {
      BOOST_LOG(error) << "VideoToolbox: frame "sv << frame->frame_nr << " has no encoded data"sv;
      return;
    }

    const bool keyframe = vt::is_keyframe(sample);
    std::vector<uint8_t> annex_b;
    if (!vt::to_annex_b(sample, keyframe, annex_b)) {
      BOOST_LOG(error) << "VideoToolbox: couldn't package frame "sv << frame->frame_nr;
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::scoped_lock lock {stats_mutex};
      if (frame->capture_time) {
        capture_latency_logger.collect_and_log(std::chrono::duration<double, std::milli>(frame->submit_time - *frame->capture_time).count());
      }
      encode_latency_logger.collect_and_log(std::chrono::duration<double, std::milli>(now - frame->submit_time).count());
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(annex_b), frame->frame_nr, keyframe);
    packet->channel_data = frame->channel_data;
    packet->frame_timestamp = frame->capture_time;
    frame->packets->raise(std::move(packet));
  }
}  // namespace video
