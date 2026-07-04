#include "pico_client.h"

#include <spdlog/spdlog.h>
#include <GLES3/gl3.h>
#include <android/bitmap.h>

extern "C" {

JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeUpdateLobbyTexture(JNIEnv * env, jobject thiz, jobject bitmap)
{
	if (!g_client || !bitmap)
		return;

	AndroidBitmapInfo info;
	if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		spdlog::error("AndroidBitmap_getInfo failed");
		return;
	}

	if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
	{
		spdlog::error("Lobby bitmap format is not RGBA_8888 (got {})", info.format);
		return;
	}

	void * pixels = nullptr;
	if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
	{
		spdlog::error("AndroidBitmap_lockPixels failed");
		return;
	}

	g_client->lobby.update_texture(info.width, info.height, pixels);

	AndroidBitmap_unlockPixels(env, bitmap);
}

} // extern "C"
