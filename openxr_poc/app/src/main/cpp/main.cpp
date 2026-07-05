// OpenXR POC: Spinning cube in stereo VR on Pico Neo 2
// Uses Pico's libopenxr_loader.so with NativeActivity.

#include <jni.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <cmath>
#include <ctime>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#define LOG_TAG "OpenXRPoc"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CHK_XR(call, msg) do { \
    XrResult _r = (call); \
    if (_r != XR_SUCCESS) { \
        LOGE("%s failed: %d", msg, _r); \
    } \
} while (0)

#define NUM_EYES 2

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

static void mat4_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.f;
}

static void mat4_perspective(float m[16], float fovLeft, float fovRight,
                             float fovUp, float fovDown, float nearZ, float farZ) {
    float tanL = tanf(fovLeft);
    float tanR = tanf(fovRight);
    float tanU = tanf(fovUp);
    float tanD = tanf(fovDown);
    float tanW = tanR - tanL;
    float tanH = tanU - tanD;

    float px = 2.f / tanW;
    float py = 2.f / tanH;
    float oz = tanL / tanW + 0.5f;
    float oy = tanD / tanH + 0.5f;

    float a = farZ == 0.f ? 1.f : farZ / (farZ - nearZ);
    float b = farZ == 0.f ? 0.f : -(farZ * nearZ) / (farZ - nearZ);

    memset(m, 0, sizeof(float) * 16);
    m[0]  = px;
    m[5]  = py;
    m[8]  = 2.f * oz - 1.f;
    m[9]  = 2.f * oy - 1.f;
    m[10] = -a;
    m[11] = -1.f;
    m[14] = -b;
}

