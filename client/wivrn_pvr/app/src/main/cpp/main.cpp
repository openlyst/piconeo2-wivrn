#include <android/log.h>
#include <android_native_app_glue.h>

#define LOG_TAG "WiVRn-PVR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void app_handle_cmd(struct android_app* app, int32_t cmd)
{
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("APP_CMD_INIT_WINDOW: window=%p", app->window);
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("APP_CMD_TERM_WINDOW");
            break;
        case APP_CMD_GAINED_FOCUS:
            LOGI("APP_CMD_GAINED_FOCUS");
            break;
        case APP_CMD_LOST_FOCUS:
            LOGI("APP_CMD_LOST_FOCUS");
            break;
    }
}

extern "C" void android_main(struct android_app* androidApp)
{
    androidApp->onAppCmd = app_handle_cmd;

    for (;;) {
        int events;
        struct android_poll_source* source;
        int ret = ALooper_pollOnce(0, nullptr, &events, (void**)&source);
        if (ret == ALOOPER_POLL_TIMEOUT || ret == ALOOPER_POLL_ERROR)
            break;
        if (source)
            source->process(androidApp, source);
        if (androidApp->destroyRequested)
            break;
    }
}
