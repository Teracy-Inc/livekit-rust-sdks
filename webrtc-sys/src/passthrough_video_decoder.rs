// ADR 0055: Rust side of the passthrough video decoder. libwebrtc's decode
// threads (one task queue per receive stream) call
// `on_passthrough_encoded_frame` through the cxx shim; it forwards to the
// process-global sink installed with `set_encoded_frame_sink`.

use std::sync::{OnceLock, RwLock};

/// One post-jitter-buffer access unit exactly as libwebrtc handed it to the
/// decoder (H.264: Annex B, SPS/PPS inline on keyframes).
pub struct EncodedFrame<'a> {
    /// First RTP packet's ssrc (0 when `PacketInfos` is empty).
    pub ssrc: u32,
    pub rtp_timestamp: u32,
    pub is_keyframe: bool,
    /// Encoded size as libwebrtc reports it; 0 when unknown (delta frames).
    pub width: u32,
    pub height: u32,
    /// 1 = H.264, 2 = VP8, 3 = VP9, 4 = H.265, 5 = AV1, 0 = unknown.
    pub codec: u8,
    pub data: &'a [u8],
}

pub type EncodedFrameSink = Box<dyn Fn(&EncodedFrame<'_>) + Send + Sync>;

static SINK: OnceLock<RwLock<Option<EncodedFrameSink>>> = OnceLock::new();

fn sink() -> &'static RwLock<Option<EncodedFrameSink>> {
    SINK.get_or_init(|| RwLock::new(None))
}

/// Installs (or clears) the sink every passthrough decoder forwards to. The
/// sink runs on libwebrtc decode threads, so it must not block. Clearing it
/// (the session is over) also drops every pending keyframe request — the
/// ssrcs are gone with the session, and a request no decoder consumes
/// would otherwise sit in the C++ set for the process lifetime, keeping
/// `Decode()` off its lock-free fast path.
pub fn set_encoded_frame_sink(new_sink: Option<EncodedFrameSink>) {
    let clearing = new_sink.is_none();
    *sink().write().unwrap_or_else(|e| e.into_inner()) = new_sink;
    if clearing {
        ffi::clear_passthrough_keyframe_requests();
    }
}

/// Makes `VideoDecoderFactory::Create` hand out passthrough decoders for
/// H.264 (every other format keeps libwebrtc's own decoders). Decoders are
/// created at subscription, so call before connecting. Disabling also
/// drops every pending keyframe request.
pub fn set_passthrough_decoding(enabled: bool) {
    ffi::set_passthrough_decoding(enabled);
}

pub fn passthrough_decoding_enabled() -> bool {
    ffi::livekit_passthrough_decoding_enabled()
}

/// Asks libwebrtc for a keyframe on the receive stream whose ssrc is `ssrc`
/// (the `EncodedFrame::ssrc` the sink sees): the passthrough decoder for
/// that stream answers its next `Decode()` with
/// `WEBRTC_VIDEO_CODEC_OK_REQUEST_KEYFRAME` — the access unit is still
/// forwarded — and `VideoReceiveStream2` sends a PLI / FIR through the
/// SFU. If that next access unit is itself a keyframe the request is
/// satisfied by it and no PLI goes out. One request per call; a request
/// for an ssrc no passthrough decoder ever sees (a track libwebrtc decodes
/// itself) stays pending until the sink is cleared or passthrough decoding
/// is disabled. Thread-safe, callable from any thread.
pub fn set_passthrough_keyframe_request(ssrc: u32) {
    ffi::set_passthrough_keyframe_request(ssrc);
}

fn on_passthrough_encoded_frame(
    ssrc: u32,
    rtp_ts: u32,
    key: bool,
    w: u32,
    h: u32,
    codec: u8,
    data: &[u8],
) {
    let guard = sink().read().unwrap_or_else(|e| e.into_inner());
    if let Some(sink) = guard.as_ref() {
        sink(&EncodedFrame {
            ssrc,
            rtp_timestamp: rtp_ts,
            is_keyframe: key,
            width: w,
            height: h,
            codec,
            data,
        });
    }
}

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    unsafe extern "C++" {
        include!("livekit/passthrough_video_decoder.h");

        fn set_passthrough_decoding(enabled: bool);
        fn livekit_passthrough_decoding_enabled() -> bool;
        fn set_passthrough_keyframe_request(ssrc: u32);
        fn clear_passthrough_keyframe_requests();
    }

    extern "Rust" {
        fn on_passthrough_encoded_frame(
            ssrc: u32,
            rtp_ts: u32,
            key: bool,
            w: u32,
            h: u32,
            codec: u8,
            data: &[u8],
        );
    }
}
