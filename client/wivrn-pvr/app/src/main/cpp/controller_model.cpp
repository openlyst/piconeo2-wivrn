#include "controller_model.h"
#include "log.h"
#include "math3d.h"
#include "gl_util.h"
#include <cstdio>
#include <cstring>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

// Textured shader: position + UV in, texture sample out.
static const char *kCtrlVS =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); }\n";

static const char *kCtrlFS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vUV; out vec4 oColor;\n"
    "uniform sampler2D uTex;\n"
    "void main(){ oColor=texture(uTex,vUV); }\n";

static GLuint sProg = 0;
static GLint  sMvpLoc = -1;
static GLint  sTexLoc = -1;

static ControllerModel sModels[2];

// System paths for OBJ + textures.
static const char *kObjPath[2] = {
    "/system/pre_resource/data/misc/user/controller/r.obj",
    "/system/pre_resource/data/misc/user/controller/controller2s.obj",
};

static const char *kTexPath[CTRL_TEX_COUNT] = {
    "/system/pre_resource/data/misc/user/controller/controller2s_idle.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_trigger.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_touchpad.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_app.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_home.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_volumedown.png",
    "/system/pre_resource/data/misc/user/controller/controller2s_volumeup.png",
};

// Load OBJ as triangles with UVs. Output: interleaved pos(3)+uv(2) per vertex.
static bool loadObjTextured(const char *path, std::vector<float> &out) {
    FILE *f = fopen(path, "r");
    if (!f) { LOGE("ctrl obj missing: %s", path); return false; }

    std::vector<float> vx, vy, vz;   // positions
    std::vector<float> tu, tv;       // tex coords
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='v' && line[1]=='t' && line[2]==' ') {
            float u, v;
            if (sscanf(line+3, "%f %f", &u, &v) == 2) { tu.push_back(u); tv.push_back(v); }
        } else if (line[0]=='v' && line[1]==' ') {
            float x, y, z;
            if (sscanf(line+2, "%f %f %f", &x, &y, &z) == 3) { vx.push_back(x); vy.push_back(y); vz.push_back(z); }
        } else if (line[0]=='f' && line[1]==' ') {
            // Parse face: v/vt/vn triples (or v//vn or v/vt or v)
            int vi[4] = {0}, ti[4] = {0};
            int n = 0;
            char *tok = strtok(line+2, " \t\r\n");
            while (tok && n < 4) {
                int vidx = 0, tidx = 0;
                // Try v/vt/vn format
                if (strchr(tok, '/')) {
                    sscanf(tok, "%d/%d", &vidx, &tidx);
                } else {
                    vidx = atoi(tok);
                }
                if (vidx != 0) vi[n] = (vidx > 0) ? vidx-1 : (int)vx.size()+vidx;
                if (tidx != 0) ti[n] = (tidx > 0) ? tidx-1 : (int)tu.size()+tidx;
                n++;
                tok = strtok(nullptr, " \t\r\n");
            }
            // Triangulate fan
            for (int i = 1; i < n - 1; i++) {
                int tri[3] = {0, i, i+1};
                for (int t = 0; t < 3; t++) {
                    int v = vi[tri[t]];
                    if (v < 0 || v >= (int)vx.size()) continue;
                    out.push_back(vx[v]); out.push_back(vy[v]); out.push_back(vz[v]);
                    int tt = ti[tri[t]];
                    if (tt >= 0 && tt < (int)tu.size()) {
                        out.push_back(tu[tt]); out.push_back(tv[tt]);
                    } else {
                        out.push_back(0.0f); out.push_back(0.0f);
                    }
                }
            }
        }
    }
    fclose(f);
    // Scale cm -> m
    for (size_t i = 0; i + 2 < out.size(); i += 5) {
        out[i]   *= 0.01f;
        out[i+1] *= 0.01f;
        out[i+2] *= 0.01f;
    }
    LOGI("ctrl obj %s -> %d triangles (%d verts)", path, (int)(out.size()/15), (int)(out.size()/5));
    return !out.empty();
}

// Load a PNG file into a GL texture. Returns 0 on failure.
static GLuint loadPngTexture(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { LOGE("ctrl tex missing: %s", path); return 0; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }

    std::vector<unsigned char> buf(sz);
    fread(buf.data(), 1, sz, f);
    fclose(f);

    int w, h, ch;
    unsigned char *img = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &ch, 4);
    if (!img) { LOGE("stb_image decode failed: %s", path); return 0; }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(img);
    LOGI("ctrl tex %s -> %u (%dx%d ch=%d)", path, tex, w, h, ch);
    return tex;
}

