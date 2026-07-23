#include "android_ui.h"
#include "gl_util.h"
#include "log.h"
#include <cstring>

static const char *kVS =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); }\n";

static const char *kFS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uTex;\n"
    "in vec2 vUV; out vec4 oColor;\n"
    "void main(){ oColor=texture(uTex,vUV); }\n";

AndroidUi::~AndroidUi()
{
    reset();
}

void AndroidUi::reset()
{
    if (m_tex)  { glDeleteTextures(1, &m_tex);  m_tex = 0; }
    if (m_fbo)  { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
    if (m_prog) { glDeleteProgram(m_prog); m_prog = 0; }
    if (m_vao)  { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo)  { glDeleteBuffers(1, &m_vbo); m_vbo = 0; }
}

bool AndroidUi::init()
{
    // Create the UI texture
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kUiW, kUiH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Compile compositor shader
    m_prog = glCreateProgram();
    GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    glAttachShader(m_prog, vs);
    glAttachShader(m_prog, fs);
    glLinkProgram(m_prog);
    GLint ok = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(m_prog, 512, nullptr, log);
        LOGE("AndroidUi shader link failed: %s", log);
        return false;
    }
    m_mvpLoc = glGetUniformLocation(m_prog, "uMVP");
    m_texLoc = glGetUniformLocation(m_prog, "uTex");

    // Build the panel quad (pos.xyz + uv.xy)
    // V is flipped (1-v) because Android Bitmap pixels are top-to-bottom
    // but OpenGL texture origin is bottom-to-left.
    float verts[] = {
        kPanelL, kPanelT, 0, 0, 0,   // top-left
        kPanelL, kPanelB, 0, 0, 1,   // bottom-left
        kPanelR, kPanelB, 0, 1, 1,   // bottom-right
        kPanelL, kPanelT, 0, 0, 0,   // top-left
        kPanelR, kPanelB, 0, 1, 1,   // bottom-right
        kPanelR, kPanelT, 0, 1, 0,   // top-right
    };
    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);

    LOGI("AndroidUi initialized: %dx%d tex=%u prog=%u", kUiW, kUiH, m_tex, m_prog);
    return true;
}

void AndroidUi::uploadPixels(const void *pixels)
{
    if (!m_tex || !pixels) return;
    size_t sz = (size_t)kUiW * kUiH * 4;
    if (m_staging.size() != sz) m_staging.resize(sz);
    // Java Bitmap.getPixels returns ARGB ints; with native-order ByteBuffer
    // the bytes in memory are BGRA. Swap to RGBA for GL.
    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t *dst = m_staging.data();
    for (size_t i = 0; i < (size_t)kUiW * kUiH; i++) {
        dst[0] = src[2];  // R <- B
        dst[1] = src[1];  // G
        dst[2] = src[0];  // B <- R
        dst[3] = src[3];  // A
        src += 4;
        dst += 4;
    }
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kUiW, kUiH,
                    GL_RGBA, GL_UNSIGNED_BYTE, m_staging.data());
}

void AndroidUi::setInput(float lx, float ly, bool pressed, bool onPanel, bool clickEdge)
{
    if (!onPanel) {
        m_touchX = -1;
        m_touchY = -1;
        m_touchPressed = false;
    } else {
        m_touchX = mToPxX(lx);
        m_touchY = mToPxY(ly);
        m_touchPressed = pressed;
    }
    m_touchClickEdge = clickEdge;
}

void AndroidUi::draw(const float mvp[16])
{
    if (!m_prog || !m_tex) return;
    glUseProgram(m_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glUniform1i(m_texLoc, 0);
    glUniformMatrix4fv(m_mvpLoc, 1, GL_FALSE, mvp);
    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

float AndroidUi::mToPxX(float m)
{
    return (m - kPanelL) / (kPanelR - kPanelL) * kUiW;
}

float AndroidUi::mToPxY(float m)
{
    return (kPanelT - m) / (kPanelT - kPanelB) * kUiH;
}

float AndroidUi::pxToMX(float px)
{
    return kPanelL + px / kUiW * (kPanelR - kPanelL);
}

float AndroidUi::pxToMY(float py)
{
    return kPanelT - py / kUiH * (kPanelT - kPanelB);
}
