// Temporary stubs for the ALVR C API. As WiVRn streaming is wired up, these
// are replaced with real implementations or the call sites are converted.

#include "alvr_client_core.h"
#include "alvr_ext.h"

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

bool alvr_poll_event(AlvrEvent *) { return false; }
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
void alvr_render_stream_opengl(void *, const struct AlvrStreamViewParams *) {}

void alvr_create_decoder(struct AlvrDecoderConfig) {}
void alvr_destroy_decoder(void) {}
bool alvr_get_frame(uint64_t *, void **) { return false; }
bool alvr_get_frame_timeout(uint64_t *, void **, uint64_t) { return false; }
void alvr_set_decoder_paused(bool) {}

struct AlvrQuat alvr_rotation_delta(struct AlvrQuat s, struct AlvrQuat) { return s; }

void alvr_set_eq_gains(const float *, int) {}
void alvr_set_eq_enabled(bool) {}
bool alvr_get_client_stats(float *) { return false; }

} // extern "C"