void initControllerModels(bool reload) {
    if (sProg == 0) {
        sProg = glCreateProgram();
        GLuint vs = compile(GL_VERTEX_SHADER, kCtrlVS);
        GLuint fs = compile(GL_FRAGMENT_SHADER, kCtrlFS);
        glAttachShader(sProg, vs);
        glAttachShader(sProg, fs);
        glLinkProgram(sProg);
        GLint ok = 0;
        glGetProgramiv(sProg, GL_LINK_STATUS, &ok);
        if (!ok) { char log[512]; glGetProgramInfoLog(sProg, 512, nullptr, log); LOGE("ctrl prog link: %s", log); }
        glDeleteShader(vs);
        glDeleteShader(fs);
        sMvpLoc = glGetUniformLocation(sProg, "uMVP");
        sTexLoc = glGetUniformLocation(sProg, "uTex");
        LOGI("controller model prog=%u", sProg);
    }

    for (int h = 0; h < 2; h++) {
        if (sModels[h].loaded && !reload) continue;
        if (reload && sModels[h].vao) {
            glDeleteVertexArrays(1, &sModels[h].vao);
            glDeleteBuffers(1, &sModels[h].vbo);
            for (int t = 0; t < CTRL_TEX_COUNT; t++)
                if (sModels[h].textures[t]) glDeleteTextures(1, &sModels[h].textures[t]);
            sModels[h] = ControllerModel();
        }

        std::vector<float> verts;
        if (!loadObjTextured(kObjPath[h], verts)) continue;

        glGenVertexArrays(1, &sModels[h].vao);
        glBindVertexArray(sModels[h].vao);
        glGenBuffers(1, &sModels[h].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, sModels[h].vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        // pos(3) + uv(2) = stride 5
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
        glBindVertexArray(0);
        sModels[h].vertCount = (int)(verts.size() / 5);

        for (int t = 0; t < CTRL_TEX_COUNT; t++)
            sModels[h].textures[t] = loadPngTexture(kTexPath[t]);

        sModels[h].loaded = true;
        LOGI("controller model %d loaded: %d verts", h, sModels[h].vertCount);
    }
}

static CtrlTexture pickTexture(bool trigger, bool touchpad, bool appButton,
                               bool home, bool volUp, bool volDown) {
    if (volUp)    return CTRL_TEX_VOLUP;
    if (volDown)  return CTRL_TEX_VOLDOWN;
    if (home)     return CTRL_TEX_HOME;
    if (appButton) return CTRL_TEX_APP;
    if (trigger)  return CTRL_TEX_TRIGGER;
    if (touchpad) return CTRL_TEX_TOUCHPAD;
    return CTRL_TEX_IDLE;
}

void drawControllerModel(int hand, const float quat[4], const float pos[3],
                         const float mvp[16],
                         bool trigger, bool grip, bool touchpad,
                         bool appButton, bool home,
                         bool volUp, bool volDown,
                         bool connected) {
    (void)grip; // grip doesn't have its own texture; falls through
    if (!connected || hand < 0 || hand > 1) return;
    ControllerModel &m = sModels[hand];
    if (!m.loaded || m.vertCount <= 0) return;

    CtrlTexture tex = pickTexture(trigger, touchpad, appButton, home, volUp, volDown);
    GLuint texId = m.textures[tex];
    if (!texId) texId = m.textures[CTRL_TEX_IDLE];
    if (!texId) return;

    Mat4 R = quatToMat4(quat[0], quat[1], quat[2], quat[3]);
    R.m[12] = pos[0]; R.m[13] = pos[1]; R.m[14] = pos[2];

    // mvp is already proj*view, so final = mvp * model
    Mat4 pvm;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += mvp[i*4+k] * R.m[k*4+j];
            pvm.m[i*4+j] = s;
        }

    glUseProgram(sProg);
    glUniformMatrix4fv(sMvpLoc, 1, GL_FALSE, pvm.m);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    glUniform1i(sTexLoc, 0);
    glBindVertexArray(m.vao);
    glDrawArrays(GL_TRIANGLES, 0, m.vertCount);
    glBindVertexArray(0);
}

void cleanupControllerModels() {
    for (int h = 0; h < 2; h++) {
        if (sModels[h].vao) glDeleteVertexArrays(1, &sModels[h].vao);
        if (sModels[h].vbo) glDeleteBuffers(1, &sModels[h].vbo);
        for (int t = 0; t < CTRL_TEX_COUNT; t++)
            if (sModels[h].textures[t]) glDeleteTextures(1, &sModels[h].textures[t]);
        sModels[h] = ControllerModel();
    }
    if (sProg) { glDeleteProgram(sProg); sProg = 0; }
}