static void mat4_translation(float m[16], float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void mat4_rotation_y(float m[16], float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4_identity(m);
    m[0] = c;  m[2] = s;
    m[8] = -s; m[10] = c;
}

static void mat4_rotation_x(float m[16], float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4_identity(m);
    m[5] = c;  m[6] = -s;
    m[9] = s;  m[10] = c;
}

static void mat4_multiply(float out[16], const float a[16], const float b[16]) {
    float t[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t[i * 4 + j] = 0.f;
            for (int k = 0; k < 4; k++)
                t[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
        }
    }
    memcpy(out, t, sizeof(float) * 16);
}

static void mat4_invert_rigid(float out[16], const float m[16]) {
    float r[16];
    mat4_identity(r);
    r[0] = m[0];  r[1] = m[4];  r[2] = m[8];
    r[4] = m[1];  r[5] = m[5];  r[6] = m[9];
    r[8] = m[2];  r[9] = m[6];  r[10] = m[10];

    float tx = -(r[0] * m[12] + r[4] * m[13] + r[8]  * m[14]);
    float ty = -(r[1] * m[12] + r[5] * m[13] + r[9]  * m[14]);
    float tz = -(r[2] * m[12] + r[6] * m[13] + r[10] * m[14]);
    r[12] = tx; r[13] = ty; r[14] = tz;
    memcpy(out, r, sizeof(float) * 16);
}

static void pose_to_mat4(float out[16], const XrPosef& pose) {
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;

    float n = qx * qx + qy * qy + qz * qz + qw * qw;
    float s = n > 0.f ? 2.f / n : 0.f;

    float xs = qx * s, ys = qy * s, zs = qz * s;
    float wx = qw * xs, wy = qw * ys, wz = qw * zs;
    float xx = qx * xs, xy = qx * ys, xz = qx * zs;
    float yy = qy * ys, yz = qy * zs, zz = qz * zs;

    out[0]  = 1.f - (yy + zz);
    out[1]  = xy + wz;
    out[2]  = xz - wy;
    out[3]  = 0.f;
    out[4]  = xy - wz;
    out[5]  = 1.f - (xx + zz);
    out[6]  = yz + wx;
    out[7]  = 0.f;
    out[8]  = xz + wy;
    out[9]  = yz - wx;
    out[10] = 1.f - (xx + yy);
    out[11] = 0.f;
    out[12] = pose.position.x;
    out[13] = pose.position.y;
    out[14] = pose.position.z;
    out[15] = 1.f;
}

// ---------------------------------------------------------------------------
// Cube geometry
// ---------------------------------------------------------------------------

struct Vertex {
    float pos[3];
    float color[3];
};

static const Vertex cubeVerts[] = {
    {{ 0.5f, -0.5f, -0.5f}, {1, 0, 0}}, {{ 0.5f,  0.5f, -0.5f}, {1, 0, 0}},
    {{ 0.5f,  0.5f,  0.5f}, {1, 0, 0}}, {{ 0.5f, -0.5f,  0.5f}, {1, 0, 0}},
    {{-0.5f, -0.5f,  0.5f}, {0, 1, 0}}, {{-0.5f,  0.5f,  0.5f}, {0, 1, 0}},
    {{-0.5f,  0.5f, -0.5f}, {0, 1, 0}}, {{-0.5f, -0.5f, -0.5f}, {0, 1, 0}},
    {{-0.5f,  0.5f, -0.5f}, {0, 0, 1}}, {{-0.5f,  0.5f,  0.5f}, {0, 0, 1}},
    {{ 0.5f,  0.5f,  0.5f}, {0, 0, 1}}, {{ 0.5f,  0.5f, -0.5f}, {0, 0, 1}},
    {{-0.5f, -0.5f,  0.5f}, {1, 1, 0}}, {{-0.5f, -0.5f, -0.5f}, {1, 1, 0}},
    {{ 0.5f, -0.5f, -0.5f}, {1, 1, 0}}, {{ 0.5f, -0.5f,  0.5f}, {1, 1, 0}},
    {{ 0.5f, -0.5f,  0.5f}, {1, 0, 1}}, {{ 0.5f,  0.5f,  0.5f}, {1, 0, 1}},
    {{-0.5f,  0.5f,  0.5f}, {1, 0, 1}}, {{-0.5f, -0.5f,  0.5f}, {1, 0, 1}},
    {{-0.5f, -0.5f, -0.5f}, {0, 1, 1}}, {{-0.5f,  0.5f, -0.5f}, {0, 1, 1}},
    {{ 0.5f,  0.5f, -0.5f}, {0, 1, 1}}, {{ 0.5f, -0.5f, -0.5f}, {0, 1, 1}},
};

static const unsigned short cubeIndices[] = {
     0,  1,  2,   0,  2,  3,
     4,  5,  6,   4,  6,  7,
     8,  9, 10,   8, 10, 11,
    12, 13, 14,  12, 14, 15,
    16, 17, 18,  16, 18, 19,
    20, 21, 22,  20, 22, 23,
};

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

static const char* vertSrc = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
uniform mat4 uMVP;
out vec3 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4((uMVP * vec4(aPos, 1.0)).xyz, 1.0);
}
)GLSL";

