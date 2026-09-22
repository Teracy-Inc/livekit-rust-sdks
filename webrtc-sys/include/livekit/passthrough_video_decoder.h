/*
 * ADR 0055: a webrtc::VideoDecoder that never decodes. libwebrtc hands it
 * post-jitter-buffer H.264 access units; it forwards them to a
 * process-global Rust sink (passthrough_video_decoder.rs) and returns OK
 * without ever producing a decoded frame. VideoDecoderFactory::Create hands
 * it out for H.264 only (video_decoder_factory.cpp).
 */

#pragma once

#include <cstdint>
#include <memory>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"

namespace livekit_ffi {

// Process-global switch read by VideoDecoderFactory::Create. Decoders are
// created when a track is subscribed, so set it before connecting.
void set_passthrough_decoding(bool enabled);
bool livekit_passthrough_decoding_enabled();

// Asks libwebrtc for a keyframe on the receive stream whose first RTP ssrc
// is `ssrc`: the passthrough decoder for that stream answers its next
// Decode() with WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME (the access unit is
// still forwarded), which makes VideoReceiveStream2 send a PLI / FIR through
// the SFU — unless that next access unit is itself a keyframe, which
// satisfies the request on its own. One request per call; a request for an
// ssrc no decoder ever sees stays pending until
// clear_passthrough_keyframe_requests() (also run by
// set_passthrough_decoding(false)). Thread-safe.
void set_passthrough_keyframe_request(uint32_t ssrc);
void clear_passthrough_keyframe_requests();

std::unique_ptr<webrtc::VideoDecoder> CreatePassthroughVideoDecoder(
    const webrtc::Environment& env,
    const webrtc::SdpVideoFormat& format);

}  // namespace livekit_ffi
