#include "passthrough.h"
#include "pico_sdk.h"
#include "log.h"
#include <cstring>

// Fullscreen clip-space quad: position (xyz) + UV. No view/projection needed —
// the quad already fills the eye buffer edge to edge. UVs follow the same
// top-left-origin flip the lobby UI texture uses (row 0 = top of camera frame).
static const char *vert_src = R"(#version 310 es
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main()
{
    gl_Position = vec4(a_pos, 1.0);
    v_uv = a_uv;
}
)";

static const char *frag_src = R"(#version 310 es
precision mediump float;
in vec2 v_uv;
uniform sampler2D u_tex;
out vec4 frag_color;
void main()
{
    frag_color = texture(u_tex, v_uv);
}
)";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("passthrough shader error: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void pico_passthrough::build_shaders()
{
    GLuint v = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!v || !f) return;

    program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE)
    {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        LOGE("passthrough program link error: %s", log);
        glDeleteProgram(program);
        program = 0;
    }
    glDeleteShader(v);
    glDeleteShader(f);

    sampler_loc = glGetUniformLocation(program, "u_tex");
}

void pico_passthrough::build_geometry()
{
    float quad[] = {
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
         1.0f, -1.0f, 0.0f,  1.0f, 1.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,  0.0f, 1.0f,
         1.0f,  1.0f, 0.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,  0.0f, 0.0f,
    };
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
    glBindVertexArray(0);
}

void pico_passthrough::upload_frame(int eye, unsigned char *data, int w, int h)
{
    if (!data || w <= 0 || h <= 0) return;

    // The Neo 2 camera delivers grayscale (1 byte/pixel). The SDK's own
    // SeeThrough renderer uploads with GL_LUMINANCE / GL_UNSIGNED_BYTE.
    // Using GL_RGBA here caused a 4x buffer overread → SIGSEGV in glTexImage2D.
    if (eye_tex[eye] == 0)
    {
        glGenTextures(1, &eye_tex[eye]);
        glBindTexture(GL_TEXTURE_2D, eye_tex[eye]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
        eye_w[eye] = w;
        eye_h[eye] = h;
    }
    else
    {
        glBindTexture(GL_TEXTURE_2D, eye_tex[eye]);
        if (w != eye_w[eye] || h != eye_h[eye])
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                         GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
            eye_w[eye] = w;
            eye_h[eye] = h;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void pico_passthrough::init()
{
    if (gl_ready) return;

    build_shaders();
    build_geometry();
    gl_ready = (program != 0 && vao != 0);
    if (gl_ready)
        LOGI("passthrough GL resources ready");
    else
        LOGE("passthrough GL init failed");
}

void pico_passthrough::start()
{
    if (camera_on) return;

    // Init the camera via GetCameraData_Ext (triggers GuardianSystem::InitializeCamera),
    // then start the preview and enable the SeeThrough layer. The SeeThrough
    // system has its own frame delivery path that actually receives frames
    // from the camera hardware — the raw GetCameraData_Ext path returns zero
    // buffers because the camera_callback never fires through it.
    Pvr_GetCameraData_Ext();
    PVR_SetCameraImageRect(kCamW, kCamH);
    PVR_StartCameraPreview(1);
    Pvr_BoundarySetSeeThroughVisible(true);
    camera_on = true;
    LOGI("passthrough camera started (%dx%d)", kCamW, kCamH);
}

void pico_passthrough::stop()
{
    if (!camera_on) return;
    Pvr_BoundarySetSeeThroughVisible(false);
    PVR_StartCameraPreview(0);
    camera_on = false;
    LOGI("passthrough camera stopped");
}

void pico_passthrough::draw(int eye)
{
    if (!gl_ready || !camera_on) return;
    if (eye < 0 || eye > 1) return;

    // Pvr_BoundaryGetSeeThroughData goes through the SeeThrough system which
    // has working frame delivery. Returns: data ptr, valid flag, w, h, count, timestamp.
    bool valid = false;
    unsigned int w = 0, h = 0, count = 0;
    long long ts = 0;
    unsigned char *frame = Pvr_BoundaryGetSeeThroughData(eye, &valid, &w, &h, &count, &ts);

    if (!frame || !valid || w == 0 || h == 0) {
        // Fall back to the other eye
        int other = eye ^ 1;
        frame = Pvr_BoundaryGetSeeThroughData(other, &valid, &w, &h, &count, &ts);
        if (!frame || !valid || w == 0 || h == 0)
            return;
    }

    static int frame_count = 0;
    if (++frame_count % 300 == 0) {
        LOGI("passthrough: see-through frame %d eye=%d %dx%d bytes=[%d,%d,%d,%d]",
             frame_count, eye, (int)w, (int)h, frame[0], frame[1], frame[2], frame[3]);
    }

    upload_frame(eye, frame, (int)w, (int)h);

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, eye_tex[eye]);
    glUniform1i(sampler_loc, 0);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    glDepthMask(GL_TRUE);
}

pico_passthrough::~pico_passthrough()
{
    if (program) glDeleteProgram(program);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    for (int e = 0; e < 2; e++)
        if (eye_tex[e]) glDeleteTextures(1, &eye_tex[e]);
}
