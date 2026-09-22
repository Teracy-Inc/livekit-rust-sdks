/*
 * ADR 0055: passthrough video decoder. See the header.
 */

#include "livekit/passthrough_video_decoder.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "absl/strings/match.h"
#include "api/video/encoded_image.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/video_decoder.h"
#include "media/base/media_constants.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rust/cxx.h"
#include "webrtc-sys/src/passthrough_video_decoder.rs.h"

namespace livekit_ffi {
namespace {

std::atomic<bool> g_passthrough_decoding{false};
std::atomic<uint32_t> g_decoder_count{0};

// ssrcs with a keyframe request pending (set_passthrough_keyframe_request,
// consumed by the matching decoder's next Decode). A handful at most, so a
// vector under a mutex; `g_keyframe_pending_count` mirrors its size so the
// per-frame check on the decode threads stays lock-free while nothing is
// pending — the common case.
std::mutex g_keyframe_mutex;
std::vector<uint32_t> g_keyframe_pending;
std::atomic<uint32_t> g_keyframe_pending_count{0};

// Clears and reports a pending request for `ssrc`.
bool TakeKeyframeRequest(uint32_t ssrc) {
  if (ssrc == 0 ||
      g_keyframe_pending_count.load(std::memory_order_acquire) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_keyframe_mutex);
  auto it =
      std::find(g_keyframe_pending.begin(), g_keyframe_pending.end(), ssrc);
  if (it == g_keyframe_pending.end()) return false;
  g_keyframe_pending.erase(it);
  g_keyframe_pending_count.store(
      static_cast<uint32_t>(g_keyframe_pending.size()),
      std::memory_order_release);
  return true;
}

// Codec ids on the loopback wire (route3-wire.md): 1 = H.264 (Annex B),
// 2 = VP8, 3 = VP9. 4 = H.265 and 5 = AV1 are spike additions; 0 = unknown.
uint8_t CodecIdFromFormat(const webrtc::SdpVideoFormat& format) {
  if (absl::EqualsIgnoreCase(format.name, webrtc::kH264CodecName)) return 1;
  if (absl::EqualsIgnoreCase(format.name, webrtc::kVp8CodecName)) return 2;
  if (absl::EqualsIgnoreCase(format.name, webrtc::kVp9CodecName)) return 3;
  if (absl::EqualsIgnoreCase(format.name, webrtc::kH265CodecName)) return 4;
  if (absl::EqualsIgnoreCase(format.name, webrtc::kAv1CodecName)) return 5;
  return 0;
}

class PassthroughVideoDecoder final : public webrtc::VideoDecoder {
 public:
  explicit PassthroughVideoDecoder(const webrtc::SdpVideoFormat& format)
      : format_name_(format.name),
        codec_(CodecIdFromFormat(format)),
        index_(g_decoder_count.fetch_add(1)) {}

  ~PassthroughVideoDecoder() override {
    RTC_LOG(LS_INFO) << "PassthroughVideoDecoder#" << index_ << " destroyed after "
                     << decoded_ << " access units";
  }

  bool Configure(const Settings& settings) override {
    RTC_LOG(LS_INFO) << "PassthroughVideoDecoder#" << index_ << " configured: "
                     << format_name_ << " codec_type=" << settings.codec_type()
                     << " cores=" << settings.number_of_cores();
    return true;
  }

  using webrtc::VideoDecoder::Decode;

  int32_t Decode(const webrtc::EncodedImage& input,
                 int64_t /* render_time_ms */) override {
    // PacketInfos carries one RtpPacketInfo per RTP packet the frame was
    // assembled from; the first one's ssrc identifies the receive stream.
    uint32_t ssrc = 0;
    const auto& infos = input.PacketInfos();
    if (!infos.empty()) {
      ssrc = infos.begin()->ssrc();
    }
    const bool key = input._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    if (decoded_ == 0) {
      RTC_LOG(LS_INFO) << "PassthroughVideoDecoder#" << index_ << " first AU: "
                       << format_name_ << " ssrc=" << ssrc
                       << " packets=" << infos.size() << " key=" << key
                       << " size=" << input.size() << " " << input._encodedWidth
                       << "x" << input._encodedHeight;
    }
    ++decoded_;
    on_passthrough_encoded_frame(
        ssrc, input.RtpTimestamp(), key, input._encodedWidth,
        input._encodedHeight, codec_,
        rust::Slice<const uint8_t>(input.data(), input.size()));
    // OK_REQUEST_KEYFRAME counts as a successful decode (deltas keep flowing
    // to the sink — a tile that is not waiting for a keyframe keeps
    // rendering) but makes VideoReceiveStream2 request a keyframe at once;
    // ERROR would instead mark the stream keyframe-required and drop every
    // delta until one arrives. A pending request is consumed by this AU
    // either way; when the AU is itself a keyframe it already satisfies
    // whoever asked, and a PLI would only make the publisher send a second
    // one.
    if (TakeKeyframeRequest(ssrc)) {
      if (key) {
        RTC_LOG(LS_INFO) << "PassthroughVideoDecoder#" << index_
                         << " ssrc=" << ssrc
                         << ": keyframe request satisfied by this keyframe";
      } else {
        RTC_LOG(LS_INFO) << "PassthroughVideoDecoder#" << index_
                         << " ssrc=" << ssrc << ": keyframe requested";
        return WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME;
      }
    }
    // OK, not NO_OUTPUT: VideoReceiveStream2 treats anything but OK (or
    // OK_REQUEST_KEYFRAME) as a decode error and starts requesting
    // keyframes on its own.
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override {
    // Stored and never called: no decoded frame ever leaves this decoder.
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    callback_ = nullptr;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  DecoderInfo GetDecoderInfo() const override {
    DecoderInfo info;
    info.implementation_name = "passthrough";
    info.is_hardware_accelerated = false;
    return info;
  }

 private:
  const std::string format_name_;
  const uint8_t codec_;
  const uint32_t index_;
  uint64_t decoded_ = 0;
  webrtc::DecodedImageCallback* callback_ = nullptr;
};

}  // namespace

void clear_passthrough_keyframe_requests() {
  std::lock_guard<std::mutex> lock(g_keyframe_mutex);
  g_keyframe_pending.clear();
  g_keyframe_pending_count.store(0, std::memory_order_release);
}

void set_passthrough_decoding(bool enabled) {
  g_passthrough_decoding.store(enabled);
  if (!enabled) {
    // Nothing will consume them any more.
    clear_passthrough_keyframe_requests();
  }
}

bool livekit_passthrough_decoding_enabled() {
  return g_passthrough_decoding.load();
}

void set_passthrough_keyframe_request(uint32_t ssrc) {
  if (ssrc == 0) return;
  std::lock_guard<std::mutex> lock(g_keyframe_mutex);
  if (std::find(g_keyframe_pending.begin(), g_keyframe_pending.end(), ssrc) ==
      g_keyframe_pending.end()) {
    g_keyframe_pending.push_back(ssrc);
  }
  g_keyframe_pending_count.store(
      static_cast<uint32_t>(g_keyframe_pending.size()),
      std::memory_order_release);
}

std::unique_ptr<webrtc::VideoDecoder> CreatePassthroughVideoDecoder(
    const webrtc::Environment& /* env */,
    const webrtc::SdpVideoFormat& format) {
  return std::make_unique<PassthroughVideoDecoder>(format);
}

}  // namespace livekit_ffi
