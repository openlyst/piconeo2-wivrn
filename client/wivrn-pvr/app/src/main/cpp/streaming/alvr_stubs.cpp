// Compatibility shim: the legacy wivrn-pvr render loop still speaks the ALVR C API
// surface, but every operation that actually touches streaming is backed by the
// WiVRn streaming_client and pico_blit renderer.

#include "alvr_client_core.h"
#include "alvr_ext.h"

#include "streaming_client.h"
#include "wivrn_stream_adapter.h"
#include "stream_swap.h"

#include <GLES3/gl3.h>
#include <cstring>
#include <string>

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

	// Render into the swapchain at EYE dimensions, not the server's stream
	// dimensions. The PVR warp's distortion mesh is built for the eye buffer
	// resolution; if the swapchain is larger (server often sends more pixels
	// than requested), the excess stays black. The blit pipeline scales the
	// decoded frame (gStreamW x gStreamH) down to the eye dimensions.
	const int eyeW = g_stream->eye_width.load();
	const int eyeH = g_stream->eye_height.load();
	if (eyeW <= 0 || eyeH <= 0)
		return;

	for (int eye = 0; eye < 2; eye++) {
		glBindFramebuffer(GL_FRAMEBUFFER, gStreamFbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, gSwap[eye][gSwapIdx], 0);
		glViewport(0, 0, static_cast<GLsizei>(eyeW), static_cast<GLsizei>(eyeH));
		wivrn_blit_eye(eye, eyeW, eyeH);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
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
