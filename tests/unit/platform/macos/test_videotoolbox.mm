/**
 * @file tests/unit/platform/macos/test_videotoolbox.mm
 * @brief Unit tests for src/platform/macos/videotoolbox.*.
 */
// Only compile these tests on macOS
#ifdef __APPLE__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <chrono>
  #include <cstdint>
  #include <cstring>
  #include <memory>
  #include <optional>
  #include <utility>
  #include <vector>

  // platform includes
  #include <CoreMedia/CoreMedia.h>
  #include <CoreVideo/CoreVideo.h>

  // local includes
  #include <src/config.h>
  #include <src/platform/macos/av_img_t.h>

  // Avoid conflict between AVFoundation and libavutil both defining AVMediaType
  #define AVMediaType AVMediaType_FFmpeg
  #include <src/platform/macos/videotoolbox.h>
  #undef AVMediaType

namespace {
  using namespace std::chrono_literals;
  using steady_time = std::chrono::steady_clock::time_point;

  /**
   * @brief Wrap a blank pixel buffer in a captured image, as the capture backends do.
   *
   * @param width Image width in pixels.
   * @param height Image height in pixels.
   * @param pixel_format CoreVideo format of the image; NV12 or a packed RGB format.
   * @return Captured image holding retained sample and pixel buffers.
   */
  std::shared_ptr<platf::av_img_t> make_image(int width, int height, OSType pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
    CVPixelBufferRef pixel_buffer = nullptr;
    const void *keys[] {kCVPixelBufferIOSurfacePropertiesKey};
    const void *values[] {CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(values[0]);
    CVPixelBufferCreate(kCFAllocatorDefault, width, height, pixel_format, attributes, &pixel_buffer);
    CFRelease(attributes);

    CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    if (CVPixelBufferIsPlanar(pixel_buffer)) {
      for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(pixel_buffer); ++plane) {
        std::memset(CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, plane), plane == 0 ? 16 : 128, CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, plane) * CVPixelBufferGetHeightOfPlane(pixel_buffer, plane));
      }
    } else {
      std::memset(CVPixelBufferGetBaseAddress(pixel_buffer), 0, CVPixelBufferGetBytesPerRow(pixel_buffer) * CVPixelBufferGetHeight(pixel_buffer));
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

    CMVideoFormatDescriptionRef format = nullptr;
    CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel_buffer, &format);
    CMSampleTimingInfo timing = {kCMTimeInvalid, kCMTimeInvalid, kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixel_buffer, true, nullptr, nullptr, format, &timing, &sample);
    CFRelease(format);
    CVPixelBufferRelease(pixel_buffer);

    auto img = std::make_shared<platf::av_img_t>();
    img->sample_buffer = std::make_shared<platf::av_sample_buf_t>(sample);
    img->pixel_buffer = std::make_shared<platf::av_pixel_buf_t>(sample);
    img->width = width;
    img->height = height;
    CFRelease(sample);
    return img;
  }

  /**
   * @brief Split an Annex B stream into NAL units, without start codes.
   *
   * @param bytes Annex B byte stream using 4-byte start codes.
   * @param size Length of the byte stream.
   * @return NAL unit payloads in stream order.
   */
  std::vector<std::vector<uint8_t>> split_nal_units(const uint8_t *bytes, size_t size) {
    std::vector<std::vector<uint8_t>> units;
    size_t start = 0;
    for (size_t i = 0; i + 4 <= size; ++i) {
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
        if (i > start) {
          units.emplace_back(bytes + start, bytes + i);
        }
        start = i + 4;
        i += 3;
      }
    }
    if (start < size) {
      units.emplace_back(bytes + start, bytes + size);
    }
    return units;
  }

  /**
   * @brief Read chroma_format_idc from an HEVC SPS NAL unit.
   *
   * @param nal SPS NAL unit, including its two-byte header.
   * @return chroma_format_idc: 1 for 4:2:0, 3 for 4:4:4.
   */
  int hevc_chroma_format_idc(const std::vector<uint8_t> &nal) {
    std::vector<uint8_t> rbsp;
    for (size_t i = 0; i < nal.size(); ++i) {
      if (i >= 2 && nal[i] == 3 && nal[i - 1] == 0 && nal[i - 2] == 0) {
        continue;
      }
      rbsp.push_back(nal[i]);
    }

    size_t bit = 0;
    auto read_bit = [&]() {
      const int value = (rbsp[bit / 8] >> (7 - bit % 8)) & 1;
      ++bit;
      return value;
    };
    auto read_ue = [&]() {
      int zeros = 0;
      while (!read_bit()) {
        ++zeros;
      }
      int value = 0;
      for (int i = 0; i < zeros; ++i) {
        value = (value << 1) | read_bit();
      }
      return (1 << zeros) - 1 + value;
    };

    // NAL header and sps_video_parameter_set_id.
    bit = 16 + 4;
    int max_sub_layers_minus1 = 0;
    for (int i = 0; i < 3; ++i) {
      max_sub_layers_minus1 = (max_sub_layers_minus1 << 1) | read_bit();
    }
    // sps_temporal_id_nesting_flag, then profile_tier_level's general profile and level.
    bit += 1 + 96;

    // Low-latency rate control adds temporal sub-layers, each of which may carry its own profile and level.
    std::vector<std::pair<bool, bool>> sub_layers;
    for (int i = 0; i < max_sub_layers_minus1; ++i) {
      const bool profile_present = read_bit();
      const bool level_present = read_bit();
      sub_layers.emplace_back(profile_present, level_present);
    }
    if (max_sub_layers_minus1 > 0) {
      bit += 2 * (8 - max_sub_layers_minus1);
    }
    for (const auto &[profile_present, level_present] : sub_layers) {
      bit += (profile_present ? 88 : 0) + (level_present ? 8 : 0);
    }

    read_ue();  // sps_seq_parameter_set_id
    return read_ue();
  }

  /**
   * @brief Build an encoded H.264 sample with a single length-prefixed payload.
   *
   * @param format Format description carrying the stream's parameter sets.
   * @param payload Sample payload, including its 4-byte length prefixes.
   * @param not_sync Whether to mark the sample as not a sync sample.
   * @return Retained sample buffer.
   */
  CMSampleBufferRef make_encoded_sample(CMFormatDescriptionRef format, const std::vector<uint8_t> &payload, bool not_sync) {
    CMBlockBufferRef block = nullptr;
    CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, payload.size(), kCFAllocatorDefault, nullptr, 0, payload.size(), kCMBlockBufferAssureMemoryNowFlag, &block);
    CMBlockBufferReplaceDataBytes(payload.data(), block, 0, payload.size());

    const size_t sample_size = payload.size();
    CMSampleBufferRef sample = nullptr;
    CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1, &sample_size, &sample);
    CFRelease(block);

    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    auto attachment = (CFMutableDictionaryRef) CFArrayGetValueAtIndex(attachments, 0);
    CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync, not_sync ? kCFBooleanTrue : kCFBooleanFalse);
    return sample;
  }

  /**
   * @brief Prefix a NAL unit with a 4-byte big-endian length.
   *
   * @param nal NAL unit payload.
   * @param declared_length Length to write in the prefix.
   * @return Length-prefixed bytes.
   */
  std::vector<uint8_t> length_prefixed(const std::vector<uint8_t> &nal, uint32_t declared_length) {
    std::vector<uint8_t> bytes {
      static_cast<uint8_t>(declared_length >> 24),
      static_cast<uint8_t>(declared_length >> 16),
      static_cast<uint8_t>(declared_length >> 8),
      static_cast<uint8_t>(declared_length)
    };
    bytes.insert(bytes.end(), nal.begin(), nal.end());
    return bytes;
  }

  /**
   * @brief Stream configuration used by the encoder tests.
   *
   * @param video_format 0 for H.264, 1 for HEVC.
   * @param yuv444 Whether the stream uses 4:4:4 chroma sampling.
   * @return Configuration for a small 60 fps stream.
   */
  video::config_t test_config(int video_format, bool yuv444 = false) {
    video::config_t config {};
    config.width = 256;
    config.height = 144;
    config.framerate = 60;
    config.bitrate = 2000;
    config.videoFormat = video_format;
    config.chromaSamplingType = yuv444 ? 1 : 0;
    return config;
  }

  /**
   * @brief Fixture that lets VideoToolbox fall back to software encoding on runners without a hardware encoder.
   */
  class VideoToolboxEncodeTest: public BaseTest {
  protected:
    void SetUp() override {
      BaseTest::SetUp();
      saved_allow_sw = config::video.vt.vt_allow_sw;
      saved_require_sw = config::video.vt.vt_require_sw;
      saved_max_bitrate = config::video.max_bitrate;
      config::video.vt.vt_allow_sw = 1;
      config::video.vt.vt_require_sw = 0;
      config::video.max_bitrate = 0;
      packets = mail->queue<video::packet_t>("videotoolbox-test-packets");
    }

    void TearDown() override {
      config::video.vt.vt_allow_sw = saved_allow_sw;
      config::video.vt.vt_require_sw = saved_require_sw;
      config::video.max_bitrate = saved_max_bitrate;
      BaseTest::TearDown();
    }

    /**
     * @brief Create a session fed with a blank frame.
     *
     * @param video_format 0 for H.264, 1 for HEVC.
     * @param yuv444 Whether to encode 4:4:4 from BGRA input.
     * @return Ready session, or null when this machine cannot encode the format.
     */
    std::unique_ptr<video::videotoolbox_encode_session_t> make_session(int video_format, bool yuv444 = false) {
      const auto config = test_config(video_format, yuv444);
      auto device = std::make_unique<platf::videotoolbox_encode_device_t>();
      if (device->init(yuv444 ? platf::pix_fmt_e::yuv444p : platf::pix_fmt_e::nv12, nullptr, nullptr)) {
        return nullptr;
      }
      device->colorspace = video::colorspace_from_client_config(config, false);

      auto session = std::make_unique<video::videotoolbox_encode_session_t>(config, std::move(device));
      if (!session->init(video::videotoolbox.codec_from_config(config))) {
        return nullptr;
      }

      auto img = make_image(config.width, config.height, yuv444 ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
      if (session->convert(*img)) {
        return nullptr;
      }
      return session;
    }

    std::shared_ptr<safe::mail_raw_t> mail = std::make_shared<safe::mail_raw_t>();
    safe::mail_raw_t::queue_t<video::packet_t> packets;

  private:
    int saved_allow_sw = 0;
    int saved_require_sw = 0;
    int saved_max_bitrate = 0;
  };
}  // namespace

