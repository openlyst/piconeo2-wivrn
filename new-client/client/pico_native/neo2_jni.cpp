/*
 * JNI entry points for Pico Neo 2 WiVRn client.
 * Android activity lifecycle, intent handling, per-frame tracking,
 * and eye rendering callbacks from Java.
 */

#include "neo2_client.h"
#include "neo2_blit.h"
#include "wivrn_packets.h"
#include "wivrn_config.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <jni.h>
#include <android/hardware_buffer.h>

#include <cstring>
#include <optional>
#include <string>

static std::optional<std::string> get_server_uri_from_intent(JNIEnv * env, jobject intent)
{
	if (!intent)
		return std::nullopt;

	jclass intent_class = env->GetObjectClass(intent);
	jmethodID get_data = env->GetMethodID(intent_class, "getDataString", "()Ljava/lang/String;");
	jstring data_str = (jstring)env->CallObjectMethod(intent, get_data);

	if (data_str)
	{
		const char * data = env->GetStringUTFChars(data_str, nullptr);
		std::string uri(data);
		env->ReleaseStringUTFChars(data_str, data);
		env->DeleteLocalRef(intent_class);

		if (uri.starts_with("wivrn://") || uri.starts_with("wivrn+tcp://"))
			return uri;
	}
	else
	{
		env->DeleteLocalRef(intent_class);
	}

	return std::nullopt;
}

static std::pair<std::string, int> parse_uri(const std::string & uri)
{
	std::string host;
	int port = wivrn::default_port;

	size_t scheme_end = uri.find("://");
	if (scheme_end == std::string::npos)
		return {host, port};

	size_t host_start = scheme_end + 3;

	size_t at_pos = uri.find('@', host_start);
	if (at_pos != std::string::npos)
		host_start = at_pos + 1;

	size_t path_start = uri.find('/', host_start);
	size_t query_start = uri.find('?', host_start);
	size_t host_end = std::min(
		{path_start == std::string::npos ? uri.size() : path_start,
		 query_start == std::string::npos ? uri.size() : query_start,
		 uri.size()});

	std::string host_port = uri.substr(host_start, host_end - host_start);

	size_t colon = host_port.rfind(':');
	if (colon != std::string::npos)
	{
		host = host_port.substr(0, colon);
		port = std::stoi(host_port.substr(colon + 1));
	}
	else
	{
		host = host_port;
	}

	return {host, port};
}

static std::optional<std::string> parse_pin_from_uri(const std::string & uri)
{
	size_t scheme_end = uri.find("://");
	if (scheme_end != std::string::npos)
	{
		size_t host_start = scheme_end + 3;
		size_t at_pos = uri.find('@', host_start);
		if (at_pos != std::string::npos)
		{
			std::string userinfo = uri.substr(host_start, at_pos - host_start);
			size_t colon = userinfo.find(':');
			if (colon != std::string::npos)
			{
				std::string pin = userinfo.substr(colon + 1);
				if (!pin.empty())
					return pin;
			}
		}
	}

	size_t query_start = uri.find('?');
	if (query_start == std::string::npos)
		return std::nullopt;

	std::string query = uri.substr(query_start + 1);
	size_t pin_pos = query.find("pin=");
	if (pin_pos == std::string::npos)
		return std::nullopt;

	size_t value_start = pin_pos + 4;
	size_t value_end = query.find('&', value_start);
	if (value_end == std::string::npos)
		value_end = query.size();

	return query.substr(value_start, value_end - value_start);
}

