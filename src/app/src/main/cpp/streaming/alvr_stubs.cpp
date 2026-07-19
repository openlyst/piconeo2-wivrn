// Compatibility shim: the legacy wivrn-pvr render loop still speaks the ALVR C API
// surface, but every operation that actually touches streaming is backed by the
// WiVRn streaming_client and pico_blit renderer.

#include "alvr_client_core.h"
#include "alvr_ext.h"

#include "streaming_client.h"
#include "wivrn_stream_adapter.h"
#include "pico_decoder.h"
#include "stream_swap.h"

#include <GLES3/gl3.h>
#include <cstring>
#include <string>
#include <mutex>
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "wivrn-pvr", __VA_ARGS__)

static uint64_t hash_path(const char *path)
{
	uint64_t h = 14695981039346656037ULL;
	for (const char *p = path; *p; ++p)
	{
		h ^= (uint64_t)(uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

// Server render poses from the last synchronized blit, for the render
// thread's warp baseline. Written by alvr_render_stream_opengl, read by
// the render thread when setting up the warp pose.
XrPosef gLastServerPoses[2] = {};
std::mutex gServerPoseMutex;

extern "C" {

void alvr_initialize_logging(void) {}
void alvr_initialize_android_context(void *, void *) {}
void alvr_try_get_permission(const char *) {}
void alvr_initialize(struct AlvrClientCapabilities) {}
void alvr_destroy(void) {}
void alvr_resume(void) {}
void alvr_pause(void) {}

uint64_t alvr_path_string_to_id(const char *path) { return hash_path(path); }
void alvr_log(AlvrLogLevel, const char *) {}
void alvr_dbg_client_impl(const char *) {}
void alvr_dbg_decoder(const char *) {}
void alvr_log_time(const char *) {}
uint64_t alvr_mdns_service(char *) { return 0; }
uint64_t alvr_hostname(char *) { return 0; }
uint64_t alvr_protocol_id(char *) { return 0; }

bool alvr_poll_event(AlvrEvent *ev)
{
	if (!ev)
		return false;

	static bool was_streaming = false;
	static bool was_ready = false;

	if (!g_stream) {
		if (was_streaming) {
			ev->tag = ALVR_EVENT_STREAMING_STOPPED;
			was_streaming = false;
			was_ready = false;
			return true;
		}
		return false;
	}

	bool streaming = wivrn_streaming();
	bool ready = wivrn_stream_ready();

	if (!was_streaming && streaming) {
		int w = 0, h = 0;
		wivrn_stream_resolution(&w, &h);
		ev->tag = ALVR_EVENT_STREAMING_STARTED;
		ev->STREAMING_STARTED.view_width = static_cast<uint32_t>(w);
		ev->STREAMING_STARTED.view_height = static_cast<uint32_t>(h);
		ev->STREAMING_STARTED.refresh_rate_hint = 72.0f;
		ev->STREAMING_STARTED.encoding_gamma = 1.0f;
		ev->STREAMING_STARTED.enable_foveated_encoding = true;
		ev->STREAMING_STARTED.enable_hdr = false;
		was_streaming = true;
		return true;
	}

	if (was_streaming && !streaming) {
		ev->tag = ALVR_EVENT_STREAMING_STOPPED;
		was_streaming = false;
		was_ready = false;
		return true;
	}

	if (was_streaming && !was_ready && ready) {
		ev->tag = ALVR_EVENT_DECODER_CONFIG;
		ev->DECODER_CONFIG.codec = ALVR_CODEC_H264;
		was_ready = true;
		return true;
	}

	return false;
}

uint64_t alvr_hud_message(char *) { return 0; }
uint64_t alvr_hud_message_bounded(char *, uint64_t) { return 0; }
uint64_t alvr_get_settings_json(char *) { return 0; }
uint64_t alvr_get_settings_json_bounded(char *, uint64_t) { return 0; }
uint64_t alvr_get_server_version(char *) { return 0; }
uint64_t alvr_get_decoder_config(char *) { return 0; }
uint64_t alvr_get_decoder_config_bounded(char *, uint64_t) { return 0; }

void alvr_send_battery(uint64_t, float, bool) {}
void alvr_send_playspace(float, float) {}
void alvr_send_active_interaction_profile(uint64_t, uint64_t) {}
void alvr_send_custom_interaction_profile(uint64_t, const uint64_t *, uint64_t) {}
void alvr_send_button(uint64_t, struct AlvrButtonValue) {}
void alvr_send_view_params(const struct AlvrViewParams *) {}
void alvr_send_eye_openness(float, float) {}
void alvr_send_tracking(uint64_t,
                        const struct AlvrDeviceMotion *,
                        uint64_t,
                        const struct AlvrPose *const *,
                        const struct AlvrPose *const *) {}

void alvr_set_decoder_input_callback(void *, bool (*)(struct AlvrVideoFrameData)) {}
void alvr_report_frame_decoded(uint64_t) {}
void alvr_report_fatal_decoder_error(const char *) {}
void alvr_report_compositor_start(uint64_t, struct AlvrViewParams *) {}
void alvr_report_submit(uint64_t, uint64_t) {}

void alvr_initialize_opengl(void) {}
void alvr_destroy_opengl(void) {}
void alvr_resume_opengl(uint32_t, uint32_t, const uint32_t **, uint32_t) {}
void alvr_pause_opengl(void) {}
void alvr_update_hud_message_opengl(const char *) {}
void alvr_start_stream_opengl(struct AlvrStreamConfig) {}
void alvr_render_lobby_opengl(const struct AlvrLobbyViewParams *, bool) {}

void alvr_render_stream_opengl(void *, const struct AlvrStreamViewParams *)
{
	if (!g_stream || gStreamW == 0 || gStreamH == 0)
		return;

	const int eyeW = g_stream->eye_width.load();
	const int eyeH = g_stream->eye_height.load();
	if (eyeW <= 0 || eyeH <= 0)
		return;

	// If stream 1 falls far behind stream 0, flush its decoder so it can
	// catch up. This prevents the right eye from showing stale frames.
	{
		uint64_t idx0 = g_stream->latest_decoded_frame_index_per_stream[0].load(std::memory_order_acquire);
		uint64_t idx1 = g_stream->latest_decoded_frame_index_per_stream[1].load(std::memory_order_acquire);
		if (idx0 > 0 && idx1 > 0)
		{
			int64_t gap = (int64_t)idx0 - (int64_t)idx1;
			if (gap > 15 && g_stream->decoders[1])
			{
				static int flush_log_count = 0;
				if (flush_log_count++ % 100 == 0)
					LOGI("stream 1 behind by %lld frames, flushing", (long long)gap);
				g_stream->decoders[1]->flush();
			}
		}
	}

	// Get synchronized frames for both eyes (same frame index) to prevent
	// the right eye from stuttering when stream 1 lags behind stream 0.
	std::shared_ptr<pico_decoded_frame> frames[2];
	XrPosef poses[2];
	wivrn_get_synced_frames(frames, poses);

	for (int eye = 0; eye < 2; eye++) {
		glBindFramebuffer(GL_FRAMEBUFFER, gStreamFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, gSwap[eye][gSwapIdx], 0);
		glViewport(0, 0, static_cast<GLsizei>(eyeW), static_cast<GLsizei>(eyeH));
		wivrn_blit_eye_frame(eye, frames[eye], eyeW, eyeH, &poses[eye]);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Store the exact server poses from the frames that were blitted.
	{
		std::lock_guard<std::mutex> lk(gServerPoseMutex);
		gLastServerPoses[0] = poses[0];
		gLastServerPoses[1] = poses[1];
	}
}

void alvr_create_decoder(struct AlvrDecoderConfig) {}
void alvr_destroy_decoder(void) {}
bool alvr_get_frame(uint64_t *out_ts, void **) { return false; }
bool alvr_get_frame_timeout(uint64_t *out_ts, void **, uint64_t)
{
	if (!wivrn_stream_ready() || !g_stream)
		return false;
	auto frame = g_stream->get_latest_frame(0);
	if (!frame || !frame->valid)
		return false;
	if (out_ts)
		*out_ts = frame->frame_index * 10000ULL;
	return true;
}
void alvr_set_decoder_paused(bool) {}

struct AlvrQuat alvr_rotation_delta(struct AlvrQuat s, struct AlvrQuat) { return s; }

void alvr_set_eq_gains(const float *, int) {}
void alvr_set_eq_enabled(bool) {}
bool alvr_get_client_stats(float *) { return false; }

} // extern "C"