TEST(VideoToolboxPresentationTime, FirstFrameUsesCaptureTimeRelativeToEpoch) {
  const steady_time epoch {};
  const auto pts = video::vt::next_presentation_time(epoch + 10ms, epoch, kCMTimeInvalid, epoch, epoch + 20ms);

  EXPECT_EQ(pts.timescale, video::vt::timescale);
  EXPECT_EQ(pts.value, 900);
}

TEST(VideoToolboxPresentationTime, FirstFrameWithoutCaptureTimeStartsAtZero) {
  const steady_time epoch {};
  const auto pts = video::vt::next_presentation_time(std::nullopt, epoch, kCMTimeInvalid, epoch, epoch + 5ms);

  EXPECT_EQ(pts.value, 0);
  EXPECT_EQ(pts.timescale, video::vt::timescale);
}

TEST(VideoToolboxPresentationTime, LaterCaptureTimeIsUsedAsIs) {
  const steady_time epoch {};
  const auto last = CMTimeMake(900, video::vt::timescale);
  const auto pts = video::vt::next_presentation_time(epoch + 30ms, epoch, last, epoch + 10ms, epoch + 31ms);

  EXPECT_EQ(pts.value, 2700);
}

TEST(VideoToolboxPresentationTime, RepeatedFrameAdvancesByElapsedTime) {
  const steady_time epoch {};
  const auto last = CMTimeMake(900, video::vt::timescale);
  const auto pts = video::vt::next_presentation_time(std::nullopt, epoch, last, epoch + 10ms, epoch + 30ms);

  EXPECT_EQ(pts.value, 900 + 1800);
}