extern "C" {

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnInit(JNIEnv * env, jobject thiz, jlong ptr, jobject intent)
{
	static auto logger = spdlog::android_logger_mt("WiVRn-Neo2", "WiVRn-Neo2");
	spdlog::set_default_logger(logger);
	spdlog::set_level(spdlog::level::debug);

	spdlog::info("WiVRn Pico Neo2 client starting");

	auto * client = new neo2_client();
	g_neo2_client = client;

	env->GetJavaVM(&client->vm);
	client->activity = env->NewGlobalRef(thiz);

	auto uri = get_server_uri_from_intent(env, intent);
	if (uri)
	{
		spdlog::info("Server URI: {}", *uri);
		auto [host, port] = parse_uri(*uri);
		client->server_host = host;
		client->server_port = port;
		client->tcp_only = uri->starts_with("wivrn+tcp://");

		auto pin = parse_pin_from_uri(*uri);
		if (pin)
		{
			client->pairing_pin = *pin;
			spdlog::info("PIN from URI: {}", *pin);
		}
	}
	else
	{
		spdlog::warn("No server URI, waiting for intent");
		client->server_host = "";
		client->server_port = wivrn::default_port;
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDestroy(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_neo2_client)
		return;

	g_neo2_client->shutdown = true;
	g_neo2_client->running = false;

	if (g_neo2_client->network_thread.joinable())
		g_neo2_client->network_thread.join();
	if (g_neo2_client->tracking_thread.joinable())
		g_neo2_client->tracking_thread.join();

	if (g_neo2_client->activity)
		env->DeleteGlobalRef(g_neo2_client->activity);

	delete g_neo2_client;
	g_neo2_client = nullptr;

	spdlog::info("WiVRn Pico Neo2 client destroyed");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnPause(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (g_neo2_client)
		g_neo2_client->running = false;
	spdlog::info("nativePause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnResume(JNIEnv * env, jobject thiz, jlong ptr)
{
	if (!g_neo2_client)
		return;

	g_neo2_client->try_connect();
	g_neo2_client->running = true;
	spdlog::info("nativeResume");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnNewIntent(JNIEnv * env, jobject thiz, jlong ptr, jobject intent)
{
	if (!g_neo2_client)
		return;

	auto uri = get_server_uri_from_intent(env, intent);
	if (uri)
	{
		spdlog::info("New server URI: {}", *uri);
		auto [host, port] = parse_uri(*uri);
		g_neo2_client->server_host = host;
		g_neo2_client->server_port = port;
		g_neo2_client->tcp_only = uri->starts_with("wivrn+tcp://");

		auto pin = parse_pin_from_uri(*uri);
		if (pin)
		{
			g_neo2_client->pairing_pin = *pin;
			spdlog::info("PIN from new intent: {}", *pin);
		}

		g_neo2_client->try_connect();
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnOnFrameBegin(
	JNIEnv * env, jobject thiz, jlong ptr,
	jfloatArray headOrient, jfloatArray headPos,
	jfloatArray leftOrient, jfloatArray leftPos, jint leftTrigger, jintArray leftTouch, jint leftBattery,
	jboolean leftA, jboolean leftB, jboolean leftGrip, jboolean leftClick, jboolean leftMenu,
	jfloatArray rightOrient, jfloatArray rightPos, jint rightTrigger, jintArray rightTouch, jint rightBattery,
	jboolean rightA, jboolean rightB, jboolean rightGrip, jboolean rightClick, jboolean rightMenu)
{
	if (!g_neo2_client)
		return;

	float head_o[4], head_p[3];
	env->GetFloatArrayRegion(headOrient, 0, 4, head_o);
	env->GetFloatArrayRegion(headPos, 0, 3, head_p);

	{
		std::lock_guard lock(g_neo2_client->frame_mutex);
		memcpy(g_neo2_client->head_orientation, head_o, sizeof(head_o));
		memcpy(g_neo2_client->head_position, head_p, sizeof(head_p));
		g_neo2_client->has_head_pose = true;
	}

	if (leftOrient && leftPos)
	{
		float o[4], p[3];
		env->GetFloatArrayRegion(leftOrient, 0, 4, o);
		env->GetFloatArrayRegion(leftPos, 0, 3, p);
		std::lock_guard lock(g_neo2_client->frame_mutex);
		memcpy(g_neo2_client->controllers[0].orientation, o, sizeof(o));
		memcpy(g_neo2_client->controllers[0].position, p, sizeof(p));
		g_neo2_client->controllers[0].trigger = leftTrigger;
		g_neo2_client->controllers[0].battery = leftBattery;
		g_neo2_client->controllers[0].button_a = leftA;
		g_neo2_client->controllers[0].button_b = leftB;
		g_neo2_client->controllers[0].grip = leftGrip;
		g_neo2_client->controllers[0].thumbstick_click = leftClick;
		g_neo2_client->controllers[0].menu = leftMenu;
		g_neo2_client->controllers[0].connected = true;
		if (leftTouch)
		{
			int t[2];
			env->GetIntArrayRegion(leftTouch, 0, 2, t);
			g_neo2_client->controllers[0].touch[0] = t[0];
			g_neo2_client->controllers[0].touch[1] = t[1];
		}
	}
	else
	{
		std::lock_guard lock(g_neo2_client->frame_mutex);
		g_neo2_client->controllers[0].connected = false;
	}

	if (rightOrient && rightPos)
	{
		float o[4], p[3];
		env->GetFloatArrayRegion(rightOrient, 0, 4, o);
		env->GetFloatArrayRegion(rightPos, 0, 3, p);
		std::lock_guard lock(g_neo2_client->frame_mutex);
		memcpy(g_neo2_client->controllers[1].orientation, o, sizeof(o));
		memcpy(g_neo2_client->controllers[1].position, p, sizeof(p));
		g_neo2_client->controllers[1].trigger = rightTrigger;
		g_neo2_client->controllers[1].battery = rightBattery;
		g_neo2_client->controllers[1].button_a = rightA;
		g_neo2_client->controllers[1].button_b = rightB;
		g_neo2_client->controllers[1].grip = rightGrip;
		g_neo2_client->controllers[1].thumbstick_click = rightClick;
		g_neo2_client->controllers[1].menu = rightMenu;
		g_neo2_client->controllers[1].connected = true;
		if (rightTouch)
		{
			int t[2];
			env->GetIntArrayRegion(rightTouch, 0, 2, t);
			g_neo2_client->controllers[1].touch[0] = t[0];
			g_neo2_client->controllers[1].touch[1] = t[1];
		}
	}
	else
	{
		std::lock_guard lock(g_neo2_client->frame_mutex);
		g_neo2_client->controllers[1].connected = false;
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDrawEye(JNIEnv * env, jobject thiz, jlong ptr, jint eye)
{
	if (!g_neo2_client || !g_neo2_client->gl_initialized)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	if (eye < 0 || eye > 2)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		return;
	}

	std::shared_ptr<neo2_decoded_frame> decoded;
	{
		std::lock_guard lock(g_neo2_client->decoded_frame_mutex);
		decoded = g_neo2_client->latest_decoded_frames[eye];
	}

	GLuint tex = 0;
	if (decoded && decoded->valid && decoded->hw_buffer)
	{
		AHardwareBuffer * hb = decoded->hw_buffer;

		if (g_neo2_client->eye_textures[eye] == 0)
		{
			glGenTextures(1, &g_neo2_client->eye_textures[eye]);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_neo2_client->eye_textures[eye]);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
			spdlog::info("Created eye texture {} for eye {}", g_neo2_client->eye_textures[eye], eye);
		}

		if (g_neo2_client->last_hb[eye] != hb)
		{
			if (g_neo2_client->eye_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				g_neo2_client->egl_destroy_image(
					eglGetDisplay(EGL_DEFAULT_DISPLAY), g_neo2_client->eye_egl_images[eye]);
				g_neo2_client->eye_egl_images[eye] = EGL_NO_IMAGE_KHR;
			}

			g_neo2_client->eye_current_frames[eye].reset();
			g_neo2_client->last_hb[eye] = nullptr;

			g_neo2_client->eye_current_frames[eye] = decoded;
			g_neo2_client->last_hb[eye] = hb;

			EGLClientBuffer client_buffer = g_neo2_client->egl_get_native_buffer(hb);
			if (client_buffer)
			{
				EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
				g_neo2_client->eye_egl_images[eye] = g_neo2_client->egl_create_image(
					eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_CONTEXT,
					EGL_NATIVE_BUFFER_ANDROID, client_buffer, attrs);
			}

			if (g_neo2_client->eye_egl_images[eye] != EGL_NO_IMAGE_KHR)
			{
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_neo2_client->eye_textures[eye]);
				g_neo2_client->gl_egl_image_target(
					GL_TEXTURE_EXTERNAL_OES, g_neo2_client->eye_egl_images[eye]);
				glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
				spdlog::debug("EGLImage for eye {} tex={}", eye, g_neo2_client->eye_textures[eye]);
			}
			else
			{
				spdlog::warn("Failed EGLImage for eye {}", eye);
			}
		}

		tex = g_neo2_client->eye_textures[eye];
	}

	g_neo2_client->blit_pipeline.draw(eye, tex);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnFrameEnd(JNIEnv * env, jobject thiz, jlong ptr)
{
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnInitGL(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	if (!g_neo2_client)
		return;

	spdlog::info("nativeInitGL: {}x{}", w, h);

	g_neo2_client->eye_width = w;
	g_neo2_client->eye_height = h;

	g_neo2_client->blit_pipeline.init(w, h);
	g_neo2_client->load_egl_procs();
	g_neo2_client->gl_initialized = true;

	spdlog::info("GLES initialized");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnDeInitGL(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeDeInitGL");
	if (g_neo2_client)
		g_neo2_client->gl_initialized = false;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnSurfaceChanged(JNIEnv * env, jobject thiz, jlong ptr, jint w, jint h)
{
	spdlog::info("nativeSurfaceChanged: {}x{}", w, h);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderPause(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderPause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderResume(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRenderResume");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRendererShutdown(JNIEnv * env, jobject thiz, jlong ptr)
{
	spdlog::info("nativeRendererShutdown");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeWivrnRenderEvent(JNIEnv * env, jobject thiz, jlong ptr, jint event)
{
	spdlog::info("nativeRenderEvent: {}", event);
}

} // extern "C"