static const char* fragSrc = R"GLSL(#version 300 es
precision mediump float;
in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 1.0);
}
)GLSL";

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint make_program() {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vertSrc);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("Program link error: %s", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

struct SwapchainState {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct AppState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;

    SwapchainState swapchains[NUM_EYES];
    XrViewConfigurationView viewConfigs[NUM_EYES] = {};

    GLuint program = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLint mvpLoc = -1;
    GLuint fbo = 0;

    float startTime = 0.f;
    bool shouldExit = false;
};

// ---------------------------------------------------------------------------
// EGL: windowless context (pbuffer surface) for OpenXR rendering
// ---------------------------------------------------------------------------

static bool egl_init(AppState* app) {
    app->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (app->display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(app->display, &major, &minor)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    LOGI("EGL %d.%d", major, minor);

    const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(app->display, configAttribs, &app->config, 1, &numConfigs) || numConfigs == 0) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    app->context = eglCreateContext(app->display, app->config, EGL_NO_CONTEXT, ctxAttribs);
    if (app->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint pbAttribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    app->surface = eglCreatePbufferSurface(app->display, app->config, pbAttribs);
    if (app->surface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(app->display, app->surface, app->surface, app->context)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    LOGI("EGL context created (GLES3, windowless)");
    return true;
}

static void egl_shutdown(AppState* app) {
    if (app->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(app->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app->surface != EGL_NO_SURFACE) {
            eglDestroySurface(app->display, app->surface);
            app->surface = EGL_NO_SURFACE;
        }
        if (app->context != EGL_NO_CONTEXT) {
            eglDestroyContext(app->display, app->context);
            app->context = EGL_NO_CONTEXT;
        }
        eglTerminate(app->display);
        app->display = EGL_NO_DISPLAY;
    }
}

// ---------------------------------------------------------------------------
// OpenXR initialization
// ---------------------------------------------------------------------------

static bool openxr_init(struct android_app* androidApp, AppState* app) {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (XR_SUCCEEDED(
            xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  (PFN_xrVoidFunction*)(&initializeLoader)))) {
        XrLoaderInitInfoAndroidKHR loaderInitInfo;
        memset(&loaderInitInfo, 0, sizeof(loaderInitInfo));
        loaderInitInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        loaderInitInfo.next = nullptr;
        loaderInitInfo.applicationVM = androidApp->activity->vm;
        loaderInitInfo.applicationContext = androidApp->activity->clazz;
        CHK_XR(initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfo),
               "xrInitializeLoaderKHR");
        LOGI("xrInitializeLoaderKHR succeeded");
    }

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount);
    for (auto& e : exts) e.type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasGLES = false, hasAndroidCI = false;
    for (const auto& e : exts) {
        LOGI("Extension: %s", e.extensionName);
        if (strcmp(e.extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) == 0)
            hasGLES = true;
        if (strcmp(e.extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) == 0)
            hasAndroidCI = true;
    }
    if (!hasGLES) {
        LOGE("XR_KHR_opengl_es_enable not supported");
        return false;
    }

    const char* enabledExts[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidCI = {};
    androidCI.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidCI.next = nullptr;
    androidCI.applicationVM = androidApp->activity->vm;
    androidCI.applicationActivity = androidApp->activity->clazz;

    XrInstanceCreateInfo ci = {};
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.next = &androidCI;
    ci.enabledExtensionCount = hasAndroidCI ? 2 : 1;
    ci.enabledExtensionNames = enabledExts;
    strcpy(ci.applicationInfo.applicationName, "OpenXR POC");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "No Engine");
    ci.applicationInfo.engineVersion = 0;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    XrResult r = xrCreateInstance(&ci, &app->instance);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateInstance failed: %d", r);
        return false;
    }

    XrInstanceProperties ip = {};
    ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    xrGetInstanceProperties(app->instance, &ip);
    LOGI("OpenXR runtime: %s (v%d.%d.%d)", ip.runtimeName,
         XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si = {};
    si.type = XR_TYPE_SYSTEM_GET_INFO;
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(app->instance, &si, &app->systemId);
    if (r != XR_SUCCESS) {
        LOGE("xrGetSystem failed: %d", r);
        return false;
    }

    XrSystemProperties sp = {};
    sp.type = XR_TYPE_SYSTEM_PROPERTIES;
    xrGetSystemProperties(app->instance, app->systemId, &sp);
    LOGI("System: %s, maxLayers=%u, maxSwapchainW=%u, maxSwapchainH=%u",
         sp.systemName, sp.graphicsProperties.maxLayerCount,
         sp.graphicsProperties.maxSwapchainImageWidth,
         sp.graphicsProperties.maxSwapchainImageHeight);

    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGLESReqs = nullptr;
    xrGetInstanceProcAddr(app->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetGLESReqs));
    if (!pfnGetGLESReqs) {
        LOGE("Failed to load xrGetOpenGLESGraphicsRequirementsKHR");
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    r = pfnGetGLESReqs(app->instance, app->systemId, &req);
    if (r != XR_SUCCESS) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR failed: %d", r);
        return false;
    }
    LOGI("GLES requirements: min=%d.%d, max=%d.%d",
         XR_VERSION_MAJOR(req.minApiVersionSupported),
         XR_VERSION_MINOR(req.minApiVersionSupported),
         XR_VERSION_MAJOR(req.maxApiVersionSupported),
         XR_VERSION_MINOR(req.maxApiVersionSupported));

    return true;
}