TEST(VideoToolboxPresentationTime, StaleCaptureTimeStillMovesForward) {
  const steady_time epoch {};
  const auto last = CMTimeMake(2700, video::vt::timescale);
  const auto pts = video::vt::next_presentation_time(epoch + 10ms, epoch, last, epoch + 30ms, epoch + 40ms);

  EXPECT_EQ(pts.value, 2700 + 900);
}

TEST(VideoToolboxPresentationTime, ImmediateResubmissionAdvancesByOneTick) {
  const steady_time epoch {};
  const auto last = CMTimeMake(900, video::vt::timescale);
  const auto pts = video::vt::next_presentation_time(std::nullopt, epoch, last, epoch + 10ms, epoch + 10ms);

  EXPECT_EQ(pts.value, 901);
}

TEST(VideoToolboxMappings, CodecTypes) {
  EXPECT_EQ(video::vt::codec_type(0), kCMVideoCodecType_H264);
  EXPECT_EQ(video::vt::codec_type(1), kCMVideoCodecType_HEVC);
  EXPECT_EQ(video::vt::codec_type(2), kCMVideoCodecType_AV1);
}

TEST(VideoToolboxMappings, ColorProperties) {
  video::sunshine_colorspace_t colorspace {};

  colorspace.colorspace = video::colorspace_e::rec601;
  EXPECT_TRUE(CFEqual(video::vt::color_primaries(colorspace), kCVImageBufferColorPrimaries_SMPTE_C));
  EXPECT_TRUE(CFEqual(video::vt::transfer_function(colorspace), kCVImageBufferTransferFunction_ITU_R_709_2));
  EXPECT_TRUE(CFEqual(video::vt::ycbcr_matrix(colorspace), kCVImageBufferYCbCrMatrix_ITU_R_601_4));

  colorspace.colorspace = video::colorspace_e::rec709;
  EXPECT_TRUE(CFEqual(video::vt::color_primaries(colorspace), kCVImageBufferColorPrimaries_ITU_R_709_2));
  EXPECT_TRUE(CFEqual(video::vt::transfer_function(colorspace), kCVImageBufferTransferFunction_ITU_R_709_2));
  EXPECT_TRUE(CFEqual(video::vt::ycbcr_matrix(colorspace), kCVImageBufferYCbCrMatrix_ITU_R_709_2));

  colorspace.colorspace = video::colorspace_e::bt2020sdr;
  EXPECT_TRUE(CFEqual(video::vt::color_primaries(colorspace), kCVImageBufferColorPrimaries_ITU_R_2020));
  EXPECT_TRUE(CFEqual(video::vt::transfer_function(colorspace), kCVImageBufferTransferFunction_ITU_R_709_2));
  EXPECT_TRUE(CFEqual(video::vt::ycbcr_matrix(colorspace), kCVImageBufferYCbCrMatrix_ITU_R_2020));

  colorspace.colorspace = video::colorspace_e::bt2020;
  EXPECT_TRUE(CFEqual(video::vt::color_primaries(colorspace), kCVImageBufferColorPrimaries_ITU_R_2020));
  EXPECT_TRUE(CFEqual(video::vt::transfer_function(colorspace), kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ));
  EXPECT_TRUE(CFEqual(video::vt::ycbcr_matrix(colorspace), kCVImageBufferYCbCrMatrix_ITU_R_2020));
}

