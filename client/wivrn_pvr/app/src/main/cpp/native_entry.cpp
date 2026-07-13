#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/native_window.h>

#define LOG_TAG "WiVRn-Pico"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
void wivrn_pvr_set_window(ANativeWindow * window);
void wivrn_pvr_init_sdk(JNIEnv * env, jobject activity);
JNIEXPORT void JNICALL Java_org_meumeu_wivrn_MainActivity_nativeStart(JNIEnv *, jobject, jobject, jobject);
}

static void app_handle_cmd(struct android_app * app, int32_t cmd)
{
	switch (cmd)
	{
		case APP_CMD_INIT_WINDOW:
			LOGI("APP_CMD_INIT_WINDOW: window=%p", app->window);
			wivrn_pvr_set_window(app->window);
			break;
		case APP_CMD_TERM_WINDOW:
			LOGI("APP_CMD_TERM_WINDOW");
			wivrn_pvr_set_window(nullptr);
			break;
		case APP_CMD_GAINED_FOCUS:
			LOGI("APP_CMD_GAINED_FOCUS");
			break;
		case APP_CMD_LOST_FOCUS:
			LOGI("APP_CMD_LOST_FOCUS");
			break;
		case APP_CMD_DESTROY:
			LOGI("APP_CMD_DESTROY");
			break;
	}
}

extern "C" void android_main(struct android_app * androidApp)
{
	androidApp->onAppCmd = app_handle_cmd;

	JNIEnv * env = nullptr;
	androidApp->activity->vm->AttachCurrentThread(&env, nullptr);

	// Let Java set up the client with the launch intent.
	jclass clazz = env->GetObjectClass(androidApp->activity->clazz);
	jmethodID nativeStart = env->GetMethodID(clazz, "nativeStart", "(Landroid/app/Activity;Landroid/content/Intent;)V");
	if (nativeStart)
	{
		jmethodID getIntent = env->GetMethodID(clazz, "getIntent", "()Landroid/content/Intent;");
		jobject intent = getIntent ? env->CallObjectMethod(androidApp->activity->clazz, getIntent) : nullptr;
		env->CallVoidMethod(androidApp->activity->clazz, nativeStart, androidApp->activity->clazz, intent);
	}
	env->DeleteLocalRef(clazz);

	// PVR SDK must be initialized on the NativeActivity thread before the render thread starts.
	wivrn_pvr_init_sdk(env, androidApp->activity->clazz);

	for (;;)
	{
		int events;
		struct android_poll_source * source;
		int ret = ALooper_pollOnce(-1, nullptr, &events, (void **)&source);
		if (ret == ALOOPER_POLL_TIMEOUT || ret == ALOOPER_POLL_ERROR)
			break;
		if (source)
			source->process(androidApp, source);
		if (androidApp->destroyRequested)
			break;
	}

	androidApp->activity->vm->DetachCurrentThread();
}