static bool openxr_create_session(AppState* app) {
    XrGraphicsBindingOpenGLESAndroidKHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = app->display;
    binding.config = app->config;
    binding.context = app->context;

    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO;
    sci.next = &binding;
    sci.systemId = app->systemId;

    XrResult r = xrCreateSession(app->instance, &sci, &app->session);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateSession failed: %d", r);
        return false;
    }
    LOGI("Session created");

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.f;

    r = xrCreateReferenceSpace(app->session, &rsci, &app->localSpace);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateReferenceSpace failed: %d", r);
        return false;
    }

    for (uint32_t i = 0; i < NUM_EYES; i++)
        app->viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

    uint32_t actualCount = NUM_EYES;
    r = xrEnumerateViewConfigurationViews(
        app->instance, app->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        NUM_EYES, &actualCount, app->viewConfigs);
    if (r != XR_SUCCESS) {
        LOGE("xrEnumerateViewConfigurationViews failed: %d", r);
        return false;
    }
    for (uint32_t i = 0; i < NUM_EYES; i++) {
        LOGI("Eye %u: %ux%u, samples=%u",
             i, app->viewConfigs[i].recommendedImageRectWidth,
             app->viewConfigs[i].recommendedImageRectHeight,
             app->viewConfigs[i].recommendedSwapchainSampleCount);
    }

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        XrSwapchainCreateInfo swci = {};
        swci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swci.format = GL_RGBA8;
        swci.width = app->viewConfigs[eye].recommendedImageRectWidth;
        swci.height = app->viewConfigs[eye].recommendedImageRectHeight;
        swci.sampleCount = app->viewConfigs[eye].recommendedSwapchainSampleCount;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;

        r = xrCreateSwapchain(app->session, &swci, &app->swapchains[eye].handle);
        if (r != XR_SUCCESS) {
            LOGE("xrCreateSwapchain eye %u failed: %d", eye, r);
            return false;
        }
        app->swapchains[eye].width = swci.width;
        app->swapchains[eye].height = swci.height;

        uint32_t imgCount = 0;
        xrEnumerateSwapchainImages(app->swapchains[eye].handle, 0, &imgCount, nullptr);
        app->swapchains[eye].images.resize(imgCount);
        for (auto& img : app->swapchains[eye].images)
            img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(app->swapchains[eye].handle, imgCount, &imgCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(app->swapchains[eye].images.data()));
        LOGI("Eye %u swapchain: %ux%u, %u images", eye, swci.width, swci.height, imgCount);
    }

    return true;
}

static void openxr_destroy_session(AppState* app) {
    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        if (app->swapchains[eye].handle) {
            xrDestroySwapchain(app->swapchains[eye].handle);
            app->swapchains[eye].handle = XR_NULL_HANDLE;
            app->swapchains[eye].images.clear();
        }
    }
    if (app->localSpace) {
        xrDestroySpace(app->localSpace);
        app->localSpace = XR_NULL_HANDLE;
    }
    if (app->session) {
        xrDestroySession(app->session);
        app->session = XR_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// GL resources
// ---------------------------------------------------------------------------

static bool gl_init(AppState* app) {
    app->program = make_program();
    if (!app->program) return false;
    app->mvpLoc = glGetUniformLocation(app->program, "uMVP");

    glGenVertexArrays(1, &app->vao);
    glBindVertexArray(app->vao);

    glGenBuffers(1, &app->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, app->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(cubeVerts), cubeVerts, GL_STATIC_DRAW);

    glGenBuffers(1, &app->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, app->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(cubeIndices), cubeIndices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));

    glBindVertexArray(0);

    glGenFramebuffers(1, &app->fbo);
    return true;
}