TEST(VideoToolboxMappings, HevcProfiles) {
  EXPECT_TRUE(CFEqual(video::vt::hevc_profile(8, false), kVTProfileLevel_HEVC_Main_AutoLevel));
  EXPECT_TRUE(CFEqual(video::vt::hevc_profile(10, false), kVTProfileLevel_HEVC_Main10_AutoLevel));
  EXPECT_TRUE(CFEqual(video::vt::hevc_profile(8, true), CFSTR("HEVC_Main444_AutoLevel")));
  EXPECT_TRUE(CFEqual(video::vt::hevc_profile(10, true), CFSTR("HEVC_Main44410_AutoLevel")));
}

TEST(VideoToolboxEncodeDevice, PixelFormats) {
  EXPECT_EQ(platf::videotoolbox_pixel_format(platf::pix_fmt_e::nv12), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
  EXPECT_EQ(platf::videotoolbox_pixel_format(platf::pix_fmt_e::p010), kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);
  EXPECT_EQ(platf::videotoolbox_pixel_format(platf::pix_fmt_e::yuv444p), kCVPixelFormatType_32BGRA);
  EXPECT_EQ(platf::videotoolbox_pixel_format(platf::pix_fmt_e::yuv444p16), 0U);
  EXPECT_EQ(platf::videotoolbox_pixel_format(platf::pix_fmt_e::yuv420p), 0U);
}

TEST(VideoToolboxEncodeDevice, InitConfiguresCaptureAndRejectsUnsupportedFormats) {
  OSType requested_format = 0;
  int requested_width = 0;
  int requested_height = 0;

  auto resize = [&](int width, int height) {
    requested_width = width;
    requested_height = height;
  };
  auto select_format = [&](OSType format) {
    requested_format = format;
  };

  platf::videotoolbox_encode_device_t device;
  ASSERT_EQ(device.init(platf::pix_fmt_e::p010, resize, select_format), 0);
  EXPECT_EQ(requested_format, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange);

  device.set_resolution(1920, 1080);
  EXPECT_EQ(requested_width, 1920);
  EXPECT_EQ(requested_height, 1080);

  platf::videotoolbox_encode_device_t unsupported;
  EXPECT_NE(unsupported.init(platf::pix_fmt_e::yuv420p, nullptr, nullptr), 0);
  EXPECT_NE(unsupported.init(platf::pix_fmt_e::unknown, nullptr, nullptr), 0);

  platf::videotoolbox_encode_device_t without_callbacks;
  EXPECT_EQ(without_callbacks.init(platf::pix_fmt_e::nv12, nullptr, nullptr), 0);
  without_callbacks.set_resolution(640, 360);
}

TEST(VideoToolboxEncodeDevice, CaptureSizeShrinksUnevenRatiosItself) {
  EXPECT_EQ(platf::videotoolbox_capture_size(3840, 2160, 2560, 1440), std::make_pair(3840, 2160));
  EXPECT_EQ(platf::videotoolbox_capture_size(4096, 2304, 2560, 1440), std::make_pair(4096, 2304));
}

TEST(VideoToolboxEncodeDevice, CaptureSizeLeavesOtherCasesToScreenCaptureKit) {
  // Whole-number ratio
  EXPECT_EQ(platf::videotoolbox_capture_size(5120, 2880, 2560, 1440), std::make_pair(2560, 1440));
  // Same size
  EXPECT_EQ(platf::videotoolbox_capture_size(2560, 1440, 2560, 1440), std::make_pair(2560, 1440));
  // Growing
  EXPECT_EQ(platf::videotoolbox_capture_size(1920, 1080, 2560, 1440), std::make_pair(2560, 1440));
  // Different shape
  EXPECT_EQ(platf::videotoolbox_capture_size(3840, 2400, 2560, 1440), std::make_pair(2560, 1440));
  // No stream size yet
  EXPECT_EQ(platf::videotoolbox_capture_size(3840, 2160, 0, 0), std::make_pair(0, 0));
}

TEST(VideoToolboxEncodeDevice, ConvertScalesFramesToTheEncodeResolution) {
  platf::videotoolbox_encode_device_t device;
  ASSERT_EQ(device.init(platf::pix_fmt_e::yuv444p, nullptr, nullptr), 0);
  device.set_resolution(64, 36);

  auto bgra = make_image(96, 54, kCVPixelFormatType_32BGRA);
  ASSERT_EQ(device.convert(*bgra), 0);
  CVPixelBufferRef scaled = device.current_frame();
  ASSERT_NE(scaled, bgra->pixel_buffer->buf);
  EXPECT_EQ(CVPixelBufferGetWidth(scaled), 64U);
  EXPECT_EQ(CVPixelBufferGetHeight(scaled), 36U);
  EXPECT_EQ(CVPixelBufferGetPixelFormatType(scaled), kCVPixelFormatType_32BGRA);

  auto nv12 = make_image(96, 54);
  ASSERT_EQ(device.convert(*nv12), 0);
  scaled = device.current_frame();
  EXPECT_EQ(CVPixelBufferGetWidth(scaled), 64U);
  EXPECT_EQ(CVPixelBufferGetHeight(scaled), 36U);
  ASSERT_EQ(CVPixelBufferGetPixelFormatType(scaled), kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);

  // Each plane is filtered on its own, so a flat frame stays flat, edges included.
  CVPixelBufferLockBaseAddress(scaled, kCVPixelBufferLock_ReadOnly);
  const auto *luma = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(scaled, 0));
  const auto *chroma = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(scaled, 1));
  const auto luma_stride = CVPixelBufferGetBytesPerRowOfPlane(scaled, 0);
  EXPECT_NEAR(luma[0], 16, 1);
  EXPECT_NEAR(luma[35 * luma_stride + 63], 16, 1);
  EXPECT_NEAR(chroma[0], 128, 1);
  EXPECT_NEAR(chroma[1], 128, 1);
  CVPixelBufferUnlockBaseAddress(scaled, kCVPixelBufferLock_ReadOnly);
}

