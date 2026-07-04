#include "pico_client.h"
#include "pico_stutter.h"
#include "pico_render_thread.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/android_sink.h>

#include <cstring>
#include <cmath>
#include <android/native_window_jni.h>

using namespace wivrn;

static pico_render_thread g_render_thread;

std::optional<std::string> get_server_uri_from_intent(JNIEnv * env, jobject intent)
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

std::pair<std::string, int> parse_uri(const std::string & uri)
{
	std::string host;
	int port = 5353;

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

std::optional<std::string> parse_pin_from_uri(const std::string & uri)
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

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeStart(JNIEnv * env, jobject thiz, jobject activity, jobject intent)
{
	static auto logger = spdlog::android_logger_mt("WiVRn-Pico", "WiVRn-Pico");
	spdlog::set_default_logger(logger);
	spdlog::set_level(spdlog::level::debug);

	spdlog::info("WiVRn PvrSDK-Native client starting (ATW mode)");

	auto * client = new pico_client();
	g_client = client;

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
		spdlog::warn("No server URI provided, waiting for intent");
		client->server_host = "";
		client->server_port = 5353;
	}

	g_render_thread.start(client);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeSurfaceChanged(JNIEnv * env, jobject thiz, jobject surface)
{
	if (!surface)
	{
		g_render_thread.clear_surface();
		return;
	}

	ANativeWindow * win = ANativeWindow_fromSurface(env, surface);
	if (win)
		g_render_thread.set_surface(win);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeSurfaceDestroyed(JNIEnv * env, jobject thiz)
{
	g_render_thread.clear_surface();
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeStop(JNIEnv * env, jobject thiz)
{
	g_render_thread.stop();

	if (g_client)
	{
		g_client->shutdown = true;
		g_client->running = false;

		g_client->tracker.stop();

		if (g_client->network_thread.joinable())
			g_client->network_thread.join();

		if (g_client->activity)
			env->DeleteGlobalRef(g_client->activity);

		delete g_client;
		g_client = nullptr;

		spdlog::info("WiVRn PvrSDK-Native client destroyed");
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativePause(JNIEnv * env, jobject thiz)
{
	if (g_client)
		g_client->running = false;
	spdlog::info("nativePause");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeResume(JNIEnv * env, jobject thiz)
{
	if (!g_client)
		return;

	g_client->try_connect();
	g_client->running = true;
	spdlog::info("nativeResume");
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeNewIntent(JNIEnv * env, jobject thiz, jobject intent)
{
	if (!g_client)
		return;

	auto uri = get_server_uri_from_intent(env, intent);
	if (uri)
	{
		spdlog::info("New server URI: {}", *uri);
		auto [host, port] = parse_uri(*uri);
		g_client->server_host = host;
		g_client->server_port = port;
		g_client->tcp_only = uri->starts_with("wivrn+tcp://");

		auto pin = parse_pin_from_uri(*uri);
		if (pin)
		{
			g_client->pairing_pin = *pin;
			spdlog::info("PIN from new intent URI: {}", *pin);
		}

		if (g_client->session)
		{
			spdlog::info("Tearing down existing session for reconnection");
			g_client->tracker.stop();
			g_client->tracker.session = nullptr;
			g_client->session.reset();
			if (g_client->network_thread.joinable())
				g_client->network_thread.join();
		}

		g_client->try_connect();
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeGetHeadData(
	JNIEnv * env, jobject thiz, jfloatArray out)
{
	if (!g_client || !out)
		return;
	if (env->GetArrayLength(out) < 7)
		return;

	float orient[4] = {0, 0, 0, 1};
	float pos[3] = {0, 0, 0};
	g_client->tracker.get_head_pose(orient, pos);

	float buf[7] = {
		orient[0], orient[1], orient[2], orient[3],
		pos[0], pos[1], pos[2]};
	env->SetFloatArrayRegion(out, 0, 7, buf);
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeControllerState(
	JNIEnv * env, jobject thiz, jint hand, jint conn, jfloatArray sensor, jfloatArray angVel, jintArray keys)
{
	if (!g_client)
		return;

	float sensor_buf[7] = {0};
	if (sensor && env->GetArrayLength(sensor) >= 7)
		env->GetFloatArrayRegion(sensor, 0, 7, sensor_buf);

	int keys_buf[11] = {0};
	if (keys && env->GetArrayLength(keys) >= 11)
		env->GetIntArrayRegion(keys, 0, 11, keys_buf);

	g_client->tracker.update_controller_from_jni(hand, conn,
		sensor ? sensor_buf : nullptr,
		nullptr,
		keys ? keys_buf : nullptr);
}

JNIEXPORT jboolean JNICALL Java_org_meumeu_wivrn_MainActivity_nativeDrainHaptic(
	JNIEnv * env, jobject thiz, jint hand, jfloatArray out)
{
	if (!g_client || hand < 0 || hand > 1 || !out)
		return JNI_FALSE;
	if (env->GetArrayLength(out) < 2)
		return JNI_FALSE;

	float amp;
	int ms;
	{
		std::lock_guard lock(g_client->haptics_mutex);
		auto & slot = g_client->rumble[hand];
		if (!slot.active)
			return JNI_FALSE;
		amp = slot.amplitude;
		ms = slot.duration_ms;
		slot.active = false;
		slot.amplitude = 0.f;
		slot.duration_ms = 0;
	}

	float vals[2] = {amp, static_cast<float>(ms)};
	env->SetFloatArrayRegion(out, 0, 2, vals);
	return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeSubmitPin(JNIEnv * env, jobject thiz, jstring pin)
{
	if (!g_client)
		return;

	const char * pin_str = env->GetStringUTFChars(pin, nullptr);
	if (pin_str)
	{
		std::string pin_cpp(pin_str);
		env->ReleaseStringUTFChars(pin, pin_str);
		spdlog::warn("Received PIN from Java dialog: \"{}\"", pin_cpp);
		try
		{
			g_client->pin_promise.set_value(pin_cpp);
		}
		catch (const std::future_error & e)
		{
			spdlog::error("Failed to set PIN promise: {}", e.what());
		}
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeConnect(JNIEnv * env, jobject thiz, jstring hostname, jint port, jboolean tcpOnly)
{
	if (!g_client)
		return;

	const char * host_str = env->GetStringUTFChars(hostname, nullptr);
	if (host_str)
	{
		g_client->server_host = host_str;
		g_client->server_port = port;
		g_client->tcp_only = tcpOnly;
		g_client->pairing_pin.clear();
		env->ReleaseStringUTFChars(hostname, host_str);

		spdlog::info("Connect requested: {}:{} tcp={}", g_client->server_host, g_client->server_port, g_client->tcp_only);

		if (g_client->connect_thread.joinable())
			g_client->connect_thread.join();

		g_client->running = true;
		g_client->shutdown = false;
		g_client->connect_thread = std::thread([&]() {
			g_client->try_connect();
		});
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeDisconnect(JNIEnv * env, jobject thiz)
{
	if (!g_client)
		return;

	spdlog::info("Disconnect requested");
	g_client->running = false;
	g_client->shutdown = true;
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeRequestAppList(JNIEnv * env, jobject thiz)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Requesting application list from server");
	try
	{
		g_client->session->send_control(from_headset::get_application_list{
			.language = "en",
			.country = "US",
			.variant = "",
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to request application list: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeStartApp(JNIEnv * env, jobject thiz, jstring appId)
{
	if (!g_client || !g_client->session)
		return;

	const char * app_id_str = env->GetStringUTFChars(appId, nullptr);
	if (app_id_str)
	{
		spdlog::info("Starting application: {}", app_id_str);
		try
		{
			g_client->session->send_control(from_headset::start_app{
				.app_id = app_id_str,
			});
		}
		catch (std::exception & e)
		{
			spdlog::warn("Failed to start application: {}", e.what());
		}
		env->ReleaseStringUTFChars(appId, app_id_str);
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeRequestRunningApps(JNIEnv * env, jobject thiz)
{
	if (!g_client || !g_client->session)
		return;

	try
	{
		g_client->session->send_control(from_headset::get_running_applications{});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to request running applications: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeSetActiveApp(JNIEnv * env, jobject thiz, jint appId)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Setting active application: {}", appId);
	try
	{
		g_client->session->send_control(from_headset::set_active_application{
			.id = (uint32_t)appId,
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to set active application: {}", e.what());
	}
}

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeStopApp(JNIEnv * env, jobject thiz, jint appId)
{
	if (!g_client || !g_client->session)
		return;

	spdlog::info("Stopping application: {}", appId);
	try
	{
		g_client->session->send_control(from_headset::stop_application{
			.id = (uint32_t)appId,
		});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Failed to stop application: {}", e.what());
	}
}

} // extern "C"