static void gl_cleanup(AppState* app) {
    if (app->fbo) glDeleteFramebuffers(1, &app->fbo);
    if (app->vao) glDeleteVertexArrays(1, &app->vao);
    if (app->vbo) glDeleteBuffers(1, &app->vbo);
    if (app->ibo) glDeleteBuffers(1, &app->ibo);
    if (app->program) glDeleteProgram(app->program);
    app->fbo = app->vao = app->vbo = app->ibo = app->program = 0;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

static void poll_events(AppState* app) {
    XrEventDataBuffer edb = {};
    edb.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(app->instance, &edb) == XR_SUCCESS) {
        switch (edb.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* evt = reinterpret_cast<XrEventDataSessionStateChanged*>(&edb);
            XrSessionState old = app->sessionState;
            app->sessionState = evt->state;
            LOGI("Session state: %d -> %d", old, evt->state);

            if (evt->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {};
                bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(app->session, &bi);
                if (r == XR_SUCCESS) {
                    app->sessionRunning = true;
                    LOGI("Session begun");
                } else {
                    LOGE("xrBeginSession failed: %d", r);
                }
            } else if (evt->state == XR_SESSION_STATE_STOPPING) {
                app->sessionRunning = false;
                xrEndSession(app->session);
                LOGI("Session ended");
            } else if (evt->state == XR_SESSION_STATE_EXITING) {
                app->shouldExit = true;
                LOGI("Session exiting");
            }
            break;
        }
        default:
            break;
        }
        edb.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// ---------------------------------------------------------------------------
// Render frame
// ---------------------------------------------------------------------------

static void render_frame(AppState* app) {
    XrFrameWaitInfo fw = {};
    fw.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState fs = {};
    fs.type = XR_TYPE_FRAME_STATE;
    XrResult r = xrWaitFrame(app->session, &fw, &fs);
    if (r != XR_SUCCESS) {
        LOGE("xrWaitFrame failed: %d", r);
        return;
    }

    XrFrameBeginInfo fb = {};
    fb.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = xrBeginFrame(app->session, &fb);
    if (r != XR_SUCCESS) {
        LOGE("xrBeginFrame failed: %d", r);
        return;
    }

    XrViewState vstate = {};
    vstate.type = XR_TYPE_VIEW_STATE;

    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = app->localSpace;

    XrView views[NUM_EYES] = {};
    for (auto& v : views) v.type = XR_TYPE_VIEW;

    uint32_t viewCount = 0;
    r = xrLocateViews(app->session, &vli, &vstate, NUM_EYES, &viewCount, views);
    if (r != XR_SUCCESS || viewCount != NUM_EYES) {
        LOGE("xrLocateViews failed: %d (count=%u)", r, viewCount);
        xrEndFrame(app->session, nullptr);
        return;
    }

    XrCompositionLayerProjection layers[1];
    XrCompositionLayerProjectionView layerViews[NUM_EYES];

    float time = (float)((double)fs.predictedDisplayTime / 1e9) - app->startTime;

    for (uint32_t eye = 0; eye < NUM_EYES; eye++) {
        uint32_t imgIndex = 0;
        XrSwapchainImageAcquireInfo ai = {};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        r = xrAcquireSwapchainImage(app->swapchains[eye].handle, &ai, &imgIndex);
        if (r != XR_SUCCESS) {
            LOGE("xrAcquireSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }

        XrSwapchainImageWaitInfo wi = {};
        wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        wi.timeout = XR_INFINITE_DURATION;
        r = xrWaitSwapchainImage(app->swapchains[eye].handle, &wi);
        if (r != XR_SUCCESS) {
            LOGE("xrWaitSwapchainImage eye %u failed: %d", eye, r);
            continue;
        }

        GLuint glImg = app->swapchains[eye].images[imgIndex].image;
        int32_t w = app->swapchains[eye].width;
        int32_t h = app->swapchains[eye].height;

        glBindFramebuffer(GL_FRAMEBUFFER, app->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glImg, 0);

        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.15f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glUseProgram(app->program);

        float proj[16];
        mat4_perspective(proj,
                         views[eye].fov.angleLeft,
                         views[eye].fov.angleRight,
                         views[eye].fov.angleUp,
                         views[eye].fov.angleDown,
                         0.05f, 100.f);

        float viewMat[16], invView[16];
        pose_to_mat4(viewMat, views[eye].pose);
        mat4_invert_rigid(invView, viewMat);

        float rotY[16], rotX[16], trans[16], model[16], mv[16], mvp[16];
        mat4_rotation_y(rotY, time * 0.8f);
        mat4_rotation_x(rotX, time * 0.5f);
        mat4_translation(trans, 0.f, 0.f, -2.f);
        mat4_multiply(model, trans, rotY);
        mat4_multiply(model, model, rotX);
        mat4_multiply(mv, invView, model);
        mat4_multiply(mvp, proj, mv);

        glUniformMatrix4fv(app->mvpLoc, 1, GL_FALSE, mvp);

        glBindVertexArray(app->vao);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
        glBindVertexArray(0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        xrReleaseSwapchainImage(app->swapchains[eye].handle, &ri);

        layerViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        layerViews[eye].pose = views[eye].pose;
        layerViews[eye].fov = views[eye].fov;
        layerViews[eye].subImage.swapchain = app->swapchains[eye].handle;
        layerViews[eye].subImage.imageRect.offset = {0, 0};
        layerViews[eye].subImage.imageRect.extent = {w, h};
        layerViews[eye].subImage.imageArrayIndex = 0;
    }

    layers[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layers[0].layerFlags = 0;
    layers[0].space = app->localSpace;
    layers[0].viewCount = NUM_EYES;
    layers[0].views = layerViews;

    const XrCompositionLayerBaseHeader* layerHeaders[1] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layers[0])
    };

    XrFrameEndInfo fe = {};
    fe.type = XR_TYPE_FRAME_END_INFO;
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = 1;
    fe.layers = layerHeaders;

    r = xrEndFrame(app->session, &fe);
    if (r != XR_SUCCESS) {
        LOGE("xrEndFrame failed: %d", r);
    }
}

// ---------------------------------------------------------------------------
// Android main
// ---------------------------------------------------------------------------

struct AndroidAppState {
    ANativeWindow* nativeWindow = nullptr;
    bool resumed = false;
};

static void app_handle_cmd(struct android_app* app, int32_t cmd) {
    AndroidAppState* state = (AndroidAppState*)app->userData;
    switch (cmd) {
        case APP_CMD_RESUME:
            state->resumed = true;
            break;
        case APP_CMD_PAUSE:
            state->resumed = false;
            break;
        case APP_CMD_INIT_WINDOW:
            state->nativeWindow = app->window;
            break;
        case APP_CMD_TERM_WINDOW:
            state->nativeWindow = nullptr;
            break;
        case APP_CMD_DESTROY:
            state->nativeWindow = nullptr;
            break;
    }
}

static int32_t on_input_event(struct android_app* app, AInputEvent* event) {
    int type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(event);
        if (code == AKEYCODE_BACK) {
            return 1;
        }
    }
    return 0;
}

extern "C" void android_main(struct android_app* androidApp) {
    AndroidAppState androidState = {};
    androidApp->userData = &androidState;
    androidApp->onAppCmd = app_handle_cmd;
    androidApp->onInputEvent = on_input_event;

    AppState app;

    if (!egl_init(&app)) {
        LOGE("EGL init failed");
        return;
    }

    if (!openxr_init(androidApp, &app)) {
        LOGE("OpenXR init failed");
        egl_shutdown(&app);
        return;
    }

    if (!openxr_create_session(&app)) {
        LOGE("Session creation failed");
        xrDestroyInstance(app.instance);
        egl_shutdown(&app);
        return;
    }

    if (!gl_init(&app)) {
        LOGE("GL init failed");
        openxr_destroy_session(&app);
        xrDestroyInstance(app.instance);
        egl_shutdown(&app);
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    app.startTime = (float)ts.tv_sec + (float)ts.tv_nsec / 1e9f;

    LOGI("Entering main loop");

    while (androidApp->destroyRequested == 0) {
        for (;;) {
            int events;
            struct android_poll_source* source;
            int timeout = (!androidState.resumed && !app.sessionRunning) ? -1 : 0;
            int ret = ALooper_pollOnce(timeout, nullptr, &events, (void**)&source);
            if (ret == ALOOPER_POLL_TIMEOUT || ret == ALOOPER_POLL_ERROR)
                break;
            if (source)
                source->process(androidApp, source);
            if (androidApp->destroyRequested)
                break;
        }

        poll_events(&app);

        if (app.shouldExit)
            break;

        if (app.sessionRunning) {
            render_frame(&app);
        } else {
            usleep(16000);
        }
    }

    gl_cleanup(&app);
    openxr_destroy_session(&app);
    if (app.instance)
        xrDestroyInstance(app.instance);
    egl_shutdown(&app);

    LOGI("Main loop exited");
}