TEST(VideoToolboxEncodeDevice, ConvertPassesFramesAtTheEncodeResolutionThrough) {
  platf::videotoolbox_encode_device_t device;
  device.set_resolution(64, 64);

  auto image = make_image(64, 64);
  ASSERT_EQ(device.convert(*image), 0);
  EXPECT_EQ(device.current_frame(), image->pixel_buffer->buf);
}

TEST(VideoToolboxEncodeDevice, ConvertRejectsFramesItCannotScale) {
  platf::videotoolbox_encode_device_t device;
  device.set_resolution(32, 32);

  auto image = make_image(64, 64, kCVPixelFormatType_32ARGB);
  EXPECT_NE(device.convert(*image), 0);
  EXPECT_EQ(device.current_frame(), nullptr);
}

TEST(VideoToolboxEncodeDevice, ConvertRetainsTheLatestPixelBuffer) {
  platf::videotoolbox_encode_device_t device;
  EXPECT_EQ(device.current_frame(), nullptr);

  platf::av_img_t empty;
  EXPECT_NE(device.convert(empty), 0);

  auto first = make_image(64, 64);
  ASSERT_EQ(device.convert(*first), 0);
  EXPECT_EQ(device.current_frame(), first->pixel_buffer->buf);

  auto second = make_image(64, 64);
  ASSERT_EQ(device.convert(*second), 0);
  EXPECT_EQ(device.current_frame(), second->pixel_buffer->buf);

  // The device keeps its own reference after the capture image is recycled.
  CVPixelBufferRef retained = device.current_frame();
  second.reset();
  EXPECT_EQ(CVPixelBufferGetWidth(retained), 64U);
}

TEST_F(VideoToolboxEncodeTest, PacketArrivesWithoutSubmittingAnotherFrame) {
  auto session = make_session(0);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox H.264 encoding is unavailable";
  }
  EXPECT_TRUE(session->emits_packets_asynchronously());

  const auto captured = std::chrono::steady_clock::now();
  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(7, packets, nullptr, captured), 0);

  // FFmpeg's wrapper only released a frame's packet when the next frame was submitted.
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);
  EXPECT_TRUE(packet->is_idr());
  EXPECT_EQ(packet->frame_index(), 7);
  ASSERT_TRUE(packet->frame_timestamp);
  EXPECT_EQ(*packet->frame_timestamp, captured);
  EXPECT_EQ(packet->channel_data, nullptr);

  const auto units = split_nal_units(packet->data(), packet->data_size());
  ASSERT_GE(units.size(), 3U);
  EXPECT_EQ(units[0][0] & 0x1F, 7);  // SPS
  EXPECT_EQ(units[1][0] & 0x1F, 8);  // PPS
}

TEST_F(VideoToolboxEncodeTest, KeyframeRequestsAreHonoredOnTheRequestedFrame) {
  auto session = make_session(0);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox H.264 encoding is unavailable";
  }

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(1, packets, nullptr, std::nullopt), 0);
  auto first = packets->pop(2s);
  ASSERT_TRUE(first);
  EXPECT_TRUE(first->is_idr());

  session->request_normal_frame();
  ASSERT_EQ(session->encode_frame(2, packets, nullptr, std::nullopt), 0);
  auto second = packets->pop(2s);
  ASSERT_TRUE(second);
  EXPECT_FALSE(second->is_idr());
  EXPECT_FALSE(second->frame_timestamp);

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(3, packets, nullptr, std::nullopt), 0);
  auto third = packets->pop(2s);
  ASSERT_TRUE(third);
  EXPECT_EQ(third->frame_index(), 3);
  EXPECT_TRUE(third->is_idr());

  session->invalidate_ref_frames(2, 3);
  ASSERT_EQ(session->encode_frame(4, packets, nullptr, std::nullopt), 0);
  auto fourth = packets->pop(2s);
  ASSERT_TRUE(fourth);
  EXPECT_TRUE(fourth->is_idr());
}

TEST_F(VideoToolboxEncodeTest, HevcKeyframesCarryParameterSets) {
  auto session = make_session(1);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox HEVC encoding is unavailable";
  }

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(1, packets, nullptr, std::nullopt), 0);
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);
  EXPECT_TRUE(packet->is_idr());

  const auto units = split_nal_units(packet->data(), packet->data_size());
  ASSERT_GE(units.size(), 4U);
  EXPECT_EQ((units[0][0] >> 1) & 0x3F, 32);  // VPS
  EXPECT_EQ((units[1][0] >> 1) & 0x3F, 33);  // SPS
  EXPECT_EQ((units[2][0] >> 1) & 0x3F, 34);  // PPS
}

TEST_F(VideoToolboxEncodeTest, Hevc444SessionEncodesFullChroma) {
  auto session = make_session(1, true);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox HEVC 4:4:4 encoding is unavailable";
  }
  // With low-latency rate control the encoder would fall back to 4:2:0.
  EXPECT_FALSE(session->low_latency_rate_control());

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(1, packets, nullptr, std::nullopt), 0);
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);

  const auto units = split_nal_units(packet->data(), packet->data_size());
  ASSERT_GE(units.size(), 2U);
  ASSERT_EQ((units[1][0] >> 1) & 0x3F, 33);  // SPS
  EXPECT_EQ(hevc_chroma_format_idc(units[1]), 3);
}

TEST_F(VideoToolboxEncodeTest, Hevc420SessionStaysAt420) {
  auto session = make_session(1);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox HEVC encoding is unavailable";
  }

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(1, packets, nullptr, std::nullopt), 0);
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);

  const auto units = split_nal_units(packet->data(), packet->data_size());
  ASSERT_GE(units.size(), 2U);
  EXPECT_EQ(hevc_chroma_format_idc(units[1]), 1);
}

TEST_F(VideoToolboxEncodeTest, H264444IsRejected) {
  const auto config = test_config(0, true);
  auto device = std::make_unique<platf::videotoolbox_encode_device_t>();
  ASSERT_EQ(device->init(platf::pix_fmt_e::yuv444p, nullptr, nullptr), 0);

  video::videotoolbox_encode_session_t session {config, std::move(device)};
  EXPECT_FALSE(session.init(video::videotoolbox.codec_from_config(config)));
}

TEST_F(VideoToolboxEncodeTest, EncodeWithoutAFrameFails) {
  const auto config = test_config(0);
  auto device = std::make_unique<platf::videotoolbox_encode_device_t>();
  ASSERT_EQ(device->init(platf::pix_fmt_e::nv12, nullptr, nullptr), 0);
  device->colorspace = video::colorspace_from_client_config(config, false);

  video::videotoolbox_encode_session_t session {config, std::move(device)};
  if (!session.init(video::videotoolbox.codec_from_config(config))) {
    GTEST_SKIP() << "VideoToolbox H.264 encoding is unavailable";
  }

  EXPECT_NE(session.encode_frame(1, packets, nullptr, std::nullopt), 0);

  platf::av_img_t empty;
  EXPECT_NE(session.convert(empty), 0);
}

TEST_F(VideoToolboxEncodeTest, SessionWithoutDeviceCannotInitOrEncode) {
  video::videotoolbox_encode_session_t session {test_config(0), nullptr};

  EXPECT_FALSE(session.init(video::videotoolbox.codec_from_config(test_config(0))));
  EXPECT_NE(session.encode_frame(1, packets, nullptr, std::nullopt), 0);

  platf::av_img_t empty;
  EXPECT_NE(session.convert(empty), 0);
}

TEST_F(VideoToolboxEncodeTest, AnnexBConversionHandlesParameterSetsAndMalformedSamples) {
  auto session = make_session(0);
  if (!session) {
    GTEST_SKIP() << "VideoToolbox H.264 encoding is unavailable";
  }

  session->request_idr_frame();
  ASSERT_EQ(session->encode_frame(1, packets, nullptr, std::nullopt), 0);
  auto packet = packets->pop(2s);
  ASSERT_TRUE(packet);

  const auto units = split_nal_units(packet->data(), packet->data_size());
  ASSERT_GE(units.size(), 3U);
  const uint8_t *parameter_sets[] {units[0].data(), units[1].data()};
  const size_t parameter_set_sizes[] {units[0].size(), units[1].size()};
  CMFormatDescriptionRef format = nullptr;
  ASSERT_EQ(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, parameter_sets, parameter_set_sizes, 4, &format), noErr);

  const std::vector<uint8_t> slice {0x65, 0x88, 0x84, 0x00};

  auto keyframe = make_encoded_sample(format, length_prefixed(slice, slice.size()), false);
  EXPECT_TRUE(video::vt::is_keyframe(keyframe));
  std::vector<uint8_t> annex_b;
  ASSERT_TRUE(video::vt::to_annex_b(keyframe, true, annex_b));
  const auto keyframe_units = split_nal_units(annex_b.data(), annex_b.size());
  ASSERT_EQ(keyframe_units.size(), 3U);
  EXPECT_EQ(keyframe_units[0], units[0]);
  EXPECT_EQ(keyframe_units[1], units[1]);
  EXPECT_EQ(keyframe_units[2], slice);
  CFRelease(keyframe);

  auto delta = make_encoded_sample(format, length_prefixed(slice, slice.size()), true);
  EXPECT_FALSE(video::vt::is_keyframe(delta));
  ASSERT_TRUE(video::vt::to_annex_b(delta, false, annex_b));
  EXPECT_EQ(split_nal_units(annex_b.data(), annex_b.size()), std::vector<std::vector<uint8_t>> {slice});
  CFRelease(delta);

  auto overlong = make_encoded_sample(format, length_prefixed(slice, slice.size() + 10), true);
  EXPECT_FALSE(video::vt::to_annex_b(overlong, false, annex_b));
  EXPECT_TRUE(annex_b.empty());
  CFRelease(overlong);

  auto empty_nal = make_encoded_sample(format, length_prefixed({}, 0), true);
  EXPECT_FALSE(video::vt::to_annex_b(empty_nal, false, annex_b));
  CFRelease(empty_nal);

  auto truncated_prefix = make_encoded_sample(format, {0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x84, 0x00, 0x00, 0x00}, true);
  EXPECT_FALSE(video::vt::to_annex_b(truncated_prefix, false, annex_b));
  CFRelease(truncated_prefix);

  CFRelease(format);
}

TEST(VideoToolboxAnnexB, RejectsSamplesFromOtherCodecs) {
  CMVideoFormatDescriptionRef format = nullptr;
  ASSERT_EQ(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_JPEG, 16, 16, nullptr, &format), noErr);

  auto sample = make_encoded_sample(format, {0x00, 0x00, 0x00, 0x01, 0xFF}, false);
  std::vector<uint8_t> annex_b {0x01};
  EXPECT_FALSE(video::vt::to_annex_b(sample, true, annex_b));
  EXPECT_TRUE(annex_b.empty());

  CFRelease(sample);
  CFRelease(format);
}

TEST(VideoToolboxAnnexB, SampleWithoutAttachmentsIsAKeyframe) {
  CMVideoFormatDescriptionRef format = nullptr;
  ASSERT_EQ(CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_H264, 16, 16, nullptr, &format), noErr);

  CMBlockBufferRef block = nullptr;
  CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, 4, kCFAllocatorDefault, nullptr, 0, 4, kCMBlockBufferAssureMemoryNowFlag, &block);
  const size_t sample_size = 4;
  CMSampleBufferRef sample = nullptr;
  ASSERT_EQ(CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1, &sample_size, &sample), noErr);

  EXPECT_TRUE(video::vt::is_keyframe(sample));

  CFRelease(sample);
  CFRelease(block);
  CFRelease(format);
}
#endif
